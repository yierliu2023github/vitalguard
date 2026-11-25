# Measure Your Alarm Latency on QNX 8.0

*A hands-on codelab: build a QNX 8.0 target image, run a real
multi-process alarm pipeline on it, inject faults, and measure —
with distributions, not anecdotes — how long the system takes to
annunciate an alarm, under CPU load and across a process restart.*

This is the measurement method behind VitalGuard, a patient-monitor
alarm core (synthetic vital-sign generation → DSP detection →
IEC 60601-1-8-style alarm management → supervision) built for the
QNX Global Hackathon. You will reproduce its on-target latency
campaign end to end: the same image recipe, the same fault-injection
driver, the same onset-interpolation analysis, and the same kernel
trace of the alarm path.

**Audience.** You are a competent C developer. No prior QNX
experience is assumed; the QNX-specific mechanisms are introduced
where they are used.

**What you will do**

1. Set up the workspace and read the scope caveats (this step).
2. Build a bootable QNX 8.0 QEMU image with `mkqnximage`, with the
   VitalGuard binaries baked in and the pipeline set to autostart.
3. Build VitalGuard for host and target, boot the image, and confirm
   the supervised pipeline comes up on its own.
4. Drive the running monitor with `faultctl`: inject ventricular
   fibrillation, watch the alarm raise, latch, and clear in the
   audit log.
5. Run the scripted measurement campaign and compute latency
   distributions (min / median / p95 / max) against a budget table.
6. Repeat the headline measurement with the CPUs saturated.
7. Capture a kernel trace with `tracelogger` and read the actual
   `MsgSend` chain of an alarm event out of it.
8. Kill the DSP process and time the supervisor's recovery.
9. Study two real defects the campaign found — a restart-transient
   false alarm and a bradycardia budget miss — and their fixes.
10. Exercises to extend the method.

**Where things live.** This codelab uses the project's real layout:

| Path | What is there |
|---|---|
| `~/workspace/vitalguard/` | The source repo (also on GitHub: https://github.com/yierliu2023github/vitalguard) |
| `~/workspace/qnx/sdp/qnx800/` | The installed QNX SDP 8.0 (`qcc`, `mkqnximage`, `traceprinter`) |
| `~/workspace/qnx/custom-image/` | The target workspace: `build/` (the mkqnximage project), the Python measurement harness (`latency_test.py`, `analyze_latency.py`, `hrfix_test.py`, `latency_sink.py`), the reports, and `logs/` with the raw data |

If your checkout lives elsewhere, adjust the paths — every command
below is written against this layout. Design background is in
`docs/design.md`; the full measurement report this codelab follows
is `docs/latency-report.md` in the repo (raw data and scripts in the
target workspace above).

---

## Step 1 — Prerequisites and honest scope

### You need

- **QNX SDP 8.0 installed, with a licence deployed.** VitalGuard was
  built against SDP 8.0 with a non-commercial single-user licence
  obtained through myQNX. The SDP provides the cross-compiler
  (`qcc`), the image builder (`mkqnximage`) and the trace decoder
  (`traceprinter`). Every SDP command below assumes you have first
  sourced the environment script:

  ```bash
  source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
  ```

- **Host tools:** `gcc` and `make` (host build and test suite),
  `python3` (the measurement drivers and analysis), and
  `qemu-system-x86_64` (the target runs as a QEMU guest).

- **The VitalGuard repo**, checked out at `~/workspace/vitalguard`.

### The emulation caveat — read this before quoting any number

> **⚠️ The target in this codelab is a QEMU guest running under TCG
> software emulation — the development machine has no KVM.** Every
> timing you will measure is an *emulated distribution*, not a
> hardware measurement. The pipeline is real-time-paced (100 ms
> sample blocks on a signal clock), so guest clocks track host real
> time closely, but CPU-bound work runs far slower than on any real
> target and scheduling jitter is larger.
>
> What the method demonstrates is *timing behaviour and budget
> compliance in emulation*: that the alarm latency is dominated by
> the designed detection and persistence windows, that it holds
> under CPU saturation, and that it survives a process kill and
> restart. Never present these figures as hardware numbers. The
> original report (`docs/latency-report.md`) carries this same
> caveat at its head, and so should anything you publish from your
> own runs.

A second scope note: VitalGuard is a research/education prototype
running on synthetic signals. It is not a medical device.

**Why this matters before you start:** latency claims are only as
good as the clock discipline and the honesty about the environment
behind them. This codelab will keep two clocks carefully separate
(signal time vs. guest wall time, Step 5) and will report
distributions with sample sizes, never single runs.

---

## Step 2 — Build the target image with `mkqnximage`

### Why this route exists

The usual way to get a QNX QEMU target is the QNX Software Center's
quick-start package (QSTI). In this project's environment that
package could not be installed: early attempts died on a full root
filesystem (the Software Center cache lives on a small overlay
disk), and later attempts failed within seconds in the server-side
authorization/signed-URL step, while the same endpoint answered a
plain unauthenticated request normally. Rather than keep retrying a
blocked path, the project bypassed it: with the SDP *baseline*
installed, `mkqnximage` — the SDP's own image builder — composes a
bootable QEMU image from stock parts. That is the recipe below. It
needs no downloads and no credentials beyond the SDP licence you
already have, and it leaves you with an image you understand piece
by piece — which pays off in Step 7, where the instrumented kernel
in this image is what makes kernel tracing possible.

### 2.1 Build the target binaries first

The image recipe copies the cross-compiled binaries out of the
repo's `bin-qnx/` directory, so build them now (full build
instructions are in Step 3; this is the same command):

