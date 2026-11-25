/*
 * faultctl — VitalGuard fault-injection & operator console.
 *
 * This is the demo's control surface (and the tests' remote control):
 *
 *   faultctl vfib [secs]            inject ventricular fibrillation
 *   faultctl rhythm <name> [secs]   normal|tachy|brady|afib|vtach|vfib|asystole
 *   faultctl desat <pct> [secs]     SpO2 desaturation event
 *   faultctl apnea [secs]           respiration pause
 *   faultctl hog start|stop         CPU hog (low-priority busy loop)
 *   faultctl kill dsp               supervisor SIGKILLs vitaldsp
 *   faultctl ack <alarm-id>         acknowledge an alarm
 *   faultctl silence [alarm-id]     audio-pause alarms (default: all)
 *   faultctl status                 supervisor status file
 *
 * Commands are one-shot IPC messages; nothing here is stateful.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "vg.h"
#include "vg_time.h"
#include "vg_path.h"
#include "ipc.h"
#include "signals.h"   /* vg_rhythm_t for the rhythm name map */

static int send_msg(const char *endpoint, uint32_t type,
                    const void *payload, uint32_t len)
{
    vg_ipc_conn_t *c = vg_ipc_connect(endpoint, 3000);
    int rc;
    if (!c) {
        fprintf(stderr, "faultctl: cannot reach %s "
                "(is the system running?)\n", endpoint);
        return -1;
    }
    rc = vg_ipc_send(c, type, payload, len, vg_now_ms());
    vg_ipc_close(c);
    if (rc != 0) {
        fprintf(stderr, "faultctl: send to %s failed\n", endpoint);
        return -1;
    }
    return 0;
}

static int send_fault(const char *endpoint, uint32_t kind, uint32_t rhythm,
                      float fparam, uint32_t duration_ms)
{
    vg_fault_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind        = kind;
    cmd.rhythm      = rhythm;
    cmd.fparam      = fparam;
    cmd.duration_ms = duration_ms;
    return send_msg(endpoint, VG_MSG_FAULT_CMD, &cmd, sizeof(cmd));
}

static int rhythm_by_name(const char *name, vg_rhythm_t *out)
{
    static const struct { const char *name; vg_rhythm_t r; } map[] = {
        { "normal",   VG_RHYTHM_NORMAL   },
        { "tachy",    VG_RHYTHM_TACHY    },
        { "brady",    VG_RHYTHM_BRADY    },
        { "afib",     VG_RHYTHM_AFIB     },
        { "vtach",    VG_RHYTHM_VTACH    },
        { "vfib",     VG_RHYTHM_VFIB     },
        { "asystole", VG_RHYTHM_ASYSTOLE },
    };
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].name) == 0) {
            *out = map[i].r;
            return 0;
        }
    return -1;
}

static uint32_t secs_arg(const char *s, uint32_t default_ms)
{
    return s ? (uint32_t)(strtod(s, NULL) * 1000.0) : default_ms;
}

/* ---- CPU hog management ---- */

static const char *g_argv0;   /* for vg_self_dir() in hog_start() */

static int hog_start(void)
{
    char exe[PATH_MAX], path[PATH_MAX], pidpath[PATH_MAX], logpath[PATH_MAX];
    pid_t pid;
    FILE *f;

    /* Portable self-location (see vg_path.h) — /proc/self/exe does
     * not exist as a path link on QNX. */
    if (vg_self_dir(exe, sizeof(exe), g_argv0) != 0)
        return -1;
    if (snprintf(path, sizeof(path), "%s/vghog", exe) >= (int)sizeof(path))
        return -1;
    snprintf(pidpath, sizeof(pidpath), "%s/hog.pid", vg_ipc_runtime_dir());
    snprintf(logpath, sizeof(logpath), "%s/hog.log", vg_ipc_runtime_dir());

    pid = fork();
    if (pid == 0) {
        int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        setsid();
        execl(path, "vghog", (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    f = fopen(pidpath, "w");
    if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
    }
    printf("hog started pid=%d\n", (int)pid);
    return 0;
}

static int hog_stop(void)
{
    char pidpath[PATH_MAX];
    FILE *f;
    long pid = -1;

    snprintf(pidpath, sizeof(pidpath), "%s/hog.pid", vg_ipc_runtime_dir());
    f = fopen(pidpath, "r");
    if (!f) {
        fprintf(stderr, "faultctl: no hog pidfile\n");
        return -1;
    }
    if (fscanf(f, "%ld", &pid) != 1)
        pid = -1;
    fclose(f);
    if (pid > 0) {
        kill((pid_t)pid, SIGTERM);
        printf("hog stopped pid=%ld\n", pid);
    }
    unlink(pidpath);
    return 0;
}

/* ---- main ---- */

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  faultctl vfib [secs]\n"
        "  faultctl rhythm <normal|tachy|brady|afib|vtach|vfib|asystole> [secs]\n"
        "  faultctl desat <pct> [secs]\n"
        "  faultctl apnea [secs]\n"
        "  faultctl hog start|stop\n"
        "  faultctl kill dsp\n"
        "  faultctl ack <alarm-id>\n"
        "  faultctl silence [alarm-id]\n"
        "  faultctl status\n");
}

