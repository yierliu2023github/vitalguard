/*
 * ecg.c — ECG waveform synthesiser.
 *
 * Beat morphology is a sum of Gaussians over the beat phase
 * (P wave, QRS complex, T wave) — the classic ECGSYN approach in
 * miniature.  Rhythms are produced by changing the RR-interval
 * process and the morphology:
 *
 *   NORMAL    base HR with mild respiratory sinus arrhythmia
 *   TACHY     ~132 bpm sinus
 *   BRADY     ~38 bpm sinus
 *   AFIB      irregularly irregular RR, P wave removed, fibrillatory
 *             baseline ripple
 *   VTACH     ~178 bpm, wide bizarre QRS
 *   VFIB      no beats at all: two drifting oscillators, amplitude
 *             ~0.25 mV — derivative far below the QRS detector's
 *             operating point, RMS well above asystole's
 *   ASYSTOLE  baseline wander + noise only
 */
#include "signals.h"
#include "vg_rng.h"
#include "vg.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DT_S (1.0 / VG_SAMPLE_RATE_HZ)

struct vg_ecg {
    uint32_t    rng;
    float       base_hr;
    vg_rhythm_t rhythm;
    double      rhythm_until_s; /* revert to NORMAL at this time; <0 = hold */
    double      t_s;
    double      phase;          /* position in current beat, [0,1)        */
    double      rr_s;           /* current RR interval                    */
};

static double gauss(double p, double centre, double width, double amp)
{
    double d = p - centre;
    return amp * exp(-(d * d) / (2.0 * width * width));
}

/* One beat of morphology at phase p for the given rhythm. */
static float morphology(const vg_ecg_t *e, double p)
{
    double v;
    switch (e->rhythm) {
    case VG_RHYTHM_VTACH:
        /* Wide, deep, monomorphic ventricular complex. */
        v = gauss(p, 0.32, 0.055, 1.15)
          + gauss(p, 0.62, 0.075, -0.45);
        return (float)v;
    default:
        v = gauss(p, 0.310, 0.010,  1.00)   /* R */
          + gauss(p, 0.292, 0.006, -0.10)   /* Q */
          + gauss(p, 0.330, 0.007, -0.22)   /* S */
          + gauss(p, 0.560, 0.034,  0.27);  /* T */
        if (e->rhythm != VG_RHYTHM_AFIB)
            v += gauss(p, 0.160, 0.020, 0.12);  /* P (absent in AFib) */
        return (float)v;
    }
}

static double pick_rr(vg_ecg_t *e)
{
    float u = vg_rng_unit(&e->rng);
    switch (e->rhythm) {
    case VG_RHYTHM_TACHY: return 60.0 / 132.0 * (0.97 + 0.06 * u);
    case VG_RHYTHM_BRADY: return 60.0 / 38.0  * (0.97 + 0.06 * u);
    case VG_RHYTHM_AFIB:  return 0.42 + 0.50 * u;      /* 65–143 bpm, irregular */
    case VG_RHYTHM_VTACH: return 60.0 / 178.0 * (0.98 + 0.04 * u);
    default: {  /* NORMAL: base rate + gentle sinus arrhythmia */
        double base = 60.0 / e->base_hr;
        return base * (1.0 + 0.03 * sin(2.0 * M_PI * e->t_s / 8.0))
                    * (0.985 + 0.03 * u);
    }
    }
}

vg_ecg_t *vg_ecg_new(uint32_t seed, float base_hr_bpm)
{
    vg_ecg_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->rng            = seed;
    e->base_hr        = base_hr_bpm > 20.0f ? base_hr_bpm : 72.0f;
    e->rhythm         = VG_RHYTHM_NORMAL;
    e->rhythm_until_s = -1.0;
    e->t_s            = 0.0;
    e->phase          = 0.0;
    e->rr_s           = 60.0 / e->base_hr;
    return e;
}

void vg_ecg_free(vg_ecg_t *ecg)
{
    free(ecg);
}

void vg_ecg_set_rhythm(vg_ecg_t *ecg, vg_rhythm_t rhythm, double duration_s)
{
    ecg->rhythm         = rhythm;
    ecg->rhythm_until_s = duration_s > 0.0 ? ecg->t_s + duration_s : -1.0;
    if (rhythm != VG_RHYTHM_VFIB && rhythm != VG_RHYTHM_ASYSTOLE)
        ecg->rr_s = pick_rr(ecg);
}

vg_rhythm_t vg_ecg_rhythm(const vg_ecg_t *ecg)
{
    return ecg->rhythm;
}

float vg_ecg_nominal_hr(const vg_ecg_t *ecg)
{
    switch (ecg->rhythm) {
    case VG_RHYTHM_TACHY: return 132.0f;
    case VG_RHYTHM_BRADY: return 38.0f;
    case VG_RHYTHM_VTACH: return 178.0f;
    case VG_RHYTHM_VFIB:
    case VG_RHYTHM_ASYSTOLE: return 0.0f;
    default: return ecg->base_hr;
    }
}

double vg_ecg_time_s(const vg_ecg_t *ecg)
{
    return ecg->t_s;
}

float vg_ecg_next(vg_ecg_t *e)
{
    float noise = vg_rng_signed(&e->rng);
    double wander, v;

    e->t_s += DT_S;
    if (e->rhythm_until_s >= 0.0 && e->t_s >= e->rhythm_until_s) {
        e->rhythm         = VG_RHYTHM_NORMAL;
        e->rhythm_until_s = -1.0;
        e->rr_s           = pick_rr(e);
    }

    wander = 0.04 * sin(2.0 * M_PI * 0.27 * e->t_s);   /* baseline wander */

    if (e->rhythm == VG_RHYTHM_VFIB) {
        /* Chaotic coarse fibrillation: two detuned, slowly drifting
         * oscillators.  Amplitude ~0.25 mV, dominant energy 3–7 Hz. */
        v = 0.16 * sin(2.0 * M_PI * 4.3 * e->t_s
                       + 1.7 * sin(2.0 * M_PI * 0.9 * e->t_s))
          + 0.09 * sin(2.0 * M_PI * 6.1 * e->t_s
                       + 0.8 * sin(2.0 * M_PI * 1.7 * e->t_s));
        return (float)(v + wander * 0.3 + noise * 0.010);
    }
    if (e->rhythm == VG_RHYTHM_ASYSTOLE)
        return (float)(wander + noise * 0.008);

    e->phase += DT_S / e->rr_s;
    if (e->phase >= 1.0) {
        e->phase -= 1.0;
        e->rr_s   = pick_rr(e);
    }

    v = morphology(e, e->phase) + wander + noise * 0.015;
    if (e->rhythm == VG_RHYTHM_AFIB)
        v += 0.030 * sin(2.0 * M_PI * 9.0 * e->t_s);   /* fibrillatory ripple */
    return (float)v;
}
