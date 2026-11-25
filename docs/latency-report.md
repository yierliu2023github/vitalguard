# VitalGuard — On-Target Latency Report (QNX 8.0, custom QEMU image)

*Provenance: this is the measurement report produced with the target
workspace (raw logs, the `.kev` kernel trace and the analysis scripts
live alongside it there). Copied into the repo so the numbers quoted
in the README and the Devpost submission can be checked against the
full method, the raw distributions, and the stated caveats.*

*Measured 2026-11-25 in the guest, via the serial-socket harness in this
directory. Raw data: `logs/latency-*` (see inventory at the end).*

> **⚠️ EMULATION CAVEAT — READ FIRST.** This VM has **no KVM**. The guest
> runs under **QEMU TCG (software) emulation** on a shared 1.5-vCPU host.
> Every number in this report is a **TCG-emulated distribution**, not a
> hardware measurement. The pipeline is real-time-paced (100 ms sample
> blocks on a signal clock), so guest wall/signal clocks track host real
> time closely, but CPU-bound work runs far slower than on any real
> target and scheduling jitter is larger. These numbers demonstrate the
> design's timing behaviour and budget compliance *in emulation*; they
> must never be quoted as hardware figures.

## Method

- **System under test:** the full pipeline (`vitalgen → vitaldsp →
  alarmgr`, native QNX MsgSend backend) running in the custom QNX 8.0
  QEMU image, autostarted and supervised by `vgsupervisor`. The DSP
  warm-up defect is fixed (see `WARMUP-FIX-REPORT.md`); every set was
  preceded by a streaming check, and no measured run falls inside a
  post-restart warm-up window.
- **Injection:** scripted `faultctl` commands over the guest shell
  (`latency_test.py`), ~10 repetitions per condition.
- **Latency definition:** signal-clock onset → alarm raise.
  - *Onset:* the fault command's guest wall-clock stamp (from the
    `vitalgen` log) mapped onto the signal clock by linear interpolation
    between the DSP's `(wall, signal)` vitals anchors (anchor spacing
    measured ≈ 5000 ms, so interpolation is tight).
  - *Raise:* the `RAISED` line's signal-clock stamp in the alarm audit
    log.
  - **Bias:** the reported figure is an *upper bound*. The fault takes
    effect at a sample-block boundary after the command arrives, so the
    true onset is slightly later than the interpolated one; the true
    latency may be up to ~0.3 s lower than reported. This matters only
    for the borderline HR_HIGH case, and is flagged there.
- **Ack protocol:** HIGH alarms latch. After each injection the driver
  waited for the clear and sent `faultctl ack` before the next run —
  an un-acked latched instance suppresses later raises of the same
  condition (this invalidated most of the first apnea set; see
  "Anomalies").
- **Restart timing** uses guest wall-clock (CLOCK_MONOTONIC) log stamps,
  as stated per row.
- **Kernel trace:** `tracelogger -s 45` in the guest during one VFib
  injection under load; the `.kev` was transferred to the host and
  decoded with SDP 8.0 `traceprinter`.

## 1. Alarm latency — baseline (no added load)

All values in milliseconds, onset → raise, signal clock.

| Condition (injection) | n | min | median | p95 | max | Design budget | Verdict | Phase 1 host |
|---|---|---|---|---|---|---|---|---|
| VFib (`vfib 15`) | 10 | 1209 | 1664 | 2231 | 2298 | ≤ 4000 | ✅ within | 1499 |
| Asystole (`asystole 12`) | 10 | 3513 | 3828 | 4188 | 4205 | — (see note) | ✅ by construction | 3799 |
| Tachycardia HR_HIGH (`tachy 150 25`) | 10 | 7610 | 7846 | 8031 | 8062 | ≤ 8000 | ⚠️ borderline | not measured |
| Bradycardia HR_LOW (`brady 38 25`) | 10 | 13076 | 14140 | 14541 | 14542 | ≤ 8000 | ❌ **MISS** | not measured |
| SpO₂ drop (`desat 84 45`) | 10 | 17102 | 17176 | 17200 | 17212 | ≤ 24000 | ✅ within | 16900 |
| Apnea (`apnea 25`) | 10 | 6815 | 8923 | 10997 | 11393 | ≤ 12000 | ✅ within | not measured |

Notes:

