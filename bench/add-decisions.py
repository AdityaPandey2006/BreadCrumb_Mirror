#!/usr/bin/env python3
"""One-off helper (Step 10): append decisions D49-D62 and update O05 in
design/speculation-decisions.csv and .xlsx, keeping both files in step.
Idempotent: rows whose ID already exists are replaced, not duplicated."""
import copy
import csv
from pathlib import Path

import openpyxl

DESIGN = Path(__file__).resolve().parents[1] / "design"
S10 = "DECIDED - STEP 10"
ROWS = [
    ("D49", "Step 10: Timing source for measurements", S10,
     "User-mode rdtsc cycle counter per attempt (begin / operation / completion / total), converted with a per-boot calibration against host wall time (spbench cal 300 ticks).",
     "xv6 ticks are 10 ms; one attempt is often shorter. CR4.TSD is not set, so user rdtsc works; under QEMU TCG the guest TSC follows the host clock (measured 0.313 ns/cycle, 10.0 ms/tick on every boot). Ticks are used only as a cross-check. CSV lines are written with one write() outside speculation."),
    ("D50", "Step 10: Emulator configuration for evidence", S10,
     "QEMU TCG (no KVM), 512 MiB, serial to a pipe (not a terminal). Micro-benchmarks on 1 CPU; concurrency on 1, 2 and 4 CPUs; correctness on 1-4 CPUs.",
     "Absolute numbers are emulator numbers; compare ratios between configurations. Console costs include the emulated UART/CGA output of released bytes."),
    ("D51", "Step 10: Original-xv6 baseline", S10,
     "Commit fd7d454 (stock x86 xv6 + course add/hello syscalls) with FSSIZE raised 1000 -> 2000 and the portable spbench (ORIG_XV6). Labelled 'original xv6 (baseline)'.",
     "Modified kernel in ordinary mode is reported separately: it pays the filesystem gate and shadow-index checks, so it is not an original-xv6 baseline. bench/stage.sh orig builds it reproducibly."),
    ("D52", "Step 10: Samples and statistics", S10,
     "50 iterations per point x 5 independent boots (console 20 x 5); iteration 0 per boot is a warm-up reported separately; medians with interquartile ranges; n reported.",
     "Concurrency: 3 boots per CPU count, 20 attempts per process. Raw per-iteration rows kept in micro.csv / conc.csv."),
    ("D53", "Step 10: Workloads and failed attempts", S10,
     "getpid; empty speculation; overwrite 0.5-64 KiB; create+write 0-64 KiB; extend 0.5-8 KiB; unlink; console 64-6144 B; outside pipe 64-4000 B; internal pipe 64-512 B; concurrency P=1-8 own file vs same block; memory per attempt.",
     "Attempts that hit a cap (short write) are kept, labelled FAILED with the bytes actually written, drawn with hollow markers, and never averaged into successful timings. The capacity cliff is shown, not cropped."),
    ("D54", "Step 10: Resource-use measurement", S10,
     "Measure pages held by one live attempt only in the fault-injection build (ST_FREEPAGES), never during timing runs.",
     "Timings come only from production builds without test hooks."),
    ("D55", "Step 10: Additional fault injection and crash points", S10,
     "SHADOW_TEST only: ST_FAIL value selects shadow (0/1), held-console (2) or held-pipe (3) page; ST_FREEPAGES counts free pages; crash points 5-8 in the output-release window.",
     "Point 5 after durable commit, 6 after the first held pipe record, 7 before console release, 8 after 16 console bytes. Harness suite output-crash reboots the same fs.img: new filesystem always; lost/partial output documented, never replayed. Resolves the testing side of O05."),
    ("D56", "Step 10: Plotting tool", S10,
     "matplotlib 3.10.7 + numpy 2.5.4 in a virtual environment (bench/requirements.txt); bench/plot.py regenerates every PNG/SVG figure and summary table from raw CSV.",
     "One-time pip install; figures use the validated categorical palette with secondary encodings (marker shape, dash) and failure markers."),
    ("D57", "Step 10: Demonstration vehicle", S10,
     "Guest utility specrun (commit | abort | keep PATH; steps as comma-joined single arguments) plus bench/demo.py (20 checked demonstrations) and rule-based agent modes demo speculative / speculative-benign.",
     "The keep guard is a caller-supplied rule evaluated on the private view; the barrier contains effects but does not detect prompt injection. No LLM/API dependency. Command lines must fit xv6 sh's 100-byte buffer and 10-argument limit."),
    ("D58", "Step 10: GDB session log (handout requirement)", "DONE - STEP 10",
     "Record a real GDB session on defect D1 (root-directory hole), before and after the fix: tests/gdb-root-hole.sh.",
     "QEMU halted with a gdb stub; breakpoints on bmap() when root block 1 is unallocated and on fs_finish(). Stock mkfs: hole filled only in the cached inode at boot, re-filled after each abort (entries lost, spectest FAIL). Fixed mkfs: single normal extension via writei/dirlink, spectest PASS. Transcripts in design/evidence/step10/gdb/."),
    ("D59", "Step 10: Capacity configuration", S10,
     "Keep NSHADOW=8, MAXOPBLOCKS=10, LOGSIZE=30, NBUF=30 for all Step-10 evidence; R04 stays a planned upgrade.",
     "Every figure is labelled with the configuration."),
    ("D60", "Step 10: Defect D1 - root directory size covered an unallocated block", "IMPLEMENTED - STEP 10",
     "mkfs rounds the root size up to the blocks actually allocated and fails the build if the size covers an unallocated block.",
     "Stock rounding ((off/BSIZE)+1)*BSIZE created a hole with exactly 32 root entries; bmap filled it only in the cached inode (no iupdate), and inode_invalidate() on abort lost every entry in that block. Kernel lazy iupdate unchanged (limitation). Regression: tests/regress-root-hole.sh."),
    ("D61", "Step 10: Defect D2 - held output released while holding the filesystem gate", "IMPLEMENTED - STEP 10",
     "At the durable point only detach held pipe/console output; deliver it after fs_leave() with no gate, inode lock or spinlock held. Order unchanged: durable -> pipe -> console.",
     "Old code slept in the pipe drain holding the gate; an outside reader's console printf (begin_op) then waited for the gate: system-wide hang. xcommit still returns after release, so it waits for a slow outside reader. A kill during release drops the rest (partial delivery). Regression: spfailure drainfull."),
    ("D62", "Step 10: Defect D3 - timing-based ordering in spfiletest", "IMPLEMENTED - STEP 10",
     "Replace the outsider's sleep(2) with a probe that rewrites an original byte of the last copied block until D20 refuses it.",
     "Other older tests still order processes with fixed sleeps (a speculative process cannot signal an outsider without externalizing); none failed in Step-10 runs. Documented as a test limitation."),
]
O05 = ("O05", "Failure and crash semantics", "DOCUMENTED - STEP 10 (D26, D39, D44, D55, D61)",
       "Atomic file commit; output release follows durability and is not part of the disk log. Crash or kill after the durable point can lose or partially deliver held output; nothing replays it.",
       "Verified by crash points 1-4 (filesystem old/new) and 5-8 (filesystem new; console none or 16-byte prefix; pipe delivery observed). No exactly-once output is claimed.")


