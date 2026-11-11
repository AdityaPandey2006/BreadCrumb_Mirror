// Step 10 failure tests: gaps in the earlier suites, resource/progress
// outcomes, and the output-release crash scenario.
//
//   spfailure                 all tests (fault-injection parts need SHADOW_TEST=1)
//   spfailure drainfull       only the full-external-pipe commit drain test
//   spfailure outcrash setup|N|verify   output-release crash scenario (test build)
//
// Tokens named LEAK_* must never reach the console or an outside reader;
// tests/run-spec-tests.py checks the raw serial log for them.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "param.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

// Console output inside a speculation is itself held (and discarded on
// abort), so checks made between xbegin and completion are recorded silently
// and reported by leave() once the process is ordinary again.
static int in_spec;
static char *spec_fail;

static void
check(int ok, char *msg)
{
  if(in_spec){
    if(!ok && spec_fail == 0)
      spec_fail = msg;
    return;
  }
  if(!ok){
    printf(1, "FAIL: %s\n", msg);
    exit();
  }
  printf(1, "PASS: %s\n", msg);
}

static void
enter(void)
{
  in_spec = 1;
  spec_fail = 0;
}

// Call right after xcommit/xabort; `done` is that call's result.
static void
leave(int done, char *what)
{
  in_spec = 0;
  if(spec_fail){
    printf(1, "FAIL: %s (inside the speculation)\n", spec_fail);
    exit();
  }
  check(done == 0, what);
}

