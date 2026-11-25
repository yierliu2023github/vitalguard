# Devpost submission draft — VitalGuard

*Draft for the QNX Global Hackathon submission form. Paste-ready; the
two bracketed links are filled in at submission time.*

- **Project name:** VitalGuard
- **Tagline:** A patient-monitor alarm core for QNX 8.0 that treats alarm
  latency as a real-time property — and proves it with measurements.
- **Category:** Robotics & Medical
- **Also targeting:** Best use of QNX-specific Technologies ·
  Best Codelab / Tutorial (companion codelab, see below)
- **Repository:** https://github.com/yierliu2023github/vitalguard
- **Demo video:** [YOUTUBE URL — filled in when the owner uploads
  `demo/vitalguard-demo.mp4`, 2 min 11 s]
- **License:** Apache-2.0

---

## Inspiration

Patient monitors fail in two directions. Alarm too late, and a lethal
rhythm — ventricular fibrillation — goes unannounced while the patient
is still salvageable. Alarm too eagerly, and clinicians drown in
nuisance alarms until they start ignoring all of them: alarm fatigue is
a documented patient-safety hazard, and it is why the IEC 60601-1-8
alarm standard is built around persistence delays, priorities and
latching rather than raw threshold crossings.

We wanted to build the alarm core of a monitor the way the standard
implies it should behave — and, because "it should be fast" is not an
engineering claim, to *measure* it: bounded, distributed latency numbers
on the target system, under CPU load, and across process failure and
restart. QNX is the operating system real medical devices use for
exactly this, which made the QNX Global Hackathon the right place to
build it.

## What it does

VitalGuard is a multi-process vital-signs monitor running on QNX 8.0:

- **vitalgen** synthesises ECG (7 rhythms, including VFib, asystole and
  AFib), SpO₂ (with desaturation events) and respiration (with apnea
  events) at 250 Hz in 100 ms blocks, from a deterministic PRNG — every
  run is reproducible bit-for-bit.
- **vitaldsp** extracts the vital signs: Pan–Tompkins-lite QRS
  detection, heart rate (8-beat median plus an interval path), an 8 s
  SpO₂ moving average, and a hysteresis respiration detector. Each
  alarm condition must persist for its configured delay before it may
  annunciate.
- **alarmgr** is an IEC 60601-1-8-inspired alarm manager: HIGH /
  MEDIUM / LOW priorities, HIGH alarms latch until acknowledged,
  unacknowledged alarms escalate after 60 s, silence is a 120 s audio
  pause after which alarms re-annunciate, and **every** state
  transition is written to an append-only audit log.
- **vgsupervisor** keeps the pipeline alive: heartbeats, a 3 s
  timeout, a restart budget of 10 per 60 s, and 0.5 s → 4 s
  exponential backoff, with dependency-ordered startup. The target
  image autostarts the whole pipeline at boot.
- **faultctl** is the fault-injection CLI that makes the system
  testable: inject VFib, desaturation or apnea; saturate the CPU with
  a hog process; kill the DSP and watch supervision recover.

The demo scenario is the one the design point demands: boot → normal
monitoring → VFib (annunciated, latched, acknowledged) → DSP killed
mid-stream → supervisor restart → monitoring resumes with no false
alarm from the restarted DSP.

## How we built it

C11, one source tree, two builds: a host build (gcc, POSIX transport)
for fast unit/integration testing, and the QNX target build (qcc,
native transport) — exactly one IPC backend is linked per build, behind
a single `ipc.h` interface.

QNX 8.0 mechanisms, used concretely:

- **Native message passing** for all inter-process traffic:
  `name_attach()` / `name_open()` rendezvous, `MsgSend` /
  `MsgReceive` / `MsgReply`, with a HELLO handshake keyed by the
  kernel-supplied connection id (scoid). The server inherits the
  sender's priority while handling a message, bounding priority
  inversion on the alarm path.
- **Backpressure by construction:** the receive → queue → reply
  discipline keeps at most two messages in flight per connection
  instead of an unbounded queue.