def update_csv():
    path = DESIGN / "speculation-decisions.csv"
    raw = path.read_bytes()
    eol = "\r\n" if b"\r\n" in raw else "\n"
    rows = list(csv.reader(raw.decode().splitlines()))
    index = {r[0]: i for i, r in enumerate(rows)}
    for row in [O05] + ROWS:
        if row[0] in index:
            rows[index[row[0]]] = list(row)
        else:
            rows.append(list(row))
    with path.open("w", newline="") as f:
        csv.writer(f, quoting=csv.QUOTE_ALL, lineterminator=eol).writerows(rows)


def update_xlsx():
    path = DESIGN / "speculation-decisions.xlsx"
    wb = openpyxl.load_workbook(path)
    ws = wb.worksheets[0]
    index = {ws.cell(row=r, column=1).value: r for r in range(1, ws.max_row + 1)}
    template = ws.max_row
    for row in [O05] + ROWS:
        r = index.get(row[0])
        if r is None:
            r = ws.max_row + 1
            for c in range(1, 6):
                src = ws.cell(row=template, column=c)
                dst = ws.cell(row=r, column=c)
                dst.font = copy.copy(src.font)
                dst.alignment = copy.copy(src.alignment)
                dst.border = copy.copy(src.border)
                dst.fill = copy.copy(src.fill)
                dst.number_format = src.number_format
        for c, v in enumerate(row, start=1):
            ws.cell(row=r, column=c).value = v
    wb.save(path)


if __name__ == "__main__":
    update_csv()
    update_xlsx()
    print("decisions updated")
