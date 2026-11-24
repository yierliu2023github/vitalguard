# VitalGuard — STATUS

*Last updated: 2026-11-24. Repo: local git, initial commit `13251a4` (no
remote yet — the public GitHub repo is created by the account holder).*

## Phase 1 (host-side build): DONE

Everything that does not require the QNX SDP is built, compiling clean
under `gcc -std=c11 -Wall -Wextra` with **zero warnings**, and covered by
a test suite that was actually run (output below).

### What is built

- **Repo scaffold** — README.md (pitch, mermaid architecture, evidence
  table), Apache-2.0 LICENSE, .gitignore, docs/design.md (alarm
  philosophy, latency budget, QNX 8.0 mechanism mapping), qemu/ notes.
- **Signals** (`src/signals/`) — ECG with 7 rhythms (normal with
  respiratory sinus arrhythmia, tachy, brady, AFib, VTach, VFib,
  asystole) with timed injection and auto-return; SpO₂ with first-order
  desaturation/recovery events; respiration with apnea events.
  Deterministic xorshift32 PRNG — reproducible demos.
- **DSP** (`src/dsp/`) — Pan–Tompkins-lite QRS, median-RR heart rate,
  VFib/asystole discrimination by 1 s RMS, 8 s SpO₂ moving average,
  hysteresis respiration detector, per-condition persistence timing.
  All timing on the signal clock (block timestamps), so the pipeline is
  deterministic offline.
- **Alarm manager** (`src/alarm/`) — HIGH/MEDIUM/LOW priorities,
  HIGH latching, 60 s escalation (MEDIUM promotes to HIGH), 120 s
  silence with re-annunciation, ack, append-only audit log.
- **IPC** (`src/ipc/`) — one interface, two backends: POSIX Unix-socket
  framed transport (working, host) and `ipc_qnx.c`, a clearly marked
  QNX port skeleton behind `#ifdef __QNX__` (the only stub in the tree,
  together with the QEMU launcher script).
- **Supervisor** (`src/supervisor/` + `src/apps/vgsupervisor.c`) —
  policy/mechanism split: heartbeat timeout 3 s, restart budget
  10 per 60 s, backoff 0.5 s → 4 s; dependency-ordered startup
  (alarmgr → vitaldsp → vitalgen); status file. QNX target: QPLMS/`slm`.
- **Processes + faults** — vitalgen / vitaldsp / alarmgr /
  vgsupervisor, `faultctl` CLI (vfib, rhythm, desat, apnea, hog
  start/stop, kill dsp, ack, silence, status), `vghog` CPU hog, and a
  guided `tools/demo.sh` (smoke-tested: boots, reports status, shuts
  down cleanly).

### Test results (real output, `make test`, 2026-11-24)

```
===== tests/test_dsp =====
test_hr_accuracy
  target 60 bpm -> measured 59.6 bpm
  target 72 bpm -> measured 72.3 bpm
  target 90 bpm -> measured 88.8 bpm
  target 120 bpm -> measured 118.1 bpm
test_vfib_latency
  VFib onset t=10000 ms, raised at 11499 ms (latency 1499 ms)
test_spo2_persistence
  desat at t=5000 ms, SPO2_LOW raised at 21900 ms (16900 ms after desat began)
test_asystole
  asystole onset t=8000 ms, raised at 11799 ms (latency 3799 ms)
test_dsp: ALL PASSED
===== tests/test_alarm =====
  audit log lines: 11
test_alarm: ALL PASSED
===== tests/test_ipc =====
test_ipc: 200 ping-pongs in 4 ms (avg 0.020 ms round trip)
test_ipc: ALL PASSED
===== tests/test_supervisor.sh =====
  vitaldsp pid 5475 -> 5541, restarts=1        (after `faultctl kill dsp`)
  t=6199 alarm #2 RAISED cond=VFIB prio=HIGH   (post-restart VFib, audit log)
test_supervisor: ALL PASSED
==================================
ALL TESTS PASSED
```

### Bugs the tests caught and fixed during Phase 1

1. **Dropped one-shot commands.** faultctl sends a command and closes
   immediately, so server sockets show `POLLIN|POLLHUP` together; the
   supervisor and alarm manager originally handled HUP first and
   discarded the unread command (`kill dsp` silently did nothing).
   Both loops now read before honouring a hangup.
2. **Respiration over-counting.** A single-threshold breath detector
   read a 13/min signal as ~24/min (noise wiggle near the threshold on
   the descending phase passed the refractory as phantom breaths).
   Fixed with threshold hysteresis (arm < 0.10, trigger ≥ 0.30);
   the detector now converges to exactly 13.0/min.
3. **SIGPIPE hardening.** All long-lived processes ignore SIGPIPE and
   the POSIX backend sends with `MSG_NOSIGNAL`, so a peer dying
   mid-send cannot kill the sender.

## QNX SDP — licence cleared; install pending one credentialed step

- [x] **myQNX licence**: the account holder accepted and deployed the
      free QNX Everywhere non-commercial licence for QNX SDP 8.0
      (perpetual) to their myQNX account — **done 2026-11-24**. This
      blocker is cleared.
