/*
 * test_dsp.c — offline tests for the signal generators + DSP core.
 *
 * Feeds synthetic blocks (built exactly the way vitalgen builds them)
 * straight into the DSP and checks:
 *   1. HR extraction accuracy across 60/72/90/120 bpm;
 *   2. VFib alarm latency after onset (budget: <= 4 s of signal);
 *   3. SpO2 alarm honours its 10 s persistence delay (no early fire,
 *      fires within the budgeted window after a desaturation);
 *   4. Asystole detection after ~4 s of flat line;
 *   5. Mid-stream (re)join: no spurious alarms from zeroed detector
 *      state, genuine VFib still detected after warm-up;
 *   6. A t0 discontinuity resets detector state (no spurious alarm,
 *      genuine detection afterwards unaffected).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vg.h"
#include "signals.h"
#include "dsp.h"

static int g_failures;

#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
        }                                                       \
    } while (0)

typedef struct {
    vg_ecg_t  *ecg;
    vg_spo2_t *spo2;
    vg_resp_t *resp;
    vg_dsp_t  *dsp;
    vg_vitals_t last_vitals;
    /* first raise time per condition, ms; -1 = never */
    int64_t raise_ms[VG_COND_COUNT];
    int     raise_count[VG_COND_COUNT];
} rig_t;

static void rig_init(rig_t *r, uint32_t seed, float base_hr)
{
    memset(r, 0, sizeof(*r));
    r->ecg  = vg_ecg_new(seed, base_hr);
    r->spo2 = vg_spo2_new(seed ^ 0x51AB, 98.0f);
    r->resp = vg_resp_new(seed ^ 0x9E37, 13.0f);
    r->dsp  = vg_dsp_new(NULL);
    for (int i = 0; i < VG_COND_COUNT; i++)
        r->raise_ms[i] = -1;
}

static void rig_free(rig_t *r)
{
    vg_ecg_free(r->ecg);
    vg_spo2_free(r->spo2);
    vg_resp_free(r->resp);
    vg_dsp_free(r->dsp);
}

/* Feed `seconds` of signal through the DSP, recording alarm raises.
 * Block timestamps are the generator's clock plus t0_off (used to
 * simulate timestamp discontinuities). */
static void rig_feed_off(rig_t *r, double seconds, uint64_t t0_off)
{
    int blocks = (int)(seconds * 10.0);
    for (int b = 0; b < blocks; b++) {
        vg_sample_block_t block;
        vg_alarm_event_t events[8];
        int nev, i;
        block.t0_ms = (uint64_t)(vg_ecg_time_s(r->ecg) * 1000.0) + t0_off;
        for (i = 0; i < VG_BLOCK_SAMPLES; i++) {
            block.ecg[i]   = vg_ecg_next(r->ecg);
            block.pleth[i] = vg_spo2_pleth_next(
                r->spo2, vg_ecg_nominal_hr(r->ecg));
            block.resp[i]  = vg_resp_next(r->resp);
        }
        vg_spo2_tick(r->spo2, VG_BLOCK_MS / 1000.0);
        block.spo2_pct = vg_spo2_value(r->spo2);
        nev = vg_dsp_process_block(r->dsp, &block, &r->last_vitals,
                                   events, 8);
        for (i = 0; i < nev; i++) {
            if (events[i].active &&
                r->raise_ms[events[i].condition] < 0)
                r->raise_ms[events[i].condition] =
                    (int64_t)events[i].t_event_ms;
            if (events[i].active)
                r->raise_count[events[i].condition]++;
        }
    }
}

static void rig_feed(rig_t *r, double seconds)
{
    rig_feed_off(r, seconds, 0);
}

/* Run the generators for `seconds` WITHOUT feeding the DSP — a
 * stream that has been running before this DSP joins it. */
static void rig_advance_gen(rig_t *r, double seconds)
{
    int blocks = (int)(seconds * 10.0);
    for (int b = 0; b < blocks; b++) {
        int i;
        for (i = 0; i < VG_BLOCK_SAMPLES; i++) {
            (void)vg_ecg_next(r->ecg);
            (void)vg_spo2_pleth_next(r->spo2,
                                     vg_ecg_nominal_hr(r->ecg));
            (void)vg_resp_next(r->resp);
        }
        vg_spo2_tick(r->spo2, VG_BLOCK_MS / 1000.0);
    }
}