int main(int argc, char **argv)
{
    g_argv0 = argc > 0 ? argv[0] : NULL;
    if (argc < 2) {
        usage();
        return 2;
    }

    if (strcmp(argv[1], "vfib") == 0) {
        return send_fault(VG_EP_GEN, VG_FAULT_RHYTHM, VG_RHYTHM_VFIB, 0,
                          secs_arg(argc > 2 ? argv[2] : NULL, 0)) ? 1 : 0;
    }
    if (strcmp(argv[1], "rhythm") == 0 && argc >= 3) {
        vg_rhythm_t r;
        if (rhythm_by_name(argv[2], &r) != 0) {
            usage();
            return 2;
        }
        return send_fault(VG_EP_GEN, VG_FAULT_RHYTHM, (uint32_t)r, 0,
                          secs_arg(argc > 3 ? argv[3] : NULL, 0)) ? 1 : 0;
    }
    if (strcmp(argv[1], "desat") == 0 && argc >= 3) {
        return send_fault(VG_EP_GEN, VG_FAULT_DESAT, 0,
                          (float)strtod(argv[2], NULL),
                          secs_arg(argc > 3 ? argv[3] : NULL, 0)) ? 1 : 0;
    }
    if (strcmp(argv[1], "apnea") == 0) {
        return send_fault(VG_EP_GEN, VG_FAULT_APNEA, 0, 0,
                          secs_arg(argc > 2 ? argv[2] : NULL, 15000)) ? 1 : 0;
    }
    if (strcmp(argv[1], "hog") == 0 && argc >= 3) {
        if (strcmp(argv[2], "start") == 0)
            return hog_start() ? 1 : 0;
        if (strcmp(argv[2], "stop") == 0)
            return hog_stop() ? 1 : 0;
        usage();
        return 2;
    }
    if (strcmp(argv[1], "kill") == 0 && argc >= 3 &&
        strcmp(argv[2], "dsp") == 0) {
        return send_fault(VG_EP_SUPERVISOR, VG_FAULT_KILL_DSP, 0, 0, 0)
               ? 1 : 0;
    }
    if (strcmp(argv[1], "ack") == 0 && argc >= 3) {
        vg_alarm_cmd_t cmd;
        cmd.alarm_id = (uint32_t)strtoul(argv[2], NULL, 10);
        cmd.action   = VG_ALARM_CMD_ACK;
        return send_msg(VG_EP_ALARM, VG_MSG_ALARM_CMD,
                        &cmd, sizeof(cmd)) ? 1 : 0;
    }
    if (strcmp(argv[1], "silence") == 0) {
        vg_alarm_cmd_t cmd;
        cmd.alarm_id = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
        cmd.action   = VG_ALARM_CMD_SILENCE;
        return send_msg(VG_EP_ALARM, VG_MSG_ALARM_CMD,
                        &cmd, sizeof(cmd)) ? 1 : 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        char path[PATH_MAX];
        FILE *f;
        int ch;
        snprintf(path, sizeof(path), "%s/supervisor.status",
                 vg_ipc_runtime_dir());
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "faultctl: no status file "
                    "(supervisor not running?)\n");
            return 1;
        }
        while ((ch = fgetc(f)) != EOF)
            putchar(ch);
        fclose(f);
        return 0;
    }

    usage();
    return 2;
}