- **Asystole** has no explicit budget line in `docs/design.md`; by
  construction it is the 3.5 s persistence plus block phase and
  detection lag. Measured median 3828 ms matches the host's 3799 ms.
- **Apnea** spread (6.8–11.4 s) is expected: detection waits for the
  first missed breath, and the last pre-onset breath can precede onset
  by up to one full respiratory period (≈ 4.6 s at 13 breaths/min),
  on top of the 10 s persistence.
- On-target (TCG) medians track the Phase 1 host numbers closely for
  the three conditions measured in both places (VFib 1664 vs 1499;
  asystole 3828 vs 3799; SpO₂ 17176 vs 16900) — the latencies are
  dominated by the designed detection/persistence windows, not by
  compute, so emulation speed has limited effect on them.

### Budget misses — stated plainly

- **HR_LOW (bradycardia) misses its ≤ 8 s budget by a wide margin**
  (median 14140 ms, max 14542 ms). This is a design property, not an
  emulation artefact: heart rate is the median of an 8-beat RR ring,
  and at 38 bpm the ring needs ~5 new beats (≈ 7.9 s) before the
  median crosses the 50 bpm limit; the 5 s persistence then runs.
  Total ≈ 13–14.5 s, as measured. Phase 1 never measured bradycardia
  latency on the host, so there is no host number it "regressed"
  from — the budget was simply never achievable with an 8-beat median
  at very low rates. **Fix options (not applied):** shorten the RR
  ring, weight recent beats, or restate the HR_LOW budget honestly
  (e.g. ≤ 15 s at 38 bpm). Until then, the design doc's ≤ 8 s claim
  for HR_LOW is **not met** and should not be quoted.
- **HR_HIGH (tachycardia) is borderline:** median 7846 ms is within
  the ≤ 8000 ms budget, but p95 (8031 ms) and max (8062 ms) exceed it
  by up to 62 ms. The measurement bias (above) overstates latency by
  up to ~0.3 s, so the true values are plausibly within budget — but
  as measured, the tail exceeds it. Reported as **borderline / not
  proven within budget**, not as a pass.

## 2. VFib under CPU contention (the preemption demo)

Two `vghog` instances spinning at nice 10, one per vCPU, verified
running and streaming before the set.

| Set | n | min | median | p95 | max |
|---|---|---|---|---|---|
| VFib baseline | 10 | 1209 | 1664 | 2231 | 2298 |
| VFib with 2× vghog | 10 | 1780 | 2009 | 2277 | 2332 |

**The alarm latency holds under load.** Median rises by ~345 ms and
the max by only 34 ms; every run remains well inside the ≤ 4 s
budget. The fixed-priority pipeline (DSP/alarm path above the hogs'
nice-10 threads) is not starved by CPU saturation in emulation.

## 3. Supervisor restart latency

`faultctl kill dsp`, guest wall clock, n = 5 consecutive kills
(~21 s apart, streaming verified after each):

| Kill # | kill → respawn (ms) | kill → vitalgen reconnected (ms) | kill → first vitals from new DSP (ms) |
|---|---|---|---|
| 1 | 673 | 783 | 790 |
| 2 | 1020 | 1127 | 1135 |
| 3 | 2027 | 2087 | 2093 |
| 4 | 2025 | 2077 | 2083 |
| 5 | 2038 | 2107 | 2114 |

**Range: 0.78–2.11 s from kill to streaming resumed.** The growth
across kills is the supervisor's designed backoff (0.5 s doubling
toward the 4 s cap, restart budget 10 per 60 s), not degradation:
detection + spawn is ~0.17–0.5 s on top of the current backoff. The
restarted DSP logs a vitals line on its first processed block, so
"first vitals" ≈ processing resumed; it follows reconnection by
< 10 ms in every run.

## 4. Kernel-trace evidence (VFib under load)

Capture: `latency.kev` (5,064,416 bytes, `tracelogger -s 45`),
decoded text `logs/latency-trace.txt` (322,604 lines), curated
excerpt `logs/latency-trace-excerpt.txt`. The window contains the
full path for alarm #11 (VFib, raised under both hogs running):

`faultctl →(VG_MSG_FAULT_CMD)→ vitalgen →(sample blocks)→ vitaldsp
→(VG_MSG_ALARM_EVENT)→ alarmgr → audit-log write`

