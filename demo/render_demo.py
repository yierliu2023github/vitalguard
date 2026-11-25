#!/usr/bin/env python3
"""Render the VitalGuard Devpost demo video from demo-events.json.

Reads the recorded QNX guest session (demo_session.py output) and
renders a 1920x1080 MP4: title card, terminal scenes that reveal the
real console lines progressively with an explanatory caption bar,
a measured-latency closing card, and an end card.

  python3 render_demo.py            # -> vitalguard-demo.mp4

Frames are rendered with PIL and assembled with ffmpeg's concat
demuxer (one PNG per reveal state, explicit durations).
"""
import json
import os
import re
import subprocess
import sys
import textwrap

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
EVENTS = os.path.join(HERE, "demo-events.json")
FRAMES = os.path.join(HERE, ".frames")
OUT = os.path.join(HERE, "vitalguard-demo.mp4")

W, H = 1920, 1080
BG = (11, 14, 20)
PANEL = (15, 20, 29)
PANEL_HEAD = (22, 28, 40)
INK = (201, 212, 227)      # terminal output
DIM = (139, 152, 169)      # secondary text
BRIGHT = (230, 237, 243)   # headings / commands
GREEN = (126, 231, 135)
RED = (255, 123, 114)
ORANGE = (255, 166, 87)
ACCENT = (47, 129, 247)

FD = "/usr/share/fonts/truetype/dejavu"
MONO = f"{FD}/DejaVuSansMono.ttf"
MONO_B = f"{FD}/DejaVuSansMono-Bold.ttf"
SANS = f"{FD}/DejaVuSans.ttf"
SANS_B = f"{FD}/DejaVuSans-Bold.ttf"

f_term = ImageFont.truetype(MONO, 25)
f_term_b = ImageFont.truetype(MONO_B, 25)
f_head = ImageFont.truetype(MONO, 22)
f_cap = ImageFont.truetype(SANS, 29)
f_title = ImageFont.truetype(SANS_B, 96)
f_subtitle = ImageFont.truetype(SANS, 52)
f_body = ImageFont.truetype(SANS, 31)
f_small = ImageFont.truetype(SANS, 24)
f_card_title = ImageFont.truetype(SANS_B, 42)
f_table = ImageFont.truetype(MONO, 26)
f_table_b = ImageFont.truetype(MONO_B, 26)

TERM_X, TERM_Y, TERM_W = 70, 64, 1780
TERM_H = 826
TEXT_X, TEXT_Y = 100, 138
LINE_H = 33
MAX_CHARS = 112


def printable(s):
    s = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", s)  # CSI sequences
    s = re.sub(r"\x1b.", "", s)
    return "".join(c for c in s if c == "\n" or ord(c) >= 32)


def lines_of(text):
    out = []
    for ln in printable(text).split("\n"):
        ln = ln.rstrip()
        if len(ln) > MAX_CHARS:
            ln = ln[:MAX_CHARS - 1] + "…"
        out.append(ln)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def line_style(ln):
    """(colour, bold) for a terminal line."""
    if "RAISED" in ln:
        return RED, True
    if "CONDITION_CLEARED_LATCHED" in ln or "ACKED" in ln or " CLEARED" in ln:
        return GREEN, False
    if "cond=0x2" in ln:
        return ORANGE, False
    if "restarts=1" in ln:
        return ORANGE, True
    if "Startup complete" in ln:
        return GREEN, True
    if "restart in" in ln:
        return GREEN, False
    return INK, False


