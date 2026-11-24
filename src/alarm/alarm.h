/*
 * alarm.h — VitalGuard alarm manager.
 *
 * The alarm manager owns everything that happens *after* the DSP has
 * decided a condition is real (condition persistence lives in the DSP):
 *
 *   - alarm instances with IEC 60601-1-8-style priorities
 *     (HIGH / MEDIUM / LOW);
 *   - latching: HIGH-priority alarms stay annunciated after the
 *     condition clears, until an operator acknowledges them — a
 *     transient lethal rhythm must not silently vanish;
 *   - acknowledgement and silence (audio pause) with a reminder:
 *     an acknowledged-but-still-active alarm re-annunciates after
 *     the escalation interval, a silenced alarm when its pause ends;
 *   - escalation: an unacknowledged alarm escalates after the
 *     escalation interval (MEDIUM alarms are promoted to HIGH),
 *     and escalated alarms keep re-annunciating;
 *   - a complete audit log of every transition (via callback).
 *
 * All times are on the signal clock (see vg.h).  The manager is
 * driven by events (vg_alarm_mgr_event) and a periodic tick fed by
 * the vitals stream (vg_alarm_mgr_tick), which makes it fully
 * deterministic in unit tests.
 */
#ifndef VG_ALARM_H
#define VG_ALARM_H

#include "vg.h"

typedef enum {
    VG_ALM_FREE      = 0,   /* slot unused                              */
    VG_ALM_ACTIVE    = 1,   /* annunciated, awaiting ack/resolution     */
    VG_ALM_ACKED     = 2,   /* acknowledged by operator                 */
    VG_ALM_SILENCED  = 3,   /* audio paused, visually still active      */
    VG_ALM_ESCALATED = 4,   /* unacknowledged past the escalation delay */
    VG_ALM_INACTIVE  = 5    /* finished (condition cleared / acked)     */
} vg_alarm_state_t;

typedef enum {
    VG_NOTE_RAISED                    = 1,
    VG_NOTE_ANNUNCIATE                = 2,  /* re-annunciation            */
    VG_NOTE_ESCALATED                 = 3,
    VG_NOTE_CLEARED                   = 4,  /* instance finished          */
    VG_NOTE_CONDITION_CLEARED_LATCHED = 5,  /* condition gone, alarm held */
    VG_NOTE_ACKED                     = 6,
    VG_NOTE_SILENCED                  = 7,
    VG_NOTE_SILENCE_EXPIRED           = 8,
    VG_NOTE_REMINDER                  = 9   /* acked but still active     */
} vg_note_kind_t;

typedef struct {
    uint32_t alarm_id;
    uint32_t condition;         /* vg_condition_t   */
    uint32_t priority;          /* vg_priority_t    */
    uint32_t kind;              /* vg_note_kind_t   */
    uint64_t t_ms;
} vg_alarm_note_t;

typedef struct {
    uint32_t id;
    uint32_t condition;
    uint32_t priority;
    uint32_t state;             /* vg_alarm_state_t */
    int      condition_active;  /* underlying condition still present */
    float    value;             /* value at raise time                */
    uint64_t t_raised_ms;
    uint64_t t_last_annunciate_ms;
    uint64_t t_ack_ms;
    uint64_t silence_until_ms;
    uint32_t annunciations;
} vg_alarm_instance_t;

typedef struct {
    uint32_t escalation_ms;     /* default 60 s: unacked -> escalate,
                                   acked+active -> reminder            */
    uint32_t silence_ms;        /* default 120 s audio pause (IEC cap) */
} vg_alarm_config_t;

void vg_alarm_config_defaults(vg_alarm_config_t *cfg);

/* Audit-log sink: receives one formatted line per transition. */
typedef void (*vg_alarm_log_fn)(void *ctx, const char *line);

typedef struct vg_alarm_mgr vg_alarm_mgr_t;

vg_alarm_mgr_t *vg_alarm_mgr_new(const vg_alarm_config_t *cfg,
                                 vg_alarm_log_fn log_fn, void *log_ctx);
void vg_alarm_mgr_free(vg_alarm_mgr_t *mgr);

/* Feed one DSP condition transition.  Notes (0 or 1) are written to
 * notes[]; returns the note count. */
int vg_alarm_mgr_event(vg_alarm_mgr_t *mgr, const vg_alarm_event_t *ev,
                       vg_alarm_note_t *notes, int max_notes);

/* Advance time: escalation, reminders, silence expiry.  Call at
 * vitals rate (10 Hz).  Notes written to notes[]; returns count. */
int vg_alarm_mgr_tick(vg_alarm_mgr_t *mgr, uint64_t now_ms,
                      vg_alarm_note_t *notes, int max_notes);

/* Operator actions.  Return 0 on success, -1 when no such live alarm. */
int vg_alarm_mgr_ack(vg_alarm_mgr_t *mgr, uint32_t alarm_id, uint64_t now_ms,
                     vg_alarm_note_t *notes, int max_notes);
int vg_alarm_mgr_silence(vg_alarm_mgr_t *mgr, uint32_t alarm_id,
                         uint64_t now_ms,
                         vg_alarm_note_t *notes, int max_notes);

/* Queries (used by the console UI and the tests). */
int vg_alarm_mgr_get(const vg_alarm_mgr_t *mgr, uint32_t alarm_id,
                     vg_alarm_instance_t *out);
int vg_alarm_mgr_find(const vg_alarm_mgr_t *mgr, uint32_t condition,
                      vg_alarm_instance_t *out);
int vg_alarm_mgr_live_count(const vg_alarm_mgr_t *mgr);

const char *vg_condition_name(uint32_t condition);
const char *vg_priority_name(uint32_t priority);

#endif /* VG_ALARM_H */
