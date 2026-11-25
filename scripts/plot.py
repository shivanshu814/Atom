#!/usr/bin/env python3
"""atom_bench CSV -> SVG charts + Markdown tables (medians). Standard library only."""

import argparse
import csv
import math
import os
import statistics
from collections import defaultdict

# Okabe-Ito (colour-blind safe), one colour per structure.
COLOURS = {
    "MutexQueue": "#000000",
    "MutexStack": "#000000",
    "SpscQueue": "#0072B2",
    "NaiveSpscQueue": "#56B4E9",
    "MpscQueue": "#D55E00",
    "BoundedMpmcQueue": "#009E73",
    "Treiber+HP": "#CC79A7",
    "Treiber+EBR": "#E69F00",
}
DASHED = {"MutexQueue", "MutexStack"}


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def medians(rows, key_fields, value_field):
    groups = defaultdict(list)
    for r in rows:
        groups[tuple(r[k] for k in key_fields)].append(float(r[value_field]))
    return {k: statistics.median(v) for k, v in groups.items()}


def fmt_ns(v):
    if v >= 1e6:
        return f"{v / 1e6:.1f} ms"
    if v >= 1e3:
        return f"{v / 1e3:.1f} µs"
    return f"{v:.0f} ns"


def nice_ticks(lo, hi, log):
    if log:
        a, b = math.floor(math.log10(lo)), math.ceil(math.log10(hi))
        return [10 ** e for e in range(a, b + 1)]
    step = 10 ** math.floor(math.log10(max(hi - lo, 1e-9)))
    for m in (1, 2, 5, 10):
        if (hi - lo) / (step * m) <= 6:
            step *= m
            break
    t, ticks = 0.0, []
    while t <= hi + step * 0.01:
        ticks.append(t)
        t += step
    return ticks


