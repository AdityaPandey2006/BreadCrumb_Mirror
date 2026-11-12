#!/usr/bin/env python3
"""Regenerate every Step-10 figure and summary table from the raw CSVs.

    ~/bc10/venv/bin/python bench/plot.py --res bench/results --out bench/figures

Inputs: micro.csv, cal.csv, conc.csv, res.csv (written by bench/run-bench.py).
Outputs: fig*.png + fig*.svg, summary-micro.csv, summary-conc.csv.

Statistics: cycles are converted to time with the calibration of the SAME
boot (ns per TSC cycle). Iteration 0 of every data point in every boot is a
cold-cache warm-up: it is excluded from the plotted median/IQR and reported
separately in summary-micro.csv (column warm_median_ms). Points are medians
with interquartile ranges (25th-75th percentile); n is the number of
iterations used. Attempts whose status is not "ok" (short write, failure)
are never mixed into successful timings: they are drawn with their own
hollow markers and labelled with the bytes actually written.
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Validated reference palette (dataviz skill, light mode), fixed by role.
COL = {"ord": "#2a78d6", "commit": "#eb6834", "abort": "#1baf7a", "orig": "#52514e"}
MARK = {"ord": "o", "commit": "s", "abort": "^", "orig": "D"}
LABEL = {"orig": "original xv6 (baseline)", "ord": "modified kernel, ordinary",
         "commit": "speculative + commit", "abort": "speculative + abort"}
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"
CONFIG_NOTE = "x86 xv6 in QEMU TCG, 1 CPU; NSHADOW=8, LOGSIZE=30, NBUF=30"

plt.rcParams.update({
    "font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2,
    "ytick.color": INK2, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 2,
    "legend.frameon": False, "figure.dpi": 110, "savefig.bbox": "tight", "axes.axisbelow": True,
})


def q(vals, p):
    vals = sorted(vals)
    if len(vals) == 1:
        return vals[0]
    return statistics.quantiles(vals, n=4, method="inclusive")[{25: 0, 50: 1, 75: 2}[p]]


def load(res):
    cal = {}
    for r in csv.DictReader(open(res / "cal.csv")):
        if not r["boot"].endswith("-after") and not r["boot"].startswith("conc"):
            cal[(r["config"], r["cpus"], r["boot"])] = float(r["ns_per_cycle"])
    cal_conc = {}
    for r in csv.DictReader(open(res / "cal.csv")):
        if r["boot"].startswith("conc"):
            cal_conc[(r["config"], r["cpus"], r["boot"][4:])] = float(r["ns_per_cycle"])
    micro = list(csv.DictReader(open(res / "micro.csv")))
    for r in micro:
        ns = cal[(r["config"], r["cpus"], r["boot"])]
        for k in ("cyc_begin", "cyc_op", "cyc_complete", "cyc_total"):
            r[k.replace("cyc_", "ms_")] = int(r[k]) * ns / 1e6
    conc = list(csv.DictReader(open(res / "conc.csv"))) if (res / "conc.csv").exists() else []
    for r in conc:
        if r["kind"] == "run":
            r["seconds"] = int(r["cycles"]) * cal_conc[(r["config"], r["cpus"], r["boot"])] / 1e9
    resl = list(csv.DictReader(open(res / "res.csv"))) if (res / "res.csv").exists() else []
    return micro, conc, resl, cal


def series_key(r):
    return "orig" if r["config"] == "orig" else r["mode"]


def summarize(micro):
    groups = defaultdict(list)
    for r in micro:
        groups[(r["workload"], series_key(r), int(r["param"]))].append(r)
    out = {}
    for key, rows in groups.items():
        warm = [r for r in rows if r["iter"] == "0"]
        steady = [r for r in rows if r["iter"] != "0"]
        ok = [r for r in steady if r["status"] == "ok"]
        bad = [r for r in steady if r["status"] != "ok"]
        s = {"n_total": len(steady), "n_ok": len(ok), "n_fail": len(bad),
             "statuses": sorted({r["status"] for r in bad}),
             "bytes_fail": sorted({int(r["bytes"]) for r in bad}),
             "warm_median_ms": statistics.median(r["ms_total"] for r in warm) if warm else None}
        use = ok if ok else bad          # failed points are summarized separately
        for ph in ("total", "begin", "op", "complete"):
            v = [r["ms_" + ph] for r in use]
            s[ph] = (q(v, 25), q(v, 50), q(v, 75)) if v else None
        s["failed_point"] = not ok
        out[key] = s
    return out


def write_summary(summ, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["workload", "series", "param", "n_used", "n_ok", "n_failed", "fail_status",
                    "bytes_written_when_failed", "median_ms", "q1_ms", "q3_ms",
                    "median_begin_ms", "median_op_ms", "median_complete_ms", "warm_median_ms"])
        for (wl, se, p), s in sorted(summ.items()):
            t = s["total"]
            w.writerow([wl, se, p, s["n_total"], s["n_ok"], s["n_fail"], "|".join(s["statuses"]),
                        "|".join(map(str, s["bytes_fail"])), f"{t[1]:.4f}", f"{t[0]:.4f}",
                        f"{t[2]:.4f}", f"{s['begin'][1]:.4f}", f"{s['op'][1]:.4f}",
                        f"{s['complete'][1]:.4f}",
                        "" if s["warm_median_ms"] is None else f"{s['warm_median_ms']:.4f}"])


def save(fig, out, name):
    fig.savefig(out / f"{name}.png")
    fig.savefig(out / f"{name}.svg")
    plt.close(fig)


def size_plot(ax, summ, workload, unit_bytes, cap_bytes, cap_label, xlabel):
    for se in ("orig", "ord", "commit", "abort"):
        pts = sorted((p, s) for (w, s_, p), s in summ.items() if w == workload and s_ == se)
        if not pts:
            continue
        good = [(p * unit_bytes / 1024, s["total"]) for p, s in pts if not s["failed_point"]]
        bad = [(p * unit_bytes / 1024, s) for p, s in pts if s["failed_point"]]
        if good:
            xs = [x for x, _ in good]
            ax.plot(xs, [t[1] for _, t in good], color=COL[se], marker=MARK[se], markersize=5,
                    linestyle="--" if se == "orig" else "-", label=LABEL[se], zorder=3)
            ax.fill_between(xs, [t[0] for _, t in good], [t[2] for _, t in good],
                            color=COL[se], alpha=0.15, linewidth=0)
        if bad:
            ax.scatter([x for x, _ in bad], [s["total"][1] for _, s in bad], s=46,
                       facecolors="white", edgecolors=COL[se], marker=MARK[se], linewidths=1.6,
                       zorder=4, label=f"{LABEL[se]}: FAILED attempt (short write)")
    ax.axvline(cap_bytes / 1024, color=INK2, linewidth=1, linestyle=":")
    ax.text(cap_bytes / 1024, ax.get_ylim()[1], f" {cap_label}", color=INK2, va="top", fontsize=8)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ticks = [0.5, 1, 2, 4, 8, 16, 32, 64]
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"{t:g}" for t in ticks])
    ax.set_xlabel(xlabel)
    ax.set_ylabel("time per attempt, ms (median, IQR band)")


def fig_sizes(summ, out):
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    size_plot(ax, summ, "overwrite", 512, 4096, "8-block cap (4 KiB)", "bytes overwritten in an existing file, KiB")
    ax.set_title("Write cost inside and outside a speculation (overwrite of an existing file)",
                 loc="left", fontsize=10, color=INK)
    ax.legend(loc="upper left", bbox_to_anchor=(1.01, 1), fontsize=7.5)
    fig.text(0, -0.04, CONFIG_NOTE + ". Hollow markers: the speculative write stopped at the "
             "shadow cap (returned 4096 bytes); they are failed attempts, not timings of the full size.",
             fontsize=7, color=INK2, wrap=True)
    save(fig, out, "fig1-overwrite-size-cliff")

    fig, axes = plt.subplots(1, 2, figsize=(10, 4.2), sharey=True)
    size_plot(axes[0], summ, "create", 512, 2560, "cap: 5 data + 3 metadata blocks", "bytes written to a NEW file, KiB")
    axes[0].set_title("Create + write", loc="left", fontsize=10)
    size_plot(axes[1], summ, "extend", 512, 3072, "cap: 6 data + 2 metadata blocks", "bytes appended to a 1-block file, KiB")
    axes[1].set_title("Extend existing file", loc="left", fontsize=10)
    axes[1].set_ylabel("")
    axes[1].legend(loc="upper left", bbox_to_anchor=(1.01, 1), fontsize=7)
    fig.text(0, -0.04, CONFIG_NOTE + ". Metadata (inode, bitmap, directory, indirect) blocks share "
             "the 8-block cap, so the cliff comes earlier than for overwrites.", fontsize=7, color=INK2)
    save(fig, out, "fig2-create-extend-cliff")


def fig_phases(summ, out):
    ks = [k for k in range(1, 9) if ("overwrite", "commit", k) in summ]
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.8), sharey=True)
    for ax, mode in zip(axes, ("commit", "abort")):
        bottom = [0] * len(ks)
        for ph, colr, lab in (("begin", "#6250d6", "xbegin"), ("op", COL["ord"], "open+write+close"),
                              ("complete", COL[mode], "xcommit" if mode == "commit" else "xabort")):
            vals = [summ[("overwrite", mode, k)][ph][1] for k in ks]
            ax.bar(ks, vals, bottom=bottom, color=colr, width=0.7, edgecolor="white",
                   linewidth=1.5, label=lab)
            bottom = [b + v for b, v in zip(bottom, vals)]
        ordv = [summ[("overwrite", "ord", k)]["total"][1] for k in ks]
        ax.plot(ks, ordv, color=INK, linestyle="--", linewidth=1.4, marker="o", markersize=3,
                label="ordinary write (same blocks)")
        ax.set_xlabel("distinct blocks overwritten (= shadow entries)")
        ax.set_title(f"Phase breakdown, speculative + {mode}", loc="left", fontsize=10)
        ax.legend(fontsize=7, loc="upper left")
    axes[0].set_ylabel("median time per attempt, ms")
    fig.text(0, -0.05, CONFIG_NOTE + ". Stacked medians of each phase (median of sums may differ "
             "slightly from sum of medians).", fontsize=7, color=INK2)
    save(fig, out, "fig3-phase-breakdown")


def bar_group(ax, summ, workload, params, series, fmt):
    width = 0.8 / len(series)
    for i, se in enumerate(series):
        xs, ys, lo, hi, fails = [], [], [], [], []
        for j, p in enumerate(params):
            s = summ.get((workload, se, p))
            if not s:
                continue
            t = s["total"]
            x = j + (i - (len(series) - 1) / 2) * width
            xs.append(x); ys.append(t[1]); lo.append(t[1] - t[0]); hi.append(t[2] - t[1])
            if s["failed_point"]:
                fails.append((x, t[1], s["bytes_fail"]))
        ax.bar(xs, ys, width=width * 0.92, color=COL[se], label=LABEL[se],
               yerr=[lo, hi], error_kw={"elinewidth": 0.8, "ecolor": INK2, "capsize": 2})
        for x, y, b in fails:
            ax.annotate(f"short:\n{b[0]} B", (x, y), fontsize=6.5, color=INK2, ha="center",
                        va="bottom", xytext=(0, 4), textcoords="offset points")
    ax.set_xticks(range(len(params)))
    ax.set_xticklabels([fmt(p) for p in params])


def fig_overheads(summ, out):
    fig, ax = plt.subplots(figsize=(7, 3.6))
    items = [("null", "orig"), ("null", "ord"), ("empty", "commit"), ("empty", "abort"),
             ("unlink", "orig"), ("unlink", "ord"), ("unlink", "commit"), ("unlink", "abort")]
    labels, ys, lo, hi, cols = [], [], [], [], []
    for w, se in items:
        s = summ.get((w, se, 0)) or summ.get((w, se, 1))
        if not s:
            continue
        t = s["total"]
        labels.append(("getpid()" if w == "null" else "begin + empty " + se if w == "empty"
                       else "unlink") + ("" if w == "empty" else f"\n{LABEL[se]}"))
        ys.append(t[1] * 1000); lo.append((t[1] - t[0]) * 1000); hi.append((t[2] - t[1]) * 1000)
        cols.append(COL[se])
    ax.barh(range(len(ys)), ys, color=cols, xerr=[lo, hi],
            error_kw={"elinewidth": 0.8, "ecolor": INK2, "capsize": 2})
    ax.set_yticks(range(len(ys)))
    ax.set_yticklabels(labels, fontsize=7)
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xlabel("time per attempt, µs (median, IQR whiskers; log scale)")
    ax.set_title("Fixed costs: syscall, empty speculation, speculative unlink", loc="left", fontsize=10)
    for i, y in enumerate(ys):
        ax.text(y + hi[i], i, f"   {y:,.0f} µs", va="center", fontsize=7, color=INK)
    ax.set_xlim(right=max(y + h for y, h in zip(ys, hi)) * 4)
    fig.text(0, -0.06, CONFIG_NOTE, fontsize=7, color=INK2)
    save(fig, out, "fig4-fixed-costs")


def fig_output(summ, out):
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.8))
    bar_group(axes[0], summ, "console", [64, 1024, 4096, 6144], ["orig", "ord", "commit", "abort"], lambda p: f"{p} B")
    axes[0].set_title("Console write", loc="left", fontsize=10)
    axes[0].set_yscale("log")
    axes[0].set_ylabel("time per attempt, ms (median, IQR)")
    bar_group(axes[1], summ, "pipe_ext", [64, 512, 2048, 4000], ["orig", "ord", "commit", "abort"], lambda p: f"{p} B")
    axes[1].set_title("Pipe to an OUTSIDE reader", loc="left", fontsize=10)
    axes[1].set_yscale("log")
    bar_group(axes[2], summ, "pipe_int", [64, 256, 512], ["orig", "ord", "commit", "abort"], lambda p: f"{p} B")
    axes[2].set_title("Pipe INSIDE the speculation", loc="left", fontsize=10)
    axes[2].set_yscale("log")
    axes[1].legend(fontsize=7, loc="upper left")
    fig.text(0, -0.06, CONFIG_NOTE + ". Console times include the emulated UART/CGA output of every "
             "released byte (serial captured to a file, not a terminal). Held output is capped at "
             "4096 bytes per speculation (D34/D41): larger writes are short.", fontsize=7, color=INK2)
    save(fig, out, "fig5-console-pipes")


def fig_conc(conc, out):
    runs = [r for r in conc if r["kind"] == "run"]
    workers = [r for r in conc if r["kind"] == "worker"]
    if not runs:
        return
    cpus_list = sorted({int(r["cpus"]) for r in runs})
    fig, axes = plt.subplots(2, len(cpus_list), figsize=(3.4 * len(cpus_list), 6), sharey="row")
    rows_out = []
    for ci, cpus in enumerate(cpus_list):
        for ri, shared in enumerate(("0", "1")):
            ax = axes[ri][ci]
            for se in ("orig", "ord", "commit"):
                cfg, mode = ("orig", "ord") if se == "orig" else ("normal", se)
                pts = defaultdict(list)
                for r in runs:
                    if r["config"] == cfg and r["mode"] == mode and r["shared"] == shared and int(r["cpus"]) == cpus:
                        com = sum(int(w["commits"]) for w in workers if w["config"] == cfg and w["mode"] == mode
                                  and w["shared"] == shared and w["cpus"] == r["cpus"] and w["boot"] == r["boot"]
                                  and w["P"] == r["P"])
                        pts[int(r["P"])].append(com / r["seconds"])
                if not pts:
                    continue
                ps = sorted(pts)
                med = [statistics.median(pts[p]) for p in ps]
                ax.plot(ps, med, color=COL[se], marker=MARK[se], markersize=5,
                        linestyle="--" if se == "orig" else "-", label=LABEL[se])
                for p in ps:
                    ax.scatter([p] * len(pts[p]), pts[p], color=COL[se], s=8, alpha=0.5)
                    rows_out.append([cpus, shared, se, p, len(pts[p]), f"{statistics.median(pts[p]):.2f}"])
            ax.set_xscale("log", base=2)
            ax.set_xticks([1, 2, 4, 8]); ax.set_xticklabels(["1", "2", "4", "8"])
            ax.set_title(f"{cpus} CPU{'s' if cpus > 1 else ''}, {'same block' if shared == '1' else 'own file each'}",
                         loc="left", fontsize=9)
            if ci == 0:
                ax.set_ylabel("successful writes per second")
            if ri == 1:
                ax.set_xlabel("concurrent processes P (20 attempts each)")
    axes[0][0].legend(fontsize=7)
    fig.suptitle("Throughput under concurrency (dots: individual boots; line: median)", x=0.01,
                 ha="left", fontsize=10)
    fig.text(0, -0.03, "x86 xv6 in QEMU TCG; NSHADOW=8. Each attempt overwrites one 512-byte block; "
             "speculative attempts are begin + write + commit, conflicts are aborted and not retried.",
             fontsize=7, color=INK2)
    save(fig, out, "fig6-concurrency-throughput")

    # conflict rate, shared block, speculative mode
    fig, ax = plt.subplots(figsize=(6, 3.6))
    srow = []
    for i, cpus in enumerate(cpus_list):
        rates = defaultdict(list)
        for r in runs:
            if r["config"] == "normal" and r["mode"] == "commit" and int(r["cpus"]) == cpus:
                ws = [w for w in workers if w["config"] == "normal" and w["mode"] == "commit"
                      and w["cpus"] == r["cpus"] and w["boot"] == r["boot"] and w["P"] == r["P"]
                      and w["shared"] == r["shared"]]
                att = sum(int(w["commits"]) + int(w["conflicts"]) for w in ws)
                rates[(r["shared"], int(r["P"]))].append(100 * sum(int(w["conflicts"]) for w in ws) / att)
        for shared, ls in (("1", "-"), ("0", ":")):
            ps = sorted(p for s, p in rates if s == shared)
            if ps:
                ax.plot([p * (1 + 0.04 * (i - 1)) for p in ps],   # small x offset: identical values stay visible
                        [statistics.median(rates[(shared, p)]) for p in ps],
                        color=[COL["ord"], COL["commit"], COL["abort"]][i % 3], linestyle=ls,
                        marker=["o", "s", "^"][i % 3], markersize=5, label=f"{cpus} CPU{'s' if cpus > 1 else ''}, "
                                            f"{'same block' if shared == '1' else 'own file each'}")
                for p in ps:
                    srow.append([cpus, shared, p, f"{statistics.median(rates[(shared, p)]):.1f}"])
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8]); ax.set_xticklabels(["1", "2", "4", "8"])
    ax.set_xlabel("concurrent speculative processes P")
    ax.set_ylabel("commits refused as conflicts, % of attempts")
    ax.set_title("Conflict rate (first committer wins)", loc="left", fontsize=10)
    ax.legend(fontsize=7)
    sh = [w for w in workers if w["config"] == "normal" and w["mode"] == "commit" and w["shared"] == "1"]
    tot_att = sum(int(w["commits"]) + int(w["conflicts"]) for w in sh)
    tot_con = sum(int(w["conflicts"]) for w in sh)
    fig.text(0, -0.1, f"Median over 3 boots; markers offset slightly in x so equal values stay visible. "
             f"Same-block total: {tot_con} conflicts in {tot_att} attempts. Low because the coarse "
             "filesystem gate and very short attempts serialize most work. 'Own file each' could "
             "conflict only through shared metadata; overwrites change none.", fontsize=7, color=INK2,
             wrap=True)
    save(fig, out, "fig7-conflict-rate")
    with open(out / "summary-conc.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cpus", "shared", "series", "P", "boots", "median_writes_per_s"])
        w.writerows(rows_out)
        w.writerow([])
        w.writerow(["cpus", "shared", "P", "median_conflict_pct"])
        w.writerows(srow)


def fig_res(resl, out):
    if not resl:
        return
    fig, ax = plt.subplots(figsize=(9, 3.8))
    unit = {"overwrite": "blk", "create": "blk", "extend": "blk", "console": "B", "pipe_ext": "B"}
    labels = [f"{r['workload'].replace('_ext', ' (outside)')} {r['param']} {unit.get(r['workload'], '')}".strip()
              for r in resl]
    held = [int(r["pages_held"]) for r in resl]
    # filesystem attempts hold one page per shadow entry; output pages are separate
    cols = [COL["commit"] if r["workload"] not in ("console", "pipe_ext") else COL["ord"] for r in resl]
    ax.bar(range(len(held)), held, color=cols, width=0.7)
    for i in range(len(held)):
        ax.text(i, held[i], f"{held[i]}", ha="center", va="bottom", fontsize=7, color=INK)
    from matplotlib.patches import Patch
    ax.legend(handles=[Patch(color=COL["commit"], label="shadow entries (1 page each, cap 8)"),
                       Patch(color=COL["ord"], label="held-output page (console / outside pipe)")],
              fontsize=7, loc="upper right")
    ax.set_xticks(range(len(held)))
    ax.set_xticklabels(labels, rotation=60, ha="right", fontsize=7)
    ax.set_ylabel("4 KiB pages held during the attempt")
    ax.set_title("Kernel memory held by one live speculative attempt", loc="left", fontsize=10)
    leaked = sum(int(r["pages_not_returned"]) for r in resl)
    ax.set_ylim(0, 10)
    fig.text(0, -0.2, "Fault-injection build (free-page counter, ST_FREEPAGES); attempt measured live, then "
             "aborted. 'blk' = 512-byte blocks written. Pages not returned after abort, summed over "
             f"all cases: {leaked}.", fontsize=7, color=INK2)
    save(fig, out, "fig8-memory-per-attempt")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--res", type=Path, default=Path("bench/results"))
    ap.add_argument("--out", type=Path, default=Path("bench/figures"))
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    micro, conc, resl, _ = load(a.res)
    summ = summarize(micro)
    write_summary(summ, a.out / "summary-micro.csv")
    fig_sizes(summ, a.out)
    fig_phases(summ, a.out)
    fig_overheads(summ, a.out)
    fig_output(summ, a.out)
    fig_conc(conc, a.out)
    fig_res(resl, a.out)
    print("figures written to", a.out)


if __name__ == "__main__":
    main()
