# VitalGuard design notes

This document records the design decisions that the code implements, so a
reviewer can check the code against the intent. Three topics: the alarm
philosophy, the latency budget, and how the host design maps onto real
QNX 8.0 mechanisms.

## 1. Alarm philosophy (inspired by IEC 60601-1-8)

IEC 60601-1-8 is the collateral standard for alarm systems in medical
electrical equipment. VitalGuard is a prototype, not a certified device,
but its alarm manager is modelled on the standard's concepts:

- **Priorities.** Conditions carry HIGH / MEDIUM / LOW priority.
  HIGH = a potentially life-threatening condition requiring an immediate
  operator response (VFib, asystole). MEDIUM = an abnormal condition
  requiring a prompt response (sustained SpO₂ below threshold, sustained
  tachy/bradycardia, apnea). Escalation can promote a MEDIUM alarm to
  HIGH when it goes unacknowledged.
- **Persistence before annunciation.** A condition must persist for a
  configured delay before it becomes an alarm. This is the single most
  effective alarm-fatigue defence: transient dips are physiology and
  sensor noise, sustained ones are events.

  | Condition | Threshold | Persistence |
  |---|---|---|
  | VFIB | no QRS ≥ 2 s + chaotic RMS | 0 s (detection time *is* the delay) |
  | ASYSTOLE | no QRS ≥ 4 s + flat RMS | 0 s |
  | HR_HIGH | HR > 120 bpm | 5 s |
  | HR_LOW | HR < 40 bpm | 5 s |
  | SPO2_LOW | 8 s-averaged SpO₂ < 90 % | 10 s |
  | APNEA | no breath ≥ 6 s | 5 s |

- **Latching.** HIGH-priority alarms latch: when the condition clears,
  the alarm stays annunciated until an operator acknowledges it. A VFib
  that self-terminates still happened, and someone must see it.
  MEDIUM alarms clear automatically when the condition clears.
- **Escalation.** An alarm that is neither acknowledged nor cleared
  within 60 s escalates: it re-annunciates and (for MEDIUM) is promoted
  to HIGH. Silence is an *audio pause*, not an acknowledgement: after
  120 s the alarm re-annunciates if the condition persists.
- **Audit.** Every state transition — raise, clear, latch, escalate,
  acknowledge, silence, silence-expiry — is written to an append-only
  audit log (`alarm_events.log`) with signal-clock timestamps. If it
  isn't in the log, it didn't happen.

## 2. Latency budget

The end-to-end budget for the headline case (VFib onset → annunciation)
is **4 seconds of signal time**, allocated as follows:

| Stage | Budget | Host measurement |
|---|---|---|
| Detection (no-QRS window + RMS classification) | ≤ 3 s | VFib raised 1.5 s after onset in tests |
| Transport, gen → dsp → alarmgr (2 hops) | ≤ 50 ms | 0.02 ms average round trip, framed messages |
| Alarm manager processing | ≤ 10 ms | in-process state machine, no I/O on the hot path |
| Margin | ~0.9 s | — |

Other conditions are *deliberately* slower, by design, and the budget
says so: SPO2_LOW is 10 s persistence + up to ~8 s of averaging lag, so
its budget is ≤ 24 s from the start of a step desaturation (measured:
16.9 s). HR_HIGH/HR_LOW carry 5 s persistence, budget ≤ 8 s. APNEA's
budget is ≤ 12 s. A test that "fires faster" than its persistence delay
is a failing test — see `tests/test_dsp.c`, which asserts SPO2_LOW does
*not* fire before 10 s.

**Honesty about environments.** Host numbers come from a Linux VM and
from QEMU in the target phase. QEMU is not cycle-accurate hardware: the
claim VitalGuard makes is *bounded, measured behaviour under load and
fault injection* (distributions: median / p99 / max over ≥ 30 runs),
never absolute microsecond guarantees. On target, the distributions are
captured with the QNX instrumented kernel + `tracelogger` and the System
Analysis Toolkit, not with wall-clock prints.

## 3. Mapping to QNX 8.0 (what actually exists in 8.0)

An earlier design draft referenced APS, PPS and HAM. All three are
**discontinued or deprecated in QNX SDP 8.0** (per BlackBerry's official
SDP 8.0 discontinuation notice: APS discontinued, PPS/PiPS discontinued
with RMPS as the device-publisher successor, HAM deprecated in favour of
QPLMS). The design therefore uses only mechanisms that exist in 8.0:

| Concern | QNX 8.0 mechanism | Host equivalent |
|---|---|---|
| Inter-process transport | Native message passing: `ChannelCreate` / `ConnectAttach` / `MsgSend` / `MsgReceive` / `MsgReply`, `name_attach` for rendezvous; priority inheritance on the channel bounds priority inversion | Unix-domain stream sockets + length-prefixed frames behind the same `ipc.h` interface |
| Scheduling | Fixed-priority preemptive scheduling (0–255, SCHED_FIFO); alarm path at the top, DSP next, generator and UI below | Process separation only (host makes no priority claims) |
| Supervision | QPLMS with the `slm` component: dependency-ordered startup, heartbeat monitoring, restart | `vgsupervisor`: same policy (3 s heartbeat timeout, restart budget 10/60 s, 0.5→4 s backoff) with fork/exec/waitpid mechanics |
| Logging | `slogger2` structured logging | stderr structured lines (`vg_log`), same call sites |
| Measurement | Instrumented kernel, `tracelogger`, System Analysis Toolkit | Signal-clock timestamps in messages + audit log |

Two clocks, deliberately: the **signal clock** (each 100 ms block is
stamped `t0_ms` by vitalgen and the stamp propagates downstream) makes
DSP and alarm behaviour deterministic and unit-testable offline;
**heartbeats** run on the wall/monotonic clock because supervision is
about real elapsed time, not signal time.

## 4. Signal and DSP notes (honest scope)

- ECG morphology is a Gaussian P/Q/R/S/T complex train with respiratory
  sinus arrhythmia on the normal rhythm; VFib is a two-oscillator
  chaotic waveform with no QRS and RMS well above the asystole floor —
  enough to exercise the detector honestly, not a physiological model.
- QRS detection is Pan–Tompkins-*lite*: derivative → square → 30-sample
  moving-window integration → adaptive threshold (SPKI/NPKI) → 200 ms
  refractory → peak localisation on the raw signal. Measured accuracy on
  the synthetic generator: within ±2 bpm at 60/72/90/120 bpm.
- The respiration detector uses threshold hysteresis (arm below 0.10,
  trigger at 0.30) plus a 1.5 s refractory. This was not decoration: a
  single-threshold version over-counted a 13/min signal as ~24/min
  (noise wiggle on the descending phase), which the debug trace caught
  and the hysteresis fixed (converges to 13.0/min).
- All generators use a deterministic xorshift32 PRNG with fixed seeds,
  so every demo and test run is reproducible bit-for-bit.
