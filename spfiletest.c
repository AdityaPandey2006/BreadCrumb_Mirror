// Step 4 end-to-end tests: speculative reads and non-extending writes of
// existing regular files, using only ordinary file system calls. Ordering
// between processes uses pipes. Allocation-failure injection is used only in
// SHADOW_TEST=1 builds (through the test-only shadowtest syscall).
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "param.h"
#include "fs.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

#define F       "sf"
#define NBLK    14             // 12 direct + 2 through the indirect block
#define FSZ     (NBLK*BSIZE)

static int failures;
static char buf[1024];
static char tmp[BSIZE];

static void
check(int ok, char *what)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    failures++;
}

static char
pat(int i)
{
  return 'a' + (i*7 + i/BSIZE) % 26;
}

static void
send(int fd, int ok)
{
  char c = ok ? 'P' : 'F';
  write(fd, &c, 1);
}

static int
receive(int fd)
{
  char c = 0;
  return read(fd, &c, 1) == 1 && c == 'P';
}

// Create (or replace) F with NBLK blocks of the known pattern. Ordinary only.
static void
mkfile(void)
{
  int fd, i, b;
  unlink(F);
  fd = open(F, O_CREATE|O_RDWR);
  for(b = 0; fd >= 0 && b < NBLK; b++){
    for(i = 0; i < BSIZE; i++)
      tmp[i] = pat(b*BSIZE + i);
    if(write(fd, tmp, BSIZE) != BSIZE){
      close(fd);
      fd = -1;
    }
  }
  if(fd < 0){
    printf(1, "FAIL: could not prepare %s\n", F);
    exit();
  }
  close(fd);
}

// xv6 has no lseek: advance by reading.
static int
skip(int fd, int n)
{
  int k;
  while(n > 0){
    k = n > BSIZE ? BSIZE : n;
    if(read(fd, tmp, k) != k)
      return -1;
    n -= k;
  }
  return 0;
}

static int
write_at(int off, char *data, int n)
{
  int fd, r;
  if((fd = open(F, O_RDWR)) < 0)
    return -100;
  if(skip(fd, off) < 0){
    close(fd);
    return -101;
  }
  r = write(fd, data, n);
  close(fd);
  return r;
}

static int
read_at(int off, char *dst, int n)
{
  int fd, r;
  if((fd = open(F, O_RDONLY)) < 0)
    return -100;
  if(skip(fd, off) < 0){
    close(fd);
    return -101;
  }
  r = read(fd, dst, n);
  close(fd);
  return r;
}

static int
expect_at(int off, char *s)
{
  int n = strlen(s), i;
  if(read_at(off, buf, n) != n)
    return 0;
  for(i = 0; i < n; i++)
    if(buf[i] != s[i])
      return 0;
  return 1;
}

// The public/original pattern at [off, off+n).
static int
expect_pattern(int off, int n)
{
  int k, i;
  while(n > 0){
    k = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
    if(read_at(off, buf, k) != k)
      return 0;
    for(i = 0; i < k; i++)
      if(buf[i] != pat(off + i))
        return 0;
    off += k;
    n -= k;
  }
  return 1;
}

static int
file_size(void)
{
  struct stat st;
  if(stat(F, &st) < 0)
    return -1;
  return st.size;
}

