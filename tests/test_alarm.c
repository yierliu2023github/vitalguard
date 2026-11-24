/*
 * test_alarm.c — unit tests for the alarm manager state machine.
 *
 * Covers: raise, escalation timing, HIGH-priority latching,
 * acknowledgement of a latched alarm, silence + expiry
 * re-annunciation, automatic clear of a MEDIUM alarm, and
 * MEDIUM -> HIGH promotion on escalation.
 *
 * Test config: escalation 5 s, silence 10 s (compressed from the
 * 60 s / 120 s production defaults so the test stays readable).
 */
#include <stdio.h>
#include <string.h>

#include "vg.h"
#include "alarm.h"

static int g_failures;
static int g_log_lines;

#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
        }                                                       \
    } while (0)

static void count_log(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
    g_log_lines++;
}

static vg_alarm_event_t make_ev(uint32_t cond, uint32_t prio, int active,
                                uint64_t t)
{
    vg_alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.condition  = cond;
    ev.priority   = prio;
    ev.active     = active;
    ev.value      = 0.0f;
    ev.t_onset_ms = t;
    ev.t_event_ms = t;
    return ev;
}

static int has_note(const vg_alarm_note_t *notes, int n, uint32_t kind)
{
    int i;
    for (i = 0; i < n; i++)
        if (notes[i].kind == kind)
            return 1;
    return 0;
}

int main(void)
{
    vg_alarm_config_t cfg;
    vg_alarm_mgr_t *m;
    vg_alarm_note_t notes[8];
    vg_alarm_instance_t inst;
    vg_alarm_event_t ev;
    int n;

    vg_alarm_config_defaults(&cfg);
    cfg.escalation_ms = 5000;
    cfg.silence_ms    = 10000;
    m = vg_alarm_mgr_new(&cfg, count_log, NULL);

    printf("test_alarm: raise + escalate + latch + ack\n");
    ev = make_ev(VG_COND_VFIB, VG_PRIO_HIGH, 1, 1000);
    n = vg_alarm_mgr_event(m, &ev, notes, 8);
    CHECK(n == 1 && has_note(notes, n, VG_NOTE_RAISED), "VFIB not raised");
    CHECK(vg_alarm_mgr_get(m, 1, &inst) == 0, "alarm #1 missing");
    CHECK(inst.state == VG_ALM_ACTIVE && inst.annunciations == 1,
          "alarm #1 not ACTIVE with 1 annunciation");

    n = vg_alarm_mgr_tick(m, 5999, notes, 8);
    CHECK(!has_note(notes, n, VG_NOTE_ESCALATED),
          "escalated before the escalation delay");
    n = vg_alarm_mgr_tick(m, 6000, notes, 8);
    CHECK(has_note(notes, n, VG_NOTE_ESCALATED),
          "no escalation at raise+5000 ms");
    CHECK(vg_alarm_mgr_get(m, 1, &inst) == 0 &&
          inst.state == VG_ALM_ESCALATED, "alarm #1 not ESCALATED");

    ev = make_ev(VG_COND_VFIB, VG_PRIO_HIGH, 0, 7000);
    n = vg_alarm_mgr_event(m, &ev, notes, 8);
    CHECK(has_note(notes, n, VG_NOTE_CONDITION_CLEARED_LATCHED),
          "HIGH alarm did not latch on condition clear");
    CHECK(vg_alarm_mgr_get(m, 1, &inst) == 0 &&
          inst.state == VG_ALM_ESCALATED && !inst.condition_active,
          "latched alarm state wrong");

    CHECK(vg_alarm_mgr_ack(m, 1, 8000, notes, 8) == 0, "ack failed");
    CHECK(vg_alarm_mgr_get(m, 1, &inst) == 0 &&
          inst.state == VG_ALM_INACTIVE,
          "acked latched alarm did not finish");
    CHECK(vg_alarm_mgr_live_count(m) == 0, "live count not 0 after ack");

    printf("test_alarm: silence + expiry + medium promotion\n");
    ev = make_ev(VG_COND_SPO2_LOW, VG_PRIO_MEDIUM, 1, 10000);
    vg_alarm_mgr_event(m, &ev, notes, 8);
    CHECK(vg_alarm_mgr_get(m, 2, &inst) == 0 &&
          inst.state == VG_ALM_ACTIVE, "alarm #2 not ACTIVE");
    CHECK(vg_alarm_mgr_silence(m, 2, 11000, notes, 8) == 0,
          "silence failed");
    CHECK(vg_alarm_mgr_get(m, 2, &inst) == 0 &&
          inst.state == VG_ALM_SILENCED, "alarm #2 not SILENCED");
    n = vg_alarm_mgr_tick(m, 20999, notes, 8);
    CHECK(!has_note(notes, n, VG_NOTE_SILENCE_EXPIRED),
          "silence expired early");
    n = vg_alarm_mgr_tick(m, 21000, notes, 8);
    CHECK(has_note(notes, n, VG_NOTE_SILENCE_EXPIRED),
          "silence did not expire at +10000 ms");
    CHECK(vg_alarm_mgr_get(m, 2, &inst) == 0 &&
          inst.state == VG_ALM_ACTIVE, "alarm #2 not re-ACTIVE");
    n = vg_alarm_mgr_tick(m, 26000, notes, 8);
    CHECK(has_note(notes, n, VG_NOTE_ESCALATED),
          "alarm #2 did not escalate after re-activation");
    CHECK(vg_alarm_mgr_get(m, 2, &inst) == 0 &&
          inst.priority == VG_PRIO_HIGH,
          "MEDIUM alarm not promoted to HIGH on escalation");

    printf("test_alarm: medium alarm auto-clears\n");
    ev = make_ev(VG_COND_HR_HIGH, VG_PRIO_MEDIUM, 1, 30000);
    vg_alarm_mgr_event(m, &ev, notes, 8);
    ev = make_ev(VG_COND_HR_HIGH, VG_PRIO_MEDIUM, 0, 31000);
    n = vg_alarm_mgr_event(m, &ev, notes, 8);
    CHECK(has_note(notes, n, VG_NOTE_CLEARED),
          "MEDIUM alarm did not auto-clear");
    CHECK(vg_alarm_mgr_find(m, VG_COND_HR_HIGH, &inst) != 0,
          "cleared HR_HIGH still live");

    CHECK(g_log_lines >= 8, "audit log too short: %d lines", g_log_lines);
    printf("  audit log lines: %d\n", g_log_lines);

    vg_alarm_mgr_free(m);
    if (g_failures) {
        printf("test_alarm: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("test_alarm: ALL PASSED\n");
    return 0;
}
