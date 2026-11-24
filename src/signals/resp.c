/*
 * resp.c — respiration waveform synthesiser.
 *
 * Roughly sinusoidal breathing with a longer, shallower expiratory
 * phase.  Apnea events ramp the amplitude down to (near) zero for a
 * scripted duration — the DSP's job is to notice the silence.
 */
#include "signals.h"
#include "vg_rng.h"
#include "vg.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct vg_resp {
    uint32_t rng;
    float    rate_per_min;
    double   phase;         /* [0,1) within the breath              */
    double   t_s;
    double   apnea_until_s; /* amplitude suppressed until then      */
    float    amp;           /* smoothed amplitude, 1 = breathing    */
};

vg_resp_t *vg_resp_new(uint32_t seed, float rate_per_min)
{
    vg_resp_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->rng           = seed;
    r->rate_per_min  = rate_per_min > 2.0f ? rate_per_min : 13.0f;
    r->apnea_until_s = -1.0;
    r->amp           = 1.0f;
    return r;
}

void vg_resp_free(vg_resp_t *resp)
{
    free(resp);
}

void vg_resp_apnea(vg_resp_t *resp, double duration_s)
{
    resp->apnea_until_s = resp->t_s + duration_s;
}

float vg_resp_next(vg_resp_t *r)
{
    float target_amp, s;
    r->t_s += 1.0 / VG_SAMPLE_RATE_HZ;
    target_amp = (r->apnea_until_s >= 0.0 && r->t_s < r->apnea_until_s)
                 ? 0.02f : 1.0f;
    r->amp += (target_amp - r->amp) * 0.01f;    /* ~0.4 s ramp */

    r->phase += (1.0 / VG_SAMPLE_RATE_HZ) * (r->rate_per_min / 60.0);
    if (r->phase >= 1.0)
        r->phase -= 1.0;

    s = (float)sin(2.0 * M_PI * r->phase);
    /* Inspiration taller than expiration is deep. */
    return r->amp * (s >= 0.0f ? s : 0.35f * s)
         + vg_rng_signed(&r->rng) * 0.012f;
}
