#include "supervisor.h"

#include <string.h>

void vg_sup_init(vg_sup_t *sup)
{
    memset(sup, 0, sizeof(*sup));
}

int vg_sup_add(vg_sup_t *sup, const char *name, uint32_t role)
{
    vg_child_t *c;
    if (sup->n >= VG_SUP_MAX_CHILDREN)
        return -1;
    c = &sup->children[sup->n];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->role = role;
    c->pid  = -1;
    return sup->n++;
}

vg_child_t *vg_sup_by_role(vg_sup_t *sup, uint32_t role)
{
    int i;
    for (i = 0; i < sup->n; i++)
        if (sup->children[i].role == role)
            return &sup->children[i];
    return NULL;
}

vg_child_t *vg_sup_by_pid(vg_sup_t *sup, pid_t pid)
{
    int i;
    for (i = 0; i < sup->n; i++)
        if (sup->children[i].running && sup->children[i].pid == pid)
            return &sup->children[i];
    return NULL;
}

void vg_sup_started(vg_sup_t *sup, uint32_t role, pid_t pid, uint64_t now_ms)
{
    vg_child_t *c = vg_sup_by_role(sup, role);
    if (!c)
        return;
    c->pid            = pid;
    c->running        = 1;
    c->last_hb_ms     = now_ms;   /* grace period until first heartbeat */
    c->next_restart_ms = 0;
}

void vg_sup_heartbeat(vg_sup_t *sup, uint32_t role, uint32_t seq, uint64_t now_ms)
{
    vg_child_t *c = vg_sup_by_role(sup, role);
    if (!c)
        return;
    c->last_hb_ms = now_ms;
    c->hb_seq     = seq;
}

int64_t vg_sup_exited(vg_sup_t *sup, uint32_t role, uint64_t now_ms)
{
    vg_child_t *c = vg_sup_by_role(sup, role);
    int recent = 0, i;
    int64_t backoff;

    if (!c)
        return -1;
    c->running = 0;
    c->pid     = -1;

    /* Count restarts inside the trailing window. */
    for (i = 0; i < c->restart_log_n; i++)
        if (now_ms - c->restart_log[i] < VG_SUP_RESTART_WINDOW_MS)
            recent++;
    if (recent >= VG_SUP_RESTART_BUDGET) {
        c->gave_up         = 1;
        c->next_restart_ms = 0;
        return -1;
    }

    /* Exponential backoff: 500 ms, 1 s, 2 s, 4 s (capped). */
    backoff = 500;
    for (i = 0; i < recent && backoff < 4000; i++)
        backoff *= 2;
    if (backoff > 4000)
        backoff = 4000;

    if (c->restart_log_n < VG_SUP_RESTART_BUDGET)
        c->restart_log[c->restart_log_n++] = now_ms;
    else {
        memmove(&c->restart_log[0], &c->restart_log[1],
                sizeof(uint64_t) * (VG_SUP_RESTART_BUDGET - 1));
        c->restart_log[VG_SUP_RESTART_BUDGET - 1] = now_ms;
    }
    c->restarts++;
    c->next_restart_ms = now_ms + (uint64_t)backoff;
    return backoff;
}

int vg_sup_hung(const vg_child_t *child, uint64_t now_ms)
{
    return child->running &&
           now_ms - child->last_hb_ms > VG_SUP_HB_TIMEOUT_MS;
}