// Outsider (forked before xbegin) vs. speculation; D20 refusal; non-snapshot.
static void
test_outsider_isolation(void)
{
  int p2o[2], o2p[2], pid, i;

  printf(1, "-- outsider isolation and outside-writer refusal\n");
  mkfile();
  pipe(p2o);
  pipe(o2p);
  if((pid = fork()) == 0){
    close(p2o[1]);
    close(o2p[0]);
    receive(p2o[0]);
    send(o2p[1], expect_pattern(0, 2*BSIZE + 16));   // original bytes
    send(o2p[1], write_at(10, "OUT", 3) == -1);       // block 0 copied
    send(o2p[1], expect_pattern(0, BSIZE));
    send(o2p[1], unlink(F) == -1);                    // file has copies
    for(i = 0; i < 2*BSIZE; i++)
      buf[i % sizeof(buf)] = 'Z';
    // Block 1 is not copied, block 2 is: short write of exactly 512 bytes.
    send(o2p[1], write_at(BSIZE, buf, 2*BSIZE) == BSIZE);
    send(o2p[1], expect_pattern(2*BSIZE, BSIZE));
    send(o2p[1], write_at(5*BSIZE + 3, "OK", 2) == 2); // uncopied block
    exit();
  }
  close(p2o[0]);
  close(o2p[1]);

  check(xbegin() == 0, "speculation begins");
  check(write_at(0, "WORLD", 5) == 5, "speculative overwrite of block 0");
  check(write_at(2*BSIZE + 7, "XY", 2) == 2, "speculative overwrite of block 2");
  check(expect_at(0, "WORLD") && expect_at(2*BSIZE + 7, "XY"),
        "speculation reads its own private bytes");
  check(expect_pattern(5, BSIZE - 5), "rest of private block keeps original bytes");
  check(file_size() == FSZ, "stat size unchanged by non-extending write");
  send(p2o[1], 1);
  check(receive(o2p[0]), "outsider reads original bytes");
  check(receive(o2p[0]), "outsider write to a privately copied block returns -1");
  check(receive(o2p[0]), "refused outsider write left the public block unchanged");
  check(receive(o2p[0]), "outsider unlink of a file with private copies returns -1");
  check(receive(o2p[0]), "outsider write stops short at a copied block (512 bytes)");
  check(receive(o2p[0]), "copied block untouched by the short outsider write");
  check(receive(o2p[0]), "outsider may write an uncopied block");
  wait();
  check(expect_at(5*BSIZE + 3, "OK") && expect_at(BSIZE, "ZZZZ"),
        "speculation sees outsider writes to uncopied blocks (no snapshot)");
  check(expect_at(0, "WORLD"), "private bytes survive outsider activity");
  // Step 5 made nonempty commit work; spcommit covers it. This test aborts.
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, BSIZE) && expect_pattern(2*BSIZE, BSIZE),
        "abort preserves the original public bytes");
  check(write_at(10, "abc", 3) == 3, "outside write allowed again after abort");
  check(unlink(F) == 0, "outside unlink allowed again after abort");
  close(p2o[1]);
  close(o2p[0]);
}

static void
test_parent_child_share(void)
{
  int c2p[2], pid;

  printf(1, "-- parent and child share private updates\n");
  mkfile();
  pipe(c2p);
  check(xbegin() == 0, "speculation begins");
  check(write_at(0, "PARENT", 6) == 6, "parent writes privately");
  if((pid = fork()) == 0){
    close(c2p[0]);
    send(c2p[1], expect_at(0, "PARENT"));
    send(c2p[1], write_at(3*BSIZE, "CHILD", 5) == 5);
    exit();
  }
  close(c2p[1]);
  check(pid > 0, "member child forked");
  check(receive(c2p[0]), "child sees the parent's private bytes");
  check(receive(c2p[0]), "child writes privately");
  wait();
  check(expect_at(3*BSIZE, "CHILD"), "parent sees the child's private bytes");
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, 8) && expect_pattern(3*BSIZE, 8),
        "abort discards both members' changes");
  close(c2p[0]);
  unlink(F);
}