The raise chain, timed by kernel cycles (TRACE_CYCLES_PER_SEC =
1,491,988,175), relative to the DSP's send call:

| Step | Event | Δ from send |
|---|---|---|
| 1 | vitaldsp `KER_CALL MSG_SENDV`, msg type 3 (VG_MSG_ALARM_EVENT) | 0 µs |
| 2 | `SND_MESSAGE` delivered to alarmgr (pid 327702) | +12.3 µs |
| 3 | vitaldsp `THREPLY` (blocked on reply) | +26.5 µs |
| 4 | alarmgr `THRUNNING` | +29.1 µs |
| 5 | alarmgr `REC_MESSAGE` / receive exit, rmsg type 3 | +55.5 / +64.1 µs |
| 6 | alarmgr `KER_CALL MSG_REPLYV` | +82.1 µs |
| 7 | `REPLY_MESSAGE` → vitaldsp `THREADY` (round trip complete) | +94.3 µs |
| 8 | alarmgr audit-log write: `MSG_SENDV` type 0x102 → fs (pid 65544) | +378.3 µs |

Cross-checks inside the same trace:

- **Fault command → alarm-event send = 1.465 s** (kernel cycles),
  consistent with the log-based VFib distributions above (the two
  figures differ slightly in definition: kernel timing starts at the
  fault command's send, log timing at the interpolated onset).
- **Raise send → clear send = 15.251 s** for a 15.0 s injection.
- Exactly 2 type-3 (ALARM_EVENT) sends and 403 type-2 (VITALS) sends
  occur in the window, as expected.
- The vghog pids (671759, 679938) appear `THRUNNING`/`THREADY`
  throughout the raise window — the chain above executed under full
  CPU contention.

**Takeaway:** the complete MsgSend round trip for the alarm event
takes ~94 µs under load in emulation — roughly 0.005 % of the ~2 s
onset→raise latency. End-to-end alarm latency is governed by the
designed detection and persistence windows, not by IPC or scheduling.

## 5. Anomalies and findings

1. **Apnea priority: code ≠ design doc.** `src/dsp/dsp.c`
   (`priority_for`) classifies APNEA as **HIGH** (latched), while
   `docs/design.md` describes it as MEDIUM. Consequence during
   measurement: the first apnea set produced only 1 valid run — the
   latched run-1 instance suppressed runs 2–10 until acked. The
   valid distribution comes from a dedicated re-run (Phase A2) with
   an ack after every injection. Either the code or the doc should
   change; flagged, not fixed, in this phase.
2. **HR_LOW budget miss** — see §1. A design/budget inconsistency,
   not a platform problem.
3. No run in any set was contaminated by the post-restart DSP
   warm-up window; sets were gated on explicit streaming checks.

## 6. Raw data inventory (this directory)

- `latency_test.py` — the measurement driver; `analyze_latency.py` —
  onset interpolation + statistics; `latency_sink.py` — host receiver
  for the `.kev` transfer.
- `logs/latency-{a,a2,b}-runs.json` — per-run driver records.
- `logs/latency-{a,a2,b}-{console,gen,dsp,audit,sup}.log` — guest
  console and per-process log segments the numbers were computed
  from.
- `logs/latency-stats.json`, `logs/latency-analysis.out` — computed
  statistics (all rows, incl. per-run onset/latency).
- `latency.kev` — raw kernel trace; `logs/latency-trace.txt` — full
  traceprinter decode; `logs/latency-trace-excerpt.txt` — the alarm
  path chain (§4).
- `logs/latency-b-pidin.txt` — process table during the contention
  phase (pid ↔ process mapping used by §4).

## 7. Addendum (2026-11-25): HR budgets fixed, APNEA priority resolved

Both findings of §5 are closed. Same emulation caveat as the head
of this report: all guest figures are TCG distributions, and the
latency method (including its ≤ ~0.3 s one-sided bias) is the one
from "Method" above, unchanged.

### 7.1 HR_LOW / HR_HIGH — dual-path detection

**Change** (`src/dsp/dsp.c` only; thresholds and persistence values
untouched). The HR conditions are now raw when the 8-beat median
rate is beyond the limit **or** the most recent completed RR
interval is beyond the same limit (RR > 1.5 s for HR_LOW at the
40 bpm limit, RR < 0.5 s for HR_HIGH at 120 bpm). For HR_LOW the
in-progress interval also counts: once the gap since the last beat
exceeds 1.5 s the instantaneous rate is already below the limit —
bounded by the same 3 s staleness rule `dsp_hr()` uses, so a
no-beat stream (VFib/asystole) holds that raw for at most 1.5 s
and can never raise HR_LOW from it. The persistence clock now
starts at the first abnormal interval instead of after ~5 beats.
The 5 s persistence still requires raw to hold *continuously*: one
abnormal interval holds raw only until the next beat (~1
interval), so an isolated ectopic beat or a single missed
detection cannot raise — verified by the new host test
`test_transient_hr_no_raise` and by burst probing (old vs new
code): bradycardia bursts up to 4 s raise nothing in either;
tachycardia bursts ≤ 2.5 s raise nothing in either.

**One deliberate boundary move.** A tachycardia burst ≥ ~3 s now
raises HR_HIGH (old code: no raise even at 4 s). Reason: after a
3 s burst the *median* rate remains above 120 bpm for > 5 s in
total, so the condition as measured has met the design's own
persistence requirement; the interval path merely starts the
clock when the condition actually starts. Documented in
`docs/design.md` §2.

**Host results** (new tests in `tests/test_dsp.c`, step change
from 72 bpm; `make test` ALL PASSED, zero warnings):

| Test | Result | Budget |
|---|---|---|
| `test_brady_latency` (→ 38 bpm) | HR_LOW raised at **6999 ms** | ≤ 8000 ✅ |
| `test_tachy_latency` (→ 132 bpm) | HR_HIGH raised at **5799 ms** | ≤ 8000 ✅ |
| Phase 1 numbers | VFib 1499 ms, asystole 3799 ms, SpO₂ +16900 ms — **unchanged** | — |
| Warm-up regressions | `test_rejoin_warmup`, `test_discontinuity_reset` — pass | — |

**Guest confirmation** (rebuilt binaries + image; driver
`hrfix_test.py`, analysis `analyze_hrfix.py`, raw data
`logs/hrfix-{runs.json,stats.json,audit,gen,dsp,sup,console}.log`):

| Set | n | min | median | max | Budget | Before (§1) |
|---|---|---|---|---|---|---|
| Bradycardia HR_LOW (`rhythm brady 30`) | 5 | 6585 | **7269** | **7693** | ≤ 8000 | median 14140, max 14542 ❌ |
| Tachycardia HR_HIGH (`rhythm tachy 20`) | 3 | 5988 | **6165** | **6203** | ≤ 8000 | median 7846, max 8062 ⚠️ |

Every run is within budget. The HR_LOW worst run (7693 ms) is an
upper-bound figure: with the method's ≤ ~0.3 s bias the true value
is ≲ 7.4 s, but the margin is honestly modest — the residual delay
is beat-phase alignment at the rhythm switch plus the 5 s
persistence, both by design. The audit segment for the whole
session contains exactly the 8 expected RAISED/CLEARED pairs
(HR_LOW/HR_HIGH, MEDIUM) and nothing else. Note the HR_LOW raise
`value=` fields (42–51 bpm): the alarm now annunciates on interval
evidence while the median estimate is still descending through
the 40s — the reported value remains the median HR, as before.

### 7.2 APNEA priority — resolved: HIGH

The code was right; the design doc was wrong. Sustained apnea has
gone ≥ 11 s without a breath by annunciation (6 s detection + 5 s
persistence): an arrest-class event on the direct path to hypoxic
arrest, and mainstream monitor practice under IEC 60601-1-8
treats apnea-time-exceeded as a high-priority alarm. Latching is
also consistent with the doc's own VFib rationale (a
self-terminating event still happened and must be seen), and a
MEDIUM start would have waited out a 60 s escalation before
reaching HIGH — far too slow for apnea. `docs/design.md` §1 now
classifies apnea as HIGH with this rationale, and its persistence
table carries a Priority column for every condition (including
SPO2_LOW's code rule: MEDIUM, HIGH when < 80 %). A repo-wide grep
confirms no contradictory statement remains; `priority_for` in
`src/dsp/dsp.c` is the single source of truth. §5 item 1 is
closed; the §1 table's HR rows and §5 item 2 are superseded by
§7.1 above.