```bash
cd ~/workspace/vitalguard
source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
make -f Makefile.qnx        # -> bin-qnx/
```

### 2.2 Create the image project and build

```bash
mkdir -p ~/workspace/qnx/custom-image/build
cd ~/workspace/qnx/custom-image/build
source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
mkqnximage --type=qemu --arch=x86_64 --cpu=2 --ram=2G \
           --hostname=vitalguard --ssh-ident=none --noprompt --verbose
```

What this does: `mkqnximage` lays down a project directory —
`local/` (recorded options in `local/options`, customisation points
in `local/snippets/`) and `output/` (the artifacts) — and composes
the image from the SDP's built-in **standard** profile: the
instrumented kernel (`procnto-smp-instr`), a serial console running
`ksh`, core utilities, and io-sock networking with DHCP. Two options
matter for later steps: `--cpu=2 --ram=2G` gives the guest the shape
the measurements were taken on, and the instrumented kernel is what
`tracelogger` will need in Step 7.

You should see, in `output/`:

| Artifact | Size (this project) |
|---|---|
| `output/ifs.bin` — the boot image filesystem, passed to QEMU via `-kernel` | ≈ 10.4 MiB |
| `output/disk-qemu` — the raw disk image (system + data partitions) | ≈ 150 MiB |

### 2.3 Bake VitalGuard into the image

`mkqnximage` projects are customised through snippet files in
`local/snippets/`. VitalGuard adds two. Their verbatim sources live
next to the reports in the target workspace, so copy them in:

```bash
cd ~/workspace/qnx/custom-image
cp system_files.custom.vitalguard build/local/snippets/system_files.custom
cp post_start.custom.vitalguard  build/local/snippets/post_start.custom
```

What each snippet does:

- **`system_files.custom`** installs eight binaries into the system
  partition's `bin/` (on the guest's `PATH` as `/system/bin`): the
  six VitalGuard programs (`vitalgen`, `vitaldsp`, `alarmgr`,
  `vgsupervisor`, `faultctl`, `vghog`) plus the message-passing
  spike pair (`msgserver`, `msgclient`). The entries carry absolute
  host paths into the repo's `bin-qnx/` — **edit them to point at
  your checkout** if it is not `~/workspace/vitalguard`. No extra
  library entries are needed: libc/libm/libsocket are already in the
  boot image.

- **`post_start.custom`** runs at the end of system startup and is
  what makes the pipeline autostart:

  ```sh
  mkdir -p /tmp/vitalguard
  VG_BINDIR=/system/bin /system/bin/vgsupervisor >> /tmp/vitalguard/vgsupervisor.out 2>&1 &
  ```

  The runtime directory is created here because the shell
  redirection needs it before the supervisor starts. `VG_BINDIR`
  pins the supervisor's sibling-binary lookup to `/system/bin` —
  necessary because QNX procfs has no `/proc/self/exe` path link to
  discover it from (the repo's `src/common/vg_path.c` documents the
  fallback chain).

### 2.4 Rebuild with the snippets

Snippet edits only take effect on a clean rebuild:

```bash
cd ~/workspace/qnx/custom-image/build
mkqnximage --type=qemu --arch=x86_64 --cpu=2 --ram=2G \
           --hostname=vitalguard --ssh-ident=none --noprompt --clean
```

**Why a whole image, instead of copying binaries into a running
guest?** Because the measurement target should be one reproducible
artifact: anyone who builds this image gets the same kernel, the
same startup script and the same binaries, and the pipeline's
autostart behaviour (Step 3) is part of what is being tested.

---

## Step 3 — Build VitalGuard, boot the image, confirm autostart

### 3.1 Build for host and target

```bash
cd ~/workspace/vitalguard
make                 # host build (POSIX IPC backend) -> bin/
make test            # host suite — must be green before you measure anything
source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
make -f Makefile.qnx # target build (native QNX IPC backend) -> bin-qnx/
```

The two builds compile the same source tree. The only difference
that matters here: the host Makefile links `src/ipc/ipc_posix.c`
(Unix-domain sockets), the target Makefile links
`src/ipc/ipc_qnx.c` (native QNX message passing —
`name_attach()`/`name_open()` rendezvous, `MsgSend`/`MsgReceive`/
`MsgReply`). Exactly one backend is linked per build, behind the
single `ipc.h` interface. If you rebuilt the target binaries, go
back to Step 2.4 and rebuild the image so it picks them up.

### 3.2 Boot

From the repo root:

```bash
qemu/run-qemu.sh
```

The script runs QEMU with the serial console on stdio
(`-nographic`); exit QEMU with **Ctrl-A x**. It looks for the image
in `~/workspace/qnx/custom-image/build/output` (override with
`VG_IMAGE_DIR`).

You should see the QNX startup run and finish with `Startup
complete` — about 8 s under TCG emulation — leaving you at a root
shell (`#` prompt) on the serial console. Because of the
`post_start` snippet, you did **not** start anything by hand: the
supervisor and the whole pipeline are already running.

### 3.3 Confirm the pipeline is up

All runtime state lives in `/tmp/vitalguard/` in the guest. Check
the supervisor's status file:

```sh
cat /tmp/vitalguard/supervisor.status
```