// A and B are independent speculations; the parent is an outsider.
static void
test_independent(void)
{
  int a[2], b[2], r[2], toa[2], tob[2], i;

  printf(1, "-- independent speculations do not see each other\n");
  mkfile();
  pipe(a); pipe(b); pipe(r); pipe(toa); pipe(tob);
  if(fork() == 0){                       // A
    int ok = xbegin() == 0 && write_at(0, "AAAA", 4) == 4;
    send(a[1], ok);
    receive(toa[0]);                     // B has written its own copy
    ok = ok && expect_at(0, "AAAA");
    ok = ok && xabort() == 0;
    send(r[1], ok);
    exit();
  }
  if(fork() == 0){                       // B
    int ok;
    receive(tob[0]);
    ok = xbegin() == 0 && expect_pattern(0, 4);
    ok = ok && write_at(0, "BBBB", 4) == 4 && expect_at(0, "BBBB");
    send(b[1], ok);
    receive(tob[0]);
    ok = ok && expect_at(0, "BBBB") && xabort() == 0;
    send(r[1], ok);
    exit();
  }
  check(receive(a[0]), "speculation A writes block 0 privately");
  send(tob[1], 1);
  check(receive(b[0]), "speculation B sees original bytes, then its own");
  check(expect_pattern(0, 4), "outsider still sees original bytes");
  send(toa[1], 1);
  send(tob[1], 1);
  for(i = 0; i < 2; i++)
    check(receive(r[0]), "each speculation keeps only its own private bytes");
  wait();
  wait();
  check(expect_pattern(0, BSIZE), "both aborts preserve the original");
  close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(r[0]); close(r[1]);
  close(toa[0]); close(toa[1]); close(tob[0]); close(tob[1]);
  unlink(F);
}

// Reads with no private copies, cross-block, repeated, cap and partial write.
static void
test_blocks_and_capacity(void)
{
  int fd, i, r, size;
  char c;

  printf(1, "-- reads without copies, cross-block, reuse, cap, partial write\n");
  mkfile();
  check(xbegin() == 0, "speculation begins");
  check(expect_pattern(0, FSZ), "reads without private copies return originals");
  for(i = 0; i < 30; i++)
    buf[i] = '0' + i % 10;
  buf[30] = 0;
  check(write_at(500, buf, 30) == 30, "cross-block write (blocks 0 and 1)");
  check(expect_at(500, "012345678901234567890123456789"),
        "cross-block bytes read back");
  check(expect_pattern(490, 10) && expect_pattern(530, 10),
        "bytes around the cross-block write unchanged");
  fd = open(F, O_RDWR);
  skip(fd, 100);
  for(r = 0, i = 0; i < 20; i++){
    c = 'r';
    r += write(fd, &c, 1) == 1;
  }
  close(fd);
  check(r == 20, "20 repeated writes inside block 0");
  // Blocks 0,1 used. 2..6 and 12 (via the indirect block) reach 8 entries.
  r = 1;
  for(i = 2; i <= 6; i++)
    r = r && write_at(i*BSIZE + 1, "D", 1) == 1;
  r = r && write_at(12*BSIZE + 1, "I", 1) == 1;
  check(r, "distinct blocks up to the cap (repeated writes reused entries)");
  check(expect_at(12*BSIZE + 1, "I"), "private write through an indirect block");
  check(write_at(8*BSIZE, "N", 1) == -1, "write needing a ninth block returns -1");
  check(expect_pattern(8*BSIZE, 4), "rejected block unchanged in private view");
  // Block 6 is copied, block 7 is not: 12 bytes fit, then the cap stops it.
  for(i = 0; i < 40; i++)
    buf[i] = 'P';
  fd = open(F, O_RDWR);
  skip(fd, 6*BSIZE + 500);
  check(write(fd, buf, 40) == 12, "write spanning into a ninth block is partial (12)");
  check(read(fd, tmp, 4) == 4 && tmp[0] == pat(7*BSIZE),
        "file offset advanced by exactly the bytes written");
  close(fd);
  check(expect_at(6*BSIZE + 500, "PPPPPPPPPPPP") && expect_pattern(7*BSIZE, 8),
        "partial write left a documented private view");
  size = file_size();
  check(size == FSZ, "stat size unchanged");
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, FSZ), "abort restores every original byte");
  unlink(F);
}