- **Disconnect pulses** (`_PULSE_CODE_DISCONNECT`) give peer-death
  semantics identical to the POSIX backend's socket hangup, which is
  what lets one source tree serve both builds.
- **Fixed-priority preemptive scheduling**: the alarm path is designed
  to sit above the DSP, which sits above the generator and tooling —
  and the CPU-saturation measurements below show the alarm latency
  holding while two hog processes spin.
- **Instrumented kernel + `tracelogger`**: we captured kernel traces
  of the full alarm path and decoded them with `traceprinter`. In a
  trace taken *while both CPU hogs were running*, the DSP →
  alarm-manager `MsgSend` round trip for an alarm event completes in
  **94 µs** — about 0.005 % of the ~2 s end-to-end latency. The budget
  is spent in the designed detection and persistence windows, which is
  where a monitor should spend it.
- **A self-built target image**: `mkqnximage` from the SDP 8.0
  baseline (instrumented kernel, serial console, io-sock networking),
  with the VitalGuard binaries in `/system/bin` and a `post_start`
  snippet that autostarts the supervisor. The image needs no login and
  no manual step to bring the monitor up.
- **A deterministic signal clock**: every sample block is stamped by
  the generator and the stamp propagates downstream, so detection
  timing is unit-testable offline and the on-target numbers can be
  compared like-for-like with the host tests.

One honesty note: supervision on target is currently our own
`vgsupervisor` (policy proven on host and target). The mapping of that
policy onto QPLMS / `slm` mechanics is designed and documented in
`docs/design.md`; the image still runs with the hand-written
supervisor. We also verified against BlackBerry's SDP 8.0
discontinuation notice early on and designed *only* around mechanisms
that exist in 8.0 — an earlier draft referenced APS, PPS and HAM, all
discontinued or deprecated, and was rewritten.

## Challenges — told the way they happened

**The target image that wouldn't download.** The QNX Software Center
could not install the QEMU quick-start package (QSTI) in our
environment: five attempts failed — the first two on a full root
filesystem (the QSC cache lives on a small overlay disk), the rest in
about 11 seconds each with an Apache HttpClient `ProtocolException`
on the download redirect, while the same endpoint answered a plain
curl with a healthy HTTP 401. Rather than keep retrying a blocked
path, we bypassed it: the SDP *baseline* was installed, and we built
the QEMU image ourselves with `mkqnximage`. QSTI was never needed
again — and the detour left us with a reproducible image recipe we
understand end to end, which is arguably better for a codelab anyway.

**The restarted DSP that cried VFib.** After the port, killing the
DSP produced a brief spurious HIGH alarm (VFib / apnea / asystole,
~1–5 s) almost every time. Root cause: a DSP that rejoins a running
stream starts with zeroed detector state and judged the stream
against absolute stream time — "no beat for 60 s" was instantly true.
The fix is a 5 s warm-up guard after any (re)join plus a detector
reset on any stream discontinuity, with the trade-off stated in the
docs: a genuine VFib whose onset falls inside the window is announced
at the window boundary, worst case ≈ 3 s later than usual, only within
5 s of a rejoin. Regression tests on host and a two-boot guest run
verify the fix.

**The budget we could not meet — because the design couldn't.** Our
first on-target measurement campaign (71 fault injections) found
bradycardia annunciating at a 14.1 s median against our own ≤ 8 s
budget. Not an emulation artefact: heart rate was the median of an
8-beat ring, which at 38 bpm needs ~5 beats (≈ 7.9 s) just to cross
the limit, before the 5 s persistence even starts. The fix was a
second, interval-based detection path so the persistence clock starts
when the condition starts — without weakening the false-alarm
contract (an isolated ectopic beat still cannot raise). Bradycardia
now measures 7.27 s median / 7.69 s max on target; tachycardia, which
had been borderline, measures 6.17 s / 6.20 s. Both fixes, and the
miss that motivated them, are written up in the latency report.