You should see one `state=running` line per child (pids and
heartbeat ages will differ):

```
alarmgr    pid=299029 restarts=0 hb_age_ms=199 state=running
vitaldsp   pid=303127 restarts=0 hb_age_ms=200 state=running
vitalgen   pid=323608 restarts=0 hb_age_ms=0   state=running
```

Then confirm the DSP is actually streaming vitals:

```sh
tail /tmp/vitalguard/vitaldsp.log
```

```
[13135] vitaldsp: t=5s HR=71 SpO2=98.0 RR=-1 cond=0x0
```

Read this line carefully — it is the measurement primitive for the
whole codelab. The bracketed number is the process's **guest
wall-clock** stamp (milliseconds, CLOCK_MONOTONIC); `t=5s` is the
**signal clock** (the generator stamps every sample block, and the
stamp propagates downstream). `cond=0x0` means no alarm condition is
currently raw. The DSP logs a line like this roughly every 5 s of
signal time — those (wall, signal) pairs are the *anchors* Step 5
uses to convert between the two clocks.

Two more orientation checks:

```sh
ls /dev/name/local        # the four native-IPC endpoints: vitalgen, vitaldsp, alarmgr, vgsupervisor
pidin                     # the process table; note procnto-smp-instr at the top
```

**Why autostart is part of the test:** a monitor that needs a human
to start it after a power cycle is a different (worse) product. The
latency campaign in Step 5 boots this image fresh for every phase
and gates on exactly the checks you just did — three
`state=running` lines plus a clean `cond=0x0` vitals line — before
it injects anything.

---

## Step 4 — Drive the system with `faultctl`

`faultctl` is the fault-injection CLI. It talks to the pipeline over
the same native message passing as everything else (a
`VG_MSG_FAULT_CMD` message to `vitalgen`), so what you are about to
do exercises the real control path, not a debug backdoor. Its full
command set:

```
faultctl vfib [secs]
faultctl rhythm <normal|tachy|brady|afib|vtach|vfib|asystole> [secs]
faultctl desat <pct> [secs]
faultctl apnea [secs]
faultctl hog start|stop
faultctl kill dsp
faultctl ack <alarm-id>
faultctl silence [alarm-id]
faultctl status
```

### 4.1 Inject a VFib and read the audit log

From the guest shell:

```sh
faultctl vfib 10
cat /tmp/vitalguard/alarm_events.log
```

The audit log is the alarm manager's append-only record of every
state transition, stamped in **signal time** (milliseconds). A
10-second VFib injection produces the lifecycle of alarm #1
(values from the recorded demo session):

```
t=9799 alarm #1 RAISED cond=VFIB prio=HIGH value=71.9
...
t=26500 alarm #1 ACKED cond=VFIB prio=HIGH value=71.9
```

With a 15 s injection (the campaign's setting) you see the full
latch sequence:

```
t=26400 alarm #1 RAISED cond=VFIB prio=HIGH value=71.9
t=41399 alarm #1 CONDITION_CLEARED_LATCHED cond=VFIB prio=HIGH value=71.9
t=43499 alarm #1 ACKED cond=VFIB prio=HIGH value=71.9
t=43499 alarm #1 CLEARED cond=VFIB prio=HIGH value=71.9
```

Read it as the IEC 60601-1-8-inspired semantics the alarm manager
implements:

- **RAISED** — the condition persisted for its configured delay and
  annunciated. VFib's persistence is 0 s (detection time *is* the
  delay), so the raise follows detection closely: onset→raise here
  is the ≈1.5–2 s the detector needs.
- **CONDITION_CLEARED_LATCHED** — the rhythm recovered (note
  41399 − 26400 ≈ 15.0 s, the injection length), but because VFib
  is **HIGH** priority, the alarm *latches*: it stays annunciated
  until a human acknowledges it. A self-terminating VFib still
  happened, and someone must see it.
- **ACKED / CLEARED** — produced by `faultctl ack 1`. Only then does
  the alarm instance close.

Try it yourself: inject again, watch the raise, then acknowledge:

```sh
faultctl vfib 15
# wait for the RAISED line to appear in the audit log, then:
faultctl ack <id>
```

### 4.2 The ack protocol matters — learn it now

A latched, unacknowledged HIGH alarm **suppresses later raises of
the same condition**. If you inject VFib three times and only ack
at the end, the audit log shows one raise, not three. This is
correct monitor behaviour — and it will silently corrupt a
measurement campaign that doesn't respect it. The driver in Step 5
acks after every HIGH injection for exactly this reason (and
Step 9 shows what happened to the first apnea set when this was
missed).

### 4.3 The other injectors

These are the exact command lines the campaign uses:

```sh
faultctl rhythm asystole 15    # ASYSTOLE (HIGH)
faultctl rhythm tachy 20       # HR_HIGH: rate step to ~132 bpm
faultctl rhythm brady 30       # HR_LOW: rate step to 38 bpm
faultctl desat 84 45           # SPO2_LOW: SpO2 step to 84 %
faultctl apnea 25              # APNEA (HIGH): no breaths for 25 s
```

While an injection runs, watch `tail /tmp/vitalguard/vitaldsp.log`:
the `cond=` bitmask shows conditions going raw in real time, and
the reported HR/SpO₂/RR values show the detectors tracking the
injected physiology. The generator is deterministic (a fixed-seed
PRNG), so the same command produces the same signal on every run —
a precondition for comparing runs at all.

