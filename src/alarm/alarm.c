#include "alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_MAX_ALARMS 32

struct vg_alarm_mgr {
    vg_alarm_config_t  cfg;
    vg_alarm_log_fn    log_fn;
    void              *log_ctx;
    vg_alarm_instance_t alarms[VG_MAX_ALARMS];
    uint32_t           next_id;
};

void vg_alarm_config_defaults(vg_alarm_config_t *cfg)
{
    cfg->escalation_ms = 60000;
    cfg->silence_ms    = 120000;
}

const char *vg_condition_name(uint32_t condition)
{
    switch (condition) {
    case VG_COND_VFIB:     return "VFIB";
    case VG_COND_ASYSTOLE: return "ASYSTOLE";
    case VG_COND_HR_HIGH:  return "HR_HIGH";
    case VG_COND_HR_LOW:   return "HR_LOW";
    case VG_COND_SPO2_LOW: return "SPO2_LOW";
    case VG_COND_APNEA:    return "APNEA";
    default:               return "UNKNOWN";
    }
}

const char *vg_priority_name(uint32_t priority)
{
    switch (priority) {
    case VG_PRIO_HIGH:   return "HIGH";
    case VG_PRIO_MEDIUM: return "MEDIUM";
    case VG_PRIO_LOW:    return "LOW";
    default:             return "?";
    }
}

vg_alarm_mgr_t *vg_alarm_mgr_new(const vg_alarm_config_t *cfg,
                                 vg_alarm_log_fn log_fn, void *log_ctx)
{
    vg_alarm_mgr_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    if (cfg)
        m->cfg = *cfg;
    else
        vg_alarm_config_defaults(&m->cfg);
    m->log_fn  = log_fn;
    m->log_ctx = log_ctx;
    m->next_id = 1;
    return m;
}

void vg_alarm_mgr_free(vg_alarm_mgr_t *mgr)
{
    free(mgr);
}

/* ------------------------------------------------------------------ */

static void emit_log(vg_alarm_mgr_t *m, uint64_t t_ms, const char *what,
                     const vg_alarm_instance_t *a)
{
    char line[192];
    if (!m->log_fn)
        return;
    snprintf(line, sizeof(line),
             "t=%llu alarm #%u %s cond=%s prio=%s value=%.1f",
             (unsigned long long)t_ms, a->id, what,
             vg_condition_name(a->condition),
             vg_priority_name(a->priority), a->value);
    m->log_fn(m->log_ctx, line);
}

static int push_note(vg_alarm_note_t *notes, int max_notes, int n,
                     const vg_alarm_instance_t *a, uint32_t kind, uint64_t t_ms)
{
    if (notes && n < max_notes) {
        notes[n].alarm_id  = a->id;
        notes[n].condition = a->condition;
        notes[n].priority  = a->priority;
        notes[n].kind      = kind;
        notes[n].t_ms      = t_ms;
        return n + 1;
    }
    return n;
}

static vg_alarm_instance_t *find_slot(vg_alarm_mgr_t *m, uint32_t condition,
                                      int live_only)
{
    int i;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        vg_alarm_instance_t *a = &m->alarms[i];
        if (a->state == VG_ALM_FREE)
            continue;
        if (a->condition != condition)
            continue;
        if (live_only && a->state == VG_ALM_INACTIVE)
            continue;
        return a;
    }
    return NULL;
}

static vg_alarm_instance_t *alloc_slot(vg_alarm_mgr_t *m)
{
    int i;
    /* Prefer a finished slot, then a never-used one. */
    for (i = 0; i < VG_MAX_ALARMS; i++)
        if (m->alarms[i].state == VG_ALM_INACTIVE)
            return &m->alarms[i];
    for (i = 0; i < VG_MAX_ALARMS; i++)
        if (m->alarms[i].state == VG_ALM_FREE)
            return &m->alarms[i];
    return NULL;
}

