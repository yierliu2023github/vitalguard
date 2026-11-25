#!/usr/bin/env python3
"""HR latency-fix confirmation (2026-11-25).

Boots the autostart image (rebuilt with the dual-path HR detection)
and runs a short confirmation set with the same injection and
recording method as latency_test.py phase A:

  brady x5  `faultctl rhythm brady 30`  -> HR_LOW  (was: median 14140 ms)
  tachy x3  `faultctl rhythm tachy 20`  -> HR_HIGH (was: median 7846 ms)

Records -> logs/hrfix-runs.json; segments -> logs/hrfix-{audit,gen,
dsp,sup}.log; console -> logs/hrfix-console.log.  Latencies are
computed by analyze_hrfix.py (same onset interpolation as
analyze_latency.py).  All figures are TCG-emulated, as in
LATENCY-REPORT.md.
"""
import json
import os
import re
import socket
import subprocess
import time

BUILD = os.path.expanduser("~/workspace/qnx/custom-image/build")
OUT = os.path.expanduser("~/workspace/qnx/custom-image")
RT = "/tmp/vitalguard"
AUDIT = f"{RT}/alarm_events.log"
GENLOG = f"{RT}/vitalgen.log"
DSPLOG = f"{RT}/vitaldsp.log"
SUPOUT = f"{RT}/vgsupervisor.out"
STATUS = f"{RT}/supervisor.status"

logf = open(os.path.join(OUT, "logs", "hrfix-console.log"),
            "w", buffering=1)
logf.write("# hrfix confirmation: brady x5, tachy x3\n")

records = []
segments = {}


class Guest:
    def __init__(self, tag):
        self.tag = tag
        self.sock_path = os.path.join(OUT, f"serial-{tag}.sock")
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
        logf.write(f"\n# [driver t={time.time()-self.t0:.1f}s] CMD: {cmd}\n")
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


def wait_pipeline_up(g, timeout=240):
    end = time.time() + timeout
    while time.time() < end:
        seg = g.run_cmd(f"cat {STATUS} 2>/dev/null", 30)
        if seg.count(b"state=running") >= 3:
            return True
        g.pump(3.0)
    return False


def run_injection(g, audit_off, set_name, idx, cmd, cond,
                  raise_timeout, clear_timeout):
    """One injection cycle (same protocol as latency_test.py;
    HR alarms are MEDIUM, so no ack step)."""
    rec = {"set": set_name, "run": idx, "cmd": cmd, "cond": cond}
    pre = g.ftail(AUDIT, audit_off)
    pre_raises = len([m for m in RAISE_RE.finditer(pre)
                      if m.group(3).decode() == cond])
    g.run_cmd(cmd, 30)
    g.pump(1.5)
    seg = g.run_cmd(f"tail -3 {GENLOG}", 30)
    faults = FAULT_RE.findall(seg)
    rec["fault_wall_ms"] = int(faults[-1][0]) if faults else None
    rec["fault_line"] = faults[-1][1].decode() if faults else None

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
    g.pump(6.0)  # settle on clean signal
    new_off = g.fsize(AUDIT)
    rec["audit_bytes_after"] = new_off
    records.append(rec)
    logf.write(f"\n# [driver] RECORD {json.dumps(rec)}\n")
    return rec, new_off


def save_records():
    fn = os.path.join(OUT, "logs", "hrfix-runs.json")
    with open(fn, "w") as f:
        json.dump({"records": records, "segments": segments}, f, indent=1)


def main():
    g = Guest("hrfix")
    assert g.wait_for("Startup complete", 300), "boot failed"
    g.sock.sendall(b"\r\n")
    g.pump(5.0)
    assert wait_pipeline_up(g), "pipeline did not come up"
    g.pump(15.0)  # streaming, far past any warm-up guard
    seg = g.run_cmd(f"tail -2 {DSPLOG}", 30)
    assert b"cond=0x0" in seg, "not streaming clean"
    offs = {tag: g.fsize(path) for tag, path in
            (("audit", AUDIT), ("gen", GENLOG), ("dsp", DSPLOG),
             ("sup", SUPOUT))}
    audit_off = offs["audit"]
    for i in range(1, 6):
        _, audit_off = run_injection(g, audit_off, "brady", i,
                                     "faultctl rhythm brady 30",
                                     "HR_LOW", 60, 90)
        save_records()
    for i in range(1, 4):
        _, audit_off = run_injection(g, audit_off, "tachy", i,
                                     "faultctl rhythm tachy 20",
                                     "HR_HIGH", 50, 80)
        save_records()
    for tag, path in (("audit", AUDIT), ("gen", GENLOG),
                      ("dsp", DSPLOG), ("sup", SUPOUT)):
        seg = g.ftail(path, offs[tag], timeout=180)
        fn = os.path.join(OUT, "logs", f"hrfix-{tag}.log")
        with open(fn, "wb") as f:
            f.write(seg)
        segments[tag] = len(seg)
    save_records()
    g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
    g.pump(2.0)
    g.run_cmd("rm -f /tmp/vitalguard/*; sync; echo cleaned", 20)
    g.stop()
    print("hrfix done:", len(records), "records")


if __name__ == "__main__":
    main()