#ifdef SHADOW_TEST
static void
test_allocation_failure(void)
{
  int pages;

  printf(1, "-- allocation failure (SHADOW_TEST build)\n");
  mkfile();
  pages = shadowtest(ST_PAGES, 0, 0);
  check(xbegin() == 0, "speculation begins");
  shadowtest(ST_FAIL, 0, 0);
  check(write_at(0, "X", 1) == -1, "allocation failure before any byte returns -1");
  check(expect_pattern(0, 4), "failed write left no private change");
  check(write_at(500, "Y", 1) == 1, "write after the failure succeeds");
  shadowtest(ST_FAIL, 0, 0);
  check(write_at(500, "QQQQQQQQQQQQQQQQQQQQ", 20) == 12,
        "allocation failure part-way returns bytes written (12)");
  check(expect_at(500, "QQQQQQQQQQQQ") && expect_pattern(512, 8),
        "partial private view matches the returned count");
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, FSZ), "public file unchanged after allocation failures");
  check(shadowtest(ST_PAGES, 0, 0) == pages, "no shadow pages leaked");
  unlink(F);
}
#endif

static void
test_owner_death(int by_kill)
{
  int o2t[2], t2o[2], m2o[2], hold[2], owner, i, ok;

  printf(1, "-- owner %s discards private work\n", by_kill ? "kill" : "exit");
  mkfile();
  pipe(o2t); pipe(t2o); pipe(m2o); pipe(hold);
  if((owner = fork()) == 0){
    char c;
    ok = xbegin() == 0 && write_at(0, "DEAD", 4) == 4;
    if(fork() == 0){                     // member blocks until cancelled
      send(m2o[1], 1);
      read(hold[0], &c, 1);
      exit();
    }
    receive(m2o[0]);
    send(o2t[1], ok);
    receive(t2o[0]);                     // wait for the outsider's checks
    if(by_kill)
      for(;;)
        sleep(100);
    exit();                              // voluntary exit cancels
  }
  check(receive(o2t[0]), "owner and member write privately");
  check(expect_pattern(0, 4), "outsider sees original while owner lives");
  check(write_at(0, "abcd", 4) == -1, "outsider write refused while copies exist");
  if(by_kill)
    kill(owner);
  else
    send(t2o[1], 1);
  wait();
  // The member is killed by cancellation; copies go when it detaches.
  for(ok = 0, i = 0; i < 500 && !ok; i++){
    ok = write_at(0, "aaaa", 4) == 4;
    if(!ok)
      sleep(1);
  }
  check(ok, "private copies discarded after owner death and member exit");
  for(i = 0; i < 4; i++)
    buf[i] = pat(i);
  write_at(0, buf, 4);
  check(expect_pattern(0, FSZ), "public file holds only original bytes");
  close(o2t[0]); close(o2t[1]); close(t2o[0]); close(t2o[1]);
  close(m2o[0]); close(m2o[1]); close(hold[0]); close(hold[1]);
  unlink(F);
}

static void
test_unsupported(void)
{
  int fd;
  char c = 'E';

  printf(1, "-- unsupported mutations rejected without public change\n");
  mkfile();
  unlink("sfnew"); unlink("sflink"); unlink("sfdir"); unlink("sfnod");
  check(xbegin() == 0, "speculation begins");
  check(open("sfnew", O_CREATE|O_RDWR) < 0, "creating a new file returns -1");
  fd = open(F, O_CREATE|O_RDWR);
  check(fd >= 0, "O_CREATE of an existing file opens it unchanged");
  if(fd >= 0){
    skip(fd, FSZ);
    check(write(fd, &c, 1) == -1, "extending write at EOF returns -1");
    close(fd);
  }
  check(write_at(FSZ - 2, "EEEE", 4) == -1, "write straddling EOF returns -1");
  check(expect_pattern(FSZ - 2, 2), "straddling write changed nothing");
  check(file_size() == FSZ, "size unchanged");
  check(unlink(F) == -1, "unlink returns -1");
  check(link(F, "sflink") == -1, "link returns -1");
  check(mkdir("sfdir") == -1, "mkdir returns -1");
  check(mknod("sfnod", 1, 1) == -1, "mknod returns -1");
  fd = open(".", O_RDONLY);
  check(fd >= 0 && read(fd, tmp, 16) == 16, "reading a directory opened in the speculation works");
  if(fd >= 0)
    close(fd);
  check(xabort() == 0, "abort succeeds");
  check(open("sfnew", O_RDONLY) < 0 && open("sflink", O_RDONLY) < 0 &&
        open("sfdir", O_RDONLY) < 0 && open("sfnod", O_RDONLY) < 0,
        "no rejected name reached the public namespace");
  check(file_size() == FSZ && expect_pattern(0, FSZ), "file intact after rejections");
  unlink(F);
}