static int rig_any_raise(const rig_t *r)
{
    int total = 0, i;
    for (i = 0; i < VG_COND_COUNT; i++)
        total += r->raise_count[i];
    return total;
}

/* ------------------------------------------------------------------ */

static void test_hr_accuracy(void)
{
    static const float targets[] = { 60.0f, 72.0f, 90.0f, 120.0f };
    printf("test_hr_accuracy\n");
    for (size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
        rig_t r;
        rig_init(&r, 4242 + (uint32_t)k, targets[k]);
        rig_feed(&r, 20.0);
        printf("  target %.0f bpm -> measured %.1f bpm\n",
               targets[k], r.last_vitals.hr_bpm);
        CHECK(r.last_vitals.hr_bpm > 0 &&
              fabsf(r.last_vitals.hr_bpm - targets[k]) <= 3.0f,
              "HR %.1f not within +/-3 of %.0f",
              r.last_vitals.hr_bpm, targets[k]);
        rig_free(&r);
    }
}

static void test_vfib_latency(void)
{
    rig_t r;
    int64_t latency;
    printf("test_vfib_latency\n");
    rig_init(&r, 777, 72.0f);
    rig_feed(&r, 10.0);
    CHECK(r.raise_ms[VG_COND_VFIB] < 0, "VFib raised during normal rhythm");
    vg_ecg_set_rhythm(r.ecg, VG_RHYTHM_VFIB, 0);
    rig_feed(&r, 8.0);
    CHECK(r.raise_ms[VG_COND_VFIB] >= 0, "VFib never raised");
    latency = r.raise_ms[VG_COND_VFIB] - 10000;
    printf("  VFib onset t=10000 ms, raised at %lld ms (latency %lld ms)\n",
           (long long)r.raise_ms[VG_COND_VFIB], (long long)latency);
    CHECK(latency >= 1000 && latency <= 4000,
          "VFib latency %lld ms outside [1000, 4000] budget",
          (long long)latency);
    rig_free(&r);
}

static void test_spo2_persistence(void)
{
    rig_t r;
    int64_t delay;
    printf("test_spo2_persistence\n");
    rig_init(&r, 31337, 72.0f);
    rig_feed(&r, 5.0);
    CHECK(r.raise_ms[VG_COND_SPO2_LOW] < 0,
          "SpO2 alarm raised at baseline 98%%");
    vg_spo2_desat(r.spo2, 84.0f, 0);
    rig_feed(&r, 30.0);
    CHECK(r.raise_ms[VG_COND_SPO2_LOW] >= 0, "SpO2 alarm never raised");
    delay = r.raise_ms[VG_COND_SPO2_LOW] - 5000;
    printf("  desat at t=5000 ms, SPO2_LOW raised at %lld ms "
           "(%lld ms after desat began)\n",
           (long long)r.raise_ms[VG_COND_SPO2_LOW], (long long)delay);
    /* Budget: 10 s persistence + up to ~8 s averaging lag + margin.
     * Firing before ~10 s would mean the persistence delay is broken. */
    CHECK(delay >= 10000, "SpO2 alarm fired after only %lld ms — "
          "persistence delay not honoured", (long long)delay);
    CHECK(delay <= 24000, "SpO2 alarm took %lld ms — outside budget",
          (long long)delay);
    rig_free(&r);
}

static void test_asystole(void)
{
    rig_t r;
    int64_t latency;
    printf("test_asystole\n");
    rig_init(&r, 999, 72.0f);
    rig_feed(&r, 8.0);
    vg_ecg_set_rhythm(r.ecg, VG_RHYTHM_ASYSTOLE, 0);
    rig_feed(&r, 8.0);
    CHECK(r.raise_ms[VG_COND_ASYSTOLE] >= 0, "asystole never raised");
    latency = r.raise_ms[VG_COND_ASYSTOLE] - 8000;
    printf("  asystole onset t=8000 ms, raised at %lld ms "
           "(latency %lld ms)\n",
           (long long)r.raise_ms[VG_COND_ASYSTOLE], (long long)latency);
    CHECK(latency >= 3500 && latency <= 6500,
          "asystole latency %lld ms outside [3500, 6500]",
          (long long)latency);
    CHECK(r.raise_ms[VG_COND_VFIB] < 0,
          "flat line misclassified as VFib");
    rig_free(&r);
}

