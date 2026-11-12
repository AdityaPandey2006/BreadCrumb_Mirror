// spasktest: owner approval prompt (xask, decision D63).
//
// Run by tests/run-spec-tests.py --suite ask, which answers each prompt by its
// tag: "ASKTEST A" -> y, "ASKTEST B" -> n, "ASKTEST C" -> y but only after
// RIVAL_COMMITTED is printed, "ASKTEST E" -> no answer (the waiting owner is
// killed). Results observed while frozen are kept in
// variables and printed only after the attempt has finished, because a frozen
// owner cannot write.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
check(int cond, const char *msg)
{
  if(!cond){
    printf(1, "FAIL: %s\n", msg);
    exit();
  }
}

static int
exists(char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static void
mkfile(char *path, char *text)
{
  int fd = open(path, O_CREATE | O_WRONLY);
  check(fd >= 0, "create file");
  check(write(fd, text, strlen(text)) == (int)strlen(text), "write file");
  close(fd);
}

// Refusals that need no console answer.
static void
test_refusals(void)
{
  int p[2], pid, ar;
  char r;
  char big[1100];

  printf(1, "BEGIN ASK_REFUSALS\n");
  check(xask("not speculative") == -1, "xask outside a speculation refused");

  // Another member still alive: refused (the owner is not alone yet).
  check(xbegin() == 0, "xbegin R1");
  check(pipe(p) == 0, "internal pipe R1");
  pid = fork();
  check(pid >= 0, "fork R1");
  if(pid == 0){
    close(p[1]);
    read(p[0], &r, 1);          // wait until the owner has tried
    exit();
  }
  close(p[0]);
  ar = xask("ASKTEST R1 must not print");
  write(p[1], "x", 1);
  close(p[1]);
  check(wait() == pid, "wait R1 child");
  check(ar == -1, "xask with a live member refused");
  check(xabort() == 0, "xabort R1");

  // A non-owner member may not ask. The child reports through an internal pipe.
  check(xbegin() == 0, "xbegin R2");
  check(pipe(p) == 0, "internal pipe R2");
  pid = fork();
  check(pid >= 0, "fork R2");
  if(pid == 0){
    close(p[0]);
    r = xask("ASKTEST R2 must not print") == -1 ? 'R' : 'X';
    write(p[1], &r, 1);
    exit();
  }
  close(p[1]);
  check(read(p[0], &r, 1) == 1, "read R2 result");
  close(p[0]);
  check(wait() == pid, "wait R2 child");
  check(r == 'R', "xask by a non-owner member refused");
  check(xabort() == 0, "xabort R2");

  // Oversized question refused without freezing.
  memset(big, 'q', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  check(xbegin() == 0, "xbegin R3");
  check(xask(big) == -1, "oversized question refused");
  check(write(1, "", 0) == 0, "not frozen after refused xask");
  check(xabort() == 0, "xabort R3");
  printf(1, "END ASK_REFUSALS\n");
}

// A: answer yes. While frozen, every effect is refused; the commit publishes
// the file and only then releases the held console line.
static void
test_yes(void)
{
  int r, w, o, f, m, u, rd, again, gp, c;
  char ch;

  printf(1, "BEGIN ASK_YES\n");
  unlink("askA");
  check(xbegin() == 0, "xbegin A");
  mkfile("askA", "approved data");
  check(write(1, "ASK_HELD_A\n", 11) == 11, "held write A");
  r = xask("ASKTEST A");
  w = write(1, "LEAK_FROZEN_WRITE\n", 18);
  o = open("askA2", O_CREATE | O_WRONLY);
  f = fork();
  if(f == 0)
    exit();                     // never reached if fork is refused
  m = mkdir("askdir");
  u = unlink("askA");
  rd = read(0, &ch, 1);
  again = xask("ASKTEST A again");
  gp = getpid();
  c = xcommit();
  printf(1, "A results: ask=%d write=%d open=%d fork=%d mkdir=%d unlink=%d read=%d again=%d commit=%d\n",
         r, w, o, f, m, u, rd, again, c);
  check(r == 1, "answer yes returns 1");
  check(w == -1 && o == -1 && f == -1 && m == -1 && u == -1 && rd == -1,
        "frozen owner effects refused");
  check(again == -1, "second xask refused");
  check(gp > 0, "getpid allowed while frozen");
  check(c == 0, "xcommit after yes");
  check(exists("askA"), "approved file published");
  check(!exists("askA2") && !exists("askdir"), "frozen effects did not happen");
  check(write(1, "", 0) == 0, "unfrozen after commit");
  unlink("askA");
  printf(1, "END ASK_YES\n");
}

// B: answer no. xcommit is refused, xabort discards file and held output.
static void
test_no(void)
{
  int r, c, a;

  printf(1, "BEGIN ASK_NO\n");
  unlink("askB");
  check(xbegin() == 0, "xbegin B");
  mkfile("askB", "rejected data");
  check(write(1, "LEAK_ASK_NO_222\n", 16) == 16, "held write B");
  r = xask("ASKTEST B");
  c = xcommit();
  a = xabort();
  printf(1, "B results: ask=%d commit=%d abort=%d\n", r, c, a);
  check(r == 0, "answer no returns 0");
  check(c == -1, "xcommit refused after no");
  check(a == 0, "xabort after no");
  check(!exists("askB"), "rejected file not published");
  printf(1, "END ASK_NO\n");
}

static int
content(char *path, char *text)
{
  char data[64];
  int fd = open(path, O_RDONLY), n = strlen(text), r;
  if(fd < 0)
    return 0;
  memset(data, 0, sizeof(data));
  r = read(fd, data, sizeof(data) - 1);
  close(fd);
  return r == n && strcmp(data, text) == 0;
}

#define PIPESIZE 512    // pipe.c

// C: the user answers yes, but a rival speculation committed the same file
// first. xcommit must fail (first committer wins, D24), the attempt becomes
// abort-only, and neither the file change nor the held output escapes.
//
// Ordering without timing: the asker writes askC (creating its shadow) and
// only then reads from a pipe the parent writes. The parent writes one byte
// more than the pipe holds, so its write returns only after the asker started
// reading, i.e. after the shadow exists. The rival commits after that, and the
// harness types "y" only after it sees RIVAL_COMMITTED.
static void
test_conflict_after_yes(void)
{
  int p[2], pid, got, n, r, c1, c2, a;
  static char buf[PIPESIZE + 1];

  printf(1, "BEGIN ASK_CONFLICT\n");
  unlink("askC");
  mkfile("askC", "original");
  check(pipe(p) == 0, "pipe C");
  pid = fork();
  check(pid >= 0, "fork C");
  if(pid == 0){
    close(p[1]);
    check(xbegin() == 0, "asker xbegin C");
    n = open("askC", O_WRONLY);
    check(n >= 0, "asker open C");
    check(write(n, "asker!!!", 8) == 8, "asker write C");   // shadow exists now
    close(n);
    write(1, "LEAK_ASK_CONFLICT_333\n", 22);              // held
    for(got = 0; got < PIPESIZE + 1; got += n)             // outside input (D44)
      if((n = read(p[0], buf, sizeof(buf))) <= 0)
        break;
    close(p[0]);
    r = xask("ASKTEST C");
    c1 = xcommit();             // loses: rival committed first
    c2 = xcommit();             // abort-only now
    a = xabort();
    printf(1, "C results: got=%d ask=%d commit=%d commit2=%d abort=%d\n",
           got, r, c1, c2, a);
    check(got == PIPESIZE + 1, "asker read the release bytes");
    check(r == 1, "answer yes returns 1");
    check(c1 == -1, "xcommit after yes fails on conflict");
    check(c2 == -1, "second xcommit refused (abort-only)");
    check(a == 0, "xabort after conflict");
    exit();
  }
  close(p[0]);
  memset(buf, 'g', sizeof(buf));
  check(write(p[1], buf, sizeof(buf)) == sizeof(buf), "release asker");
  close(p[1]);
  // The asker's shadow of askC exists. Rival commits the same file first.
  check(xbegin() == 0, "rival xbegin C");
  n = open("askC", O_WRONLY);
  check(n >= 0, "rival open C");
  check(write(n, "rival!!!", 8) == 8, "rival write C");
  close(n);
  check(xcommit() == 0, "rival commits first");
  printf(1, "RIVAL_COMMITTED\n");
  check(wait() == pid, "wait C");
  check(content("askC", "rival!!!"), "public file holds the rival's data only");
  unlink("askC");
  printf(1, "END ASK_CONFLICT\n");
}

// E: the owner is killed while waiting for an answer: same as owner death.
static void
test_kill_waiting(void)
{
  int pid;

  printf(1, "BEGIN ASK_KILL\n");
  unlink("askE");
  pid = fork();
  check(pid >= 0, "fork E");
  if(pid == 0){
    check(xbegin() == 0, "xbegin E");
    mkfile("askE", "killed data");
    write(1, "LEAK_ASK_KILL_111\n", 18);
    xask("ASKTEST E");
    xcommit();                  // not reached when killed while waiting
    exit();
  }
  sleep(100);                   // let the child reach the prompt
  check(kill(pid) == 0, "kill waiting owner");
  check(wait() == pid, "wait E");
  check(!exists("askE"), "killed owner's file not published");
  printf(1, "END ASK_KILL\n");
}

int
main(void)
{
  test_refusals();
  test_yes();
  test_no();
  test_conflict_after_yes();
  test_kill_waiting();
  printf(1, "SPASKTEST: ALL TESTS PASSED\n");
  exit();
}
