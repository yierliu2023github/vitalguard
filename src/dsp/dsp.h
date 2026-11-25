/*
 * dsp.h — VitalGuard signal-processing core.
 *
 * Consumes 100 ms sample blocks and produces:
 *   - a vitals summary (HR, averaged SpO2, respiration rate), and
 *   - alarm-condition transitions, where a condition is only raised
 *     after it has persisted for its configured delay.  Persistence
 *     delays are the monitor's first defence against alarm fatigue:
 *     a 3-second SpO2 dip is physiology/noise, a 10-second one is an
 *     alarm (see docs/design.md).
 *
 * Detection approach (deliberately "lite", and honest about it):
 *   - QRS: Pan–Tompkins-style chain — derivative, squaring, moving-
 *     window integration, adaptive threshold, refractory period —
 *     followed by peak localisation on the raw signal.
 *   - VFib vs asystole: when no QRS has been seen for >= 2 s, the
 *     1-second RMS of the trace separates chaotic fibrillation
 *     (RMS well above the noise floor) from a flat line.
 *   - HR: median of the last 8 RR intervals (the reported rate).
 *     The HR alarm conditions are dual-path: raw when the median
 *     rate is beyond a limit OR the most recent completed RR
 *     interval is beyond the same limit — the median alone reacts
 *     ~5 beats late to a step rate change and missed the 8 s HR
 *     latency budget at 38 bpm (see docs/design.md §2).  The 5 s
 *     persistence requirement applies to the combined raw signal,
 *     so transient single-interval excursions still cannot raise.
 *   - SpO2: 8 s moving average of the instantaneous value.
 *   - Respiration: upward threshold crossings with a refractory
 *     period; apnea is "no breath for too long".
 *
 * All timing derives from the block timestamps (the signal clock),
 * so the whole pipeline is deterministic and testable offline.
 *
 * Stream (re)joins: the detectors assume contiguous samples.  If a
 * block's t0 does not continue the previous block (this DSP started
 * on an already-running stream, the generator restarted, blocks
 * were dropped), all detector state is reset and re-anchored at the
 * new t0, and alarm raises are suppressed for a warm-up window
 * (WARMUP_GUARD_MS of contiguous samples, sized in dsp.c) while the
 * windows refill.  Without this, a DSP restarted by the supervisor
 * would judge a healthy stream with zeroed state and raise brief
 * spurious VFib/apnea/asystole alarms.
 */
#ifndef VG_DSP_H
#define VG_DSP_H

#include "vg.h"

typedef struct {
    float    hr_high_bpm;                 /* default 120              */
    float    hr_low_bpm;                  /* default 40               */
    float    spo2_low_pct;                /* default 90               */
    uint32_t persist_ms[VG_COND_COUNT];   /* per-condition delays     */
} vg_dsp_config_t;

void vg_dsp_config_defaults(vg_dsp_config_t *cfg);

typedef struct vg_dsp vg_dsp_t;

vg_dsp_t *vg_dsp_new(const vg_dsp_config_t *cfg);   /* NULL cfg = defaults */
void      vg_dsp_free(vg_dsp_t *dsp);

/* Process one block.  Fills *vitals_out (if non-NULL) and up to
 * max_events alarm transitions in events[]; returns the number of
 * transitions written. */
int vg_dsp_process_block(vg_dsp_t *dsp, const vg_sample_block_t *block,
                         vg_vitals_t *vitals_out,
                         vg_alarm_event_t *events, int max_events);

#endif /* VG_DSP_H */