/* Regression: a DSP that joins a stream already running (the
 * supervisor-restart case) starts with zeroed detector state.  It
 * must NOT raise the spurious VFib/apnea/asystole alarms the
 * pre-fix DSP raised within its first seconds — and a genuine
 * VFib after warm-up must still raise with normal latency. */
static void test_rejoin_warmup(void)
{
    rig_t r;
    int64_t latency;
    printf("test_rejoin_warmup\n");
    rig_init(&r, 2025, 72.0f);
    rig_advance_gen(&r, 60.0);          /* stream runs 60 s alone   */
    vg_dsp_free(r.dsp);                 /* DSP (re)joins mid-flight */
    r.dsp = vg_dsp_new(NULL);
    rig_feed(&r, 12.0);
    CHECK(rig_any_raise(&r) == 0,
          "spurious alarm(s) after mid-stream join: VFIB=%d ASYS=%d "
          "APNEA=%d HRH=%d HRL=%d SPO2=%d",
          r.raise_count[VG_COND_VFIB], r.raise_count[VG_COND_ASYSTOLE],
          r.raise_count[VG_COND_APNEA], r.raise_count[VG_COND_HR_HIGH],
          r.raise_count[VG_COND_HR_LOW], r.raise_count[VG_COND_SPO2_LOW]);
    CHECK(r.last_vitals.hr_bpm > 0 &&
          fabsf(r.last_vitals.hr_bpm - 72.0f) <= 3.0f,
          "HR after rejoin %.1f not within +/-3 of 72",
          r.last_vitals.hr_bpm);
    vg_ecg_set_rhythm(r.ecg, VG_RHYTHM_VFIB, 0);
    rig_feed(&r, 8.0);
    CHECK(r.raise_ms[VG_COND_VFIB] >= 0,
          "genuine VFib after rejoin warm-up never raised");
    latency = r.raise_ms[VG_COND_VFIB] - 72000;
    printf("  VFib onset t=72000 ms (12 s after rejoin), raised at "
           "%lld ms (latency %lld ms)\n",
           (long long)r.raise_ms[VG_COND_VFIB], (long long)latency);
    CHECK(latency >= 1000 && latency <= 4000,
          "post-rejoin VFib latency %lld ms outside [1000, 4000]",
          (long long)latency);
    rig_free(&r);
}

/* Regression: a t0 discontinuity mid-stream (generator restart /
 * dropped blocks) must reset detector state instead of feeding the
 * gap to the persistence timers — no spurious alarm, and genuine
 * detection afterwards is unaffected. */
static void test_discontinuity_reset(void)
{
    rig_t r;
    int64_t latency;
    printf("test_discontinuity_reset\n");
    rig_init(&r, 60606, 72.0f);
    rig_feed_off(&r, 10.0, 0);
    CHECK(rig_any_raise(&r) == 0, "alarm during normal rhythm");
    rig_feed_off(&r, 12.0, 30000);      /* t0 jumps +30 s mid-stream */
    CHECK(rig_any_raise(&r) == 0,
          "spurious alarm(s) after t0 jump: VFIB=%d ASYS=%d APNEA=%d",
          r.raise_count[VG_COND_VFIB], r.raise_count[VG_COND_ASYSTOLE],
          r.raise_count[VG_COND_APNEA]);
    vg_ecg_set_rhythm(r.ecg, VG_RHYTHM_VFIB, 0);
    rig_feed_off(&r, 8.0, 30000);
    CHECK(r.raise_ms[VG_COND_VFIB] >= 0,
          "genuine VFib after discontinuity never raised");
    latency = r.raise_ms[VG_COND_VFIB] - 52000;
    printf("  VFib onset t=52000 ms (post-jump stream clock), raised "
           "at %lld ms (latency %lld ms)\n",
           (long long)r.raise_ms[VG_COND_VFIB], (long long)latency);
    CHECK(latency >= 1000 && latency <= 4000,
          "post-discontinuity VFib latency %lld ms outside "
          "[1000, 4000]", (long long)latency);
    rig_free(&r);
}

int main(void)
{
    test_hr_accuracy();
    test_vfib_latency();
    test_spo2_persistence();
    test_asystole();
    test_rejoin_warmup();
    test_discontinuity_reset();
    if (g_failures) {
        printf("test_dsp: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("test_dsp: ALL PASSED\n");
    return 0;
}