**Why inject through the generator?** Because then the *entire*
chain — signal synthesis, DSP detection, persistence, alarm
management, audit — is inside the measurement. Injecting at the
alarm manager would measure a fraction of the path and call it the
whole.

---

## Step 5 — The measurement method

Single runs prove nothing about a real-time system. The campaign
measures each condition ~10 times and reports a distribution. This
step explains the method precisely, because the method *is* the
deliverable: the numbers at the end are only meaningful if you
understand exactly what was timed.

### 5.1 What "latency" means here

**Alarm latency = signal-clock onset → signal-clock raise.**

- *Onset:* when the injected fault takes effect in the signal.
- *Raise:* the `t=` stamp of the `RAISED` line in the audit log.

Both ends in signal time, because signal time is the clock the
detectors and persistence timers actually run on.

### 5.2 The onset problem, and anchor interpolation

There is a catch. The moment you *issue* the fault is known in
guest wall time: `vitalgen` logs it with its wall-clock stamp —

```
[33682] vitalgen: fault: rhythm -> 5 for 15.0 s
```

— but the audit log's raise is stamped in signal time. Subtracting
across two clock domains is meaningless until you convert.

The conversion uses the DSP's vitals lines as anchors. Each line
pairs a wall stamp with a signal stamp (`[38491] vitaldsp: t=30s
...`), and they arrive every ≈ 5000 ms of signal time. To find the
signal time of the fault:

1. Find the two anchors that bracket the fault's wall stamp.
2. Linearly interpolate between them.

That is exactly what `analyze_latency.py`'s `onset_signal()` does.
With ≈ 5 s anchor spacing and a signal clock that is paced in real
time (100 ms blocks), interpolation error is small.

**One-sided bias, stated plainly:** the reported latency is an
*upper bound*. The fault takes effect at a sample-block boundary
*after* the command arrives, so the true onset is slightly later
than the interpolated one, and the true latency may be up to
~0.3 s lower than reported. For most conditions this is noise
against multi-second windows; it matters only for a borderline
case, and Step 9 flags the one place it did.

### 5.3 The driver

`latency_test.py` (in the target workspace) automates the whole
campaign. It boots the image headless with the serial console on a
Unix socket, waits for `Startup complete`, gates on the Step 3
checks (three `state=running` lines, a clean `cond=0x0` vitals
line, plus a settle period well past any post-start transient),
then for each run it:

1. records the audit-log offset and counts pre-existing raises of
   the condition;
2. issues the `faultctl` command over the serial console;
3. reads the fault's wall stamp back from `vitalgen`'s log;
4. polls the audit log for this run's `RAISED`, then its clear;
5. acks the alarm if it is HIGH (the Step 4.2 protocol);
6. lets the signal settle clean before the next run.

Raw per-run records land in `logs/latency-<phase>-runs.json`, and
the guest log segments in `logs/latency-<phase>-{audit,gen,dsp,sup}.log`.

Run the baseline campaign (phase A, 10 runs per condition):

```bash
cd ~/workspace/qnx/custom-image
python3 latency_test.py A 10
python3 analyze_latency.py     # -> logs/latency-stats.json + a per-run table
```

The analyser recomputes each run's interpolated onset from the DSP
segment, subtracts it from the raise stamp, and reduces each set
with `stats()`: sorted values → min, median, p95 (linear
interpolation on the (n−1)·p rank), max, mean.

### 5.4 The budget, and the baseline results

A measurement without a budget is just a number. VitalGuard's
budgets come from `docs/design.md` §2 — each is the condition's
designed detection + persistence window plus transport and
processing margin:

| Condition | Budget (onset → raise) | Why |
|---|---|---|
| VFib | ≤ 4 s | detection ≤ 3 s + transport ≤ 50 ms + alarm-manager ≤ 10 ms + margin |
| HR_HIGH / HR_LOW | ≤ 8 s | 5 s persistence + detection |
| SpO₂ drop | ≤ 24 s | 10 s persistence + up to ~8 s averaging lag |
| Apnea | ≤ 12 s | missed-breath detection + 5 s persistence |
| Asystole | — (no explicit line) | 3.5 s persistence by construction |

The baseline campaign measured (signal-clock ms, onset → raise):

| Condition (injection) | n | min | median | p95 | max | Budget | Verdict |
|---|---|---|---|---|---|---|---|
| VFib (`vfib 15`) | 10 | 1209 | 1664 | 2231 | 2298 | ≤ 4000 | ✅ within |
| Asystole (`rhythm asystole 15`) | 10 | 3513 | 3828 | 4188 | 4205 | — | ✅ by construction |
| Tachycardia HR_HIGH (`rhythm tachy 20`) | 10 | 7610 | 7846 | 8031 | 8062 | ≤ 8000 | ⚠️ borderline |
| Bradycardia HR_LOW (`rhythm brady 30`) | 10 | 13076 | 14140 | 14541 | 14542 | ≤ 8000 | ❌ **miss** |
| SpO₂ drop (`desat 84 45`) | 10 | 17102 | 17176 | 17200 | 17212 | ≤ 24000 | ✅ within |
| Apnea (`apnea 25`) | 10 | 6815 | 8923 | 10997 | 11393 | ≤ 12000 | ✅ within |

Three observations worth making before you move on:

- **The medians are dominated by design, not compute.** Where a
  Phase 1 host measurement exists, the emulated target tracks it
  closely (VFib 1664 vs 1499 ms; asystole 3828 vs 3799; SpO₂ 17176
  vs 16900). The latency *is* the persistence window, by intention.