def draw_terminal(d, shown):
    d.rounded_rectangle([TERM_X, TERM_Y, TERM_X + TERM_W, TERM_Y + TERM_H],
                        radius=14, fill=PANEL, outline=(38, 46, 61), width=2)
    d.rounded_rectangle([TERM_X, TERM_Y, TERM_X + TERM_W, TERM_Y + 46],
                        radius=14, fill=PANEL_HEAD)
    d.rectangle([TERM_X, TERM_Y + 32, TERM_X + TERM_W, TERM_Y + 46], fill=PANEL_HEAD)
    for i, col in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        cx = TERM_X + 30 + i * 34
        d.ellipse([cx - 9, TERM_Y + 14, cx + 9, TERM_Y + 32], fill=col)
    d.text((TERM_X + 130, TERM_Y + 10),
           "vitalguard — QNX 8.0.0 x86_64 — serial console (QEMU)",
           font=f_head, fill=DIM)
    y = TEXT_Y
    for kind, ln in shown:
        if kind == "cmd":
            d.text((TEXT_X, y), "# ", font=f_term_b, fill=GREEN)
            d.text((TEXT_X + 32, y), ln, font=f_term_b, fill=BRIGHT)
        else:
            col, bold = line_style(ln)
            d.text((TEXT_X, y), ln, font=f_term_b if bold else f_term, fill=col)
        y += LINE_H


def draw_caption(d, caption):
    d.rectangle([70, 936, 76, 1014], fill=ACCENT)
    y = 930
    for ln in textwrap.wrap(caption, width=96):
        d.text((100, y), ln, font=f_cap, fill=BRIGHT)
        y += 40


def base():
    img = Image.new("RGB", (W, H), BG)
    return img, ImageDraw.Draw(img)


def center(d, y, text, font, fill):
    w = d.textlength(text, font=font)
    d.text(((W - w) / 2, y), text, font=font, fill=fill)


def title_card():
    img, d = base()
    center(d, 330, "VitalGuard", f_title, BRIGHT)
    center(d, 452, "on QNX 8.0 (QEMU)", f_subtitle, DIM)
    d.rectangle([760, 560, 1160, 564], fill=ACCENT)
    sub = ("A patient-monitor alarm pipeline that treats alarm latency\n"
           "as a real-time property — and proves it with measurements.")
    y = 610
    for ln in sub.split("\n"):
        center(d, y, ln, f_body, INK)
        y += 44
    center(d, 950,
           "Live session on the target image · QEMU TCG emulation — timings are "
           "emulated distributions, not hardware figures",
           f_small, DIM)
    return img


def closing_card():
    img, d = base()
    d.rounded_rectangle([70, 64, 1850, 1016], radius=14, fill=PANEL,
                        outline=(38, 46, 61), width=2)
    d.text((110, 100), "Measured on target — alarm latency (onset → annunciation)",
           font=f_card_title, fill=BRIGHT)
    rows = [
        ("VFib (n=10)", "median 1.66 s", "max 2.30 s", "budget ≤ 4 s", "PASS"),
        ("VFib, CPU saturated (n=10)", "median 2.01 s", "max 2.33 s", "budget ≤ 4 s", "PASS"),
        ("Asystole (n=10)", "median 3.83 s", "max 4.21 s", "3.5 s persistence", "by design"),
        ("Tachycardia HR_HIGH (n=3)*", "median 6.17 s", "max 6.20 s", "budget ≤ 8 s", "PASS"),
        ("Bradycardia HR_LOW (n=5)*", "median 7.27 s", "max 7.69 s", "budget ≤ 8 s", "PASS"),
        ("SpO2 drop (n=10)", "median 17.18 s", "max 17.21 s", "budget ≤ 24 s", "PASS"),
        ("Apnea (n=10)", "median 8.92 s", "max 11.39 s", "budget ≤ 12 s", "PASS"),
    ]
    y = 190
    for name, med, mx, budget, verdict in rows:
        d.text((110, y), name, font=f_table, fill=INK)
        d.text((760, y), med, font=f_table, fill=INK)
        d.text((1010, y), mx, font=f_table, fill=INK)
        d.text((1230, y), budget, font=f_table, fill=DIM)
        d.text((1560, y), verdict, font=f_table_b,
               fill=GREEN if verdict == "PASS" else DIM)
        y += 52
    y += 18
    for ln, bold in [
        ("DSP kill → streaming resumed (n=5):  0.78–2.11 s", False),
        ("Alarm-event MsgSend round trip under CPU load (kernel trace):  94 µs", False),
    ]:
        d.text((110, y), ln, font=f_table_b if bold else f_table, fill=INK)
        y += 52
    y += 26
    for ln in [
        "* HR rows measured after the dual-path detection fix (LATENCY-REPORT §7).",
        "All figures: QEMU TCG emulation (no KVM) on a shared host — distributions",
        "demonstrating budget compliance in emulation, never hardware measurements.",
    ]:
        d.text((110, y), ln, font=f_small, fill=DIM)
        y += 34
    return img