static void
test_descriptors(void)
{
  int pre, in, p[2], c2p[2];
  char c;

  printf(1, "-- descriptor rule: only files opened in the same speculation\n");
  mkfile();
  pre = open(F, O_RDWR);
  pipe(p);
  pipe(c2p);
  check(xbegin() == 0, "speculation begins");
  check(read(pre, &c, 1) == -1, "read through a pre-xbegin descriptor returns -1");
  check(write(pre, "W", 1) == -1, "write through a pre-xbegin descriptor returns -1");
  check(fstat(pre, (struct stat*)tmp) == 0, "fstat of a pre-xbegin descriptor works");
  check(write(p[1], "x", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'x',
        "pipes keep ordinary behaviour (buffering is a later step)");
  in = open(F, O_RDWR);
  check(in >= 0 && write(in, "IN", 2) == 2, "descriptor opened in the speculation works");
  if(fork() == 0){
    close(c2p[0]);
    send(c2p[1], write(in, "CH", 2) == 2);   // member shares the file object
    exit();
  }
  close(c2p[1]);
  check(receive(c2p[0]), "member child may use the speculation's descriptor");
  wait();
  check(expect_at(0, "INCH"), "shared offset among members only");
  check(xabort() == 0, "abort succeeds");
  check(read(pre, tmp, 4) == 4 && tmp[0] == pat(0),
        "pre-xbegin descriptor usable again, offset untouched");
  check(xbegin() == 0, "new speculation begins");
  check(read(in, &c, 1) == -1, "descriptor from an earlier speculation returns -1");
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, FSZ), "original preserved");
  close(pre); close(in); close(p[0]); close(p[1]); close(c2p[0]);
  unlink(F);
}

static void
test_ordinary(void)
{
  int fd;

  printf(1, "-- non-speculative behaviour unchanged\n");
  unlink("sfplain");
  fd = open("sfplain", O_CREATE|O_RDWR);
  check(fd >= 0 && write(fd, "plain", 5) == 5, "ordinary create and write");
  check(write(fd, buf, sizeof(buf)) == sizeof(buf), "ordinary extending write");
  close(fd);
  check(link("sfplain", "sfplain2") == 0 && unlink("sfplain2") == 0,
        "ordinary link and unlink");
  fd = open("sfplain", O_RDONLY);
  check(fd >= 0 && read(fd, tmp, 5) == 5 && tmp[0] == 'p' && tmp[4] == 'n',
        "ordinary read");
  close(fd);
  check(unlink("sfplain") == 0, "ordinary unlink");
}

int
main(void)
{
  test_ordinary();
  test_outsider_isolation();
  test_parent_child_share();
  test_independent();
  test_blocks_and_capacity();
#ifdef SHADOW_TEST
  test_allocation_failure();
#endif
  test_owner_death(0);
  test_owner_death(1);
  test_unsupported();
  test_descriptors();
#ifdef SHADOW_TEST
  check(shadowtest(ST_PUBWRITES, 0, 0) == 0,
        "no log_write() (dirty public buffer) by any speculative process");
#endif
  test_ordinary();
  if(failures == 0)
    printf(1, "SPFILE TESTS PASSED\n");
  else
    printf(1, "SPFILE TESTS FAILED: %d\n", failures);
  exit();
}
