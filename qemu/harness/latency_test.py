#!/usr/bin/env python3
"""On-target latency measurement driver (2026-11-25).

Phase A (baseline): boot the autostart image, then repeated fault
injections per condition (vfib / asystole / tachy / brady / desat /
apnea), capturing per run: the vitalgen fault line (guest wall-clock
application stamp), the audit RAISED / CLEARED signal-clock stamps,
and the vitaldsp (wall, signal) anchor lines used to convert the
fault wall stamp into signal time.

Phase B (load + trace + restart): boot, start vghog (faultctl's
instance + one direct instance, one per vCPU), repeat the VFib set
under load, capture a tracelogger trace over one loaded VFib
injection and curl it to the host sink, then 5x `faultctl kill dsp`
restart-latency repetitions (precise wall stamps parsed offline
from vgsupervisor.out / vitaldsp.log).

Raw segments + per-run records -> logs/latency-<phase>-runs.json and
logs/latency-<phase>-{audit,gen,dsp,sup}.log; console transcript ->
logs/latency-<phase>-console.log.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

BUILD = os.path.expanduser("~/workspace/qnx/custom-image/build")
OUT = os.path.expanduser("~/workspace/qnx/custom-image")
RT = "/tmp/vitalguard"
AUDIT = f"{RT}/alarm_events.log"
GENLOG = f"{RT}/vitalgen.log"
DSPLOG = f"{RT}/vitaldsp.log"
SUPOUT = f"{RT}/vgsupervisor.out"
STATUS = f"{RT}/supervisor.status"

PHASE = sys.argv[1] if len(sys.argv) > 1 else "A"
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 10

logf = open(os.path.join(OUT, "logs", f"latency-{PHASE.lower()}-console.log"),
            "w", buffering=1)
logf.write(f"# latency measurement phase {PHASE} runs_per_set={RUNS}\n")

records = []          # per-run dicts
segments = {}         # file tag -> bytes captured this phase


class Guest:
    def __init__(self, tag):
        self.tag = tag
        self.sock_path = os.path.join(OUT, f"serial-lat-{tag}.sock")
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        qemu_cmd = [
            "qemu-system-x86_64",
            "-machine", "pc", "-smp", "2", "-cpu", "max", "-m", "2G",
            "-kernel", os.path.join(BUILD, "output/ifs.bin"),
            "-drive", f"file={os.path.join(BUILD, 'output/disk-qemu')},format=raw,if=ide,id=drv0",
            "-chardev", f"socket,id=ser0,path={self.sock_path},server=on,wait=off",
            "-serial", "chardev:ser0",
            "-display", "none", "-monitor", "none",
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
            "-netdev", "user,id=net0",
            "-device", "virtio-net-pci,netdev=net0",
        ]
        self.t0 = time.time()
        self.proc = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.sock = None
        while time.time() - self.t0 < 30:
            if os.path.exists(self.sock_path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(self.sock_path)
                    self.sock = s
                    break
                except OSError:
                    pass
            time.sleep(0.2)
        if self.sock is None:
            raise RuntimeError("serial socket never appeared")
        self.sock.settimeout(0.5)
        self.buf = b""
        self.counter = 0

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.sock.recv(65536)
                if data:
                    self.buf += data
                    logf.write(data.decode("utf-8", "replace"))
                    logf.flush()
            except socket.timeout:
                pass

    def run_cmd(self, cmd, timeout=60):
        self.counter += 1
        marker = f"__DONE_{self.tag}_{self.counter}__"
        start_len = len(self.buf)
        logf.write(f"\n# [driver phase={self.tag} t={time.time()-self.t0:.1f}s] CMD: {cmd}\n")
        self.sock.sendall(cmd.encode() + b"\n")
        self.sock.sendall(f"echo {marker}\n".encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(0.5)
            if marker.encode() in self.buf[start_len:]:
                return self.buf[start_len:]
        logf.write(f"\n# [driver] TIMEOUT waiting for {marker}\n")
        return self.buf[start_len:]

    def wait_for(self, text, timeout=300):
        start = time.time()
        while time.time() - start < timeout:
            if text.encode() in self.buf:
                return True
            self.pump(1.0)
        return False

    def fsize(self, path):
        seg = self.run_cmd(f"echo SZ=$(wc -c < {path} 2>/dev/null)", 30)
        m = re.search(rb"SZ=(\d+)", seg)
        return int(m.group(1)) if m else 0

    def ftail(self, path, from_byte, timeout=120):
        return self.run_cmd(f"tail -c +{from_byte + 1} {path} 2>/dev/null",
                            timeout)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


RAISE_RE = re.compile(rb"t=(\d+) alarm #(\d+) RAISED\s+cond=(\w+) prio=(\w+)")
CLEAR_RE = re.compile(rb"t=(\d+) alarm #(\d+) (CONDITION_CLEARED_LATCHED|CLEARED)\s+cond=(\w+)")
FAULT_RE = re.compile(rb"\[(\d+)\] vitalgen: fault: (.+)")
ANCHOR_RE = re.compile(rb"\[(\d+)\] vitaldsp: t=(\d+)s HR=(-?[\d.]+) SpO2=([\d.]+) RR=(-?[\d.]+) cond=0x([0-9a-f]+)")


def wait_pipeline_up(g, timeout=240):
    """Wait until supervisor reports all three children running."""
    end = time.time() + timeout
    while time.time() < end:
        seg = g.run_cmd(f"cat {STATUS} 2>/dev/null", 30)
        if seg.count(b"state=running") >= 3:
            return True
        g.pump(3.0)
    return False


def run_injection(g, audit_off, set_name, idx, cmd, cond, high,
                  raise_timeout, clear_timeout):
    """One injection cycle. Returns (record, new_audit_off)."""
    rec = {"set": set_name, "run": idx, "cmd": cmd, "cond": cond}
    # count pre-existing raises of this condition in this phase's audit
    pre = g.ftail(AUDIT, audit_off)
    pre_raises = len([m for m in RAISE_RE.finditer(pre)
                      if m.group(3).decode() == cond])
    g.run_cmd(cmd, 30)
    g.pump(1.5)
    # fault application wall stamp from vitalgen's own log tail
    seg = g.run_cmd(f"tail -3 {GENLOG}", 30)
    faults = FAULT_RE.findall(seg)
    rec["fault_wall_ms"] = int(faults[-1][0]) if faults else None
    rec["fault_line"] = faults[-1][1].decode() if faults else None

    # poll audit for this run's RAISED then CLEARED
    t_raise = t_clear = alarm_id = None
    deadline = time.time() + raise_timeout
    while time.time() < deadline:
        seg = g.ftail(AUDIT, audit_off)
        raises = [m for m in RAISE_RE.finditer(seg)
                  if m.group(3).decode() == cond]
        if len(raises) > pre_raises:
            m = raises[pre_raises]
            t_raise, alarm_id = int(m.group(1)), int(m.group(2))
            break
        g.pump(3.0)
    rec["t_raise_ms"] = t_raise
    rec["alarm_id"] = alarm_id
    if t_raise is not None:
        deadline = time.time() + clear_timeout
        while time.time() < deadline:
            seg = g.ftail(AUDIT, audit_off)
            clears = [m for m in CLEAR_RE.finditer(seg)
                      if m.group(4).decode() == cond and
                      (alarm_id is None or int(m.group(2)) == alarm_id)]
            if clears:
                t_clear = int(clears[-1].group(1))
                rec["clear_kind"] = clears[-1].group(3).decode()
                break
            g.pump(3.0)
    rec["t_clear_ms"] = t_clear
    if high and alarm_id is not None:
        g.run_cmd(f"faultctl ack {alarm_id}", 30)
        g.pump(2.0)
    g.pump(6.0)  # settle on clean signal
    new_off = g.fsize(AUDIT)
    rec["audit_bytes_after"] = new_off
    records.append(rec)
    logf.write(f"\n# [driver] RECORD {json.dumps(rec)}\n")
    return rec, new_off


SETS_A = [
    # set_name, cmd, cond, high, raise_timeout, clear_timeout
    ("vfib",     "faultctl vfib 15",            "VFIB",     True,  40, 60),
    ("asystole", "faultctl rhythm asystole 15", "ASYSTOLE", True,  45, 60),
    ("tachy",    "faultctl rhythm tachy 20",    "HR_HIGH",  False, 50, 70),
    ("brady",    "faultctl rhythm brady 30",    "HR_LOW",   False, 60, 80),
    ("desat",    "faultctl desat 84 45",        "SPO2_LOW", False, 60, 120),
    ("apnea",    "faultctl apnea 25",           "APNEA",    False, 50, 70),
]


def dump_segments(g, offs):
    for tag, path in (("audit", AUDIT), ("gen", GENLOG),
                      ("dsp", DSPLOG), ("sup", SUPOUT)):
        seg = g.ftail(path, offs[tag], timeout=180)
        fn = os.path.join(OUT, "logs", f"latency-{PHASE.lower()}-{tag}.log")
        with open(fn, "wb") as f:
            f.write(seg)
        segments[tag] = len(seg)


def save_records():
    fn = os.path.join(OUT, "logs", f"latency-{PHASE.lower()}-runs.json")
    with open(fn, "w") as f:
        json.dump({"phase": PHASE, "records": records,
                   "segments": segments}, f, indent=1)


def boot_and_prepare(tag):
    g = Guest(tag)
    assert g.wait_for("Startup complete", 300), "boot failed"
    g.sock.sendall(b"\r\n")
    g.pump(5.0)
    assert wait_pipeline_up(g), "pipeline did not come up"
    g.pump(15.0)  # streaming, far past any warm-up guard
    seg = g.run_cmd(f"tail -2 {DSPLOG}", 30)
    assert b"cond=0x0" in seg, "not streaming clean"
    offs = {tag_: g.fsize(path) for tag_, path in
            (("audit", AUDIT), ("gen", GENLOG), ("dsp", DSPLOG),
             ("sup", SUPOUT))}
    return g, offs


def phase_a():
    g, offs = boot_and_prepare("a")
    audit_off = offs["audit"]
    for (name, cmd, cond, high, rt, ct) in SETS_A:
        for i in range(1, RUNS + 1):
            _, audit_off = run_injection(g, audit_off, name, i, cmd,
                                         cond, high, rt, ct)
            save_records()
    dump_segments(g, offs)
    save_records()
    g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
    g.pump(2.0)
    g.run_cmd("rm -f /tmp/vitalguard/*; sync; echo cleaned", 20)
    g.stop()


def phase_a2():
    """Apnea only, with the HIGH-ack protocol (APNEA is HIGH in
    priority_for, so it latches — phase A runs 2+ were swallowed by
    the unacked latched instance from run 1)."""
    g, offs = boot_and_prepare("a2")
    audit_off = offs["audit"]
    for i in range(1, RUNS + 1):
        _, audit_off = run_injection(g, audit_off, "apnea", i,
                                     "faultctl apnea 25", "APNEA", True,
                                     50, 70)
        save_records()
    dump_segments(g, offs)
    save_records()
    g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
    g.pump(2.0)
    g.run_cmd("rm -f /tmp/vitalguard/*; sync; echo cleaned", 20)
    g.stop()


def phase_b():
    g, offs = boot_and_prepare("b")
    audit_off = offs["audit"]

    # ---- CPU contention: two vghog instances (one per vCPU), nice 10
    g.run_cmd("faultctl hog start", 30)
    g.pump(2.0)
    g.run_cmd("vghog >> /tmp/vitalguard/hog2.log 2>&1 &", 30)
    g.pump(3.0)
    seg = g.run_cmd("cat /tmp/vitalguard/hog.log /tmp/vitalguard/hog2.log 2>/dev/null; pidin | grep -c vghog", 30)
    records.append({"set": "hog_check", "run": 0,
                    "hog_lines": seg.decode("utf-8", "replace")[-400:]})
    seg = g.run_cmd(f"tail -2 {DSPLOG}", 30)
    records.append({"set": "hog_streaming", "run": 0,
                    "ok": b"cond=0x0" in seg})

    # ---- VFib set under load
    for i in range(1, RUNS + 1):
        _, audit_off = run_injection(g, audit_off, "vfib_hog", i,
                                     "faultctl vfib 15", "VFIB", True,
                                     60, 90)
        save_records()

    # ---- tracelogger capture over one loaded VFib injection
    g.run_cmd("rm -f /tmp/latency.kev", 15)
    seg = g.run_cmd("pidin", 60)
    with open(os.path.join(OUT, "logs", "latency-b-pidin.txt"), "wb") as f:
        f.write(seg)
    g.run_cmd("tracelogger -f /tmp/latency.kev -s 45 > /tmp/tracelogger.log 2>&1 &", 15)
    g.pump(4.0)
    g.run_cmd("faultctl vfib 15", 30)
    g.pump(30.0)
    g.run_cmd("cat /tmp/tracelogger.log; ls -l /tmp/latency.kev", 30)
    g.run_cmd("curl -s -T /tmp/latency.kev http://10.0.2.2:9999/latency.kev; echo curl-exit=$?", 180)
    g.pump(2.0)

    # ---- stop the hogs
    g.run_cmd("faultctl hog stop", 30)
    g.run_cmd("slay vghog 2>/dev/null; echo hogs-down", 20)
    g.pump(3.0)

    # ---- supervisor restart latency x5 (wall stamps parsed offline)
    for i in range(1, 6):
        sup_before = g.fsize(SUPOUT)
        dsp_before = g.fsize(DSPLOG)
        seg = g.run_cmd(f"cat {STATUS}", 30)
        m0 = re.search(rb"vitaldsp pid=\d+ restarts=(\d+)", seg)
        restarts_before = int(m0.group(1)) if m0 else 0
        g.run_cmd("faultctl kill dsp", 30)
        resumed = False
        deadline = time.time() + 120
        while time.time() < deadline:
            seg = g.run_cmd(f"cat {STATUS}", 30)
            m = re.search(rb"vitaldsp pid=\d+ restarts=(\d+)", seg)
            if m and int(m.group(1)) > restarts_before and \
                    b"state=running" in seg:
                resumed = True
                break
            g.pump(2.0)
        g.pump(15.0)  # streaming + warm-up guard well past
        seg = g.run_cmd(f"tail -2 {DSPLOG}", 30)
        records.append({"set": "restart", "run": i, "resumed": resumed,
                        "sup_off_before": sup_before,
                        "dsp_off_before": dsp_before,
                        "streaming": b"cond=0x0" in seg})
        save_records()

    dump_segments(g, offs)
    save_records()
    g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
    g.pump(2.0)
    g.run_cmd("rm -f /tmp/vitalguard/*; sync; echo cleaned", 20)
    g.stop()


if __name__ == "__main__":
    if PHASE == "A":
        phase_a()
    elif PHASE == "A2":
        phase_a2()
    else:
        phase_b()
    print("PHASE", PHASE, "done:", len(records), "records")
