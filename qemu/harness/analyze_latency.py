#!/usr/bin/env python3
"""Latency analysis: convert per-run fault wall stamps to signal time
via the vitaldsp (wall, signal) anchor lines, then compute onset ->
raise latencies and per-set statistics.

Inputs : logs/latency-<phase>-runs.json, logs/latency-<phase>-dsp.log
         (phases a, a2, b), logs/latency-b-sup.log + -dsp.log for the
         restart timings.
Output : logs/latency-stats.json + a printed per-run table.
"""
import json
import os
import re
import sys

OUT = os.path.expanduser("~/workspace/qnx/custom-image/logs")
ANCHOR_RE = re.compile(
    r"\[(\d+)\] vitaldsp: t=(\d+)s HR=(-?[\d.]+) SpO2=([\d.]+) "
    r"RR=(-?[\d.]+) cond=0x([0-9a-f]+)")
DUR = {"vfib": 15.0, "vfib_hog": 15.0, "asystole": 15.0, "tachy": 20.0,
       "brady": 30.0, "desat": 45.0, "apnea": 25.0}


def load_anchors(phase):
    path = os.path.join(OUT, f"latency-{phase}-dsp.log")
    anchors = []
    seen = set()
    with open(path, "rb") as f:
        data = f.read().decode("utf-8", "replace")
    for m in ANCHOR_RE.finditer(data):
        w, s = int(m.group(1)), int(m.group(2)) * 1000
        if (w, s) not in seen:
            seen.add((w, s))
            anchors.append((w, s))
    anchors.sort()
    return anchors


def onset_signal(anchors, wall):
    """Interpolate signal time at guest wall `wall` from anchors.
    Returns (signal_ms, bracket_wall_span_ms) or (None, None)."""
    prev = None
    for (w, s) in anchors:
        if w >= wall:
            if prev is None:
                return None, None
            w1, s1 = prev
            if w == w1:
                return float(s1), 0
            frac = (wall - w1) / (w - w1)
            return s1 + frac * (s - s1), w - w1
        prev = (w, s)
    return None, None


def stats(vals):
    if not vals:
        return None
    v = sorted(vals)
    n = len(v)

    def pct(p):
        if n == 1:
            return v[0]
        k = (n - 1) * p
        f = int(k)
        c = min(f + 1, n - 1)
        return v[f] + (v[c] - v[f]) * (k - f)

    return {"n": n, "min": round(v[0]), "median": round(pct(0.5)),
            "p95": round(pct(0.95)), "max": round(v[-1]),
            "mean": round(sum(v) / n)}


def analyse_phase(phase, sets_wanted=None):
    rpath = os.path.join(OUT, f"latency-{phase}-runs.json")
    if not os.path.exists(rpath):
        return {}, []
    with open(rpath) as f:
        data = json.load(f)
    anchors = load_anchors(phase)
    per_set = {}
    rows = []
    for r in data["records"]:
        if "cmd" not in r or r.get("t_raise_ms") is None:
            continue
        if sets_wanted and r["set"] not in sets_wanted:
            continue
        onset, span = onset_signal(anchors, r["fault_wall_ms"])
        if onset is None:
            rows.append({**r, "onset_ms": None, "latency_ms": None})
            continue
        lat = r["t_raise_ms"] - onset
        delta = None
        if r.get("t_clear_ms") is not None and r["set"] in DUR:
            delta = (r["t_clear_ms"] - r["t_raise_ms"]
                     - DUR[r["set"]] * 1000)
        row = {**r, "onset_ms": round(onset), "latency_ms": round(lat),
               "anchor_span_ms": span, "clear_minus_raise_minus_dur": (
                   round(delta) if delta is not None else None)}
        rows.append(row)
        per_set.setdefault(r["set"], []).append(lat)
    return {k: stats(v) for k, v in per_set.items()}, rows


def restart_analysis():
    """Phase B: kill -> spawn -> stream-reconnect from wall stamps in
    vgsupervisor.out and vitaldsp.log segments."""
    sup = open(os.path.join(OUT, "latency-b-sup.log"), "rb").read().decode(
        "utf-8", "replace")
    dsp = open(os.path.join(OUT, "latency-b-dsp.log"), "rb").read().decode(
        "utf-8", "replace")
    kills = [int(m.group(1)) for m in re.finditer(
        r"\[(\d+)\] vgsupervisor: fault: killing vitaldsp", sup)]
    starts = [int(m.group(1)) for m in re.finditer(
        r"\[(\d+)\] vgsupervisor: started vitaldsp pid=", sup)]
    reconn = [int(m.group(1)) for m in re.finditer(
        r"\[(\d+)\] vitaldsp: vitalgen connected", dsp)]
    anchors = load_anchors("b")
    out = []
    for i, k in enumerate(kills):
        st = next((s for s in starts if s >= k), None)
        rc = next((s for s in reconn if s >= k), None)
        # first vitals anchor logged after the reconnect
        va = next((w for (w, s) in anchors if rc is not None and w >= rc),
                  None)
        out.append({"kill_wall": k,
                    "spawn_after_ms": (st - k) if st else None,
                    "reconnect_after_ms": (rc - k) if rc else None,
                    "first_vitals_after_ms": (va - k) if va else None})
    return out


if __name__ == "__main__":
    result = {}
    all_rows = []
    for phase, sets in (("a", None), ("a2", {"apnea"}), ("b", None)):
        s, rows = analyse_phase(phase, sets)
        if s:
            result[phase] = s
        all_rows.extend(rows)
    if os.path.exists(os.path.join(OUT, "latency-b-runs.json")):
        result["restart"] = restart_analysis()
    with open(os.path.join(OUT, "latency-stats.json"), "w") as f:
        json.dump({"stats": result, "rows": all_rows}, f, indent=1)
    for row in all_rows:
        print(f"{row['set']:>9} #{row['run']:<2} onset={row['onset_ms']} "
              f"raise={row['t_raise_ms']} lat={row['latency_ms']} "
              f"span={row['anchor_span_ms']} "
              f"dclr={row['clear_minus_raise_minus_dur']}")
    print(json.dumps(result, indent=1))