- [ ] **SDP 8.0 install**: host prep is done (see Phase 2 below). The
      one remaining credentialed step is **Step 2 of
      `~/workspace/qnx/INSTALL-RUNBOOK.md`**, run by the account holder
      in one shell session: myQNX cookie login → download the QSC
      installer (it is served only with a myQNX session cookie; no
      public link exists) → verify its posted checksum → run the
      installer → `qnxsoftwarecenter_clt` sync/activate + mirror +
      install baseline `com.qnx.qnx800` + package
      `com.qnx.qnx800.quickstart.qemu` into
      `~/workspace/qnx/sdp/qnx800`. The holder types their password
      once into a `read -s` environment variable; nothing is stored
      by us. Everything after that step is credential-free.
- [ ] Create the public GitHub repo under the owner's account and push.
- [ ] (Later) upload the demo video to the owner's YouTube channel;
      final Devpost submission click. Internal deadline: **2026-12-13**
      (Devpost deadline 2026-12-15 14:00 EST).

## Phase 2 prep (this VM, 2026-11-24): DONE (credential-free part)

- Workspace created: `~/workspace/qnx/{sdp,targets,traces}`.
- QEMU **8.2.2** installed via apt (`qemu-system-x86`, `qemu-utils`,
  `qemu-block-extra`) — the version Ubuntu 24.04 ships; the QSTI guide
  only recommends QEMU 10 for >720p/Vulkan, which this headless VM
  doesn't use.
- **No `/dev/kvm`** — QEMU will run in TCG emulation. In-guest timings
  are emulation-skewed distributions, never hardware numbers (already
  the project's reporting rule). QSTI default launch (8 vCPU / 4 GB)
  exceeds this VM (4 vCPU / ~6 GB shared); reduce to 2 vCPU / 2 GB at
  first boot.
- Disk: **134 GB free** of 150 GB — SDP (~6.4 GB) + QSTI (~20 GB) fit
  with wide margin; nothing needs freeing. `/tmp` is a 512 MB tmpfs —
  all QNX work stays under `~/workspace/qnx/`.
- qnx.com reachability verified with QNX's own test URL.
- Install runbook written: `~/workspace/qnx/INSTALL-RUNBOOK.md`
  (official-docs CLI route, exact commands, credentialed step marked).
- The QSC installer was **not** downloaded: per QNX's official
  unattended-install procedure it is served only with a myQNX session
  cookie, so fetching it is part of the credentialed Step 2 (including
  verification against the checksum posted on the download page).

## Milestone checklist

### Week 1 (Nov 24–30) — platform + spine
- [x] Host-side spine complete: generators → DSP → alarm manager →
      supervisor, all processes running under supervision on Linux
- [x] ECG/SpO₂/resp synthesis library with host unit tests
- [x] Repo scaffold (README, LICENSE, design docs)
- [x] Licence accepted + deployed (account holder, 2026-11-24)
- [ ] SDP 8.0 + QSTI installed — runbook Step 2 (account holder,
      ~one shell session), then credential-free Steps 3–4
- [ ] MsgSend spike on QSTI: block flow over native message passing
- [ ] First `tracelogger` capture
- **Week 1 exit criterion:** vitalgen → vitaldsp data flowing inside
  QEMU over QNX message passing. Host-side equivalent — full
  integration suite green — is **met**.

### Day 3–4 go/no-go spike (restated)
GO requires, inside QEMU: (1) sample blocks flowing vitalgen →
vitaldsp over **native QNX message passing** (not the socket backend);
(2) a `tracelogger` trace of one full block→vitals→alarm pass;
(3) a fault-command round trip answered by vitaldsp.
NO-GO fallback: demo the host build; submit the measurement tooling as
the Developer Tooling & Host Tools entry instead.

### Week 2 (Dec 1–7) — the real system
- [ ] QRS/HR/SpO₂ pipeline running on target; alarm persistence +
      latching verified on target
- [ ] slogger2 audit trail; QPLMS/`slm` supervision configuration
- [ ] Fault-injection CLI on target; kill-DSP recovery demonstrated
- [ ] Dashboard stream (io-sock TCP) or target text console fallback
- **Exit criterion:** full scenario runs unattended; DSP killed →
  QPLMS restarts it → monitoring recovers with the gap logged.

### Week 3 (Dec 8–15) — evidence + submission (submit by Dec 13)
- [ ] ≥ 30 latency measurement runs (median / p99 / max), incl. under
      CPU-hog load and across DSP restart
- [ ] Single-image build via `mkqnximage` (CTI)
- [ ] Demo video (< 3 min) recorded and published
- [ ] Companion codelab: “Measure your alarm latency on QNX 8.0”
- [ ] Devpost submission (also targets: Best use of QNX-specific
      Technologies, Best Codelab/Tutorial)

### Degradation paths (pre-agreed)
- Week 2 slips → ship ECG + SpO₂ only; still a complete demo.
- Port slips entirely → the latency-measurement tooling becomes the
  submission (TraceLab fallback, Developer Tooling category).
