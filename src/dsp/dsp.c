#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MWI_WIN        30       /* moving-window integration, samples */
#define REFRACTORY     50       /* 200 ms at 250 Hz                   */
#define RMS_WIN        250      /* 1 s RMS window                     */
#define RR_RING        8
#define SPO2_RING      80       /* 8 s of blocks                      */
#define RESP_RING      4
#define CLEAR_HYST_MS  1000u    /* raw must stay false this long to clear */
#define WARMUP_MS      3000u    /* no VFib/asystole verdicts before this  */
/* Warm-up guard: after the DSP (re)joins a stream — process start,
 * or a t0 discontinuity that resets detector state — alarm *raises*
 * are suppressed until WARMUP_GUARD_MS of contiguous samples have
 * been consumed.  The window is sized from the detectors' needs:
 *   - the VFib/asystole RMS discriminator needs its full 1 s
 *     window (250 samples) of real signal;
 *   - the QRS chain needs several beats for its adaptive thresholds
 *     to settle, and HR needs >= 2 RR intervals — at 40 bpm (the
 *     lowest non-alarm rate) two intervals take 3.0 s;
 *   - respiration needs one full breath cycle, 5.0 s at 12/min.
 * The 8 s SpO2 ring is deliberately NOT waited out: SPO2_LOW can
 * never raise before its own 10 s persistence has elapsed on real
 * averaged samples, which is already past this guard.  Persistence
 * timers keep running during warm-up (on freshly reset state), so
 * the guard only ever binds for the zero-persistence conditions
 * (VFib/asystole) — exactly the ones that misfire on zeroed state. */
#define WARMUP_GUARD_MS 5000u

struct vg_dsp {
    vg_dsp_config_t cfg;

    /* --- QRS detector state --- */
    float    x_prev1, x_prev2;
    float    mwi_ring[MWI_WIN];
    int      mwi_idx;
    float    mwi_sum;
    float    spki, npki;                /* adaptive threshold estimates */
    int      in_peak;
    float    peak_val, peak_mwi;
    uint64_t peak_sample, peak_deadline;
    uint64_t peak_ms;                   /* signal-clock time of peak    */
    uint64_t sample_idx;                /* total samples processed      */
    uint64_t last_beat_sample;
    uint64_t last_beat_ms;
    int      have_beat;
    float    rr_ring[RR_RING];          /* seconds                      */
    int      rr_count, rr_idx;

    /* --- RMS window (VFib vs asystole) --- */
    float    rms_ring[RMS_WIN];
    int      rms_idx;
    float    rms_sumsq;

    /* --- SpO2 averaging --- */
    float    spo2_ring[SPO2_RING];
    int      spo2_idx, spo2_count;
    float    spo2_sum;

    /* --- Respiration --- */
    float    resp_prev;
    int      resp_armed;   /* hysteresis: armed below 0.10 */
    uint64_t last_breath_sample;
    uint64_t last_breath_ms;
    float    resp_rr_ring[RESP_RING];
    int      resp_count, resp_idx;

    /* --- condition persistence tracking --- */
    int64_t  raw_since[VG_COND_COUNT];  /* -1 = not raw                 */
    int64_t  clear_since[VG_COND_COUNT];
    int      active[VG_COND_COUNT];
    uint64_t onset_ms[VG_COND_COUNT];

    /* --- stream (re)join state --- */
    uint64_t stream_start_ms;  /* t0 of the current contiguous run    */
    uint64_t prev_t0_ms;       /* t0 of the previous block            */
    int      have_prev;        /* prev_t0_ms is valid                 */
};

void vg_dsp_config_defaults(vg_dsp_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hr_high_bpm   = 120.0f;
    cfg->hr_low_bpm    = 40.0f;
    cfg->spo2_low_pct  = 90.0f;
    cfg->persist_ms[VG_COND_VFIB]     = 0;
    cfg->persist_ms[VG_COND_ASYSTOLE] = 0;
    cfg->persist_ms[VG_COND_HR_HIGH]  = 5000;
    cfg->persist_ms[VG_COND_HR_LOW]   = 5000;
    cfg->persist_ms[VG_COND_SPO2_LOW] = 10000;
    cfg->persist_ms[VG_COND_APNEA]    = 5000;
}

