/* vg_rng.h — xorshift32 PRNG shared by the signal generators.
 * Deterministic across platforms and runs for a given seed. */
#ifndef VG_RNG_H
#define VG_RNG_H

#include <stdint.h>

static inline uint32_t vg_rng_next(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Uniform in [0, 1). */
static inline float vg_rng_unit(uint32_t *state)
{
    return (float)(vg_rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

/* Uniform in [-1, 1). */
static inline float vg_rng_signed(uint32_t *state)
{
    return vg_rng_unit(state) * 2.0f - 1.0f;
}

#endif /* VG_RNG_H */
