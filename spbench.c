// Step 10 benchmark program. Prints raw per-iteration measurements as CSV
// lines on the console; bench/run-bench.py collects them from the serial log.
//
//   spbench cal T             TSC calibration over T timer ticks
//   spbench micro N           micro-benchmarks, N iterations per data point
//   spbench conc P R S M      P workers x R attempts; S=1 shared block, 0 disjoint;
//                             M = ord | commit
//   spbench res               resource use per operation (SHADOW_TEST build only)
//
// Timing: rdtsc read in user mode (xv6 does not set CR4.TSD). Under QEMU TCG
// the guest TSC follows the host clock; the host converts cycles to time
// with the "cal" run. Each record splits an attempt into begin / operation /
// completion phases and the total. Every CSV line is written with ONE
// write() call, and only while the process is not speculative, so held
// output never contains measurement records.
//
// Compiled with ORIG_XV6 for the original-xv6 baseline: speculative modes
// are then absent and only ordinary mode runs.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

#define M_ORD    0
#define M_COMMIT 1
#define M_ABORT  2
static char *modename[] = { "ord", "commit", "abort" };
#ifdef ORIG_XV6
#define NMODES 1
#else
#define NMODES 3
#endif

static char big[65536];
static char tmp[4096];

static inline unsigned long long
rdtsc(void)
{
  uint lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)hi << 32) | lo;
}

// ---- single-write line builder (atomic console lines) ----
static char line[256];
static int llen;

static void
ls(char *s)
{
  while(*s && llen < 240)
    line[llen++] = *s++;
}

static void
lu(uint v)
{
  char d[12];
  int i = 0;
  do { d[i++] = '0' + v % 10; v /= 10; } while(v);
  while(i > 0 && llen < 240)
    line[llen++] = d[--i];
}

static void
ld(int v)
{
  if(v < 0){ ls("-"); lu((uint)(-v)); }
  else lu((uint)v);
}

static void
lc(void)
{
  ls(",");
}

static void
lend(void)
{
  line[llen++] = '\n';
  write(1, line, llen);
  llen = 0;
}

static uint
delta(unsigned long long a, unsigned long long b)
{
  unsigned long long d = b - a;
  if(d >> 32)
    return 0xffffffff;          // saturate; reported as status "ovf"
  return (uint)d;
}

// ---- speculation wrappers ----
static int
begin(int mode)
{
#ifndef ORIG_XV6
  if(mode != M_ORD)
    return xbegin();
#endif
  return 0;
}

static int
complete(int mode)
{
#ifndef ORIG_XV6
  if(mode == M_COMMIT)
    return xcommit();
  if(mode == M_ABORT)
    return xabort();
#endif
  return 0;
}

