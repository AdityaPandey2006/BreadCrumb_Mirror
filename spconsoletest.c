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

// 1. Basic speculative write + commit
static void
test_spec_commit(void)
{
  printf(1, "BEGIN TEST_COMMIT\n");
  check(xbegin() == 0, "xbegin for test_commit");
  check(write(1, "SPEC_COMMIT_MSG_FD1\n", 20) == 20, "write fd 1");
  check(write(2, "SPEC_COMMIT_MSG_FD2\n", 20) == 20, "write fd 2");
  check(xcommit() == 0, "xcommit for test_commit");
  printf(1, "END TEST_COMMIT\n");
}

// 2. Speculative write + abort discards output
static void
test_spec_abort(void)
{
  printf(1, "BEGIN TEST_ABORT\n");
  check(xbegin() == 0, "xbegin for test_abort");
  check(write(1, "LEAK_ABORT_SECRET_999\n", 22) == 22, "write abort secret");
  check(xabort() == 0, "xabort for test_abort");
  printf(1, "END TEST_ABORT\n");
}

// 3. Speculative write + owner death discards output
static void
test_spec_kill(void)
{
  int pid;
  printf(1, "BEGIN TEST_KILL\n");
  pid = fork();
  check(pid >= 0, "fork for test_kill");
  if(pid == 0){
    check(xbegin() == 0, "child xbegin");
    check(write(1, "LEAK_KILL_SECRET_888\n", 21) == 21, "child write kill secret");
    // sleep waiting to be killed
    sleep(100);
    exit();
  }
  // give child time to begin and write
  sleep(5);
  check(kill(pid) == 0, "kill child owner");
  check(wait() == pid, "wait child owner");
  printf(1, "END TEST_KILL\n");
}

// 4. Parent and child share the same console buffer
static void
test_child_share(void)
{
  int pid;
  printf(1, "BEGIN TEST_CHILD_SHARE\n");
  check(xbegin() == 0, "parent xbegin");
  check(write(1, "PARENT_START-", 13) == 13, "parent write start");
  pid = fork();
  check(pid >= 0, "fork child share");
  if(pid == 0){
    check(write(1, "CHILD_PART-", 11) == 11, "child write part");
    exit();
  }
  check(wait() == pid, "wait child share");
  check(write(1, "PARENT_END\n", 11) == 11, "parent write end");
  check(xcommit() == 0, "parent commit shared buffer");
  printf(1, "END TEST_CHILD_SHARE\n");
}

// 5. Duplicated (dup) and explicitly opened console descriptors
static void
test_dup_and_open(void)
{
  int fd_dup, fd_open;
  printf(1, "BEGIN TEST_DUP\n");
  fd_dup = dup(1);
  check(fd_dup >= 0, "dup(1)");
  fd_open = open("console", O_WRONLY);
  check(fd_open >= 0, "open console");

  check(xbegin() == 0, "xbegin for dup/open test");
  check(write(fd_dup, "DUP_WRITE_MSG-", 14) == 14, "write dup");
  check(write(fd_open, "OPEN_WRITE_MSG\n", 15) == 15, "write open");
  check(xcommit() == 0, "xcommit dup/open");

  close(fd_dup);
  close(fd_open);
  printf(1, "END TEST_DUP\n");
}

// 6. Empty commit with only console output
static void
test_empty_commit(void)
{
  printf(1, "BEGIN TEST_EMPTY_COMMIT\n");
  check(xbegin() == 0, "xbegin empty commit");
  check(write(1, "EMPTY_COMMIT_OUTPUT\n", 20) == 20, "write empty commit");
  check(xcommit() == 0, "xcommit empty commit");
  printf(1, "END TEST_EMPTY_COMMIT\n");
}

// 7. Buffer capacity overflow and short-write behavior
static void
test_overflow(void)
{
  static char bigbuf[1024];
  int i, r;

  printf(1, "BEGIN TEST_OVERFLOW\n");
  for(i = 0; i < 1024; i++)
    bigbuf[i] = 'A';

  check(xbegin() == 0, "xbegin overflow");

  // Write 4000 bytes (4 * 1000)
  for(i = 0; i < 4; i++){
    r = write(1, bigbuf, 1000);
    check(r == 1000, "fill buffer 1000 bytes");
  }
  // Currently at 4000 bytes buffered. Capacity is 4096.
  // Remaining space = 96 bytes.
  // Now write 150 bytes: should short-write exactly 96 bytes!
  r = write(1, bigbuf, 150);
  check(r == 96, "short write on remaining space (96 bytes)");

  // Buffer is now completely full (4096 bytes).
  // Next write must fail with -1 (0 bytes can be written).
  r = write(1, bigbuf, 10);
  check(r == -1, "write to full buffer returns -1");

  // Now abort: all 4096 bytes must be discarded without leaking!
  check(xabort() == 0, "xabort overflow buffer");
  printf(1, "END TEST_OVERFLOW\n");
}

static int
memcmp_bytes(const char *a, const char *b, int n)
{
  int i;
  for(i = 0; i < n; i++){
    if(a[i] != b[i])
      return 1;
  }
  return 0;
}

// 8. Durable ordering: filesystem writes are durable when output is released
static void
test_durable_order(void)
{
  int fd;
  char buf[8];

  printf(1, "BEGIN TEST_DURABLE_ORDER\n");
  check(xbegin() == 0, "xbegin durable order");

  fd = open("durable_test.txt", O_CREATE | O_RDWR);
  check(fd >= 0, "create durable_test.txt");
  check(write(fd, "DURABLE!", 8) == 8, "write file");
  close(fd);

  check(write(1, "CONSOLE_AFTER_FS_DURABLE\n", 25) == 25, "write console");
  check(xcommit() == 0, "xcommit durable order");

  // Check file is present and has data
  fd = open("durable_test.txt", O_RDONLY);
  check(fd >= 0, "open durable_test.txt after commit");
  check(read(fd, buf, 8) == 8, "read durable_test.txt");
  check(memcmp_bytes(buf, "DURABLE!", 8) == 0, "verify file content");
  close(fd);
  unlink("durable_test.txt");

  printf(1, "END TEST_DURABLE_ORDER\n");
}

int
main(int argc, char *argv[])
{
  printf(1, "SPCONSOLETEST: START\n");

  test_spec_commit();
  test_spec_abort();
  test_spec_kill();
  test_child_share();
  test_dup_and_open();
  test_empty_commit();
  test_overflow();
  test_durable_order();

  printf(1, "SPCONSOLETEST: ALL TESTS PASSED\n");
  exit();
}
