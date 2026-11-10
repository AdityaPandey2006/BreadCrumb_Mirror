# Step 9 Verification Record

**Date:** 10 November 2026  
**Branch:** `step9`  
**Commit:** Complete conflict handling, concurrency validation, and consistency contract  
**Hardware & Toolchain:** WSL2 Ubuntu 22.04 LTS on Windows 11, x86 GCC 11.4.0, QEMU 11.1.50 (`/home/wazzaxd/qemu/build/qemu-system-i386`)

---

## 1. Summary of Verification Results

| Test Suite | CPUs | Repeats | Build Configuration | Result | Raw Evidence Log |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **spconflict** | 1 | 1 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/conflict-cpu1.txt` |
| **spconflict** | 3 | 3 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/conflict-cpu3-repeat3.txt` |
| **shadow** (all 7 suites) | 3 | 2 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/shadow-cpu3.txt` |
| **console** (isolation & drain) | 3 | 2 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/console-cpu3.txt` |
| **pipe** (isolation & drain) | 3 | 2 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/pipe-cpu3.txt` |
| **namespace** (metadata & dir) | 3 | 2 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/namespace-cpu3.txt` |
| **crash** (4 commit points) | 3 | 1 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/crash-cpu3.txt` |
| **namespace-crash** (4 points) | 3 | 1 | `SHADOW_TEST=1` | **PASS (100%)** | `design/evidence/step9/namespace-crash-cpu3.txt` |
| **usertests** (standard xv6) | 1 | 1 | Normal Production | **ALL TESTS PASSED** | `design/evidence/step9/usertests-cpu1.txt` |
| **lifecycle** (regression) | 3 | 1 | Normal Production | **PASS (100%)** | `design/evidence/step9/lifecycle-cpu3.txt` |
| **agent unit tests** (15 tests) | Host | 1 | Python 3.10 | **Ran 15 tests, OK** | Host terminal stdout |

---

## 2. Detailed Scenario Verification

### A. Data Conflicts (First-Committer-Wins)
* **Test:** `spconflict` test 1.
* **Outcome:** Speculation A and Speculation B both modified block 0 of `cf_data`. When Speculation A committed first, `xcommit()` returned 0. When Speculation B attempted commit, `shadow_publish()` detected the modified block, returned `SP_CONFLICT`, and set `sc->abort_only = 1`. A subsequent retry of `xcommit()` by B returned `-1`. B then called `xabort()`, which succeeded. Public inspection confirmed `cf_data` held only A's data, with zero bytes of B leaking.

### B. Metadata False Conflicts
* **Test:** `spconflict` test 2.
* **Outcome:** Speculation A created `cf_meta_a` and Speculation B created `cf_meta_b` concurrently in `/`. Both modified the root directory block. Speculation A committed first. Speculation B's commit validation detected that the root directory block differed from its saved original image, rejected the commit with `-1`, and entered abort-only mode. Speculation B aborted cleanly. Inspection proved `cf_meta_a` existed with its intended contents, while `cf_meta_b` was completely absent from the filesystem.

### C. Outside Operation Refusals
* **Test:** `spconflict` test 3.
* **Outcome:** An outside process attempted to write to a block shadowed by an active speculation. The write returned `<= 0` (`shadow_block_busy`). An outside attempt to unlink the shadowed file returned `-1` (`shadow_inode_busy`). After the speculation committed, outside writes and unlinks completed successfully.

### D. Concurrency with Live Participants
* **Test:** `spconflict` test 4 & test 5.
* **Outcome:** Multiple participant processes concurrently read and wrote file data. The owner was prevented from committing or aborting while a child remained alive (`xcommit()` and `xabort()` returned `-1`). Once children exited and were reaped, the owner committed all participants' changes in a single durable transaction.

### E. Pending Output Discard on Conflict
* **Test:** `spconflict` test 8 & test runner regex.
* **Outcome:** Speculation A wrote secret tokens `LEAK_CONFLICT_CONSOLE_555` to console and `LEAK_CONFLICT_PIPE_444` to an external pipe. Speculation B won the race and committed first. Speculation A's commit failed, and A aborted.
* **Host Verification:** The external pipe reader received 0 bytes (EOF). The host test runner parsed the raw serial log and verified that `LEAK_CONFLICT_CONSOLE_555` and `LEAK_CONFLICT_PIPE_444` never appeared anywhere in the console output.

### F. Lock Order & Deadlock Freedom
* **Test:** `spconflict` test 9 & multi-CPU `shadow` suite (3 CPUs, 2 repeats).
* **Outcome:** Repeated executions under 3 CPUs completed without kernel hangs, panics, or missed wakeups.

---

## 3. Reproduction Commands

To reproduce the complete verification suite inside WSL:

```bash
cd /mnt/d/Projects/BreadCrumb

# 1. Build test kernel
make -B SHADOW_TEST=1 xv6.img fs.img

# 2. Run Step 9 conflict suite (3 CPUs, 3 repeats)
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite conflict --cpus 3 --repeat 3 --log design/evidence/step9/conflict-cpu3-repeat3.txt

# 3. Run full shadow regression (3 CPUs, 2 repeats)
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite shadow --cpus 3 --repeat 2 --log design/evidence/step9/shadow-cpu3.txt

# 4. Run console and pipe suites (3 CPUs, 2 repeats)
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite console --cpus 3 --repeat 2 --log design/evidence/step9/console-cpu3.txt
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite pipe --cpus 3 --repeat 2 --log design/evidence/step9/pipe-cpu3.txt

# 5. Run namespace and crash suites (3 CPUs)
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite namespace --cpus 3 --repeat 2 --log design/evidence/step9/namespace-cpu3.txt
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite crash --cpus 3 --log design/evidence/step9/crash-cpu3.txt
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite namespace-crash --cpus 3 --log design/evidence/step9/namespace-crash-cpu3.txt

# 6. Build production kernel and run standard usertests
make -B xv6.img fs.img
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite usertests --cpus 1 --log design/evidence/step9/usertests-cpu1.txt
python3 tests/run-spec-tests.py --qemu /home/wazzaxd/qemu/build/qemu-system-i386 --suite lifecycle --cpus 3 --log design/evidence/step9/lifecycle-cpu3.txt

# 7. Run host Python agent tests
python3 -m unittest discover -s agent
```

