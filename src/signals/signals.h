/*
 * signals.h — synthetic physiological signal generators.
 *
 * These produce physiologically plausible waveforms for development
 * and demonstration.  They are NOT models of any real patient and
 * must never be connected to a real person.
 *
 * Every generator is deterministic for a given seed (xorshift32 PRNG),
 * so demos and tests are exactly reproducible.  Generators advance one
 * sample per call at VG_SAMPLE_RATE_HZ unless noted; event injections
 * (arrhythmia, desaturation, apnea) take a duration after which the
 * generator reverts to its baseline behaviour.
 */
#ifndef VG_SIGNALS_H
#define VG_SIGNALS_H

#include <stdint.h>

/* ---------------- ECG ---------------- */

typedef enum {
    VG_RHYTHM_NORMAL   = 0,     /* sinus rhythm at the configured base HR */
    VG_RHYTHM_TACHY    = 1,     /* sinus tachycardia ~132 bpm             */
    VG_RHYTHM_BRADY    = 2,     /* sinus bradycardia ~38 bpm              */
    VG_RHYTHM_AFIB     = 3,     /* atrial fibrillation: irregular RR, no P */
    VG_RHYTHM_VTACH    = 4,     /* ventricular tachycardia ~178 bpm, wide QRS */
    VG_RHYTHM_VFIB     = 5,     /* ventricular fibrillation: chaotic, no QRS  */
    VG_RHYTHM_ASYSTOLE = 6      /* flat line (wander + noise only)        */
} vg_rhythm_t;

typedef struct vg_ecg vg_ecg_t;

vg_ecg_t *vg_ecg_new(uint32_t seed, float base_hr_bpm);
void      vg_ecg_free(vg_ecg_t *ecg);

/* Switch rhythm.  duration_s > 0: revert to NORMAL after that long;
 * duration_s <= 0: stay until changed again. */
void      vg_ecg_set_rhythm(vg_ecg_t *ecg, vg_rhythm_t rhythm, double duration_s);
vg_rhythm_t vg_ecg_rhythm(const vg_ecg_t *ecg);

/* Nominal instantaneous rate of the current rhythm (for pleth sync). */
float     vg_ecg_nominal_hr(const vg_ecg_t *ecg);

/* Generator clock, seconds since creation. */
double    vg_ecg_time_s(const vg_ecg_t *ecg);

/* Next ECG sample (mV).  Advances the generator by 1/250 s. */
float     vg_ecg_next(vg_ecg_t *ecg);

/* ---------------- SpO2 ---------------- */

typedef struct vg_spo2 vg_spo2_t;

vg_spo2_t *vg_spo2_new(uint32_t seed, float baseline_pct);
void       vg_spo2_free(vg_spo2_t *spo2);

/* Desaturate towards target_pct.  duration_s > 0: recover to baseline
 * afterwards; <= 0: hold until cleared by a call with target >= baseline. */
void       vg_spo2_desat(vg_spo2_t *spo2, float target_pct, double duration_s);

/* Advance the SpO2 value model by dt seconds (call once per block). */
void       vg_spo2_tick(vg_spo2_t *spo2, double dt_s);

/* Current (noisy, instantaneous) SpO2 percentage. */
float      vg_spo2_value(const vg_spo2_t *spo2);

/* Next plethysmograph sample, phase-locked to hr_bpm. */
float      vg_spo2_pleth_next(vg_spo2_t *spo2, float hr_bpm);

/* ---------------- Respiration ---------------- */

typedef struct vg_resp vg_resp_t;

vg_resp_t *vg_resp_new(uint32_t seed, float rate_per_min);
void       vg_resp_free(vg_resp_t *resp);

/* Pause breathing for duration_s seconds (apnea event). */
void       vg_resp_apnea(vg_resp_t *resp, double duration_s);

/* Next respiration sample (arbitrary units, ~[-0.35, 1]). */
float      vg_resp_next(vg_resp_t *resp);

#endif /* VG_SIGNALS_H */
