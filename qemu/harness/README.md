# Measurement harness — the scripts behind the latency campaign

This directory holds the Python harness that produced VitalGuard's
on-target latency numbers (see `docs/latency-report.md` and
`docs/codelab.md`). The scripts are published so the campaign is
reproducible, not just described.

**How they drive the target.** Each driver boots the QNX guest
headless under QEMU with the serial console attached to a Unix
socket, then conducts the session over that socket: it waits for
`Startup complete`, gates on the pipeline's autostart checks
(supervisor status + a clean `cond=0x0` vitals line), issues
`faultctl` commands at the guest shell prompt, and pulls the
guest's logs (`vitalgen`, `vitaldsp`, `alarmgr` audit log,
`vgsupervisor`) back for offline analysis. No SSH and no guest
agent are involved — the serial console is the whole control path.

**Provenance.** These are the exact scripts, byte for byte, that
ran the 2026-11-25 measurement campaign written up in
`LATENCY-REPORT.md` (copied into this repo as
`docs/latency-report.md`). The originals live in the target
workspace, `~/workspace/qnx/custom-image/`, alongside the raw logs
and traces they produced; the copies here were verified identical
to the originals by SHA-256 at the time of copying (2026-11-25).
They were written for that workspace's layout and carry it in
their path constants (e.g. `~/workspace/qnx/custom-image/...` for
the image build output and `logs/` directory) — if your checkout
lives elsewhere, expect to adjust those constants in your own
copies.

## The scripts

| Script | What it does |
|---|---|
| `latency_test.py` | The main campaign driver. Phase A (baseline): repeated fault injections per condition (VFib, asystole, tachycardia, bradycardia, desaturation, apnea), recording each run's fault wall-clock stamp, audit-log raise/clear stamps, and the DSP (wall, signal) anchors. Phase B: the VFib set repeated under 2× `vghog` CPU saturation, a `tracelogger` capture over one loaded injection (sent to the host via `latency_sink.py`), and 5× `faultctl kill dsp` restart repetitions. Writes `logs/latency-<phase>-runs.json` and per-process log segments. |
| `analyze_latency.py` | Offline analysis for the campaign: interpolates each fault's signal-clock onset from the DSP (wall, signal) anchor lines (`onset_signal()`), subtracts it from the audit raise stamp, and reduces each set to min / median / p95 / max (`logs/latency-stats.json`); also computes the supervisor restart timings from the wall-clock stamps. |
| `latency_sink.py` | A tiny host-side HTTP server (port 9999) that receives the guest's `curl -T` upload of the `tracelogger` capture and saves it as `latency.kev` for decoding with `traceprinter`. |
| `warmup_test.py` | Verification driver for the DSP warm-up fix: kill the DSP, let the supervisor restart it, and assert no spurious alarm raises during the rejoin window; then confirm a genuine VFib still raises and clears; plus a cold-boot run asserting no startup transient. |
| `hrfix_test.py` | Confirmation driver for the dual-path HR detection fix: a short bradycardia ×5 / tachycardia ×3 set using the same injection and recording method as `latency_test.py` phase A. |
| `analyze_hrfix.py` | The matching analyser for `hrfix_test.py` — same anchor-interpolation method as `analyze_latency.py`; writes `logs/hrfix-stats.json`. |

## A note on the numbers

All figures these scripts produce on this setup are QEMU
**TCG-emulated** distributions (the development machine has no
KVM). They demonstrate timing behaviour and budget compliance —
they are not hardware measurements. `docs/codelab.md` states this
caveat in full and walks through the whole campaign step by step.