def end_card():
    img, d = base()
    center(d, 380, "VitalGuard", ImageFont.truetype(SANS_B, 72), BRIGHT)
    center(d, 500, "QNX Global Hackathon — Robotics & Medical", f_body, INK)
    center(d, 560, "Apache-2.0 · source, design notes and full measurement data in the repo",
           f_body, DIM)
    return img


def pick(lines, *needles):
    out = []
    for n in needles:
        for ln in lines:
            if n in ln:
                out.append(ln)
                break
    return out


def build_scenes(ev):
    boot = lines_of(ev["boot"]["text"])
    boot_sel = pick(boot, "Starting slogger2", "Starting PCI Services",
                    "Mounting file systems", "Direct-Access",
                    "Starting Networking", "Starting misc",
                    "Process count", "Startup complete", "QNX vitalguard 8.0.0")
    sup_out = lines_of(ev["supervisor_out"]["text"])
    status_up = lines_of(ev["status_up"]["text"])
    dsp_stream = lines_of(ev["dsp_streaming"]["text"])
    gen_fault = lines_of(ev["gen_fault"]["text"])
    dsp_vfib = lines_of(ev["dsp_vfib"]["text"])
    audit_vfib = lines_of(ev["audit_vfib"]["text"])
    audit_ack = lines_of(ev["audit_ack"]["text"])
    sup_restart_all = lines_of(ev["supervisor_restart"]["text"])
    # tail -6 shows the 3 boot-time starts, then the kill/restart lines
    sup_restart = [ln for ln in sup_restart_all
                   if "killing" in ln or "exited" in ln
                   or ("started vitaldsp" in ln and ln not in sup_out)]
    status_re = lines_of(ev["status_restarted"]["text"])
    dsp_resumed = lines_of(ev["dsp_resumed"]["text"])

    T = "term"
    scenes = []
    scenes.append({"kind": "title", "dur": 7.0})
    scenes.append({"kind": T, "dur": 13.0, "caption":
                   "Clean boot of the self-built QNX 8.0 image (mkqnximage) under "
                   "QEMU. Nobody logs in — the monitor pipeline autostarts.",
                   "lines": [("out", ln) for ln in boot_sel]})
    scenes.append({"kind": T, "dur": 11.0, "caption":
                   "The supervisor starts the pipeline in dependency order — alarm "
                   "manager, DSP, signal generator — and tracks their heartbeats.",
                   "lines": [("cmd", "cat /tmp/vitalguard/vgsupervisor.out")]
                            + [("out", ln) for ln in sup_out]
                            + [("cmd", "cat /tmp/vitalguard/supervisor.status")]
                            + [("out", ln) for ln in status_up]})
    scenes.append({"kind": T, "dur": 10.0, "caption":
                   "Normal sinus rhythm. The DSP streams vitals every 5 s: HR 71 bpm, "
                   "SpO2 98%, no alarm conditions (cond=0x0).",
                   "lines": [("cmd", "tail -4 /tmp/vitalguard/vitaldsp.log")]
                            + [("out", ln) for ln in dsp_stream]})
    scenes.append({"kind": T, "dur": 13.0, "caption":
                   "Fault injection: ventricular fibrillation for 10 s. The DSP sees "
                   "no QRS and chaotic RMS — condition flag 0x2 (VFib).",
                   "lines": [("cmd", "faultctl vfib 10")]
                            + [("out", ln) for ln in gen_fault]
                            + [("cmd", "tail -4 /tmp/vitalguard/vitaldsp.log")]
                            + [("out", ln) for ln in dsp_vfib]})
    scenes.append({"kind": T, "dur": 15.0, "caption":
                   "VFib annunciates 1.7 s after onset in this run (median 1.66 s over "
                   "10 runs). The rhythm ends at 10 s — but a HIGH alarm latches until "
                   "a human acknowledges it.",
                   "lines": [("cmd", "cat /tmp/vitalguard/alarm_events.log")]
                            + [("out", ln) for ln in audit_vfib]})
    scenes.append({"kind": T, "dur": 8.0, "caption":
                   "The operator acknowledges the latched alarm; only then does it "
                   "clear. Every transition lands in the append-only audit log.",
                   "lines": [("cmd", "faultctl ack 1"),
                             ("cmd", "tail -3 /tmp/vitalguard/alarm_events.log")]
                            + [("out", ln) for ln in audit_ack]})
    scenes.append({"kind": T, "dur": 11.0, "caption":
                   "Now break it: kill the DSP mid-stream. The supervisor misses its "
                   "heartbeats and restarts it — first backoff step 500 ms, restart "
                   "budget 10 per minute.",
                   "lines": [("cmd", "faultctl kill dsp"),
                             ("cmd", "tail -6 /tmp/vitalguard/vgsupervisor.out")]
                            + [("out", ln) for ln in sup_restart]})
    scenes.append({"kind": T, "dur": 13.0, "caption":
                   "Streaming resumes (kill → vitals 0.8–2.1 s over 5 runs). The "
                   "restarted DSP warms up for 5 s before it may raise — a rejoin "
                   "can never cause a false alarm.",
                   "lines": [("cmd", "cat /tmp/vitalguard/supervisor.status")]
                            + [("out", ln) for ln in status_re]
                            + [("cmd", "tail -4 /tmp/vitalguard/vitaldsp.log")]
                            + [("out", ln) for ln in dsp_resumed]})
    scenes.append({"kind": "closing", "dur": 16.0})
    scenes.append({"kind": "end", "dur": 7.0})
    return scenes


