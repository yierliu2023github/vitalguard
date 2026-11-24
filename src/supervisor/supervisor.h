/*
 * supervisor.h — process supervision policy for VitalGuard.
 *
 * This module is the *policy* half of supervision: which children
 * exist, when they were last heard from, when a dead child may be
 * restarted (exponential backoff with a restart budget), and when a
 * silent child counts as hung.  The *mechanism* half — fork/exec,
 * waitpid, signals — lives in the vgsupervisor application.
 *
 * QNX mapping: on target, QPLMS (the documented HAM successor) plus
 * the `slm` launcher provide exactly this policy/mechanism split —
 * dependency-ordered launch, monitoring, restart with configurable
 * backoff.  The host supervisor exists so the behaviour is developed
 * and tested before the port, and its policy constants carry over.
 */
#ifndef VG_SUPERVISOR_H
#define VG_SUPERVISOR_H

#include <stdint.h>
#include <sys/types.h>

#define VG_SUP_MAX_CHILDREN 8
#define VG_SUP_HB_TIMEOUT_MS   3000u   /* silence longer than this = hung */
#define VG_SUP_RESTART_BUDGET  10      /* restarts per window, then stop  */
#define VG_SUP_RESTART_WINDOW_MS 60000u

typedef struct {
    char     name[24];
    uint32_t role;              /* VG_PROC_*                        */
    pid_t    pid;               /* -1 when not running              */
    int      running;
    uint64_t last_hb_ms;        /* wall clock                       */
    uint32_t hb_seq;
    uint32_t restarts;          /* total restarts                   */
    uint64_t restart_log[VG_SUP_RESTART_BUDGET];
    int      restart_log_n;
    uint64_t next_restart_ms;   /* 0 = not scheduled                */
    int      gave_up;           /* budget exhausted: leave it down  */
} vg_child_t;

typedef struct {
    vg_child_t children[VG_SUP_MAX_CHILDREN];
    int        n;
} vg_sup_t;

void vg_sup_init(vg_sup_t *sup);

/* Register a child slot; returns its index or -1. */
int vg_sup_add(vg_sup_t *sup, const char *name, uint32_t role);

vg_child_t *vg_sup_by_role(vg_sup_t *sup, uint32_t role);
vg_child_t *vg_sup_by_pid(vg_sup_t *sup, pid_t pid);

/* The child process was (re)started with this pid at wall time now. */
void vg_sup_started(vg_sup_t *sup, uint32_t role, pid_t pid, uint64_t now_ms);

/* A heartbeat arrived from role. */
void vg_sup_heartbeat(vg_sup_t *sup, uint32_t role, uint32_t seq, uint64_t now_ms);

/* The child exited (any reason).  Applies the restart policy and
 * returns the backoff delay in ms before it may be restarted,
 * or -1 when the restart budget is exhausted. */
int64_t vg_sup_exited(vg_sup_t *sup, uint32_t role, uint64_t now_ms);

/* Wall-clock check: is this child running but silent too long? */
int vg_sup_hung(const vg_child_t *child, uint64_t now_ms);

#endif /* VG_SUPERVISOR_H */