static vg_alarm_instance_t *find_by_id(vg_alarm_mgr_t *m, uint32_t id)
{
    int i;
    for (i = 0; i < VG_MAX_ALARMS; i++)
        if (m->alarms[i].state != VG_ALM_FREE && m->alarms[i].id == id)
            return &m->alarms[i];
    return NULL;
}

/* ------------------------------------------------------------------ */

int vg_alarm_mgr_event(vg_alarm_mgr_t *m, const vg_alarm_event_t *ev,
                       vg_alarm_note_t *notes, int max_notes)
{
    int n = 0;
    vg_alarm_instance_t *a;

    if (ev->active) {
        a = find_slot(m, ev->condition, 1);
        if (a) {
            /* Condition re-asserted on a live instance. */
            a->condition_active = 1;
            return 0;
        }
        a = alloc_slot(m);
        if (!a)
            return 0;
        memset(a, 0, sizeof(*a));
        a->id                   = m->next_id++;
        a->condition            = ev->condition;
        a->priority             = ev->priority;
        a->state                = VG_ALM_ACTIVE;
        a->condition_active     = 1;
        a->value                = ev->value;
        a->t_raised_ms          = ev->t_event_ms;
        a->t_last_annunciate_ms = ev->t_event_ms;
        a->annunciations        = 1;
        emit_log(m, ev->t_event_ms, "RAISED", a);
        n = push_note(notes, max_notes, n, a, VG_NOTE_RAISED, ev->t_event_ms);
        return n;
    }

    /* Condition cleared. */
    a = find_slot(m, ev->condition, 1);
    if (!a)
        return 0;
    a->condition_active = 0;
    if (a->priority == VG_PRIO_HIGH && a->state != VG_ALM_ACKED) {
        /* Latching: a high-priority alarm is held for the operator
         * even though the condition has gone. */
        emit_log(m, ev->t_event_ms, "CONDITION_CLEARED_LATCHED", a);
        n = push_note(notes, max_notes, n, a,
                      VG_NOTE_CONDITION_CLEARED_LATCHED, ev->t_event_ms);
    } else {
        a->state = VG_ALM_INACTIVE;
        emit_log(m, ev->t_event_ms, "CLEARED", a);
        n = push_note(notes, max_notes, n, a, VG_NOTE_CLEARED, ev->t_event_ms);
    }
    return n;
}

int vg_alarm_mgr_tick(vg_alarm_mgr_t *m, uint64_t now_ms,
                      vg_alarm_note_t *notes, int max_notes)
{
    int n = 0, i;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        vg_alarm_instance_t *a = &m->alarms[i];
        if (a->state == VG_ALM_FREE || a->state == VG_ALM_INACTIVE)
            continue;

        if (a->state == VG_ALM_SILENCED && now_ms >= a->silence_until_ms) {
            if (a->condition_active) {
                a->state = VG_ALM_ACTIVE;
                a->annunciations++;
                a->t_last_annunciate_ms = now_ms;
                emit_log(m, now_ms, "SILENCE_EXPIRED", a);
                n = push_note(notes, max_notes, n, a,
                              VG_NOTE_SILENCE_EXPIRED, now_ms);
            } else {
                a->state = VG_ALM_INACTIVE;
                emit_log(m, now_ms, "CLEARED", a);
                n = push_note(notes, max_notes, n, a,
                              VG_NOTE_CLEARED, now_ms);
            }
            continue;
        }

        if (a->state == VG_ALM_ACTIVE &&
            now_ms - a->t_raised_ms >= m->cfg.escalation_ms) {
            a->state = VG_ALM_ESCALATED;
            if (a->priority > VG_PRIO_HIGH)
                a->priority = VG_PRIO_HIGH;   /* MEDIUM/LOW promote */
            a->annunciations++;
            a->t_last_annunciate_ms = now_ms;
            emit_log(m, now_ms, "ESCALATED", a);
            n = push_note(notes, max_notes, n, a,
                          VG_NOTE_ESCALATED, now_ms);
            continue;
        }

        if (a->state == VG_ALM_ESCALATED &&
            now_ms - a->t_last_annunciate_ms >= m->cfg.escalation_ms) {
            a->annunciations++;
            a->t_last_annunciate_ms = now_ms;
            emit_log(m, now_ms, "ANNUNCIATE", a);
            n = push_note(notes, max_notes, n, a,
                          VG_NOTE_ANNUNCIATE, now_ms);
            continue;
        }

        if (a->state == VG_ALM_ACKED && a->condition_active &&
            now_ms - a->t_ack_ms >= m->cfg.escalation_ms) {
            /* Reminder: acknowledged but the patient is still in the
             * condition — re-annunciate, as real monitors do. */
            a->state = VG_ALM_ACTIVE;
            a->t_raised_ms = now_ms;
            a->annunciations++;
            a->t_last_annunciate_ms = now_ms;
            emit_log(m, now_ms, "REMINDER", a);
            n = push_note(notes, max_notes, n, a,
                          VG_NOTE_REMINDER, now_ms);
        }
    }
    return n;
}

