#!/usr/bin/env python3
"""Collect Step-10 measurements from staged builds; write raw CSV + serial logs.

Run from the repository root inside WSL/Linux after staging the builds:
    sh bench/stage.sh orig   ~/bc10/orig
    sh bench/stage.sh normal ~/bc10/normal
    sh bench/stage.sh test   ~/bc10/test
    python3 bench/run-bench.py --orig ~/bc10/orig --normal ~/bc10/normal \
        --test ~/bc10/test --out bench/results

Outputs (all raw, nothing aggregated):
    micro.csv  one row per timed iteration (cycles per phase)
    cal.csv    TSC calibration per boot (guest cycles vs host wall time)
    conc.csv   concurrency runs (per worker and per run)
    res.csv    pages per speculative attempt (fault-injection build only)
    serial/    raw QEMU serial logs of every boot
    STAGE-*.txt  copies of each build's STAGE-INFO.txt
"""
import argparse
import csv
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest import Guest, DEFAULT_QEMU  # noqa: E402

MICRO_COLS = ["config", "cpus", "boot", "workload", "mode", "param", "iter", "status",
              "bytes", "cyc_begin", "cyc_op", "cyc_complete", "cyc_total"]
CAL_COLS = ["config", "cpus", "boot", "ticks", "cycles", "host_seconds", "ns_per_cycle",
            "ms_per_tick"]
CONC_COLS = ["config", "cpus", "boot", "kind", "P", "R", "shared", "mode", "worker",
             "commits", "conflicts", "other", "cycles", "ticks"]
RES_COLS = ["config", "workload", "param", "op_result", "abort_result", "shadow_entries",
            "pages_held", "pages_not_returned"]


def calibrate(g, ticks):
    g.events.clear()
    out = g.run(f"spbench cal {ticks}", timeout=120)
    marks = {}
    for e in g.events:              # first arrival of each marker line
        marks.setdefault(e[1].split(",")[0], e)
    if "CAL0" not in marks or "CAL1" not in marks:
        raise RuntimeError("calibration markers missing: " + out)
    f = marks["CAL1"][1].split(",")
    cycles = (int(f[2]) << 32) | int(f[3])
    secs = marks["CAL1"][0] - marks["CAL0"][0]
    return {"ticks": ticks, "cycles": cycles, "host_seconds": f"{secs:.6f}",
            "ns_per_cycle": f"{secs * 1e9 / cycles:.6f}",
            "ms_per_tick": f"{secs * 1000 / ticks:.4f}"}


def parse(out, prefix):
    return [line.split(",")[1:] for line in out.split("\n") if line.startswith(prefix + ",")]


def boot(args, build, name, cpus):
    args.serial.mkdir(parents=True, exist_ok=True)
    log = (args.serial / f"{name}.txt").open("wb")
    return Guest(build, qemu=args.qemu, cpus=cpus, log=log), log


def micro(args, writer, calw):
    for config, build in (("orig", args.orig), ("normal", args.normal)):
        if not build:
            continue
        for b in range(1, args.boots + 1):
            g, log = boot(args, build, f"micro-{config}-cpu1-boot{b}", 1)
            try:
                cal = calibrate(g, args.cal_ticks)
                calw.writerow({"config": config, "cpus": 1, "boot": b, **cal})
                t0 = time.monotonic()
                out = g.run(f"spbench micro {args.iters}", timeout=7200)
                if "MICRO done" not in out or "FAIL" in out:
                    raise RuntimeError(f"micro run failed ({config} boot {b})")
                cal2 = calibrate(g, args.cal_ticks)
                calw.writerow({"config": config, "cpus": 1, "boot": f"{b}-after", **cal2})
                rows = parse(out, "CSV")
                for r in rows:
                    writer.writerow(dict(zip(MICRO_COLS, [config, 1, b] + r)))
                print(f"micro {config} boot {b}: {len(rows)} rows, {time.monotonic() - t0:.0f}s",
                      flush=True)
            finally:
                g.stop()
                log.close()


def conc(args, writer, calw):
    for config, build in (("orig", args.orig), ("normal", args.normal)):
        if not build:
            continue
        modes = ["ord"] if config == "orig" else ["ord", "commit"]
        for cpus in args.conc_cpus:
            for b in range(1, args.conc_boots + 1):
                g, log = boot(args, build, f"conc-{config}-cpu{cpus}-boot{b}", cpus)
                try:
                    cal = calibrate(g, args.cal_ticks)
                    calw.writerow({"config": config, "cpus": cpus, "boot": f"conc{b}", **cal})
                    for P in args.workers:
                        for shared in (0, 1):
                            for mode in modes:
                                out = g.run(f"spbench conc {P} {args.attempts} {shared} {mode}",
                                            timeout=3600)
                                for r in parse(out, "CONCW"):
                                    writer.writerow(dict(zip(CONC_COLS, [config, cpus, b, "worker"]
                                                             + [r[0], args.attempts] + r[1:] + [""])))
                                for r in parse(out, "CONC"):
                                    writer.writerow(dict(zip(CONC_COLS, [config, cpus, b, "run",
                                                             r[0], r[1], r[2], r[3], "", "", "", "",
                                                             r[4], r[5]])))
                    print(f"conc {config} cpus {cpus} boot {b} done", flush=True)
                finally:
                    g.stop()
                    log.close()


def res(args, writer):
    if not args.test:
        return
    g, log = boot(args, args.test, "res-test-cpu1", 1)
    try:
        out = g.run("spbench res", timeout=1800)
        for r in parse(out, "RES"):
            writer.writerow(dict(zip(RES_COLS, ["test"] + r)))
        print("res done", flush=True)
    finally:
        g.stop()
        log.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--qemu", default=DEFAULT_QEMU)
    p.add_argument("--orig", type=Path)
    p.add_argument("--normal", type=Path)
    p.add_argument("--test", type=Path)
    p.add_argument("--out", type=Path, default=Path("bench/results"))
    p.add_argument("--boots", type=int, default=5)
    p.add_argument("--iters", type=int, default=50)
    p.add_argument("--cal-ticks", type=int, default=300)
    p.add_argument("--conc-cpus", type=int, nargs="+", default=[1, 2, 4])
    p.add_argument("--conc-boots", type=int, default=3)
    p.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4, 8])
    p.add_argument("--attempts", type=int, default=20)
    p.add_argument("--only", choices=["micro", "conc", "res"], nargs="+",
                   default=["micro", "conc", "res"])
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    args.serial = args.out / "serial"
    for name, build in (("orig", args.orig), ("normal", args.normal), ("test", args.test)):
        if build:
            shutil.copyfile(build / "STAGE-INFO.txt", args.out / f"STAGE-{name}.txt")
    calf = (args.out / "cal.csv").open("a", newline="")
    calw = csv.DictWriter(calf, CAL_COLS)
    if calf.tell() == 0:
        calw.writeheader()
    if "micro" in args.only:
        with (args.out / "micro.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, MICRO_COLS)
            w.writeheader()
            micro(args, w, calw)
    if "conc" in args.only:
        with (args.out / "conc.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, CONC_COLS)
            w.writeheader()
            conc(args, w, calw)
    if "res" in args.only:
        with (args.out / "res.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, RES_COLS)
            w.writeheader()
            res(args, w)
    calf.close()


if __name__ == "__main__":
    main()