## Accomplishments we're proud of

Measured on target (QNX 8.0 guest, 71 scripted fault injections +
5 supervisor restarts; full method and distributions in
`docs/latency-report.md`):

| Condition | Median | Max | Budget |
|---|---|---|---|
| VFib | 1.66 s | 2.30 s | ≤ 4 s |
| VFib with the CPU saturated | 2.01 s | 2.33 s | ≤ 4 s |
| Asystole | 3.83 s | 4.21 s | by construction |
| Tachycardia | 6.17 s | 6.20 s | ≤ 8 s |
| Bradycardia | 7.27 s | 7.69 s | ≤ 8 s |
| SpO₂ drop | 17.18 s | 17.21 s | ≤ 24 s |
| Apnea | 8.92 s | 11.39 s | ≤ 12 s |

Plus: DSP kill → streaming resumed in 0.78–2.11 s; the alarm-event
`MsgSend` round trip at 94 µs under load, from a kernel trace; a host
test suite that passes with zero warnings and caught real bugs (a
dropped-command race on socket hangup, a respiration detector that
counted one breath as two); and a pipeline that boots unattended into
a supervised, streaming monitor.

> **Emulation caveat, stated as prominently in the submission as in
> the code:** the guest runs under QEMU TCG software emulation (the
> development VM has no KVM). All figures are emulated distributions
> demonstrating timing behaviour and budget compliance — never
> hardware measurements.

## What we learned

- Priority inheritance is not a checkbox; the rendezvous discipline
  (who blocks on whom, for how long, with what queue bound) is the
  actual design.
- Persistence delays are a clinical feature, not latency to be
  optimised away — the interesting engineering is making everything
  *around* them fast and provable.
- A measurement campaign is a design tool: it found two real defects
  (the rejoin transient and the bradycardia path) that no amount of
  code review had caught, because both only exist in the running
  system's timing.
- Platform archaeology matters: half of "how QNX supervision works"
  on the internet describes mechanisms SDP 8.0 has discontinued.
  Checking the vendor's own discontinuation notice early saved the
  design.

## What's next

- **Real hardware:** run the same measurement campaign on a physical
  target (the QSTI image does exist for Raspberry Pi, and the
  mkqnximage route extends to x86 boards) and publish hardware
  distributions next to the emulated ones.
- **QPLMS / `slm` integration:** swap the supervisor's mechanics for
  the platform's own, policy unchanged, and re-run the restart
  measurements.
- **slogger2 audit trail** alongside the file-based audit log, and a
  live vitals dashboard stream (io-sock TCP) as the target console
  fallback grows up.
- **Companion codelab — "Measure your alarm latency on QNX 8.0":**
  a step-by-step tutorial that rebuilds this project's measurement
  method from scratch — build the image with `mkqnximage`, boot it,
  inject faults with `faultctl`, interpolate onsets from the DSP's
  wall/signal anchors, and read the alarm path out of a `tracelogger`
  trace with `traceprinter`. The method, scripts and worked numbers
  are the latency report in `docs/`; the codelab text is being
  finalised from it for the Best Codelab / Tutorial track.

## Built with

qnx · qnx-sdp-8.0 · c11 · qcc · gcc · make · mkqnximage · qemu ·
tracelogger · traceprinter · qnx-message-passing · python ·
iec-60601-1-8 (alarm-system concepts)

## Try it yourself

Host build (no QNX licence needed):

```sh
make            # zero warnings
make test       # unit + integration suite
tools/demo.sh   # guided demo: desat → VFib → CPU hog → kill DSP
```

Target: build with `make -f Makefile.qnx` under the SDP 8.0
environment, build the image per `qemu/README.md`, boot it with
`qemu/run-qemu.sh` — the monitor autostarts; drive it with `faultctl`.
The demo video is one scripted session on that image, and the raw
serial log it was rendered from is in `demo/` in the repo.

*VitalGuard is a research/education prototype on synthetic signals.
It is not a medical device and must never be used for patient care.*