int vg_alarm_mgr_ack(vg_alarm_mgr_t *m, uint32_t alarm_id, uint64_t now_ms,
                     vg_alarm_note_t *notes, int max_notes)
{
    vg_alarm_instance_t *a = find_by_id(m, alarm_id);
    if (!a || a->state == VG_ALM_INACTIVE || a->state == VG_ALM_FREE)
        return -1;
    a->state   = VG_ALM_ACKED;
    a->t_ack_ms = now_ms;
    emit_log(m, now_ms, "ACKED", a);
    push_note(notes, max_notes, 0, a, VG_NOTE_ACKED, now_ms);
    if (!a->condition_active) {
        /* Acknowledging a latched alarm whose condition has cleared
         * finishes it. */
        a->state = VG_ALM_INACTIVE;
        emit_log(m, now_ms, "CLEARED", a);
    }
    return 0;
}

int vg_alarm_mgr_silence(vg_alarm_mgr_t *m, uint32_t alarm_id,
                         uint64_t now_ms,
                         vg_alarm_note_t *notes, int max_notes)
{
    int n = 0, i, hit = 0;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        vg_alarm_instance_t *a = &m->alarms[i];
        if (a->state != VG_ALM_ACTIVE && a->state != VG_ALM_ESCALATED)
            continue;
        if (alarm_id != 0 && a->id != alarm_id)
            continue;
        a->state          = VG_ALM_SILENCED;
        a->silence_until_ms = now_ms + m->cfg.silence_ms;
        emit_log(m, now_ms, "SILENCED", a);
        n = push_note(notes, max_notes, n, a, VG_NOTE_SILENCED, now_ms);
        hit = 1;
    }
    return hit ? 0 : -1;
}

/* ------------------------------------------------------------------ */

int vg_alarm_mgr_get(const vg_alarm_mgr_t *mgr, uint32_t alarm_id,
                     vg_alarm_instance_t *out)
{
    int i;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        const vg_alarm_instance_t *a = &mgr->alarms[i];
        if (a->state != VG_ALM_FREE && a->id == alarm_id) {
            *out = *a;
            return 0;
        }
    }
    return -1;
}

int vg_alarm_mgr_find(const vg_alarm_mgr_t *mgr, uint32_t condition,
                      vg_alarm_instance_t *out)
{
    int i;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        const vg_alarm_instance_t *a = &mgr->alarms[i];
        if (a->state != VG_ALM_FREE && a->state != VG_ALM_INACTIVE &&
            a->condition == condition) {
            *out = *a;
            return 0;
        }
    }
    return -1;
}

int vg_alarm_mgr_live_count(const vg_alarm_mgr_t *mgr)
{
    int i, n = 0;
    for (i = 0; i < VG_MAX_ALARMS; i++) {
        uint32_t s = mgr->alarms[i].state;
        if (s != VG_ALM_FREE && s != VG_ALM_INACTIVE)
            n++;
    }
    return n;
}