static int
exists(char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static int
fsize(char *path)
{
  struct stat st;
  if(stat(path, &st) < 0)
    return -1;
  return st.size;
}

static int
mkfile(char *path, int bytes)
{
  int fd, n = 0;
  unlink(path);
  if((fd = open(path, O_CREATE | O_RDWR)) < 0)
    return -1;
  if(bytes > 0)
    n = write(fd, big, bytes);
  close(fd);
  return n;
}

static int
readall(int fd, char *buf, int max)
{
  int n, total = 0;
  while(total < max && (n = read(fd, buf + total, max - total)) > 0)
    total += n;
  return total;
}

// ---- workloads: op(param) returns bytes or 0 on success, <0 on failure ----
enum { W_NULL, W_EMPTY, W_OW, W_CR, W_EX, W_UL, W_CON, W_PEXT, W_PINT };
static char *wname[] = { "null", "empty", "overwrite", "create", "extend", "unlink",
                         "console", "pipe_ext", "pipe_int" };
static int pext_fd = -1, pint_r = -1, pint_w = -1;

static int
op(int w, int k)
{
  int fd, n;
  switch(w){
  case W_NULL:
    getpid();
    return 0;
  case W_EMPTY:
    return 0;
  case W_OW:
    if((fd = open("ow", O_RDWR)) < 0)
      return -1;
    n = write(fd, big, k * 512);
    close(fd);
    return n;
  case W_CR:
    if((fd = open("cr", O_CREATE | O_RDWR)) < 0)
      return -1;
    n = k ? write(fd, big, k * 512) : 0;
    close(fd);
    return n;
  case W_EX:
    if((fd = open("ex", O_RDWR)) < 0)
      return -1;
    read(fd, tmp, 512);               // xv6 has no lseek: move to the end
    n = write(fd, big, k * 512);
    close(fd);
    return n;
  case W_UL:
    return unlink("ul") == 0 ? 0 : -1;
  case W_CON:
    return write(1, big, k);
  case W_PEXT:
    return write(pext_fd, big, k);
  case W_PINT:
    n = write(pint_w, big, k);
    if(n > 0)
      n = readall(pint_r, tmp, n);
    return n;
  }
  return -1;
}

static int
expected(int w, int k)
{
  if(w == W_OW || w == W_CR || w == W_EX)
    return k * 512;
  if(w == W_CON || w == W_PEXT || w == W_PINT)
    return k;
  return 0;
}

// Untimed per-iteration setup so that every iteration starts from the same
// public state (ordinary operations, never inside a speculation).
static void
prepare(int w)
{
  if(w == W_CR && exists("cr"))
    unlink("cr");
  if(w == W_UL && !exists("ul"))
    mkfile("ul", 512);
  if(w == W_EX && fsize("ex") != 512)
    mkfile("ex", 512);
}

static void
record(int w, int mode, int k, int iter, char *status, int bytes,
       uint cb, uint co, uint ce, uint ct)
{
  ls("CSV,"); ls(wname[w]); lc(); ls(modename[mode]); lc(); ld(k); lc(); ld(iter); lc();
  ls(status); lc(); ld(bytes); lc(); lu(cb); lc(); lu(co); lc(); lu(ce); lc(); lu(ct);
  lend();
}

static void
point(int w, int mode, int k, int iters)
{
  int i, rb, n, rc;
  char *status;
  unsigned long long t0, t1, t2, t3;

  for(i = 0; i < iters; i++){
    prepare(w);
    t0 = rdtsc();
    rb = begin(mode);
    t1 = rdtsc();
    n = rb == 0 ? op(w, k) : -1;
    t2 = rdtsc();
    rc = rb == 0 ? complete(mode) : -1;
    t3 = rdtsc();
    if(rb != 0)
      status = "beginfail";
    else if(rc != 0){
      status = "completefail";
      if(mode == M_COMMIT)
        complete(M_ABORT);           // abort-only after a failed commit
    } else if(n < expected(w, k))
      status = n < 0 ? "opfail" : "short";
    else
      status = "ok";
    if((t3 - t0) >> 32)
      status = "ovf";
    record(w, mode, k, i, status, n, delta(t0, t1), delta(t1, t2), delta(t2, t3), delta(t0, t3));
  }
}

static void
sweep(int w, int *ks, int nk, int iters, int first_mode)
{
  int m, j;
  for(j = 0; j < nk; j++)
    for(m = first_mode; m < NMODES; m++)
      point(w, m, ks[j], iters);
}

static void
micro(int iters)
{
  int one[] = { 1 };
  int zero[] = { 0 };
  int ow[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 32, 64, 128 };
  int cr[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 128 };
  int ex[] = { 1, 2, 3, 4, 5, 6, 7, 8, 16 };
  int con[] = { 64, 1024, 4096, 6144 };
  int pext[] = { 64, 512, 2048, 4000 };
  int pint[] = { 64, 256, 512 };
  int p[2], ip[2], pid;

  memset(big, 'b', sizeof(big));
  for(pid = 0; pid < (int)sizeof(big); pid += 64)
    big[pid + 63] = '\n';             // console payload: 64-byte lines
  mkdir("bench");
  if(chdir("bench") < 0){
    printf(1, "FAIL: bench directory\n");
    exit();
  }
  if(mkfile("ow", 65536) != 65536){
    printf(1, "FAIL: ow file\n");
    exit();
  }
  printf(1, "MICRO start iters %d modes %d\n", iters, NMODES);
  sweep(W_NULL, zero, 1, iters, M_ORD);
  sweep(W_EMPTY, zero, 1, iters, M_ORD);
  sweep(W_OW, ow, sizeof(ow) / sizeof(ow[0]), iters, M_ORD);
  sweep(W_CR, cr, sizeof(cr) / sizeof(cr[0]), iters, M_ORD);
  sweep(W_EX, ex, sizeof(ex) / sizeof(ex[0]), iters, M_ORD);
  sweep(W_UL, one, 1, iters, M_ORD);
  sweep(W_CON, con, sizeof(con) / sizeof(con[0]), iters < 20 ? iters : 20, M_ORD);

  // Outside pipe: a persistent ordinary reader drains and discards.
  if(pipe(p) < 0 || (pid = fork()) < 0){
    printf(1, "FAIL: pipe/fork\n");
    exit();
  }
  if(pid == 0){
    close(p[1]);
    while(read(p[0], tmp, sizeof(tmp)) > 0)
      ;
    exit();
  }
  close(p[0]);
  pext_fd = p[1];
  sweep(W_PEXT, pext, sizeof(pext) / sizeof(pext[0]), iters, M_ORD);
  close(p[1]);
  wait();

  // Internal pipe: only this process holds the read end.
  if(pipe(ip) < 0){
    printf(1, "FAIL: pipe\n");
    exit();
  }
  pint_r = ip[0];
  pint_w = ip[1];
  sweep(W_PINT, pint, sizeof(pint) / sizeof(pint[0]), iters, M_ORD);
  close(ip[0]);
  close(ip[1]);

  unlink("ow"); unlink("cr"); unlink("ex"); unlink("ul");
  chdir("..");
  unlink("bench");
  printf(1, "MICRO done\n");
}

static void
cal(int ticks)
{
  uint t, start;
  unsigned long long c0, c1, d;

  t = uptime();
  while((start = uptime()) == t)
    ;                                  // align to a tick boundary
  c0 = rdtsc();
  printf(1, "CAL0\n");
  while(uptime() < start + ticks)
    ;
  c1 = rdtsc();
  d = c1 - c0;
  ls("CAL1,"); lu(ticks); lc(); lu((uint)(d >> 32)); lc(); lu((uint)d);
  lend();
}

// Concurrency: P workers each make R attempts that overwrite block 0 of a
// file (shared: one file for all; disjoint: one file each).
static void
conc(int P, int R, int shared, int mode)
{
  int i, r, pid, fd, commits, conflicts, other;
  char name[8] = "cw0";
  unsigned long long t0, t1, w0;
  uint tk0, tk1;

  memset(big, 'c', 512);
  for(i = 0; i < P; i++){
    name[2] = '0' + i;
    mkfile(shared ? "cs" : name, 512);
  }
  tk0 = uptime();
  t0 = rdtsc();
  for(i = 0; i < P; i++){
    if((pid = fork()) < 0){
      printf(1, "FAIL: fork\n");
      exit();
    }
    if(pid == 0){
      name[2] = '0' + i;
      commits = conflicts = other = 0;
      // Distinct bytes per worker and attempt: validation compares block
      // contents, so identical rewrites would never register as conflicts.
      memset(big, 'A' + i, 512);
      w0 = rdtsc();
      for(r = 0; r < R; r++){
        big[0] = '0' + r % 10;
        big[1] = 'a' + r / 10;
        if(begin(mode) != 0){
          other++;
          continue;
        }
        fd = open(shared ? "cs" : name, O_RDWR);
        if(fd < 0 || write(fd, big, 512) != 512){
          if(fd >= 0)
            close(fd);
          other++;
          // A successful completion of an empty/partial attempt is not a
          // successful write. Discard speculative work and count it once.
          if(mode != M_ORD)
            complete(M_ABORT);
          continue;
        }
        close(fd);
        if(complete(mode) == 0)
          commits++;
        else {
          conflicts++;
          complete(M_ABORT);
        }
      }
      ls("CONCW,"); ld(P); lc(); ld(shared); lc(); ls(modename[mode]); lc(); ld(i); lc();
      ld(commits); lc(); ld(conflicts); lc(); ld(other); lc(); lu(delta(w0, rdtsc()));
      lend();
      exit();
    }
  }
  for(i = 0; i < P; i++)
    wait();
  t1 = rdtsc();
  tk1 = uptime();
  ls("CONC,"); ld(P); lc(); ld(R); lc(); ld(shared); lc(); ls(modename[mode]); lc();
  lu(delta(t0, t1)); lc(); lu(tk1 - tk0);
  lend();
  for(i = 0; i < P; i++){
    name[2] = '0' + i;
    unlink(shared ? "cs" : name);
  }
}

#ifdef SHADOW_TEST
static int
settle(int target)
{
  int i, now = 0;
  for(i = 0; i < 100; i++){
    if((now = shadowtest(ST_FREEPAGES, 0, 0)) == target)
      break;
    sleep(1);
  }
  return now;
}

// Physical pages held by one speculative attempt, measured with the
// test-only free-page counter (not timed).
static void
res_point(int w, int k)
{
  int fp0, fp1, fp2, ent, n, rc;
  prepare(w);
  fp0 = shadowtest(ST_FREEPAGES, 0, 0);
  if(xbegin() != 0)
    return;
  n = op(w, k);
  fp1 = shadowtest(ST_FREEPAGES, 0, 0);
  ent = shadowtest(ST_COUNT, 0, 0);
  rc = xabort();
  fp2 = settle(fp0);
  ls("RES,"); ls(wname[w]); lc(); ld(k); lc(); ld(n); lc(); ld(rc); lc();
  ld(ent); lc(); ld(fp0 - fp1); lc(); ld(fp0 - fp2);
  lend();
}

static void
res(void)
{
  int k, p[2], pid;
  memset(big, 'r', sizeof(big));
  mkdir("bench");
  chdir("bench");
  mkfile("ow", 65536);
  printf(1, "RES start\n");
  res_point(W_EMPTY, 0);
  for(k = 1; k <= 9; k++)
    res_point(W_OW, k);
  for(k = 0; k <= 6; k++)
    res_point(W_CR, k);
  for(k = 1; k <= 7; k++)
    res_point(W_EX, k);
  res_point(W_UL, 0);
  res_point(W_CON, 64);
  res_point(W_CON, 4096);
  pipe(p);
  if((pid = fork()) == 0){
    close(p[1]);
    while(read(p[0], tmp, sizeof(tmp)) > 0)
      ;
    exit();
  }
  close(p[0]);
  pext_fd = p[1];
  res_point(W_PEXT, 64);
  res_point(W_PEXT, 4000);
  close(p[1]);
  wait();
  unlink("ow"); unlink("cr"); unlink("ex"); unlink("ul");
  chdir("..");
  unlink("bench");
  printf(1, "RES done\n");
}
#endif

int
main(int argc, char *argv[])
{
  if(argc >= 3 && strcmp(argv[1], "cal") == 0)
    cal(atoi(argv[2]));
  else if(argc >= 3 && strcmp(argv[1], "micro") == 0)
    micro(atoi(argv[2]));
  else if(argc >= 6 && strcmp(argv[1], "conc") == 0){
    int mode = strcmp(argv[5], "commit") == 0 ? M_COMMIT : M_ORD;
#ifdef ORIG_XV6
    mode = M_ORD;
#endif
    conc(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), mode);
  }
#ifdef SHADOW_TEST
  else if(argc >= 2 && strcmp(argv[1], "res") == 0)
    res();
#endif
  else
    printf(1, "usage: spbench cal T | micro N | conc P R S ord|commit | res\n");
  printf(1, "SPBENCH DONE\n");
  exit();
}
