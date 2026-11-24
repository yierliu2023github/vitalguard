# QNX QEMU target — in progress

**Status: blocked on the QNX SDP 8.0 installation.** The free QNX
Everywhere licence must be accepted by the account holder under their
own myQNX account (qnx.com/getqnx); once the SDP is installed this
folder gets the target build and the QEMU launch is completed and
verified. Nothing here has been run against a real SDP yet — this file
describes the plan and the exact steps, not results.

## Planned path

1. **Licence + SDP.** Accept the free non-commercial QNX Everywhere
   licence (myQNX), install QNX SDP 8.0 (~6.4 GB) on an Ubuntu x86-64
   host.
2. **Target image.** Use the *Quick Start Target Image for QEMU*
   (QSTI) shipped with the SDP as the base; later in the project,
   produce a single custom image with `mkqnximage` (CTI) containing
   the VitalGuard binaries and the QPLMS configuration.
3. **Build.** Cross-compile with the SDP's `qcc` (target
   `gcc_ntox86_64`). The codebase is C11/POSIX-subset; the only
   target-specific file is `src/ipc/ipc_qnx.c` (native message passing
   backend, compiled under `__QNX__`). A `Makefile.qnx` will sit next
   to the host Makefile rather than complicating it.
4. **Run.** `run-qemu.sh` (below) boots the image under QEMU with the
   VitalGuard processes started by QPLMS/`slm` in dependency order
   (alarmgr → vitaldsp → vitalgen).
5. **Measure.** Capture alarm-latency distributions with the
   instrumented kernel + `tracelogger` (≥ 30 VFib injections, report
   median / p99 / max), including under CPU-hog load and across a DSP
   kill/restart. These numbers go into the Devpost submission and the
   demo video; QEMU timings are reported as distributions under load,
   never as absolute hardware guarantees.

## Go / no-go spike (Week 1 exit criteria)

The port is considered real when, inside QEMU:

- [ ] vitalgen → vitaldsp sample blocks flow over **native QNX message
      passing** (not the POSIX socket backend);
- [ ] `tracelogger` captures a full block→vitals→alarm trace;
- [ ] vitaldsp answers a fault command round trip.

If the spike fails, the fallback is to demo the host build and submit
the measurement tooling as the Developer Tooling entry (see the project
STATUS.md).
