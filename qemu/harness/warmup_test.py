#!/usr/bin/env python3
"""Warm-up fix verification (2026-11-25).

Boot A: autostarted pipeline streams; snapshot audit log; 
  `faultctl kill dsp`; supervisor restarts the DSP; observe 20 s
  (old spurious window was 1-5 s) and assert NO new RAISED lines;
  then `faultctl vfib 10` and assert RAISED + CLEARED ~10 s apart.
  Ends by slaying the pipeline and clearing /tmp/vitalguard so the
  next boot's audit log is attributable to that boot alone.
Boot B: cold boot; pipeline autostarts; after >= 25 s of streaming
  assert the audit log has no RAISED lines (no cold-boot transient).
Console I/O -> logs/warmup-console.log; results -> logs/warmup-result.txt.
"""
import os
import re
import socket
import subprocess
import sys
import time

BUILD = os.path.expanduser("~/workspace/qnx/custom-image/build")
OUT = os.path.expanduser("~/workspace/qnx/custom-image")
LOG = os.path.join(OUT, "logs", "warmup-console.log")
AUDIT = "/tmp/vitalguard/alarm_events.log"

logf = open(LOG, "w", buffering=1)
logf.write("# warmup fix verification\n")

results = {}


class Guest:
    def __init__(self, tag):
        self.tag = tag
        self.sock_path = os.path.join(OUT, f"serial-warmup-{tag}.sock")
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
                data = self.sock.recv(4096)
                if data:
                    self.buf += data
                    logf.write(data.decode("utf-8", "replace"))
                    logf.flush()
            except socket.timeout:
                pass

    def run_cmd(self, cmd, timeout=30):
        self.counter += 1
        marker = f"__DONE_{self.tag}_{self.counter}__"
        start_len = len(self.buf)
        logf.write(f"\n# [driver boot={self.tag} t={time.time()-self.t0:.1f}s] CMD: {cmd}\n")
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

    def audit_size(self):
        seg = self.run_cmd(f"echo SZ=$(wc -c < {AUDIT} 2>/dev/null)", 30)
        m = re.search(rb"SZ=(\d+)", seg)
        return int(m.group(1)) if m else 0

    def audit_new(self, from_byte):
        """Audit-log content appended after byte offset from_byte."""
        seg = self.run_cmd(f"tail -c +{from_byte + 1} {AUDIT} 2>/dev/null", 30)
        return seg

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# ---------------------------------------------------------------- boot A
g = Guest("a")
results["a_startup"] = g.wait_for("Startup complete", 300)
g.sock.sendall(b"\r\n")
g.pump(18.0)

seg = g.run_cmd("cat /tmp/vitalguard/supervisor.status", 30)
results["a_status_all_running"] = seg.count(b"state=running") >= 3
seg = g.run_cmd("ls -l /system/bin/vitaldsp", 30)
results["a_dsp_binary_65800"] = b"65800" in seg
seg = g.run_cmd("tail -3 /tmp/vitalguard/vitaldsp.log", 30)
results["a_streaming"] = b"HR=" in seg and b"cond=0x0" in seg

s0 = g.audit_size()
results["a_audit_size_before_kill"] = str(s0)

g.run_cmd("faultctl kill dsp", 30)
# wait for the supervisor to restart the DSP
restarted = False
for _ in range(24):
    seg = g.run_cmd("cat /tmp/vitalguard/supervisor.status", 30)
    if b"vitaldsp" in seg and b"restarts=1" in seg:
        restarted = True
        break
    g.pump(2.0)
results["a_dsp_restarted"] = restarted

g.pump(20.0)  # rejoin window: old spurious alarms fired within 1-5 s
s1 = g.audit_size()
new = g.audit_new(s0)
results["a_audit_size_after_rejoin"] = str(s1)
results["a_no_spurious_after_kill"] = (s1 == s0) and (b"RAISED" not in new)
seg = g.run_cmd("tail -3 /tmp/vitalguard/vitaldsp.log", 30)
results["a_streaming_after_rejoin"] = b"HR=" in seg and b"cond=0x0" in seg

g.run_cmd("faultctl vfib 10", 30)
g.pump(20.0)
new = g.audit_new(s1)
pairs = re.findall(rb"t=(\d+) alarm #\d+ RAISED\s+cond=VFIB prio=HIGH", new)
clears = re.findall(rb"t=(\d+) alarm #\d+ CONDITION_CLEARED_LATCHED\s+cond=VFIB prio=HIGH", new)
results["a_vfib_raised"] = bool(pairs)
results["a_vfib_cleared"] = bool(clears)
if pairs and clears:
    results["a_vfib_delta_ms"] = str(int(clears[-1]) - int(pairs[-1]))

# clean slate so boot B's audit log is attributable to boot B alone
g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
g.pump(2.0)
g.run_cmd("rm -f /tmp/vitalguard/*; sync; echo cleaned", 20)
g.pump(2.0)
g.stop()

# ---------------------------------------------------------------- boot B
g = Guest("b")
results["b_startup"] = g.wait_for("Startup complete", 300)
g.sock.sendall(b"\r\n")
up = False
for _ in range(40):
    seg = g.run_cmd("cat /tmp/vitalguard/supervisor.status 2>/dev/null", 30)
    if seg.count(b"state=running") >= 3:
        up = True
        break
    g.pump(2.0)
results["b_status_all_running"] = up
g.pump(25.0)  # streaming well past any cold-boot warm-up window
seg = g.run_cmd(f"cat {AUDIT} 2>/dev/null; echo AUDIT_END", 30)
results["b_no_coldboot_alarm"] = b"RAISED" not in seg
seg = g.run_cmd("tail -3 /tmp/vitalguard/vitaldsp.log", 30)
results["b_streaming"] = b"HR=" in seg and b"cond=0x0" in seg
g.run_cmd("slay -f vgsupervisor vitalgen vitaldsp alarmgr 2>/dev/null; echo slain", 20)
g.stop()

with open(os.path.join(OUT, "logs", "warmup-result.txt"), "w") as f:
    for k, v in results.items():
        f.write(f"{k}={v}\n")
print(results)