def line_chart(path, title, x_label, y_label, xs, series, log=False, y_fmt=lambda v: f"{v:g}"):
    W, H, L, R, T, B = 720, 420, 80, 190, 50, 60
    pw, ph = W - L - R, H - T - B
    values = [v for ys in series.values() for v in ys if v is not None and v > 0]
    if not values:
        return
    lo, hi = (min(values) / 1.5, max(values) * 1.5) if log else (0.0, max(values) * 1.1)
    ticks = nice_ticks(lo, hi, log)
    lo, hi = (ticks[0], ticks[-1])

    def sx(i):
        return L + (pw * i / (len(xs) - 1) if len(xs) > 1 else pw / 2)

    def sy(v):
        if log:
            return T + ph * (1 - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))
        return T + ph * (1 - (v - lo) / (hi - lo))

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
           f'font-family="Helvetica, Arial, sans-serif" font-size="13">',
           f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
           f'<text x="{L}" y="28" font-size="16" font-weight="bold" fill="#111">{title}</text>']
    for t in ticks:
        y = sy(t)
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{L + pw}" y2="{y:.1f}" stroke="#e5e5e5"/>')
        out.append(f'<text x="{L - 8}" y="{y + 4:.1f}" text-anchor="end" fill="#444">{y_fmt(t)}</text>')
    for i, x in enumerate(xs):
        out.append(f'<text x="{sx(i):.1f}" y="{T + ph + 20}" text-anchor="middle" fill="#444">{x}</text>')
    out.append(f'<line x1="{L}" y1="{T + ph}" x2="{L + pw}" y2="{T + ph}" stroke="#888"/>')
    out.append(f'<text x="{L + pw / 2}" y="{H - 15}" text-anchor="middle" fill="#222">{x_label}</text>')
    out.append(f'<text transform="translate(18,{T + ph / 2}) rotate(-90)" text-anchor="middle" fill="#222">{y_label}</text>')

    for n, (name, ys) in enumerate(series.items()):
        colour = COLOURS.get(name, "#777")
        dash = ' stroke-dasharray="6 4"' if name in DASHED else ""
        pts = [(sx(i), sy(v)) for i, v in enumerate(ys) if v is not None and v > 0]
        if len(pts) > 1:
            d = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
            out.append(f'<polyline points="{d}" fill="none" stroke="{colour}" stroke-width="2.5"{dash}/>')
        for x, y in pts:
            out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{colour}"/>')
        ly = T + 10 + n * 22
        out.append(f'<line x1="{L + pw + 15}" y1="{ly}" x2="{L + pw + 40}" y2="{ly}" stroke="{colour}" stroke-width="2.5"{dash}/>')
        out.append(f'<text x="{L + pw + 46}" y="{ly + 4}" fill="#222">{name}</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def mops(v):
    return f"{v / 1e6:g}"


def ns_axis(v):
    return fmt_ns(v).replace(".0 ", " ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--out", default="docs/img")
    ap.add_argument("--tables", default=None, help="write Markdown tables of the medians here")
    args = ap.parse_args()
    rows = load(args.csv)
    os.makedirs(args.out, exist_ok=True)
    md = []

    tp = medians([r for r in rows if r["mode"] == "throughput"],
                 ("experiment", "queue", "producers", "consumers", "capacity"), "ops_per_sec")
    lat_rows = [r for r in rows if r["mode"] == "latency"]
    lat = {q: medians(lat_rows, ("experiment", "queue", "producers", "consumers", "capacity"), f)
           for q, f in (("p50", "p50_ns"), ("p99", "p99_ns"), ("p999", "p999_ns"))}
    rate = medians(lat_rows, ("experiment", "producers", "consumers", "capacity"), "target_rate")
    sat = defaultdict(int)
    for r in lat_rows:
        sat[(r["experiment"], r["queue"], r["producers"], r["consumers"], r["capacity"])] += int(r["saturated"])

    def queues(exp):
        seen = []
        for r in rows:
            if r["experiment"] == exp and r["queue"] not in seen:
                seen.append(r["queue"])
        return seen

    if any(r["experiment"] == "mpsc" for r in rows):
        ps = sorted({int(r["producers"]) for r in rows if r["experiment"] == "mpsc"})
        qs = queues("mpsc")
        cap = next(r["capacity"] for r in rows if r["experiment"] == "mpsc")
        key = lambda q, p: ("mpsc", q, str(p), "1", cap)
        line_chart(f"{args.out}/mpsc_throughput.svg", "MPSC throughput (1 consumer)", "producers",
                   "million items / sec", ps, {q: [tp.get(key(q, p)) for p in ps] for q in qs}, y_fmt=mops)
        for pct, label in (("p99", "p99"), ("p999", "p99.9")):
            line_chart(f"{args.out}/mpsc_latency_{pct}.svg", f"MPSC {label} latency (log scale)", "producers",
                       "push to pop", ps, {q: [lat[pct].get(key(q, p)) for p in ps] for q in qs},
                       log=True, y_fmt=ns_axis)
        md.append("### MPSC: throughput (million items/sec, median)\n")
        md.append("| producers | " + " | ".join(qs) + " |")
        md.append("|---|" + "---|" * len(qs))
        for p in ps:
            md.append(f"| {p} | " + " | ".join(f"{tp[key(q, p)] / 1e6:.1f}" for q in qs) + " |")
        md.append("\n### MPSC: latency at a fixed offered load (median of runs)\n")
        md.append("| producers | load | queue | p50 | p99 | p99.9 |")
        md.append("|---|---|---|---|---|---|")
        for p in ps:
            for q in qs:
                k = key(q, p)
                flag = " (saturated)" if sat[k] else ""
                md.append(f"| {p} | {rate[('mpsc', str(p), '1', cap)] / 1e6:.2f} M/s | {q}{flag} | "
                          f"{fmt_ns(lat['p50'][k])} | {fmt_ns(lat['p99'][k])} | {fmt_ns(lat['p999'][k])} |")
        md.append("")

    if any(r["experiment"] == "spsc" for r in rows):
        caps = sorted({int(r["capacity"]) for r in rows if r["experiment"] == "spsc"})
        qs = queues("spsc")
        md.append("### SPSC (1 producer, 1 consumer)\n")
        md.append("| capacity | queue | throughput | p50 | p99 | p99.9 |")
        md.append("|---|---|---|---|---|---|")
        for c in caps:
            for q in qs:
                k = ("spsc", q, "1", "1", str(c))
                md.append(f"| {c} | {q} | {tp[k] / 1e6:.1f} M/s | {fmt_ns(lat['p50'][k])} | "
                          f"{fmt_ns(lat['p99'][k])} | {fmt_ns(lat['p999'][k])} |")
        md.append("")

    if any(r["experiment"] == "mpmc" for r in rows):
        pcs = sorted({(int(r["producers"]), int(r["consumers"])) for r in rows if r["experiment"] == "mpmc"})
        qs = queues("mpmc")
        cap = next(r["capacity"] for r in rows if r["experiment"] == "mpmc")
        labels = [f"{p}x{c}" for p, c in pcs]
        key = lambda q, p, c: ("mpmc", q, str(p), str(c), cap)
        line_chart(f"{args.out}/mpmc_throughput.svg", "MPMC throughput (producers x consumers)",
                   "producers x consumers", "million items / sec", labels,
                   {q: [tp.get(key(q, p, c)) for p, c in pcs] for q in qs}, y_fmt=mops)
        line_chart(f"{args.out}/mpmc_latency_p99.svg", "MPMC p99 latency (log scale)",
                   "producers x consumers", "push to pop", labels,
                   {q: [lat["p99"].get(key(q, p, c)) for p, c in pcs] for q in qs}, log=True, y_fmt=ns_axis)
        md.append("### MPMC (BoundedMpmcQueue vs mutex)\n")
        md.append("| P x C | queue | throughput | p50 | p99 | p99.9 |")
        md.append("|---|---|---|---|---|---|")
        for p, c in pcs:
            for q in qs:
                k = key(q, p, c)
                flag = " (saturated)" if sat[k] else ""
                md.append(f"| {p}x{c} | {q}{flag} | {tp[k] / 1e6:.1f} M/s | {fmt_ns(lat['p50'][k])} | "
                          f"{fmt_ns(lat['p99'][k])} | {fmt_ns(lat['p999'][k])} |")
        md.append("")

    if any(r["experiment"] == "stack" for r in rows):
        ts = sorted({int(r["producers"]) for r in rows if r["experiment"] == "stack"})
        qs = queues("stack")
        key = lambda q, t: ("stack", q, str(t), str(t), "0")
        line_chart(f"{args.out}/stack_throughput.svg", "Stack throughput (50% push / 50% pop)", "threads",
                   "million ops / sec", ts, {q: [tp.get(key(q, t)) for t in ts] for q in qs}, y_fmt=mops)
        md.append("### Stacks (million ops/sec, median)\n")
        md.append("| threads | " + " | ".join(qs) + " |")
        md.append("|---|" + "---|" * len(qs))
        for t in ts:
            md.append(f"| {t} | " + " | ".join(f"{tp[key(q, t)] / 1e6:.1f}" for q in qs) + " |")
        md.append("")

    text = "\n".join(md)
    if args.tables:
        with open(args.tables, "w") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