/* Reset every piece of detector state to its power-on values and
 * re-anchor the stream clock at t0_ms.  Called at construction and
 * whenever the incoming block timestamps show the stream jumped or
 * restarted (a discontinuity): samples from before the gap must not
 * feed the detectors — the RMS ring, the beat/breath history and the
 * persistence timers all assume contiguity.  Conditions already
 * marked active stay active (their flag is the alarm manager's
 * latched state as mirrored here); their timers restart, and the
 * normal clear hysteresis retires them if the refilled detectors
 * do not re-confirm them. */
static void dsp_stream_reset(vg_dsp_t *d, uint64_t t0_ms)
{
    int i;

    /* QRS chain */
    d->x_prev1 = d->x_prev2 = 0.0f;
    memset(d->mwi_ring, 0, sizeof(d->mwi_ring));
    d->mwi_idx = 0;
    d->mwi_sum = 0.0f;
    d->spki = 0.02f;
    d->npki = 0.0005f;
    d->in_peak = 0;
    d->peak_val = d->peak_mwi = 0.0f;
    d->peak_sample = d->peak_deadline = 0;
    d->peak_ms = 0;
    d->sample_idx = 0;
    d->last_beat_sample = 0;
    d->last_beat_ms = t0_ms;
    d->have_beat = 0;
    memset(d->rr_ring, 0, sizeof(d->rr_ring));
    d->rr_count = 0;
    d->rr_idx = 0;

    /* RMS window */
    memset(d->rms_ring, 0, sizeof(d->rms_ring));
    d->rms_idx = 0;
    d->rms_sumsq = 0.0f;

    /* SpO2 averaging */
    memset(d->spo2_ring, 0, sizeof(d->spo2_ring));
    d->spo2_idx = 0;
    d->spo2_count = 0;
    d->spo2_sum = 0.0f;

    /* Respiration — breath timing is re-anchored at the join, so
     * the apnea timer measures from the (re)join, not from stream
     * time zero. */
    d->resp_prev = 0.0f;
    d->resp_armed = 0;
    d->last_breath_sample = 0;
    d->last_breath_ms = t0_ms;
    memset(d->resp_rr_ring, 0, sizeof(d->resp_rr_ring));
    d->resp_count = 0;
    d->resp_idx = 0;

    /* Persistence timers */
    for (i = 0; i < VG_COND_COUNT; i++) {
        d->raw_since[i]   = -1;
        d->clear_since[i] = -1;
        if (d->active[i])
            d->onset_ms[i] = t0_ms;
    }

    d->stream_start_ms = t0_ms;
}

vg_dsp_t *vg_dsp_new(const vg_dsp_config_t *cfg)
{
    vg_dsp_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    if (cfg)
        d->cfg = *cfg;
    else
        vg_dsp_config_defaults(&d->cfg);
    dsp_stream_reset(d, 0);
    return d;
}

void vg_dsp_free(vg_dsp_t *dsp)
{
    free(dsp);
}

/* ------------------------------------------------------------------ */
/* QRS detection                                                       */
/* ------------------------------------------------------------------ */

static void register_beat(vg_dsp_t *d, uint64_t beat_sample, uint64_t beat_ms)
{
    if (d->have_beat) {
        double rr = (double)(beat_sample - d->last_beat_sample)
                  / VG_SAMPLE_RATE_HZ;
        if (rr >= 0.25 && rr <= 2.0) {
            d->rr_ring[d->rr_idx] = (float)rr;
            d->rr_idx = (d->rr_idx + 1) % RR_RING;
            if (d->rr_count < RR_RING)
                d->rr_count++;
        }
    }
    d->last_beat_sample = beat_sample;
    d->last_beat_ms     = beat_ms;
    d->have_beat        = 1;
}

