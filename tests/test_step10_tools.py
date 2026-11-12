"""Host regression checks: python3 -m unittest discover -s tests -p 'test_*.py'."""
import csv
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Step10ToolTests(unittest.TestCase):
    def test_matrix_exit_status_tracks_failed_runs(self):
        # Run the whole orchestration script with controlled suite outcomes.
        # No QEMU or long-running tests are launched by these stand-ins.
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            for name in ('tests', 'agent', 'bin', 'build'):
                (base / name).mkdir()
            shutil.copyfile(ROOT / 'tests/run-matrix.sh', base / 'tests/run-matrix.sh')
            (base / 'agent/test_dummy.py').write_text('import unittest\nclass Smoke(unittest.TestCase):\n    def test_ok(self): pass\n')
            (base / 'tests/regress-root-hole.sh').write_text('exit "$SUITE_EXIT"\n')
            timeout = base / 'bin/timeout'
            timeout.write_text('#!/bin/sh\nexit "$SUITE_EXIT"\n')
            timeout.chmod(0o755)
            env = {**os.environ, 'PATH': str(base / 'bin') + os.pathsep + os.environ['PATH']}
            for outcome in ('0', '1'):
                log = base / ('logs' + outcome)
                result = subprocess.run(['sh', str(base / 'tests/run-matrix.sh'),
                                         '/unused/qemu', *[str(base / 'build')] * 3,
                                         str(log), 'quick'],
                                        env={**env, 'SUITE_EXIT': outcome}, capture_output=True, text=True)
                with (log / 'matrix.tsv').open() as stream:
                    rows = list(csv.DictReader(stream, delimiter='\t'))
                failed = sum(r['result'] == 'FAIL' for r in rows)
                self.assertEqual(len(rows), 45)
                self.assertEqual(failed, 0 if outcome == '0' else 44, result.stdout + result.stderr + (log / 'agent-unittest.txt').read_text())
                self.assertEqual(result.returncode, int(outcome), result.stdout + result.stderr)

    def test_original_baseline_hash_covers_final_sources(self):
        # Inspect exactly what stage.sh supplies to make, before compilation.
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            tools = base / 'bin'
            tools.mkdir()
            make = tools / 'make'
            make.write_text('''#!/usr/bin/env python3
import hashlib
from pathlib import Path
p = Path('.')
info = dict(line.split('=', 1) for line in Path('STAGE-INFO.txt').read_text().splitlines())
manifest = b''
for f in sorted((f for f in p.rglob('*') if f.is_file() and f.name not in ('STAGE-INFO.txt', 'build.txt')), key=lambda f: str(f).encode()):
    manifest += (hashlib.sha256(f.read_bytes()).hexdigest() + '  ./' + str(f) + '\\n').encode()
actual = hashlib.sha256(manifest).hexdigest()[:16]
assert actual == info['staged_source_sha256'], (actual, info)
assert Path('spbench.c').read_text().startswith('#define ORIG_XV6 1')
assert '_spbench' in Path('Makefile').read_text()
assert '2000' in next(s for s in Path('param.h').read_text().splitlines() if s.startswith('#define FSSIZE'))
assert Path('tests/run-spec-tests.py').exists()
for name in ('kernel.sym', 'fs.img', 'xv6.img'):
    Path(name).write_bytes(b'')
print('final-source hash verified')
''')
            make.chmod(0o755)
            result = subprocess.run(['sh', str(ROOT / 'bench/stage.sh'), 'orig', str(base / 'orig')],
                                    env={**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH']},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('final-source hash verified', (base / 'orig/build.txt').read_text())

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required')
    def test_benchmark_attempt_outcomes_are_mutually_exclusive(self):
        # Compile the actual concurrency function with deterministic syscall
        # stand-ins. Exercise failures that a healthy disk rarely produces.
        source = (ROOT / 'spbench.c').read_text()
        start = source.index('static void\nconc(')
        end = source.index('\n#ifdef SHADOW_TEST', start)
        prelude = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef unsigned int uint;
#define M_ORD 0
#define M_COMMIT 1
#define M_ABORT 2
#define O_RDWR 2
static char big[512];
static char *modename[] = {"ord", "commit", "abort"};
static int scenario, aborts;
static int begin(int mode) { return scenario == 4 ? -1 : 0; }
static int complete(int mode) {
  if(mode == M_ABORT) { aborts++; return 0; }
  return scenario == 3 ? -1 : 0;
}
static int mkfile(char *p, int n) { return n; }
static uint uptime(void) { return 1; }
static unsigned long long rdtsc(void) { static uint t; return ++t; }
static uint delta(unsigned long long a, unsigned long long b) { return b-a; }
static int fork(void) { return 0; }
static int open(char *p, int mode) { return scenario == 1 ? -1 : 3; }
static int write(int fd, char *p, int n) { return scenario == 2 ? n-1 : n; }
static int close(int fd) { return 0; }
static int unlink(char *p) { return 0; }
static int wait(void) { return 0; }
static void ls(char *s) { fputs(s, stdout); }
static void ld(int n) { fprintf(stdout, "%d", n); }
static void lu(uint n) { fprintf(stdout, "%u", n); }
static void lc(void) { putchar(','); }
static void lend(void) { putchar('\n'); }
static void guest_printf(int fd, char *s, ...) { }
#define printf guest_printf
#define exit() return
'''
        main = r'''
#undef printf
#undef exit
int main(int argc, char **argv) {
  scenario = atoi(argv[1]);
  conc(1, 3, 0, atoi(argv[2]));
  printf("ABORTS:%d\n", aborts);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            c = Path(folder) / 'counter.c'
            exe = Path(folder) / 'counter'
            c.write_text(prelude + source[start:end] + main)
            subprocess.run(['cc', str(c), '-o', str(exe)], check=True, capture_output=True)
            for mode in (0, 1):
                for scenario in range(5):
                    out = subprocess.check_output([str(exe), str(scenario), str(mode)], text=True)
                    worker = next(s for s in out.splitlines() if s.startswith('CONCW,')).split(',')
                    counts = tuple(map(int, worker[5:8]))
                    expected = (3, 0, 0) if scenario == 0 else (0, 3, 0) if scenario == 3 else (0, 0, 3)
                    self.assertEqual(counts, expected, (scenario, mode, out))
                    self.assertEqual(sum(counts), 3)
                    expected_aborts = 3 if scenario == 3 or (mode == 1 and scenario in (1, 2)) else 0
                    self.assertIn(f'ABORTS:{expected_aborts}\n', out)


if __name__ == '__main__':
    unittest.main()