- **Apnea's spread (6.8–11.4 s) is structural:** detection waits for
  the first missed breath, and the last pre-onset breath can precede
  onset by up to one full respiratory period (≈ 4.6 s at 13
  breaths/min) on top of the persistence.
- **Two rows are not passes.** HR_HIGH's tail exceeds its budget by
  up to 62 ms (within the method's ~0.3 s one-sided bias, so
  reported as *borderline / not proven*, not as a pass), and HR_LOW
  misses outright. Both are dissected — and fixed — in Step 9. The
  post-fix confirmation numbers are:

  | Set | n | min | median | max | Budget |
  |---|---|---|---|---|---|
  | Bradycardia HR_LOW (`rhythm brady 30`) | 5 | 6585 | 7269 | 7693 | ≤ 8000 |
  | Tachycardia HR_HIGH (`rhythm tachy 20`) | 3 | 5988 | 6165 | 6203 | ≤ 8000 |

**Why distributions, why ~10 runs:** a real-time claim is a claim
about the tail, not the average. One run gives you an anecdote;
ten give you a median and a max you can compare against a budget —
and, just as importantly, a method you can re-run after a change
and diff.

---

## Step 6 — Under-load measurement: does the latency hold?

A monitor does not get to choose a quiet moment to have its
emergency. Phase B of the driver repeats the VFib set while both
guest CPUs are saturated by `vghog` — a busy-loop spinner that runs
at nice 10, i.e. *below* the pipeline in scheduling priority.

```bash
cd ~/workspace/qnx/custom-image
python3 latency_test.py B 10
python3 analyze_latency.py
```

Inside the guest, phase B does the equivalent of:

```sh
faultctl hog start                       # one hog, via the supervisor's fault path
vghog >> /tmp/vitalguard/hog2.log 2>&1 & # a second hog: one per vCPU
# ... verify streaming (cond=0x0 in vitaldsp.log), then run the VFib set ...
faultctl hog stop
slay vghog
```

Measured (VFib, n = 10 each):

| Set | min | median | p95 | max |
|---|---|---|---|---|
| Baseline | 1209 | 1664 | 2231 | 2298 |
| With 2× vghog | 1780 | 2009 | 2277 | 2332 |

The latency holds: the median rises ~345 ms and the max only
34 ms, and every run stays well inside the ≤ 4 s budget.

**Why it holds:** two mechanisms compound. First, fixed-priority
preemptive scheduling keeps the alarm path above the hogs. Second —
and this is the deeper reason — the end-to-end latency is mostly
*persistence and detection windows measured in signal time*, not
CPU work; saturating the CPU adds jitter to the small compute
fraction, not to the designed windows. Step 7 puts a number on
that compute fraction.

---

## Step 7 — Reading the alarm path in a kernel trace

The logs tell you *when* things happened at 100 ms block
granularity. The kernel trace tells you what the message passing
actually cost, event by event. This is where the instrumented
kernel from Step 2 earns its keep.

### 7.1 Capture

In the guest, start `tracelogger` in the background, then inject
(phase B does this over one loaded VFib run):

```sh
tracelogger -f /tmp/latency.kev -s 45 > /tmp/tracelogger.log 2>&1 &
faultctl vfib 15
```