static void qrs_sample(vg_dsp_t *d, float x, uint64_t block_t0, int i)
{
    float deriv, mwi, thresh;
    uint64_t n = d->sample_idx;
    uint64_t sample_ms = block_t0
                       + (uint64_t)i * (1000 / VG_SAMPLE_RATE_HZ);

    /* derivative -> square -> moving-window integration */
    deriv = x - d->x_prev2;
    d->x_prev2 = d->x_prev1;
    d->x_prev1 = x;
    d->mwi_sum -= d->mwi_ring[d->mwi_idx];
    d->mwi_ring[d->mwi_idx] = deriv * deriv;
    d->mwi_sum += d->mwi_ring[d->mwi_idx];
    d->mwi_idx = (d->mwi_idx + 1) % MWI_WIN;
    mwi = d->mwi_sum / MWI_WIN;

    /* 1 s sliding RMS for the VFib/asystole discriminator */
    d->rms_sumsq -= d->rms_ring[d->rms_idx] * d->rms_ring[d->rms_idx];
    d->rms_ring[d->rms_idx] = x;
    d->rms_sumsq += x * x;
    d->rms_idx = (d->rms_idx + 1) % RMS_WIN;

    thresh = d->npki + 0.30f * (d->spki - d->npki);

    if (!d->in_peak) {
        /* adapt the noise estimate while clearly not in a QRS */
        d->npki = 0.95f * d->npki + 0.05f * mwi;
        if (mwi > thresh &&
            (!d->have_beat || n - d->last_beat_sample > REFRACTORY)) {
            d->in_peak       = 1;
            d->peak_val      = x;
            d->peak_mwi      = mwi;
            d->peak_sample   = n;
            d->peak_ms       = sample_ms;
            d->peak_deadline = n + MWI_WIN;
        }
    } else {
        if (x > d->peak_val) {
            d->peak_val    = x;
            d->peak_sample = n;
            d->peak_ms     = sample_ms;
        }
        if (mwi > d->peak_mwi)
            d->peak_mwi = mwi;
        if (mwi < thresh * 0.4f || n > d->peak_deadline) {
            d->spki = 0.875f * d->spki + 0.125f * d->peak_mwi;
            d->in_peak = 0;
            register_beat(d, d->peak_sample, d->peak_ms);
        }
    }

    d->sample_idx++;
}

/* ------------------------------------------------------------------ */
/* Derived vitals                                                      */
/* ------------------------------------------------------------------ */