static int
exists(char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

#ifdef SHADOW_TEST
static int
fsize(char *path)
{
  struct stat st;
  if(stat(path, &st) < 0)
    return -1;
  return st.size;
}

static int
filled(char *path, char c, int n)
{
  char buf[128];
  int fd, r, i, total = 0;
  if((fd = open(path, O_RDONLY)) < 0)
    return 0;
  while((r = read(fd, buf, sizeof(buf))) > 0){
    for(i = 0; i < r; i++)
      if(buf[i] != c){
        close(fd);
        return 0;
      }
    total += r;
  }
  close(fd);
  return total == n;
}

#endif

static int
writefill(char *path, char c, int n)
{
  char buf[100];
  int fd, k, total = 0;
  memset(buf, c, sizeof(buf));
  if((fd = open(path, O_CREATE | O_RDWR)) < 0)
    return -1;
  while(total < n){
    k = n - total < (int)sizeof(buf) ? n - total : (int)sizeof(buf);
    if(write(fd, buf, k) != k)
      break;
    total += k;
  }
  close(fd);
  return total;
}

static int
readall(int fd, char *buf, int max)
{
  int n, total = 0;
  while(total < max && (n = read(fd, buf + total, max - total)) > 0)
    total += n;
  return total;
}

// F1. Commit with more held pipe output than the outside pipe can hold
// (PIPESIZE 512) while the outside reader performs ordinary filesystem
// work (console printf -> begin_op) between reads. Release must not hold
// the filesystem gate while it waits for the reader (regression for the
// Step-10 drain deadlock).
#define DRAIN_BYTES 1500
static void
test_drain_full_pipe(void)
{
  int p[2], pid, i, n, total;
  char buf[300];
  printf(1, "BEGIN F1 drain into a full outside pipe\n");
  unlink("drain_ok");
  check(pipe(p) == 0, "F1 pipe");
  pid = fork();
  check(pid >= 0, "F1 fork reader");
  if(pid == 0){
    close(p[1]);
    total = 0;
    while((n = read(p[0], buf, 128)) > 0){
      total += n;
      printf(1, "r");            // ordinary console write: takes the gate
    }
    printf(1, "\nF1 READER GOT %d\n", total);
    if(total == DRAIN_BYTES)
      writefill("drain_ok", 'k', 1);   // ordinary file creation: gate again
    exit();
  }
  close(p[0]);
  memset(buf, 'd', sizeof(buf));
  enter();
  check(xbegin() == 0, "F1 begin");
  for(i = 0; i < DRAIN_BYTES / 300; i++)
    if(write(p[1], buf, 300) != 300)
      break;
  check(i == DRAIN_BYTES / 300, "F1 1500 bytes held for the outside reader");
  leave(xcommit(), "F1 commit releases more than PIPESIZE bytes");
  close(p[1]);
  check(wait() == pid, "F1 reader exits");
  check(exists("drain_ok"), "F1 outside reader received all 1500 bytes and kept working");
  unlink("drain_ok");
}

// F2. Cancelled sleepers: the owner is killed while members sleep in
// sleep() and in an internal-pipe read. All must be woken and exit
// promptly (D11), not after their 1000-tick sleep.
static void
test_cancelled_sleepers(void)
{
  int alive[2], sync[2], ip[2], owner, t0, n, waited;
  char c;
  printf(1, "BEGIN F2 cancelled sleepers\n");
  check(pipe(alive) == 0 && pipe(sync) == 0, "F2 pipes");
  owner = fork();
  check(owner >= 0, "F2 fork owner");
  if(owner == 0){
    close(alive[0]);
    close(sync[0]);
    if(write(sync[1], "s", 1) != 1)   // before xbegin: an ordinary write
      exit();
    close(sync[1]);
    if(xbegin() != 0 || pipe(ip) != 0)
      exit();
    if(fork() == 0){                  // member 1: long sleep
      sleep(1000);
      exit();
    }
    if(fork() == 0){                  // member 2: blocked on an internal pipe
      close(ip[1]);
      read(ip[0], &c, 1);
      exit();
    }
    sleep(1000);                      // owner sleeps too
    exit();
  }
  close(alive[1]);
  close(sync[1]);
  check(read(sync[0], &c, 1) == 1, "F2 owner started");
  close(sync[0]);
  sleep(30);                          // let members reach their sleeps
  t0 = uptime();
  check(kill(owner) == 0, "F2 kill owner");
  check(wait() == owner, "F2 owner reaped");
  n = read(alive[0], &c, 1);          // EOF once owner and both members exit
  waited = uptime() - t0;
  close(alive[0]);
  printf(1, "F2 all participants gone after %d ticks\n", waited);
  check(n == 0 && waited < 300, "F2 sleeping and blocked members were woken and exited");
  check(xbegin() == 0 && xabort() == 0, "F2 new speculation after cancellation");
}

// F3. Voluntary owner exit with held console and pipe output and a private
// file: cancellation discards all of it.
static void
test_owner_exit_discards(void)
{
  int p[2], pid;
  char buf[64];
  printf(1, "BEGIN F3 owner exit discards held output\n");
  unlink("exit_doomed");
  check(pipe(p) == 0, "F3 pipe");
  pid = fork();
  check(pid >= 0, "F3 fork");
  if(pid == 0){
    close(p[0]);
    if(xbegin() != 0)
      exit();
    write(p[1], "LEAK_EXIT_PIPE_222", 18);
    printf(1, "LEAK_EXIT_CONSOLE_333\n");
    writefill("exit_doomed", 'x', 300);
    exit();                           // no xcommit/xabort
  }
  close(p[1]);
  check(wait() == pid, "F3 owner exited");
  check(read(p[0], buf, sizeof(buf)) == 0, "F3 outside reader sees EOF, no held bytes");
  close(p[0]);
  check(!exists("exit_doomed"), "F3 private file discarded");
}

// F4. Outside reader already blocked in read() before bytes are held.
static void
blocked_reader_case(int commit)
{
  int p[2], pid, n;
  char buf[64];
  char *msg = commit ? "BLOCKED_COMMIT_MSG" : "LEAK_BLOCKED_ABORT_111";
  check(pipe(p) == 0, "F4 pipe");
  pid = fork();
  check(pid >= 0, "F4 fork reader");
  if(pid == 0){
    close(p[1]);
    n = readall(p[0], buf, sizeof(buf) - 1);
    buf[n] = 0;
    if(commit ? strcmp(buf, "BLOCKED_COMMIT_MSG") != 0 : n != 0)
      printf(1, "FAIL: F4 blocked reader got %d bytes\n", n);
    else
      printf(1, "PASS: F4 blocked reader got %d expected bytes\n", n);
    exit();
  }
  close(p[0]);
  sleep(10);                          // reader is now asleep on the empty pipe
  enter();
  check(xbegin() == 0, "F4 begin");
  check(write(p[1], msg, strlen(msg)) == strlen(msg), "F4 bytes held");
  sleep(10);                          // held bytes must not wake it with data
  leave(commit ? xcommit() : xabort(), commit ? "F4 commit" : "F4 abort");
  close(p[1]);
  check(wait() == pid, "F4 reader finished");
}

static void
test_blocked_reader(void)
{
  printf(1, "BEGIN F4 blocked outside reader\n");
  blocked_reader_case(1);
  blocked_reader_case(0);
}

// F5. Outside writer blocked on a full pipe whose reader is a speculative
// member: the member's reads let it progress (no deadlock), and abort
// cannot restore consumed input (D44).
static void
test_outside_writer_spec_reader(void)
{
  int p[2], pid, n;
  char buf[1100];
  printf(1, "BEGIN F5 outside writer, speculative reader\n");
  check(pipe(p) == 0, "F5 pipe");
  pid = fork();
  check(pid >= 0, "F5 fork writer");
  if(pid == 0){
    close(p[0]);
    memset(buf, 'w', 1024);
    n = write(p[1], buf, 1024);       // blocks after 512 until the reader reads
    exit();
  }
  close(p[1]);
  enter();
  check(xbegin() == 0, "F5 begin");
  n = readall(p[0], buf, 1024);
  check(n == 1024, "F5 speculative member consumed 1024 outside bytes");
  leave(xabort(), "F5 abort; member consumed 1024 outside bytes");
  check(wait() == pid, "F5 outside writer completed");
  check(read(p[0], buf, 1) == 0, "F5 consumed input is not restored by abort (D44)");
  close(p[0]);
}

// F6. Speculative write to an outside pipe that is already full of unread
// ordinary bytes is held without sleeping (D41); abort discards it.
static void
test_spec_write_full_external(void)
{
  int p[2], pid, n;
  char buf[600];
  printf(1, "BEGIN F6 held write to a full outside pipe\n");
  check(pipe(p) == 0, "F6 pipe");
  memset(buf, 'o', 512);
  check(write(p[1], buf, 512) == 512, "F6 ordinary bytes fill the pipe");
  pid = fork();
  check(pid >= 0, "F6 fork");
  if(pid == 0){
    close(p[0]);
    if(xbegin() != 0)
      exit();
    n = write(p[1], "LEAK_FULLPIPE_000", 17);
    xabort();                         // ordinary again: the report is visible
    if(n != 17)
      printf(1, "FAIL: F6 held write returned %d\n", n);
    exit();
  }
  check(wait() == pid, "F6 speculative writer did not sleep on the full pipe");
  close(p[1]);
  n = readall(p[0], buf, sizeof(buf));
  close(p[0]);
  check(n == 512 && buf[0] == 'o' && buf[511] == 'o', "F6 reader got only the 512 ordinary bytes");
}

// F7. Lifecycle error matrix (D03, D04, D06-D08, D14).
static void
test_lifecycle_errors(void)
{
  int ip[2], pid;
  char r[4];
  printf(1, "BEGIN F7 lifecycle error matrix\n");
  check(xcommit() == -1 && xabort() == -1, "F7 completion without a speculation fails");
  enter();
  check(xbegin() == 0, "F7 begin");
  check(xbegin() == -1, "F7 nested xbegin rejected");
  check(pipe(ip) == 0, "F7 internal pipe");
  pid = fork();
  if(pid == 0){
    close(ip[0]);
    r[0] = xbegin() == -1 ? 'y' : 'n';
    r[1] = xcommit() == -1 ? 'y' : 'n';
    r[2] = xabort() == -1 ? 'y' : 'n';
    write(ip[1], r, 3);               // internal pipe: delivered immediately
    sleep(20);
    exit();
  }
  close(ip[1]);
  check(readall(ip[0], r, 3) == 3 && r[0] == 'y' && r[1] == 'y' && r[2] == 'y',
        "F7 member cannot begin, commit or abort");
  close(ip[0]);
  check(xcommit() == -1 && xabort() == -1, "F7 owner cannot complete with a live peer");
  check(wait() == pid, "F7 member exits");
  leave(xcommit(), "F7 owner commits after the member exits (nested, member and live-peer checks held)");
  check(xcommit() == -1 && xabort() == -1, "F7 second completion fails");
  check(xbegin() == 0 && xabort() == 0, "F7 begin after completion allowed");
}

#ifdef SHADOW_TEST
static int
settle_pages(int target)
{
  int i, now = 0;
  for(i = 0; i < 100; i++){            // reparented zombies are reaped by init
    now = shadowtest(ST_FREEPAGES, 0, 0);
    if(now == target)
      break;
    sleep(1);
  }
  return now;
}

// F8. Allocation failure of the held-console and held-pipe pages.
static void
test_output_alloc_failure(void)
{
  int p[2], pid, n, before;
  char buf[64];
  printf(1, "BEGIN F8 held-output allocation failure\n");
  before = shadowtest(ST_FREEPAGES, 0, 0);
  enter();
  check(xbegin() == 0, "F8 begin console");
  check(shadowtest(ST_FAIL, 0, STF_CONSOLE) == 0, "F8 arm console failure");
  check(write(1, "LEAK_CONSFAIL_A\n", 16) == -1, "F8 console write fails when its page cannot be allocated");
  check(write(1, "CONSFAIL_OK_LINE\n", 17) == 17, "F8 next console write succeeds");
  leave(xcommit(), "F8 console page failure returned -1, later write held and released");
  check(settle_pages(before) == before, "F8 no page leaked (console)");
  check(pipe(p) == 0, "F8 pipe");
  pid = fork();
  if(pid == 0){
    close(p[1]);
    n = readall(p[0], buf, sizeof(buf) - 1);
    buf[n] = 0;
    printf(1, "F8 PIPEFAIL GOT %s\n", buf);
    if(strcmp(buf, "PIPEFAIL_OK") != 0)
      printf(1, "FAIL: F8 reader got the wrong bytes\n");
    exit();
  }
  close(p[0]);
  enter();
  check(xbegin() == 0, "F8 begin pipe");
  check(shadowtest(ST_FAIL, 0, STF_PIPE) == 0, "F8 arm pipe failure");
  check(write(p[1], "LEAK_PIPEFAIL_B", 15) == -1, "F8 pipe write fails when its page cannot be allocated");
  check(write(p[1], "PIPEFAIL_OK", 11) == 11, "F8 next held pipe write succeeds");
  leave(xcommit(), "F8 pipe page failure returned -1, later write held and released");
  close(p[1]);
  check(wait() == pid, "F8 reader exits");
  check(shadowtest(ST_PAGES, 0, 0) == 0, "F8 no shadow pages");
}

// F9. Mixed commit/abort cycles with files, held console and held pipe
// output return every page (resource outcome).
static void
test_cycle_leaks(void)
{
  int p[2], pid, i, n, before, after;
  char buf[64];
  printf(1, "BEGIN F9 resource return over 20 mixed cycles\n");
  before = shadowtest(ST_FREEPAGES, 0, 0);
  check(pipe(p) == 0, "F9 pipe");
  pid = fork();
  if(pid == 0){
    close(p[1]);
    n = 0;
    while((i = read(p[0], buf, sizeof(buf))) > 0)
      n += i;
    printf(1, "F9 CYCLE READER GOT %d\n", n);
    if(n != 10 * 32)
      printf(1, "FAIL: F9 reader expected 320 bytes\n");
    exit();
  }
  close(p[0]);
  memset(buf, 'c', 32);
  for(i = 0; i < 20; i++){
    if(xbegin() != 0)
      check(0, "F9 cycle begin");
    enter();
    check(writefill("cyclef", 'a' + i % 26, 700) == 700 && mkdir("cycled") == 0,
          "F9 private file and directory");
    check(write(p[1], buf, 32) == 32, "F9 held pipe write");
    if(i % 2){
      printf(1, "LEAK_CYCLE_ABORT_%d\n", i);
      in_spec = 0;
      if(spec_fail || xabort() != 0)
        check(0, spec_fail ? spec_fail : "F9 cycle abort");
    } else {
      printf(1, "CYCLE_COMMIT_%d\n", i);
      in_spec = 0;
      if(spec_fail || xcommit() != 0)
        check(0, spec_fail ? spec_fail : "F9 cycle commit");
      if(unlink("cyclef") != 0 || unlink("cycled") != 0)
        check(0, "F9 ordinary cleanup");
    }
  }
  check(1, "F9 20 cycles completed (10 commit, 10 abort)");
  close(p[1]);
  check(wait() == pid, "F9 reader exits");
  after = settle_pages(before);
  printf(1, "F9 free pages before %d after %d\n", before, after);
  check(after == before, "F9 every page returned");
  check(shadowtest(ST_PAGES, 0, 0) == 0, "F9 no shadow pages");
}

// Output-release crash scenario (see tests/run-spec-tests.py --suite output-crash).
static void
outcrash(char *arg)
{
  int p[2], pid, n;
  char buf[64];
  if(strcmp(arg, "setup") == 0){
    unlink("oc_file");
    check(writefill("oc_file", 'O', 600) == 600, "OC setup file");
    printf(1, "OCSETUP OK\n");
    return;
  }
  if(strcmp(arg, "verify") == 0){
    check(fsize("oc_file") == 600 && filled("oc_file", 'N', 600),
          "OC recovered file holds the complete committed contents");
    check(writefill("oc_after", 'z', 1500) == 1500 && unlink("oc_after") == 0,
          "OC allocator usable after recovery");
    printf(1, "OCVERIFY OK new\n");
    return;
  }
  check(pipe(p) == 0, "OC pipe");
  pid = fork();
  if(pid == 0){
    close(p[1]);
    while((n = read(p[0], buf, sizeof(buf) - 1)) > 0){
      buf[n] = 0;
      printf(1, "PIPEGOT:%s\n", buf);
    }
    exit();
  }
  close(p[0]);
  enter();
  check(xbegin() == 0, "OC begin");
  if(writefill("oc_file", 'N', 600) != 600 || write(p[1], "PIPE_TOKEN_FIRST", 16) != 16 ||
     write(p[1], "PIPE_TOKEN_SECOND", 17) != 17){
    xabort();
    in_spec = 0;
    check(0, "OC private overwrite and two held pipe records");
  }
  printf(1, "CONSOLE_TOKEN_0123456789_END\n");  // held: 16-byte prefix CONSOLE_TOKEN_01
  shadowtest(ST_CRASH, 0, atoi(arg));
  xcommit();
  printf(1, "unexpected commit reply\n");
}

#endif

int
main(int argc, char *argv[])
{
  if(argc > 1 && strcmp(argv[1], "drainfull") == 0){
    test_drain_full_pipe();
    printf(1, "SPFAILURE DRAINFULL PASSED\n");
    exit();
  }
#ifdef SHADOW_TEST
  if(argc > 2 && strcmp(argv[1], "outcrash") == 0){
    outcrash(argv[2]);
    exit();
  }
#endif

  test_lifecycle_errors();
  test_owner_exit_discards();
  test_blocked_reader();
  test_outside_writer_spec_reader();
  test_spec_write_full_external();
  test_cancelled_sleepers();
#ifdef SHADOW_TEST
  test_output_alloc_failure();
  test_cycle_leaks();
#endif

  test_drain_full_pipe();
  printf(1, "SPFAILURE TESTS PASSED\n");
  exit();
}
