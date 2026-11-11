#!/bin/sh
# Record the measurement/test environment (printed to stdout).
# Usage: sh bench/env-report.sh QEMU
QEMU=${1:-$HOME/qemu/build/qemu-system-i386}
REPO=$(cd "$(dirname "$0")/.." && pwd)
echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "git_head: $(git -C "$REPO" rev-parse HEAD) ($(git -C "$REPO" rev-parse --abbrev-ref HEAD))"
echo "git_dirty_files: $(git -C "$REPO" -c core.autocrlf=true status --porcelain | wc -l)"
echo "host_kernel: $(uname -srmo)"
echo "os: $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME")"
echo "cpu_model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "host_cpus: $(nproc)"
echo "host_mem: $(free -m | awk '/Mem:/{print $2 " MiB"}')"
echo "gcc: $(gcc --version | head -1)"
echo "ld: $(ld --version | head -1)"
echo "make: $(make --version | head -1)"
echo "python: $(python3 --version)"
echo "qemu_binary: $QEMU"
echo "qemu: $($QEMU --version | head -1)"
echo "qemu_accel: TCG (no -enable-kvm; harness passes -smp N -m 512, serial to a pipe)"
echo "guest_memory: 512 MiB; guest disk: raw fs.img (2000 blocks) + xv6.img"
echo "build_flags: $(grep -m1 '^CFLAGS =' "$REPO/Makefile")"
echo "fault_injection_flag: SHADOW_TEST=1 adds -DSHADOW_TEST (test build only)"
echo "kernel_limits: $(grep -E 'define (NSHADOW|NSPEC|MAXOPBLOCKS|LOGSIZE|NBUF|FSSIZE)' "$REPO/param.h" | awk '{printf "%s=%s ", $2, $3}')"