static float median_of(float *v, int n)
{
    float tmp[RR_RING];
    int i, j;
    memcpy(tmp, v, sizeof(float) * (size_t)n);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (tmp[j] < tmp[i]) {
                float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
    return (n % 2) ? tmp[n / 2] : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
}

static float dsp_hr(const vg_dsp_t *d, uint64_t now_ms)
{
    float rr[RR_RING];
    if (!d->have_beat || d->rr_count < 2)
        return -1.0f;
    if (now_ms - d->last_beat_ms > 3000)
        return -1.0f;
    memcpy(rr, d->rr_ring, sizeof(float) * (size_t)d->rr_count);
    return 60.0f / median_of(rr, d->rr_count);
}

static float dsp_resp_rate(const vg_dsp_t *d, uint64_t now_ms)
{
    float mean = 0.0f;
    int i;
    if (d->resp_count < 2 || now_ms - d->last_breath_ms > 10000)
        return -1.0f;
    for (i = 0; i < d->resp_count; i++)
        mean += d->resp_rr_ring[i];
    mean /= (float)d->resp_count;
    return 60.0f / mean;
}

static float dsp_rms(const vg_dsp_t *d)
{
    float ms = d->rms_sumsq / RMS_WIN;
    return ms > 0.0f ? (float)sqrtf(ms) : 0.0f;
}

/* ------------------------------------------------------------------ */
/* Condition evaluation                                                */
/* ------------------------------------------------------------------ */

static uint32_t priority_for(const vg_dsp_t *d, vg_condition_t cond, float value)
{
    (void)d;
    switch (cond) {
    case VG_COND_VFIB:
    case VG_COND_ASYSTOLE:
    case VG_COND_APNEA:
        return VG_PRIO_HIGH;
    case VG_COND_SPO2_LOW:
        return value < 80.0f ? VG_PRIO_HIGH : VG_PRIO_MEDIUM;
    default:
        return VG_PRIO_MEDIUM;
    }
}

static float value_for(const vg_dsp_t *d, vg_condition_t cond,
                       float hr, float spo2, float rr)
{
    (void)d;
    switch (cond) {
    case VG_COND_HR_HIGH:
    case VG_COND_HR_LOW:
    case VG_COND_VFIB:
    case VG_COND_ASYSTOLE:
        return hr;
    case VG_COND_SPO2_LOW:
        return spo2;
    default:
        return rr;
    }
}

int vg_dsp_process_block(vg_dsp_t *d, const vg_sample_block_t *block,
                         vg_vitals_t *vitals_out,
                         vg_alarm_event_t *events, int max_events)
{
    int i, nev = 0;
    uint64_t now = block->t0_ms + VG_BLOCK_MS;
    float hr, spo2_avg, rr_rate, rms;
    int raw[VG_COND_COUNT];
    uint32_t mask = 0;
    int warmed;

    /* Stream continuity: blocks must arrive back-to-back on the
     * signal clock.  A t0 that breaks the sequence (generator
     * restart, dropped blocks, or this DSP's first block on a
     * stream that has been running without it) means the detector
     * state no longer describes this stream — reset it rather than
     * interpret the gap.  The comparison tolerates +/- half a block
     * because the generator stamps t0 from a double clock, which
     * jitters by a millisecond either way (measured: occasional
     * 99/101 ms steps); a genuine break is a step backwards, a
     * duplicate, or a gap of more than a block and a half. */
    if (!d->have_prev || block->t0_ms <= d->prev_t0_ms ||
        block->t0_ms - d->prev_t0_ms > VG_BLOCK_MS + VG_BLOCK_MS / 2)
        dsp_stream_reset(d, block->t0_ms);
    d->prev_t0_ms = block->t0_ms;
    d->have_prev  = 1;

    /* Warm-up guard: no alarm raises until the detectors have
     * consumed WARMUP_GUARD_MS of contiguous samples since the
     * (re)join (see the constant's comment for the sizing). */
    warmed = (now - d->stream_start_ms) >= WARMUP_GUARD_MS;

    /* --- per-sample ECG + respiration processing --- */
    for (i = 0; i < VG_BLOCK_SAMPLES; i++) {
        qrs_sample(d, block->ecg[i], block->t0_ms, i);

        /* Respiration breath detection: upward crossing of 0.30
         * with hysteresis — the detector re-arms only after the
         * waveform falls below 0.10.  A plain single-threshold
         * detector measurably over-counts (noise wiggle near the
         * 0.30 line during the descending phase registered phantom
         * breaths: ~24/min on a 13/min signal).  A 1.5 s refractory
         * (375 samples) is the second guard. */
        if (block->resp[i] < 0.10f)
            d->resp_armed = 1;
        if (d->resp_armed && d->resp_prev < 0.30f &&
            block->resp[i] >= 0.30f &&
            d->sample_idx - d->last_breath_sample > 375) {
            d->resp_armed = 0;
            if (d->last_breath_sample > 0) {
                double interval = (double)(d->sample_idx - d->last_breath_sample)
                                / VG_SAMPLE_RATE_HZ;
                if (interval >= 1.0 && interval <= 20.0) {
                    d->resp_rr_ring[d->resp_idx] = (float)interval;
                    d->resp_idx = (d->resp_idx + 1) % RESP_RING;
                    if (d->resp_count < RESP_RING)
                        d->resp_count++;
                }
            }
            d->last_breath_sample = d->sample_idx;
            d->last_breath_ms     = block->t0_ms
                + (uint64_t)i * (1000 / VG_SAMPLE_RATE_HZ);
        }
        d->resp_prev = block->resp[i];
    }

    /* --- SpO2 block average (8 s ring) --- */
    d->spo2_sum -= d->spo2_ring[d->spo2_idx];
    d->spo2_ring[d->spo2_idx] = block->spo2_pct;
    d->spo2_sum += block->spo2_pct;
    d->spo2_idx = (d->spo2_idx + 1) % SPO2_RING;
    if (d->spo2_count < SPO2_RING)
        d->spo2_count++;
    spo2_avg = d->spo2_sum / (float)d->spo2_count;

    hr      = dsp_hr(d, now);
    rr_rate = dsp_resp_rate(d, now);
    rms     = dsp_rms(d);

    /* --- raw conditions --- */
    memset(raw, 0, sizeof(raw));
    {
        /* "No beat" is measured from the last beat, or — before the
         * first beat — from the (re)join, never from stream time
         * zero: a DSP that joins a running stream has not been
         * beat-less for the stream's whole life. */
        uint64_t no_beat = d->have_beat ? now - d->last_beat_ms
                                        : now - d->stream_start_ms;
        if (now >= WARMUP_MS && no_beat >= 2000 && rms > 0.05f)
            raw[VG_COND_VFIB] = 1;
        if (now >= WARMUP_MS && no_beat >= 4000 && rms <= 0.05f)
            raw[VG_COND_ASYSTOLE] = 1;
    }
    if (hr > 0.0f && hr > d->cfg.hr_high_bpm)
        raw[VG_COND_HR_HIGH] = 1;
    if (hr > 0.0f && hr < d->cfg.hr_low_bpm)
        raw[VG_COND_HR_LOW] = 1;
    if (d->spo2_count >= 10 && spo2_avg < d->cfg.spo2_low_pct)
        raw[VG_COND_SPO2_LOW] = 1;
    if (now >= 8000 && now - d->last_breath_ms >= 6000)
        raw[VG_COND_APNEA] = 1;

    /* --- persistence + clear hysteresis -> transitions --- */
    for (i = 1; i < VG_COND_COUNT; i++) {
        if (raw[i]) {
            d->clear_since[i] = -1;
            if (d->raw_since[i] < 0) {
                d->raw_since[i] = (int64_t)now;
                d->onset_ms[i]  = now;
            }
            if (!d->active[i] && warmed &&
                now - (uint64_t)d->raw_since[i] >= d->cfg.persist_ms[i]) {
                d->active[i] = 1;
                if (events && nev < max_events) {
                    vg_alarm_event_t *ev = &events[nev++];
                    ev->condition  = (uint32_t)i;
                    ev->priority   = priority_for(d, (vg_condition_t)i,
                                                  value_for(d, (vg_condition_t)i,
                                                            hr, spo2_avg, rr_rate));
                    ev->active     = 1;
                    ev->value      = value_for(d, (vg_condition_t)i,
                                               hr, spo2_avg, rr_rate);
                    ev->t_onset_ms = d->onset_ms[i];
                    ev->t_event_ms = now;
                }
            }
        } else {
            d->raw_since[i] = -1;
            if (d->active[i]) {
                if (d->clear_since[i] < 0)
                    d->clear_since[i] = (int64_t)now;
                if (now - (uint64_t)d->clear_since[i] >= CLEAR_HYST_MS) {
                    d->active[i]      = 0;
                    d->clear_since[i] = -1;
                    if (events && nev < max_events) {
                        vg_alarm_event_t *ev = &events[nev++];
                        ev->condition  = (uint32_t)i;
                        ev->priority   = priority_for(d, (vg_condition_t)i,
                                                      value_for(d, (vg_condition_t)i,
                                                                hr, spo2_avg, rr_rate));
                        ev->active     = 0;
                        ev->value      = value_for(d, (vg_condition_t)i,
                                                   hr, spo2_avg, rr_rate);
                        ev->t_onset_ms = d->onset_ms[i];
                        ev->t_event_ms = now;
                    }
                }
            }
        }
        if (d->active[i])
            mask |= (1u << i);
    }

    if (vitals_out) {
        vitals_out->t_ms           = now;
        vitals_out->hr_bpm         = hr;
        vitals_out->spo2_pct       = spo2_avg;
        vitals_out->resp_rate      = rr_rate;
        vitals_out->conditions_mask = mask;
    }
    return nev;
}
