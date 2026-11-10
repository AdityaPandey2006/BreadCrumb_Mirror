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

// 1. Intra-speculation pipeline: members of the same speculation communicate immediately.
static void
test_internal_pipeline(void)
{
  int p[2];
  int pid;
  char buf[32];
  int n;

  printf(1, "BEGIN TEST_INTERNAL_PIPELINE\n");
  check(xbegin() == 0, "xbegin internal");
  check(pipe(p) == 0, "pipe internal");

  pid = fork();
  check(pid >= 0, "fork internal");
  if(pid == 0){
    close(p[0]);
    check(write(p[1], "HELLO_INTERNAL_SPEC_DATA\n", 25) == 25, "child write internal");
    close(p[1]);
    exit();
  }
  close(p[1]);
  n = read(p[0], buf, sizeof(buf) - 1);
  check(n == 25, "parent read internal before commit");
  buf[n] = 0;
  close(p[0]);

  check(wait() == pid, "wait child internal");
  check(xcommit() == 0, "commit internal");
  printf(1, "END TEST_INTERNAL_PIPELINE\n");
}

// 2. Speculative external write released strictly upon durable commit.
static void
test_external_commit(void)
{
  int p[2];
  int sync[2];
  int pid;
  char buf[64];
  int n;

  printf(1, "BEGIN TEST_EXTERNAL_COMMIT\n");
  check(pipe(p) == 0, "pipe external");
  check(pipe(sync) == 0, "pipe sync");

  pid = fork();
  check(pid >= 0, "fork external");
  if(pid == 0){
    close(p[0]);
    close(sync[0]);
    check(xbegin() == 0, "child xbegin");
    check(write(p[1], "SPEC_PIPE_COMMIT_MSG\n", 21) == 21, "child write external pipe");
    close(p[1]);
    // Notify parent that write is done
    check(write(sync[1], "DONE", 4) == 4, "child sync notify");
    close(sync[1]);
    check(xcommit() == 0, "child commit");
    exit();
  }
  close(p[1]);
  close(sync[1]);

  // Read sync token
  check(read(sync[0], buf, 4) == 4, "parent read sync");
  close(sync[0]);

  check(wait() == pid, "parent wait child");
  n = read(p[0], buf, sizeof(buf) - 1);
  check(n == 21, "parent read committed pipe data");
  buf[n] = 0;
  close(p[0]);

  printf(1, "COMMITTED: %s", buf);
  printf(1, "END TEST_EXTERNAL_COMMIT\n");
}

// 3. Speculative external write discarded on abort.
static void
test_external_abort(void)
{
  int p[2];
  int pid;
  char buf[64];
  int n;

  printf(1, "BEGIN TEST_EXTERNAL_ABORT\n");
  check(pipe(p) == 0, "pipe abort");

  pid = fork();
  check(pid >= 0, "fork abort");
  if(pid == 0){
    close(p[0]);
    check(xbegin() == 0, "child xbegin abort");
    check(write(p[1], "LEAK_ABORT_PIPE_SECRET_777\n", 27) == 27, "child write abort secret");
    close(p[1]);
    check(xabort() == 0, "child xabort");
    exit();
  }
  close(p[1]);
  check(wait() == pid, "wait child abort");

  n = read(p[0], buf, sizeof(buf));
  check(n == 0, "parent read abort pipe must be 0 (EOF)");
  close(p[0]);
  printf(1, "END TEST_EXTERNAL_ABORT\n");
}

// 4. Speculative external write discarded on owner kill.
static void
test_external_kill(void)
{
  int p[2];
  int sync[2];
  int pid;
  char buf[64];
  int n;

  printf(1, "BEGIN TEST_EXTERNAL_KILL\n");
  check(pipe(p) == 0, "pipe kill");
  check(pipe(sync) == 0, "pipe sync");

  pid = fork();
  check(pid >= 0, "fork kill");
  if(pid == 0){
    close(p[0]);
    close(sync[0]);
    // Notify parent before entering speculation
    check(write(sync[1], "K", 1) == 1, "child sync notify before xbegin");
    close(sync[1]);

    check(xbegin() == 0, "child xbegin kill");
    check(write(p[1], "LEAK_KILL_PIPE_SECRET_666\n", 26) == 26, "child write kill secret");
    close(p[1]);
    // Wait to be killed
    sleep(100);
    exit();
  }
  close(p[1]);
  close(sync[1]);

  check(read(sync[0], buf, 1) == 1, "parent read sync");
  close(sync[0]);
  // Give child time to xbegin and write to p[1]
  sleep(5);

  check(kill(pid) == 0, "kill child");
  check(wait() == pid, "wait killed child");

  n = read(p[0], buf, sizeof(buf));
  check(n == 0, "parent read kill pipe must be 0 (EOF)");
  close(p[0]);
  printf(1, "END TEST_EXTERNAL_KILL\n");
}

