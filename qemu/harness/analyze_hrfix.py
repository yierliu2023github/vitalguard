#!/usr/bin/env python3
"""HR-fix latency analysis: same method as analyze_latency.py —
signal-clock onset interpolated from the vitaldsp (wall, signal)
anchors in logs/hrfix-dsp.log, raise from the audit log stamps in
logs/hrfix-runs.json.  Reported values are upper bounds (true
latency up to ~0.3 s lower), TCG-emulated as in LATENCY-REPORT.md.
Output: logs/hrfix-stats.json + printed per-run table.
"""
import json
import os
import re

OUT = os.path.expanduser("~/workspace/qnx/custom-image/logs")
ANCHOR_RE = re.compile(
    r"\[(\d+)\] vitaldsp: t=(\d+)s HR=(-?[\d.]+) SpO2=([\d.]+) "
    r"RR=(-?[\d.]+) cond=0x([0-9a-f]+)")


def load_anchors():
    with open(os.path.join(OUT, "hrfix-dsp.log"), "rb") as f:
        data = f.read().decode("utf-8", "replace")
    anchors, seen = [], set()
    for m in ANCHOR_RE.finditer(data):
        w, s = int(m.group(1)), int(m.group(2)) * 1000
        if (w, s) not in seen:
            seen.add((w, s))
            anchors.append((w, s))
    anchors.sort()
    return anchors


def onset_signal(anchors, wall):
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
    v = sorted(vals)
    n = len(v)
    med = v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])
    return {"n": n, "min": round(v[0]), "median": round(med),
            "max": round(v[-1]), "mean": round(sum(v) / n)}


def main():
    anchors = load_anchors()
    with open(os.path.join(OUT, "hrfix-runs.json")) as f:
        data = json.load(f)
    per_set, rows = {}, []
    for r in data["records"]:
        if r.get("t_raise_ms") is None or r.get("fault_wall_ms") is None:
            rows.append({**r, "onset_ms": None, "latency_ms": None})
            continue
        onset, span = onset_signal(anchors, r["fault_wall_ms"])
        if onset is None:
            rows.append({**r, "onset_ms": None, "latency_ms": None})
            continue
        lat = r["t_raise_ms"] - onset
        rows.append({**r, "onset_ms": round(onset),
                     "latency_ms": round(lat), "anchor_span_ms": span})
        per_set.setdefault(r["set"], []).append(lat)
    result = {k: stats(v) for k, v in per_set.items()}
    with open(os.path.join(OUT, "hrfix-stats.json"), "w") as f:
        json.dump({"stats": result, "rows": rows}, f, indent=1)
    for row in rows:
        print(f"{row['set']:>6} #{row['run']:<2} onset={row['onset_ms']} "
              f"raise={row['t_raise_ms']} lat={row['latency_ms']} "
              f"span={row.get('anchor_span_ms')}")
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
