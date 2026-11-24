/*
 * spo2.c — SpO2 value model + plethysmograph waveform.
 *
 * The numeric value follows a first-order lag towards its target
 * (fast-ish down, slower recovery — the direction real desaturation
 * and re-saturation take), which is what makes the DSP's 8-second
 * averaging window and the alarm persistence delay behave realistically
 * in tests.  The pleth waveform is a phase-locked pulse shape scaled
 * by the current saturation.
 */
#include "signals.h"
#include "vg_rng.h"
#include "vg.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct vg_spo2 {
    uint32_t rng;
    float    baseline;
    float    value;         /* smooth internal value               */
    float    target;        /* where the value is heading          */
    double   hold_until_s;  /* revert target to baseline then; <0 = hold */
    double   t_s;
    double   pleth_phase;   /* [0,1) within the current pulse      */
};

vg_spo2_t *vg_spo2_new(uint32_t seed, float baseline_pct)
{
    vg_spo2_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rng          = seed;
    s->baseline     = baseline_pct;
    s->value        = baseline_pct;
    s->target       = baseline_pct;
    s->hold_until_s = -1.0;
    return s;
}

void vg_spo2_free(vg_spo2_t *spo2)
{
    free(spo2);
}

void vg_spo2_desat(vg_spo2_t *spo2, float target_pct, double duration_s)
{
    spo2->target       = target_pct;
    spo2->hold_until_s = duration_s > 0.0 ? spo2->t_s + duration_s : -1.0;
}

void vg_spo2_tick(vg_spo2_t *s, double dt_s)
{
    double tau;
    s->t_s += dt_s;
    if (s->hold_until_s >= 0.0 && s->t_s >= s->hold_until_s) {
        s->target       = s->baseline;
        s->hold_until_s = -1.0;
    }
    tau = (s->target < s->value) ? 2.5 : 4.5;   /* down faster than up */
    s->value += (float)((s->target - s->value) * (dt_s / tau));
}

float vg_spo2_value(const vg_spo2_t *spo2)
{
    /* Report a slightly noisy instantaneous value; the DSP averages.
     * Noise is hashed from the generator clock so successive blocks
     * (each 100 ms later) see independent jitter. */
    uint32_t st = spo2->rng ^ (uint32_t)(spo2->t_s * 1000.0) * 2654435761u;
    return spo2->value + vg_rng_signed(&st) * 0.3f;
}

float vg_spo2_pleth_next(vg_spo2_t *s, float hr_bpm)
{
    double ph, x;
    if (hr_bpm < 20.0f)
        hr_bpm = 72.0f;                     /* pleth free-runs if HR lost */
    s->pleth_phase += (1.0 / VG_SAMPLE_RATE_HZ) * (hr_bpm / 60.0);
    if (s->pleth_phase >= 1.0)
        s->pleth_phase -= 1.0;
    ph = s->pleth_phase;
    /* Sharp systolic rise, dicrotic shoulder, exponential decay. */
    x = pow(fmax(sin(M_PI * fmin(ph / 0.28, 1.0)), 0.0), 1.5) * (ph < 0.28 ? 1.0 : 0.0)
      + (ph >= 0.28 ? exp(-(ph - 0.28) * 6.0) * 0.85 : 0.0)
      + (ph >= 0.40 && ph < 0.55 ? 0.12 * sin(M_PI * (ph - 0.40) / 0.15) : 0.0);
    return (float)(x * (s->value / 100.0) + vg_rng_signed(&s->rng) * 0.01);
}
