/*
 * vg_time.h — monotonic clock helpers.
 *
 * On QNX these map to clock_gettime(CLOCK_MONOTONIC) exactly as here;
 * high-resolution in-process timestamps on target will use ClockCycles()
 * in the measurement harness (see docs/design.md).
 */
#ifndef VG_TIME_H
#define VG_TIME_H

#include <stdint.h>

/* Milliseconds since an unspecified epoch (monotonic, never goes back). */
uint64_t vg_now_ms(void);

/* Sleep for ms milliseconds (interrupted sleeps are not resumed). */
void vg_sleep_ms(uint32_t ms);

#endif /* VG_TIME_H */