`-s 45` captures 45 seconds. The resulting file is
`/tmp/latency.kev` (5,064,416 bytes for the campaign's capture).

### 7.2 Get the `.kev` off the guest

The image has io-sock networking and `curl`; QEMU user-mode
networking maps the host to `10.0.2.2`. Start the sink on the host
first:

```bash
cd ~/workspace/qnx/custom-image
python3 latency_sink.py    # HTTP server on :9999; saves the PUT body to latency.kev
```

Then, in the guest:

```sh
curl -s -T /tmp/latency.kev http://10.0.2.2:9999/latency.kev
```

### 7.3 Decode

On the host, with the SDP environment sourced:

```bash
source ~/workspace/qnx/sdp/qnx800/qnxsdp-env.sh
traceprinter -f latency.kev -o logs/latency-trace.txt
```

(Note: `traceprinter` takes the input via `-f`, not positionally.)
The campaign's 45 s capture decodes to 322,604 lines of events —
thread state changes, kernel calls, and communication events, all
stamped in kernel cycles (`TRACE_CYCLES_PER_SEC = 1,491,988,175`
converts cycles to time).

### 7.4 Find the alarm chain

You are looking for one message: the `VG_MSG_ALARM_EVENT` that
`vitaldsp` sends to `alarmgr` when the VFib raises. Orientation
facts, all from the trace header and the wire definitions in
`src/common/vg.h`:

- Message types: `1` = SAMPLE_BLOCK (gen→dsp), `2` = VITALS
  (dsp→alarmgr), `3` = ALARM_EVENT (dsp→alarmgr), `4` = HEARTBEAT
  (→supervisor), `5` = FAULT_CMD (faultctl→vitalgen).
- COMM events name the **receiving** pid. Map pids to processes
  with a `pidin` snapshot taken during the capture window (the
  driver saves one as `logs/latency-b-pidin.txt`).
- In the campaign capture: vitalgen pid 413714, vitaldsp pid
  364569, alarmgr pid 327702; the two hogs (pids 671759, 679938)
  appear `THRUNNING`/`THREADY` throughout the raise window — the
  chain below executed under full CPU contention.

The worked example — the raise of the VFib injected under load,
timed relative to the DSP's send call:

| Step | Event | Δ from send |
|---|---|---|
| 1 | vitaldsp `KER_CALL MSG_SENDV`, msg type 3 (ALARM_EVENT) | 0 µs |
| 2 | `SND_MESSAGE` delivered to alarmgr | +12.3 µs |
| 3 | vitaldsp `THREPLY` (blocked awaiting reply) | +26.5 µs |
| 4 | alarmgr `THRUNNING` | +29.1 µs |
| 5 | alarmgr `REC_MESSAGE` / receive exit, rmsg type 3 | +55.5 / +64.1 µs |
| 6 | alarmgr `KER_CALL MSG_REPLYV` | +82.1 µs |
| 7 | `REPLY_MESSAGE` → vitaldsp `THREADY` — round trip complete | **+94.3 µs** |
| 8 | alarmgr audit-log write: `MSG_SENDV` → filesystem | +378.3 µs |

Cross-checks inside the same trace, so you know you read the right
window: the fault command's send → the alarm event's send is
**1.465 s** (kernel cycles) — consistent with the log-based VFib
distribution (the definitions differ slightly: kernel timing starts
at the command's send, log timing at the interpolated onset); the
raise send → clear send is **15.251 s** for the 15.0 s injection;
and exactly 2 type-3 sends occur in the window (raise + clear), as
expected.

**The takeaway, and the reason for this step:** the complete
`MsgSend` round trip for the alarm event costs ~94 µs under load —
roughly 0.005 % of the ~2 s end-to-end latency. The budget is spent
in the designed detection and persistence windows, which is where a
monitor *should* spend it. When someone claims a message-passing
design is "fast" or "slow", this is how you check: not by
benchmarking the IPC in isolation, but by pricing its share of the
end-to-end path, in a trace, under load.

---

## Step 8 — Supervisor restart timing

The last campaign measurement: how long is the monitor blind when
a component dies? Phase B kills the DSP five times (~21 s apart,
streaming verified after each):

```sh
faultctl kill dsp
```

You can do this by hand in the guest: watch
`/tmp/vitalguard/supervisor.status` — `vitaldsp`'s `restarts=`
counter increments and a new pid appears — and watch
`vitaldsp.log` resume. For numbers, the analyser's
`restart_analysis()` works offline from the wall-clock stamps in
`vgsupervisor.out` (the "fault: killing vitaldsp" and "started
vitaldsp pid=" lines) and `vitaldsp.log` (the "vitalgen connected"
line and the first vitals anchor after it):

| Kill # | kill → respawn (ms) | kill → vitalgen reconnected (ms) | kill → first vitals from new DSP (ms) |
|---|---|---|---|
| 1 | 673 | 783 | 790 |
| 2 | 1020 | 1127 | 1135 |
| 3 | 2027 | 2087 | 2093 |
| 4 | 2025 | 2077 | 2083 |
| 5 | 2038 | 2107 | 2114 |

**Range: 0.78–2.11 s from kill to streaming resumed.**

The growth across kills is the supervisor's *designed* backoff —
0.5 s doubling toward a 4 s cap, with a restart budget of 10 per
60 s — not degradation: detection + spawn is only ~0.17–0.5 s on
top of the current backoff delay. And "first vitals" follows
reconnection by < 10 ms in every run, because the restarted DSP
logs a vitals line on its first processed block.

Note the clock discipline difference from Step 5: restart timing is
a **guest wall-clock** measurement (CLOCK_MONOTONIC stamps in the
supervisor and DSP logs), because process death and respawn are
wall-time events. Alarm latency was signal-clock; restart latency
is wall-clock; the report keeps them in separate tables, and so
should you.

**Why the backoff shape matters:** a supervisor that respawns
instantly forever turns a crashing component into a fork bomb with
extra steps; one that gives up too early turns a transient fault
into an outage. The budget-plus-backoff policy (10 restarts per
60 s, 0.5 s → 4 s) is the middle path — and the restart *after* a
kill is exactly where the next step's first pitfall lives.

---

## Step 9 — Pitfalls: two defects the campaign found (and one it nearly caused)

A measurement campaign that only confirms what you believed is a
ceremony. This one found two real defects and invalidated one of
its own data sets. All three are lessons in how timing bugs hide.

### 9.1 The restarted DSP that cried VFib (warm-up transient)

**Symptom.** After the QNX port, killing `vitaldsp` produced a
brief spurious **HIGH** alarm — VFib, apnea or asystole, ~1–5 s —
almost every time the supervisor restarted it. A cold boot never
did it. Host code review had not caught it, because it only exists
in the running system's timing: a detector rejoining a *live*
stream mid-flight.

**Root cause.** The freshly started DSP judged the stream against
*absolute* stream time, with zeroed detector state:

- `no_beat` fell back to `now` when no beat had been seen yet, so a
  DSP joining at stream t = 60 s was instantly "60 s without a
  beat" — VFib raw the moment the 1 s RMS window filled with real
  signal, asystole raw while the zero-padded RMS ring still read
  flat. Both conditions have 0 s persistence, so they raised
  immediately.
- Apnea compared `now` against `last_breath_ms = 0`, so it went raw
  at once and raised after its 5 s persistence.
- An existing 3 s warm-up gate also keyed on absolute `now`, so it
  never engaged on a rejoin at all.
- Persistence timers consumed block timestamps directly, so the t0
  jump across the outage counted as condition time.

**Fix** (`src/dsp/dsp.c` only; no thresholds, persistence values or
alarm-manager rules changed):

- **Warm-up guard.** Alarm raises are suppressed until the DSP has
  consumed `WARMUP_GUARD_MS = 5000` ms of *contiguous samples*
  after a (re)join. The window is sized from what the detectors
  actually need: 1.0 s for the VFib/asystole RMS window (250
  samples at 250 Hz), 3.0 s for ≥ 2 RR intervals at 40 bpm, 5.0 s
  for one full breath cycle at 12/min. (The 8 s SpO₂ ring is
  deliberately *not* waited out — SPO2_LOW's own 10 s persistence
  on real averaged samples already exceeds the guard.) Persistence
  timers keep running during warm-up on the freshly reset state,
  so the guard only ever binds for the zero-persistence conditions
  — VFib and asystole, exactly the ones that misfired.
- **Discontinuity reset.** Every block's t0 is checked against the
  previous block's: a break — t0 going backwards, a duplicate, or a
  gap > 1.5 blocks — resets all detector state and re-anchors
  beat/breath timing at the join, so "no beat" is measured from the
  join, never from stream zero. One refinement the test suite
  forced: the generator stamps t0 from a double clock with ±1 ms
  jitter (measured 3 of 399 blocks at 99/101 ms), so the check
  tolerates ±half a block — the first, strict-equality version
  reset the DSP on jitter steps and visibly regressed the SpO₂
  timing until the existing tests caught it.

**Verification.** Host regression tests `test_rejoin_warmup` (join
a 60 s-old stream → zero raises in 12 s; a genuine VFib afterwards
still raises) and `test_discontinuity_reset` (+30 s t0 jump → zero
raises), plus a two-boot guest run: kill → supervisor restart →
20 s of rejoin observation with a byte-identical audit log, and a
clean cold boot.

**The trade-off, stated plainly.** A genuine VFib or asystole whose
onset falls *inside* the 5 s window after a (re)join is announced
at the window boundary instead of its usual mark — worst-case added
delay ≈ 3 s (VFib) / ≈ 1 s (asystole), only for onsets within 5 s
of a rejoin. Every other condition is unaffected: their earliest
possible raises are already past the guard.

**Lessons.**

- A detector that (re)joins a running stream must reason in
  *stream-relative* time and earn the right to alarm. "Time since
  the epoch of the stream" is not "time since I have been
  watching".
- Gate every measurement set on a streaming check, and never
  measure inside a post-restart warm-up window — the campaign
  verified no run was contaminated, and annotated the first 5 s
  after any DSP restart as guard-covered by design.

### 9.2 The bradycardia budget miss (the persistence clock started late)

**Symptom.** Baseline campaign: HR_LOW median **14140 ms**, max
14542 ms, against a ≤ 8000 ms budget — a miss by nearly a factor of
two. HR_HIGH was borderline alongside it (median 7846, max
8062 ms).

**Root cause — a design property, not an emulation artefact.**
Reported heart rate is the median of the last 8 RR intervals.
After a step change to 38 bpm, the ring needs ~5 new beats before
the median crosses the 50 bpm limit — at 38 bpm that is ≈ 7.9 s —
and *only then* does the 5 s persistence start. Total ≈ 13–14.5 s,
exactly as measured. The budget had been asserted ("5 s persistence
+ detection ≤ 8 s") without ever deriving it from the estimator's
dynamics at the worst-case rate; Phase 1 had never measured
bradycardia latency on the host either, so nothing had ever
falsified it. The design doc's ≤ 8 s claim was simply not
achievable as designed, and the honest options were to fix the
design or restate the budget. The project fixed the design.

**Fix — dual-path detection** (`src/dsp/dsp.c` only; thresholds and
the 5 s persistence untouched). An HR condition is now raw when:

- the 8-beat median rate is beyond the limit (the existing path —
  still the *reported* HR), **or**
- the most recent completed RR interval is beyond the same limit
  (RR > 1.5 s for HR_LOW at the 40 bpm limit; RR < 0.5 s for
  HR_HIGH at 120 bpm), **or** (HR_LOW only) the *in-progress*
  interval: the gap since the last beat exceeds 1.5 s — capped by
  the same 3 s staleness rule the median path uses, so a no-beat
  stream (VFib/asystole) holds that raw for at most 1.5 s and can
  never raise HR_LOW from it.

The persistence clock now starts at the first abnormal interval
instead of after ~5 beats. The false-alarm contract is unchanged:
persistence still requires raw to hold *continuously* for 5 s, and
one abnormal interval holds raw only until the next beat, so an
isolated ectopic beat or a single missed detection cannot raise —
verified by the new host test `test_transient_hr_no_raise` and by
old-vs-new burst probing (bradycardia bursts up to 4 s and
tachycardia bursts ≤ 2.5 s raise nothing in either version).

**One deliberate boundary move.** A tachycardia burst ≥ ~3 s now
raises HR_HIGH (the old code raised nothing even at 4 s): after a
3 s burst the *median* rate remains above 120 bpm for > 5 s in
total, so the condition as measured meets the design's own
persistence requirement — the interval path merely starts the
clock when the condition actually starts. Documented in
`docs/design.md` §2.

**Results.** Host: HR_LOW raised at **6999 ms** and HR_HIGH at
**5799 ms** after a step change (`test_brady_latency`,
`test_tachy_latency`), with all Phase 1 numbers unchanged (VFib
1499 ms, asystole 3799 ms, SpO₂ +16900 ms). On target (Step 5.4):
bradycardia median **7269 ms** / max 7693 ms (n = 5), tachycardia
median **6165 ms** / max 6203 ms (n = 3) — every run within budget.
Honest residue: the worst bradycardia run's margin is modest; even
after subtracting the method's ≤ ~0.3 s bias (true ≲ 7.4 s), the
remainder is beat-phase alignment at the rhythm switch plus the
by-design 5 s persistence.

**Lesson.** Derive every latency budget from the estimator's
dynamics at the worst case you claim to cover — a median-of-N
detector has an N/2-sample memory, and at low event rates that
memory is measured in seconds. And measure the slow conditions,
not just the headline one: VFib was fine from day one; bradycardia
was where the design was wrong.

### 9.3 The set the campaign threw away (respect the semantics you measure)

The first apnea set produced exactly **one** valid run out of ten.
Cause: apnea is classified **HIGH** in the code (`priority_for` in
`src/dsp/dsp.c`) — so it latches — while the measurement plan,
written against a design draft that said MEDIUM, never acked
between runs. The latched run-1 instance suppressed runs 2–10, per
the Step 4.2 rule. The valid distribution in Step 5.4 comes from a
dedicated re-run (phase A2) with an ack after every injection.

Two follow-ups fell out of this. The protocol lesson: *your
measurement procedure must respect the alarm semantics of the
system under test* — latching, escalation and acknowledgement are
part of the behaviour, not obstacles to it. And the discrepancy
itself was resolved in favour of the code: by annunciation the
patient has not breathed for ≥ 11 s (6 s detection + 5 s
persistence), an arrest-class event that IEC 60601-1-8 practice
treats as high priority; `docs/design.md` §1 now agrees with
`priority_for`, which is the single source of truth.

---

## Step 10 — What to try next

Each exercise uses only machinery you have already built.

1. **Reproduce, then diff.** Re-run phase A
   (`python3 latency_test.py A 10`) and compare your distributions
   with Step 5.4. TCG jitter will move individual runs; the medians
   should track. If one condition is systematically off, find out
   why before believing either set.
2. **Vary the run count.** Run a set with 30 repetitions and watch
   what happens to p95 and max. How many runs do you need before
   the max stabilises — and what does that imply about quoting a
   max from n = 10?
3. **Predict, then measure.** Change one persistence value in
   `src/dsp/dsp.c` (e.g. SPO2_LOW's 10 s), write down your predicted
   median shift *before* rebuilding, then rebuild the target,
   rebuild the image (Step 2.4), and measure the desat set. Did the
   median move by exactly the persistence delta? Why or why not?
4. **Measure a condition under load.** Phase B only loads the VFib
   set. Adapt the driver to run the desat set under the hogs. Does
   the SpO₂ median move more or less than VFib's did? Explain it
   from the averaging window.
5. **Trace a restart.** Repeat Step 7's capture across a
   `faultctl kill dsp`, and find the supervisor's respawn and the
   new DSP's HELLO handshake in the trace. Which message types
   appear, in what order?
6. **Explain the apnea spread.** From the Step 5.4 table, derive
   the theoretical min and max apnea latency at 13 breaths/min
   (first missed breath + 6 s detection + 5 s persistence) and
   compare with the measured 6815 / 11393 ms.
7. **Host vs. target.** Run `tools/demo.sh` on the host and time
   the VFib raise from the audit log. Compare with the target's
   1664 ms median and account for the difference (backend,
   emulation, scheduling).
8. **Read the supervision mapping.** `docs/design.md` §3 maps the
   hand-written `vgsupervisor` policy (3 s heartbeat timeout,
   restart budget 10/60 s, 0.5 s → 4 s backoff) onto QNX's QPLMS /
   `slm`. List what `post_start.custom` and the supervisor process
   currently do that `slm` would take over — the image is presently
   built with `OPT_SLM=no`.

---

## References

- `docs/design.md` — alarm philosophy, the latency budget and its
  derivation, and the mapping onto mechanisms that exist in
  QNX 8.0.
- `docs/latency-report.md` — the full on-target report this codelab
  follows: method, all distributions, the trace chain, and the §7
  addendum closing the HR budgets and the apnea-priority finding.
- Target workspace (`~/workspace/qnx/custom-image/`):
  `LATENCY-REPORT.md` (the same report, with the raw-data
  inventory), `WARMUP-FIX-REPORT.md` (Step 9.1 in full),
  `PORT-REPORT.md` and `SPIKE-REPORT.md` (the native-IPC port and
  the first trace), `REPORT.md` (the image build), and the harness
  itself — `latency_test.py`, `analyze_latency.py`,
  `hrfix_test.py`, `analyze_hrfix.py`, `latency_sink.py`,
  `warmup_test.py`.
- `qemu/README.md` — the target build, the native backend, and the
  image recipe in reference form.
- `demo/` in the repo — the recorded demo session (boot → VFib →
  latch → ack → kill DSP → restart) and the raw serial log it was
  rendered from.
- Devpost submission: https://devpost.com/software/vitalguard-hgapes
  · Demo video: https://www.youtube.com/watch?v=xsWSZhib14E

*VitalGuard is a research/education prototype on synthetic signals.
It is not a medical device and must never be used for patient care.*