// 5. Child writer exits before owner commits (no child-exit commit deadlock).
static void
test_child_writer_owner_committer(void)
{
  int p[2];
  int pid;
  char buf[128];
  int n;

  printf(1, "BEGIN TEST_CHILD_WRITER_OWNER_COMMIT\n");
  check(pipe(p) == 0, "pipe child writer");

  check(xbegin() == 0, "owner xbegin");
  pid = fork();
  check(pid >= 0, "owner fork child");
  if(pid == 0){
    close(p[0]);
    check(write(p[1], "CHILD_EXTERNAL_PIPE_MSG-", 24) == 24, "child write external pipe");
    close(p[1]);
    exit();
  }
  // Owner waits for child to exit (child writer finished without deadlock!)
  check(wait() == pid, "owner wait child");
  check(write(p[1], "OWNER_EXTERNAL_PIPE_MSG\n", 24) == 24, "owner write external pipe");
  close(p[1]);
  check(xcommit() == 0, "owner commit");

  n = read(p[0], buf, sizeof(buf) - 1);
  check(n == 48, "read combined child and owner pipe messages");
  buf[n] = 0;
  close(p[0]);

  printf(1, "DELIVERED: %s", buf);
  printf(1, "END TEST_CHILD_WRITER_OWNER_COMMIT\n");
}

// 6. Buffer capacity, short writes, and overflow refusal.
static char bigbuf[5000];

static void
test_overflow_short_write(void)
{
  int p[2];
  int pid;
  int n1, n2;
  int i;
  char buf[32];

  printf(1, "BEGIN TEST_OVERFLOW\n");
  for(i = 0; i < sizeof(bigbuf); i++)
    bigbuf[i] = 'Z';

  check(pipe(p) == 0, "pipe overflow");
  pid = fork();
  check(pid >= 0, "fork overflow");
  if(pid == 0){
    close(p[0]);
    check(xbegin() == 0, "xbegin overflow");

    // External pipe write: first write should fill available capacity with a short write
    n1 = write(p[1], bigbuf, sizeof(bigbuf));
    check(n1 > 0 && n1 <= 4096, "short write on large buffer");

    // Subsequent write when full must return -1
    n2 = write(p[1], "MORE", 4);
    check(n2 == -1, "write to full buffer returns -1");

    check(xabort() == 0, "xabort overflow");
    close(p[1]);
    exit();
  }
  close(p[1]);
  check(wait() == pid, "wait overflow child");

  // Since child aborted, parent should read EOF (0 bytes)
  check(read(p[0], buf, sizeof(buf)) == 0, "parent read abort overflow pipe");
  close(p[0]);
  printf(1, "END TEST_OVERFLOW\n");
}

// 7. Broken pipe: writing to a pipe with no open readers returns -1.
static void
test_broken_pipe(void)
{
  int p[2];
  printf(1, "BEGIN TEST_BROKEN_PIPE\n");
  check(pipe(p) == 0, "pipe broken");
  check(xbegin() == 0, "xbegin broken pipe");
  close(p[0]); // close read end
  check(write(p[1], "BROKEN", 6) == -1, "write to broken pipe returns -1");
  close(p[1]);
  check(xabort() == 0, "xabort broken pipe");
  printf(1, "END TEST_BROKEN_PIPE\n");
}

// 8. Ordinary non-speculative pipes retain standard behavior.
static void
test_ordinary_pipes(void)
{
  int p[2];
  int pid;
  char buf[32];
  int n;

  printf(1, "BEGIN TEST_ORDINARY\n");
  check(pipe(p) == 0, "pipe ordinary");
  pid = fork();
  check(pid >= 0, "fork ordinary");
  if(pid == 0){
    close(p[0]);
    check(write(p[1], "ORDINARY_PIPE_DATA\n", 19) == 19, "child write ordinary");
    close(p[1]);
    exit();
  }
  close(p[1]);
  n = read(p[0], buf, sizeof(buf) - 1);
  check(n == 19, "parent read ordinary");
  buf[n] = 0;
  close(p[0]);
  check(wait() == pid, "wait child ordinary");
  printf(1, "END TEST_ORDINARY\n");
}

int
main(void)
{
  printf(1, "SPPIPETEST: STARTING ALL TESTS\n");
  test_internal_pipeline();
  test_external_commit();
  test_external_abort();
  test_external_kill();
  test_child_writer_owner_committer();
  test_overflow_short_write();
  test_broken_pipe();
  test_ordinary_pipes();
  printf(1, "SPPIPETEST: ALL TESTS PASSED\n");
  exit();
}