def main():
    with open(EVENTS) as f:
        data = json.load(f)
    ev = {e["label"]: e for e in data["events"]}
    scenes = build_scenes(ev)

    os.makedirs(FRAMES, exist_ok=True)
    for old in os.listdir(FRAMES):
        os.unlink(os.path.join(FRAMES, old))

    entries = []  # (path, duration)
    idx = 0

    def emit(img, dur):
        nonlocal idx
        p = os.path.join(FRAMES, f"f{idx:04d}.png")
        img.save(p)
        entries.append((p, dur))
        idx += 1

    for sc in scenes:
        if sc["kind"] == "title":
            emit(title_card(), sc["dur"])
        elif sc["kind"] == "closing":
            emit(closing_card(), sc["dur"])
        elif sc["kind"] == "end":
            emit(end_card(), sc["dur"])
        else:
            lines = sc["lines"]
            n = len(lines)
            # reveal progressively; final state holds the remainder
            weights = [1.0] * n
            weights[-1] = 2.5
            total_w = sum(weights)
            for k in range(1, n + 1):
                img, d = base()
                draw_terminal(d, lines[:k])
                draw_caption(d, sc["caption"])
                emit(img, sc["dur"] * weights[k - 1] / total_w)

    lst = os.path.join(FRAMES, "list.txt")
    with open(lst, "w") as f:
        for p, dur in entries:
            f.write(f"file '{p}'\nduration {dur:.3f}\n")
        f.write(f"file '{entries[-1][0]}'\n")  # concat demuxer quirk

    total = sum(d for _, d in entries)
    print(f"{len(entries)} frames, total {total:.1f} s")
    subprocess.run(["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-r", "30",
                    "-movflags", "+faststart", OUT], check=True,
                   capture_output=True, text=True)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
