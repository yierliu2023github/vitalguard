# QNX QEMU target — running

**Status: the port runs on target.** The SDP 8.0 baseline is
installed (`~/workspace/qnx/sdp/qnx800`); since the QSTI package
could not be downloaded (see STATUS.md), the target image is built
with `mkqnximage` from the SDP baseline instead.  Inside QEMU the
full pipeline runs under `vgsupervisor` on the **native QNX IPC
backend** — sample blocks, vitals, alarm events, heartbeats and
fault commands all ride MsgSend/MsgReceive — and the image
autostarts the pipeline at boot.  Evidence: SPIKE-REPORT.md and
PORT-REPORT.md in `~/workspace/qnx/custom-image/`.

## Build

```bash
source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
make -f Makefile.qnx            # -> bin-qnx/ (native backend)
make                            # host build (POSIX backend)
make test                       # host suite; must stay green
```

`Makefile.qnx` links `src/ipc/ipc_qnx.c` instead of `ipc_posix.c`;
the two backends define the same symbols, so exactly one is linked.

## The native backend (src/ipc/ipc_qnx.c)

- One channel per endpoint via `name_attach()` (global namespace,
  `/dev/name/local/<endpoint>`); clients `name_open()` and send a
  HELLO rendezvous carrying their pid + private channel id.  The
  server keys the connection by the kernel-supplied **scoid** and
  ConnectAttach()es back (server→client path).
- `vg_ipc_send()` is a synchronous MsgSend — the server thread
  inherits the sender's priority while handling it.  The backend
  pump receives, copies the message into a per-connection slot and
  MsgReply()s immediately (receive → queue → reply); a second slot
  holds one more message with its sender deliberately unreplied,
  which bounds each connection to two messages in flight
  (backpressure instead of an unbounded queue).
- Peer death arrives as `_PULSE_CODE_DISCONNECT` carrying the
  scoid; the connection drains its queue and then `vg_ipc_recv()`
  returns −1, exactly like a socket hangup on the host.
- Event loops never poll() fds: they call `vg_ipc_wait_any()`
  (declared in ipc.h), which the POSIX backend implements over
  poll() and the QNX backend implements by pumping the shared
  listener channel with TimerTimeout() armed.  All four app loops
  (vitalgen, vitaldsp, alarmgr, vgsupervisor) are written against
  it, so one source tree serves both backends.

Self-location (supervisor spawning siblings, `faultctl hog start`)
goes through `vg_self_dir()` in `src/common/vg_path.c`: `$VG_BINDIR`
override → Linux `/proc/self/exe` → argv[0]/PATH resolution.  QNX
procfs has no exe path link (`exefile` is the image, not a path —
verified against the SDP headers), so the argv[0]/PATH route is the
target mechanism.

## Image

The mkqnximage project lives in `~/workspace/qnx/custom-image/build/`.
VitalGuard additions are snippets in `build/local/snippets/`:

- `system_files.custom` — the eight binaries (six apps + spike pair)
  into `/system/bin` (verbatim copy: `system_files.custom.vitalguard`
  next to the reports).
- `post_start.custom` — autostart: `VG_BINDIR=/system/bin
  /system/bin/vgsupervisor` in the background at the end of startup
  (verbatim copy: `post_start.custom.vitalguard`).

Rebuild (snippet edits need `--clean`):

```bash
cd ~/workspace/qnx/custom-image/build
mkqnximage --type=qemu --arch=x86_64 --cpu=2 --ram=2G \
           --hostname=vitalguard --ssh-ident=none --noprompt --clean
```

Guest drivers (serial-socket Python harnesses) and console logs are
in `~/workspace/qnx/custom-image/`: `port_test.py` (supervised run,
kill/restart, injection), `port_test2.py` (clean injection +
restart-transient attribution), `port_test3.py` (autostart boot).
Note: guest `/tmp` is on the persistent data partition, so runtime
logs accumulate across boots.

## Known findings on target

- **DSP restart transient — FIXED 2026-11-25.** A vitaldsp that
  (re)joined a running stream mid-flight used to raise a brief
  spurious HIGH alarm (VFIB, APNEA or ASYSTOLE, ~1–5 s) from its
  zeroed detector state.  The DSP now resets detector state on any
  stream discontinuity and suppresses alarm raises for a 5 s
  warm-up window after a (re)join.  Host regression tests
  (`test_rejoin_warmup`, `test_discontinuity_reset`) and a two-boot
  guest run (kill → restart → no audit lines; VFib 10 s injection
  raises/clears cleanly; cold boot silent) verify the fix — see
  WARMUP-FIX-REPORT.md in `~/workspace/qnx/custom-image/`.
- The shipped sshd segfaults at startup in this image (spike
  finding); serial console is the access path.  Not on the demo's
  critical path.
- Supervision on target is still the hand-written `vgsupervisor`
  (policy proven on host and target).  The QPLMS/`slm` mapping is
  documented in docs/design.md; the image currently runs with
  `OPT_SLM=no` and starts the supervisor from post_start instead.

## Go / no-go spike (Week 1 exit criteria) — all met

- [x] vitalgen → vitaldsp sample blocks flow over **native QNX
      message passing** (not the POSIX socket backend);
- [x] `tracelogger` captures a full block→vitals→alarm trace
      (spike: 715,569 events decoded on the host);
- [x] vitaldsp answers a fault command round trip (faultctl →
      vitalgen over native IPC; supervisor kill/restart on target).

## Done since (see STATUS.md)

- On-target alarm-latency distributions — **done 2026-11-25**:
  71 fault injections + 5 supervisor-restart runs, incl. VFib under
  2× CPU-hog load and a tracelogger capture of the alarm path
  (MsgSend round trip 94 µs under load). All figures are TCG
  distributions, never hardware numbers — LATENCY-REPORT.md in this
  workspace; a copy lives in the repo as `docs/latency-report.md`.
  The HR budget miss it found was fixed the same day (report §7).
- Demo video — **recorded 2026-11-25**: `demo/vitalguard-demo.mp4`
  in the repo (scripted guest session: boot → autostart → VFib →
  latch → ack → kill DSP → restart → streaming resumes), with the
  raw serial log and scripts alongside it in `demo/`.

## Still ahead (see STATUS.md)

- QPLMS/`slm` supervision configuration replacing the hand-written
  supervisor's mechanics (policy unchanged).
- Owner steps: public GitHub repo push, demo-video upload, Devpost
  submission (internal deadline 2026-12-13).
