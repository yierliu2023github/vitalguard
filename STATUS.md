# VitalGuard — STATUS

*Last updated: 2026-11-25. Repo: local git, initial commit `13251a4` (no
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

## QNX SDP — installed and verified; QSTI package still blocked

- [x] **myQNX licence**: the account holder accepted and deployed the
      free QNX Everywhere non-commercial licence for QNX SDP 8.0
      (perpetual) to their myQNX account — **done 2026-11-24**. This
      blocker is cleared.
- [x] **SDP 8.0 baseline installed and verified (2026-11-24)** — see
      "Phase 2 completion" below. Baseline `com.qnx.qnx800`
      **8.0.0.00141T202311271501L** in `~/workspace/qnx/sdp/qnx800`
      (3.6 GB); licence synced and activated via the CLT using the
      temporary password the holder provided for this one-time use
      (used only as CLT arguments, never written to any file);
      `qcc` (GCC 12.2.0) cross-compiles for x86_64 and aarch64le —
      verified with real QNX ELF outputs.
- [ ] **QSTI for QEMU package NOT installed** — five CLT attempts on
      2026-11-24 failed in the Collect (download) phase; two distinct
      causes, both documented with exact errors in "Phase 2
      completion" below: (1) VM filesystem layout (7.5 GB overlay
      root) → "No space left on device", fixed for the QSC cache by
      relocating it to the workspace disk; (2) afterwards the
      artifact-download requests fail in ~11 s with an Apache
      HttpClient `ProtocolException` ("The server failed to respond
      with a valid HTTP response") on the download redirect, while
      the same endpoint answers a plain curl with a valid HTTP 401 —
      i.e. the server is healthy; the failure is in the CLT/proxy
      download path and may be transient. *(Superseded 2026-11-25:
      the QEMU image was built with mkqnximage from the SDP baseline
      instead — see "Phase 2 port" below; QSTI is no longer needed
      and this item is closed by bypass, not by download.)*
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
- The QSC installer was **not** downloaded during prep: per QNX's
  official unattended-install procedure it is served only with a myQNX
  session cookie, so fetching it is part of the credentialed Step 2
  (including verification against the checksum posted on the download
  page). *(Superseded the same day — see below.)*

## Phase 2 continuation (2026-11-24, later session)

- **QSC installer obtained and installed.** The account holder
  downloaded `qnx-setup-2.0.4-202501021438-linux.run` (104,701,288
  bytes) through the signed-in browser. It was staged at
  `~/workspace/qnx/`, SHA-256
  `5695a0ec8a1fd91840753b2cba86a9325f6052d4f69c757166cc61b09bcbe507`
  (no official checksum was recorded at download time; the
  archive's own Makeself integrity check passed). The `.run`
  wrapper cannot complete on this headless VM (`/dev/tty` failure),
  so the payload's own `start.sh` was run with the same headless
  flags — the result is identical to the official route:
  **QNX Software Center 2.0.4 (build v202501021438) installed at
  `~/workspace/qnx/qnxsoftwarecenter/` (207 MB)**,
  `~/.qnx/qnx_env.ini` written. Full log in
  `~/workspace/qnx/INSTALL-RUNBOOK.md` (execution log section).
- **License-file route ruled out (with evidence).** The CLT has no
  `-importLicense`/file-import option; `-syncLicenseKeys` without
  credentials errors `Credentials for myQNX are required …`, and
  `-mirror` fails against `https://www.qnx.com/swcenter` with
  `Request requires authorization`. A license key string alone
  (`-addLicenseKey`) registers locally but does not authorize
  package downloads; QNX manual activation is GUI-only and
  activation-only. **Remaining blocker is exactly one CLT block**
  (runbook Step 2e–2g) with the holder's myQNX password supplied
  once via `read -s`. SDP 8.0 and QSTI are therefore **not yet
  installed**; QEMU boot remains the following phase.
  *(Superseded the same day — see Phase 2 completion below.)*

## Phase 2 completion (2026-11-24, credentialed session)

The holder provided a temporary myQNX password for one-time use. It
was passed only as `-myqnx.user` / `-myqnx.password` CLT arguments
(via a helper that reads it from stdin into the process environment);
it was never written to any file, log, or this repo, and is discarded.

- **Licence sync + activation: SUCCESS.**
  `-syncLicenseKeys -activateAll -listLicenseKeys` →
  `License key is added: …` and the key listed as **Activated —
  QNX Software Development Platform 8.0 — Perpetual — Developer
  License — Non-Commercial Single-User (ver. 8.0)**.
- **Mirror: SUCCESS.** `-mirror` generated metadata for the dropins
  and per-account repositories without error.
- **Baseline install: SUCCESS.** `-installBaseline com.qnx.qnx800`
  installed **com.qnx.qnx800 8.0.0.00141T202311271501L** (profile
  `com.qnx.qnx800_1`; targets x86_64 + aarch64le) into
  `~/workspace/qnx/sdp/qnx800` — **3.6 GB** on disk. The fallback ID
  was not needed.
- **Verification (runbook Step 3): PASS.**
  `qcc`, `q++`, `mkqnximage`, `traceprinter` all present;
  `qcc -V` → GCC **12.2.0**, targets `gcc_ntox86_64` (default),
  `gcc_ntoaarch64le`, plus C++ variants. A hello-world was
  cross-compiled for **both** targets, producing QNX ELF binaries
  (interpreter `/usr/lib/ldqnx-64.so.2`). One fix was needed first:
  the CLT (Java `user.home` = passwd entry) stores the activated
  licence in `/root/.qnx/license/licenses`, while native tools read
  `$HOME/.qnx/license/` (`HOME=/home/hatch` on this VM) — the
  `licenses` file was copied there, after which `qcc` reports no
  licence complaint.
- **QSTI package install: FAILED (5 attempts, 2 causes).**
  Package `com.qnx.qnx800.quickstart.qemu`
  0.5.0.00050T202609021847L; its three binary artifacts have
  download sizes ≈ 1.04 GB + 1.03 GB + 0.43 GB (≈ 10 GB uncompressed).
  - Attempts 1–2: `java.io.IOException: No space left on device` in
    the Collect phase. Root cause (attempt 1): QSC state lives under
    `/root/.qnx/` (Java `user.home`), which sits on this VM's
    **7.5 GB overlay root filesystem**, not the 150 GB workspace
    disk. Remedy applied: `/root/.qnx/swupdate` (the download cache)
    was moved to `~/workspace/qnx/swupdate-store` and symlinked back
    (symlinking all of `/root/.qnx` does not work — the CLT errors
    `Cannot create /root/.qnx`). The retry also forced
    `-Djava.io.tmpdir` to the workspace disk (ECF stages downloads
    in `java.io.tmpdir`; `/tmp` here is a 512 MB tmpfs).
  - Attempts 3–4: downloads now fail in ~11 s with
    `org.apache.http.client.ClientProtocolException … ProtocolException:
    The server failed to respond with a valid HTTP response`
    (stack shows `RedirectExec`) for all three artifacts. The same
    endpoint answers an unauthenticated curl with a valid HTTP 401
    and the runbook connectivity test still passes, so the QNX
    server is healthy; the failure is in the CLT/egress-proxy
    download path and may be transient rate-limiting after the
    earlier multi-GB transfers. **Next step:** rerun the single
    Step 2g command in a later session (helper:
    `~/workspace/qnx/run-clt.sh qsti`, password via stdin). No other
    blocker stands between that and the QEMU boot phase.
  - Attempt 5 (2026-11-24 15:38 CST, later session): the recorded
    retry (`run-clt.sh qsti`, temporary password via stdin only)
    failed identically — exit 13 after ~12 s, all three artifacts
    erroring in the Collect phase with
    `org.apache.http.client.ClientProtocolException` caused by
    `org.apache.http.ProtocolException: The server failed to respond
    with a valid HTTP response`. No partial install. Post-run, the
    log scrubber was re-run (this time with the log paths passed
    explicitly): 3 further occurrences of the temporary password in
    QSC's own logs were redacted, and a full scan of the QSC log
    locations, `~/.qnx`, `/root/.qnx` and this repo verified **0
    occurrences remain**. Next retry to be scheduled in a later
    session; the temporary password should still be rotated by the
    holder.

## Phase 2 port (2026-11-25): native backend + supervisor on target — DONE

Full evidence: `~/workspace/qnx/custom-image/PORT-REPORT.md` (spike:
`SPIKE-REPORT.md` in the same directory). The QSTI blocker was
bypassed by building the target image with `mkqnximage` from the
installed SDP baseline; QSTI itself is no longer needed.

- **Custom QEMU image boots** to a ksh shell (mkqnximage standard
  profile, 2 vCPU / 2 GB, TCG); VitalGuard binaries ride in
  `/system/bin` via the `system_files.custom` snippet.
- **`/proc/self/exe` port fix**: new `src/common/vg_path.c`
  (`vg_self_dir()`: $VG_BINDIR → Linux /proc/self/exe → argv[0]/PATH
  resolution). SDP headers verified to offer no QNX path mechanism
  (procfs `exefile` is the image, not a path). vgsupervisor and
  `faultctl hog start` use it; host behaviour unchanged.
- **`ipc_qnx.c` implemented** (was the skeleton): name_attach /
  name_open + HELLO handshake keyed by scoid, MsgSend/MsgReceive/
  MsgReply with receive→queue→reply discipline and a two-slot
  per-conn bound (backpressure), disconnect pulses mapped to the
  POSIX hangup semantics, TimerTimeout for timeouts. Apps wait via
  the new backend-neutral `vg_ipc_wait_any()` (ipc.h; poll() on
  host, channel pump on QNX) — all four app loops adapted.
  `Makefile.qnx` links the native backend instead of ipc_posix.c.
  Host `make` + `make test` green; QNX build **zero warnings**.
- **Guest-verified end-to-end on the native backend**: supervisor
  starts the pipeline in dependency order (bare-name PATH route);
  `/dev/name/local` shows all four endpoints and there are no
  `.sock` files; `faultctl kill dsp` → vitaldsp restarted
  (pid 442387 → 696339, restarts=1) and monitoring resumes;
  `faultctl vfib 10` → vitalgen logs the fault, audit log shows
  VFIB HIGH raised and cleared exactly 10.0 s apart.
- **Autostart**: `post_start.custom` starts vgsupervisor
  (VG_BINDIR=/system/bin) at boot; a clean boot brings the whole
  pipeline up unattended (verified, incl. a vfib 7 sanity alarm).
- **Native MsgSend spike + tracelogger** (spike session): demo pair
  round-trips over name_attach/MsgSend; a 30 s instrumented-kernel
  trace (715,569 events) decoded on the host with full COMM chains.
- **Finding — DSP restart transient**: a vitaldsp that rejoins a
  running stream mid-flight (i.e. after a supervisor restart) can
  raise a brief spurious HIGH alarm (VFIB / APNEA / ASYSTOLE,
  ~1–5 s) from its zeroed detector state. Does not occur on a
  normal start. Fix planned: warm-up guard in the DSP (suppress
  condition evaluation until ~5 s of contiguous samples; treat a
  t0 jump as a state reset). Latency runs must flag the first
  seconds after a DSP restart until this lands.
- Still open: QPLMS/`slm` mechanics (mapping documented in
  docs/design.md; image runs OPT_SLM=no, supervisor via post_start),
  on-target latency distributions, sshd startup segfault in the
  image (serial console is the access path), public repo + video +
  Devpost submission (owner actions).

## Milestone checklist

### Week 1 (Nov 24–30) — platform + spine
- [x] Host-side spine complete: generators → DSP → alarm manager →
      supervisor, all processes running under supervision on Linux
- [x] ECG/SpO₂/resp synthesis library with host unit tests
- [x] Repo scaffold (README, LICENSE, design docs)
- [x] Licence accepted + deployed (account holder, 2026-11-24)
- [x] SDP 8.0 + QSTI installed — **SDP 8.0 baseline installed and
      verified 2026-11-24** (see Phase 2 completion); the QSTI
      package download stayed blocked, so the target image was
      built with **mkqnximage from the SDP baseline** instead
      (2026-11-25, see "Phase 2 port" above) — QSTI no longer needed
- [x] MsgSend spike on target: block flow over native message
      passing (spike 2026-11-25; full pipeline since the port)
- [x] First `tracelogger` capture (spike: 30 s, 715,569 events)
- **Week 1 exit criterion:** vitalgen → vitaldsp data flowing inside
  QEMU over QNX message passing — **met 2026-11-25** (native
  backend, supervised, autostarting).

### Day 3–4 go/no-go spike (restated)
GO requires, inside QEMU: (1) sample blocks flowing vitalgen →
vitaldsp over **native QNX message passing** (not the socket backend);
(2) a `tracelogger` trace of one full block→vitals→alarm pass;
(3) a fault-command round trip answered by vitaldsp.
NO-GO fallback: demo the host build; submit the measurement tooling as
the Developer Tooling & Host Tools entry instead.

### Week 2 (Dec 1–7) — the real system
- [x] QRS/HR/SpO₂ pipeline running on target; alarm persistence +
      latching verified on target (port, 2026-11-25 — ahead of plan)
- [ ] slogger2 audit trail; QPLMS/`slm` supervision configuration
      (mechanics; the supervisor policy already runs on target via
      the hand-written vgsupervisor + post_start autostart)
- [x] Fault-injection CLI on target; kill-DSP recovery demonstrated
      on target (2026-11-25)
- [ ] DSP warm-up guard (restart transient, see Phase 2 port)
- [ ] Dashboard stream (io-sock TCP) or target text console fallback
- **Exit criterion:** full scenario runs unattended; DSP killed →
  QPLMS restarts it → monitoring recovers with the gap logged.
  (Unattended run + kill/restart/recovery already demonstrated with
  vgsupervisor; the QPLMS mechanics swap is what remains.)

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
