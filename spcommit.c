// Step 5 end-to-end tests: abort, durable commit, commit-time conflicts
// (D24), owner kill around the point of no return (D23), live-reader
// isolation during publication, and crash/recovery modes. Only ordinary file
// calls are used for data; the test-only shadowtest syscall (SHADOW_TEST=1
// builds) only pauses/crashes the kernel at named points and counts pages.
//
//   spcommit                      run the normal test suite
//   spcommit crashsetup           create the crash-test files (old contents)
//   spcommit crash N              commit a 3-block, 2-file speculation and
//                                 halt the machine at crash point N
//   spcommit crashverify old|new  after reboot: check complete old/new state
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "param.h"
#include "fs.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

#define F     "cf"
#define NBLK  10
#define FSZ   (NBLK*BSIZE)

static int failures;
static char buf[1024];
static char tmp[BSIZE];
static char skipbuf[BSIZE];   // skip() only: callers pass data in tmp

static void
check(int ok, char *what)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    failures++;
}

static char
pat(int seed, int i)
{
  return 'a' + (i*7 + i/BSIZE + seed) % 26;
}

#ifdef SHADOW_TEST
static char
newpat(int seed, int i)
{
  return 'A' + (i*3 + seed) % 26;
}
#endif

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

static void
mkfile_n(char *name, int seed, int nblk)
{
  int fd, i, b;
  unlink(name);
  fd = open(name, O_CREATE|O_RDWR);
  for(b = 0; fd >= 0 && b < nblk; b++){
    for(i = 0; i < BSIZE; i++)
      tmp[i] = pat(seed, b*BSIZE + i);
    if(write(fd, tmp, BSIZE) != BSIZE){
      close(fd);
      fd = -1;
    }
  }
  if(fd < 0){
    printf(1, "FAIL: could not prepare %s\n", name);
    exit();
  }
  close(fd);
}

static void
mkfile(void)
{
  mkfile_n(F, 0, NBLK);
}

static int
skip(int fd, int n)
{
  int k;
  while(n > 0){
    k = n > BSIZE ? BSIZE : n;
    if(read(fd, skipbuf, k) != k)
      return -1;
    n -= k;
  }
  return 0;
}

static int
write_at_f(char *name, int off, char *data, int n)
{
  int fd, r;
  if((fd = open(name, O_RDWR)) < 0)
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
write_at(int off, char *data, int n)
{
  return write_at_f(F, off, data, n);
}

static int
read_at_f(char *name, int off, char *dst, int n)
{
  int fd, r;
  if((fd = open(name, O_RDONLY)) < 0)
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
  if(read_at_f(F, off, buf, n) != n)
    return 0;
  for(i = 0; i < n; i++)
    if(buf[i] != s[i])
      return 0;
  return 1;
}

static int
expect_pattern_f(char *name, int seed, int off, int n)
{
  int k, i;
  while(n > 0){
    k = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
    if(read_at_f(name, off, buf, k) != k)
      return 0;
    for(i = 0; i < k; i++)
      if(buf[i] != pat(seed, off + i))
        return 0;
    off += k;
    n -= k;
  }
  return 1;
}

static int
expect_pattern(int off, int n)
{
  return expect_pattern_f(F, 0, off, n);
}

static int
file_size(char *name)
{
  struct stat st;
  if(stat(name, &st) < 0)
    return -1;
  return st.size;
}

#ifdef SHADOW_TEST
// Fill a whole block with a byte value, privately or publicly.
static int
fill_block(int blk, char c)
{
  int i;
  for(i = 0; i < BSIZE; i++)
    tmp[i] = c;
  return write_at(blk*BSIZE, tmp, BSIZE);
}

static int
block_is(int blk, char c)
{
  int i;
  if(read_at_f(F, blk*BSIZE, buf, BSIZE) != BSIZE)
    return 0;
  for(i = 0; i < BSIZE; i++)
    if(buf[i] != c)
      return 0;
  return 1;
}
#endif

static void
test_abort(void)
{
  printf(1, "-- abort restores the original view\n");
  mkfile();
  check(xbegin() == 0, "speculation begins");
  check(write_at(0, "GONE", 4) == 4 && write_at(3*BSIZE + 9, "GONE", 4) == 4,
        "private writes to two blocks");
  check(xabort() == 0, "abort succeeds");
  check(expect_pattern(0, FSZ), "every original byte restored");
  check(write_at(0, "abcd", 4) == 4, "outside write allowed after abort");
  unlink(F);
}

static void
test_commit_publishes(void)
{
  int fd, p[2];

  printf(1, "-- commit publishes private changes durably\n");
  mkfile();
  check(xbegin() == 0, "speculation begins");
  fd = open(F, O_RDWR);
  check(write_at(0, "NEW0", 4) == 4 && write_at(3*BSIZE + 100, "NEW3", 4) == 4 &&
        write_at(7*BSIZE + 511, "N", 1) == 1, "private writes to three blocks");
  check(write_at(5, "x", 1) == 1, "repeated write to an already-copied block");
  check(xcommit() == 0, "nonempty commit returns 0");
  check(expect_at(0, "NEW0") && expect_at(5, "x") && expect_at(3*BSIZE + 100, "NEW3") &&
        expect_at(7*BSIZE + 511, "N"), "all private bytes published");
  check(expect_pattern(4, 1) && expect_pattern(BSIZE, 2*BSIZE) &&
        expect_pattern(8*BSIZE, 2*BSIZE), "untouched bytes unchanged");
  check(file_size(F) == FSZ, "size unchanged");
  check(xabort() == -1, "owner is no longer speculative after commit");
  check(fd >= 0 && read(fd, tmp, 4) == 4 && tmp[0] == 'N',
        "descriptor opened in the speculation usable after commit");
  close(fd);
  pipe(p);
  if(fork() == 0){
    close(p[0]);
    send(p[1], expect_at(0, "NEW0") && expect_at(3*BSIZE + 100, "NEW3"));
    send(p[1], write_at(0, "out!", 4) == 4);   // copies released (D20)
    exit();
  }
  close(p[1]);
  check(receive(p[0]), "new outside process sees committed bytes");
  check(receive(p[0]), "outside write allowed after commit");
  wait();
  close(p[0]);
  check(xbegin() == 0 && xabort() == 0, "a new speculation can start afterwards");
  unlink(F);
}

static void
test_child_changes(void)
{
  printf(1, "-- shared child changes survive child exit and are committed\n");
  mkfile();
  check(xbegin() == 0, "speculation begins");
  if(fork() == 0){
    write_at(2*BSIZE, "KID", 3);
    exit();
  }
  wait();
  check(expect_at(2*BSIZE, "KID"), "parent sees exited child's private bytes");
  check(write_at(5*BSIZE, "MOM", 3) == 3, "parent writes privately");
  check(xcommit() == 0, "commit succeeds after child exit");
  check(expect_at(2*BSIZE, "KID") && expect_at(5*BSIZE, "MOM"),
        "child's and parent's changes both published");
  unlink(F);
}

static void
test_permissions(void)
{
  int c2p[2], p2c[2];

  printf(1, "-- permission, live-member and repeated-completion checks\n");
  mkfile();
  pipe(c2p);
  pipe(p2c);
  check(xbegin() == 0, "speculation begins");
  check(write_at(0, "PERM", 4) == 4, "private write");
  if(fork() == 0){
    close(c2p[0]);
    close(p2c[1]);
    send(c2p[1], xcommit() == -1 && xabort() == -1);
    receive(p2c[0]);
    exit();
  }
  close(c2p[1]);
  close(p2c[0]);
  check(receive(c2p[0]), "non-owner member cannot commit or abort");
  check(xcommit() == -1, "owner cannot commit while a member lives");
  check(expect_at(0, "PERM"), "refused commit keeps the private attempt");
  send(p2c[1], 1);
  wait();
  check(xcommit() == 0, "owner commits after the member exits");
  check(xcommit() == -1 && xabort() == -1, "repeated completion fails");
  check(expect_at(0, "PERM"), "commit published");
  close(c2p[0]);
  close(p2c[1]);
  unlink(F);
}

static int
wait_pages(int target)
{
#ifdef SHADOW_TEST
  int i;
  for(i = 0; i < 500; i++){
    if(shadowtest(ST_PAGES, 0, 0) == target)
      return 1;
    sleep(1);
  }
  return 0;
#else
  sleep(10);
  return 1;
#endif
}

// D24: first committer wins; the loser becomes abort-only.
static void
test_conflict(void)
{
  int a2p[2], b2p[2], p2a[2], p2b[2], r1, r2;

  printf(1, "-- commit-time conflict between speculations (D24)\n");
  mkfile();
  pipe(a2p); pipe(b2p); pipe(p2a); pipe(p2b);
  if(fork() == 0){                                  // speculation A
    int ok = xbegin() == 0 && write_at(0, "AAAA", 4) == 4 &&
             write_at(BSIZE, "A1", 2) == 2;
    receive(p2a[0]);
    int c = (xcommit() == 0);
    send(a2p[1], ok && c);
    exit();
  }
  if(fork() == 0){                                  // speculation B
    int ok = xbegin() == 0 && write_at(0, "BBBB", 4) == 4 &&
             write_at(6*BSIZE, "B6", 2) == 2;
    receive(p2b[0]);
    r1 = xcommit();
    r2 = xcommit();
    ok = ok && (r1 == -1) && (r2 == -1) && expect_at(0, "BBBB");
    int a = (xabort() == 0);
    send(b2p[1], ok);
    send(b2p[1], a);
    exit();
  }
  check(wait_pages(4), "both speculations copied block 0");
  send(p2a[1], 1);
  check(receive(a2p[0]), "first committer (A) succeeds");
  send(p2b[1], 1);
  check(receive(b2p[0]), "second committer (B) gets -1 twice (abort-only), view kept");
  check(receive(b2p[0]), "abort-only speculation can still abort");
  wait();
  wait();
  check(expect_at(0, "AAAA") && expect_at(BSIZE, "A1"), "A's changes published");
  check(expect_pattern(6*BSIZE, 2), "none of B's changes published (not even block 6)");
  close(a2p[0]); close(a2p[1]); close(b2p[0]); close(b2p[1]);
  close(p2a[0]); close(p2a[1]); close(p2b[0]); close(p2b[1]);
  unlink(F);
}

static void
test_disjoint(void)
{
  int r[2], i;

  printf(1, "-- speculations on different blocks both commit\n");
  mkfile();
  pipe(r);
  if(fork() == 0){
    send(r[1], xbegin() == 0 && write_at(BSIZE, "C1", 2) == 2 && xcommit() == 0);
    exit();
  }
  if(fork() == 0){
    send(r[1], xbegin() == 0 && write_at(4*BSIZE, "D4", 2) == 2 && xcommit() == 0);
    exit();
  }
  for(i = 0; i < 2; i++)
    check(receive(r[0]), "disjoint speculation commits");
  wait();
  wait();
  check(expect_at(BSIZE, "C1") && expect_at(4*BSIZE, "D4"), "both changes published");
  close(r[0]); close(r[1]);
  unlink(F);
}

static void
test_empty_and_capacity(void)
{
  int i, ok, fd;

  printf(1, "-- empty commit, capacity and short write then commit (D25)\n");
  mkfile();
  check(xbegin() == 0 && expect_pattern(0, FSZ) && xcommit() == 0,
        "read-only speculation commits (empty)");
  check(expect_pattern(0, FSZ), "empty commit changed nothing");
  check(xbegin() == 0, "speculation begins");
  for(ok = 1, i = 0; i < NSHADOW; i++)
    ok = ok && write_at(i*BSIZE + 1, "Q", 1) == 1;
  check(ok, "eight distinct blocks copied");
  check(write_at(NSHADOW*BSIZE, "X", 1) == -1, "ninth block refused");
  fd = open(F, O_RDWR);
  skip(fd, (NSHADOW-1)*BSIZE + 508);
  check(write(fd, "SHORTWRITE", 10) == 4, "short write returns 4 bytes");
  close(fd);
  check(xcommit() == 0, "commit after a refused block and a short write (D25)");
  for(ok = 1, i = 0; i < NSHADOW; i++)
    ok = ok && expect_at(i*BSIZE + 1, "Q");
  check(ok, "all eight blocks published in one transaction");
  check(expect_at((NSHADOW-1)*BSIZE + 508, "SHOR") && expect_pattern(NSHADOW*BSIZE, 4),
        "short write published exactly; ninth block untouched");
  unlink(F);
}

static void
test_outsider_policy(void)
{
  int o2p[2], p2o[2], i, ok;

  printf(1, "-- outside writer refused until commit (D20)\n");
  mkfile();
  pipe(o2p); pipe(p2o);
  if(fork() == 0){
    close(p2o[1]); close(o2p[0]);
    for(i = 0; i < 500; i++){
      if(write_at(0, "out", 3) == -1)
        break;
      sleep(1);
    }
    ok = (i < 500) && (unlink(F) == -1);
    send(o2p[1], ok);
    receive(p2o[0]);
    send(o2p[1], write_at(0, "out", 3) == 3);
    exit();
  }
  close(p2o[0]); close(o2p[1]);
  check(xbegin() == 0 && write_at(0, "SPEC", 4) == 4, "speculation copies block 0");
  check(receive(o2p[0]), "outside write and unlink refused while copied");
  check(xcommit() == 0, "commit succeeds");
  send(p2o[1], 1);
  check(receive(o2p[0]), "outside write allowed after commit");
  wait();
  check(expect_at(0, "out") && expect_at(3, "C"), "outsider wrote after the committed bytes");
  close(o2p[0]); close(p2o[1]);
  unlink(F);
}

static void
test_reuse(void)
{
  int i, ok = 1;
  char want[NSHADOW];
#ifdef SHADOW_TEST
  int pages = shadowtest(ST_PAGES, 0, 0);
#endif

  printf(1, "-- 40 alternating commit/abort cycles reuse contexts and memory\n");
  mkfile();
  for(i = 0; i < NSHADOW; i++)
    want[i] = pat(0, i*BSIZE);
  for(i = 0; i < 40 && ok; i++){
    char c = 'A' + i % 26;
    int blk = i % NSHADOW;
    ok = xbegin() == 0 && write_at(blk*BSIZE, &c, 1) == 1;
    if(i % 2 == 0){
      ok = ok && xcommit() == 0;
      want[blk] = c;
    } else
      ok = ok && xabort() == 0;
  }
  check(ok, "every cycle begins and completes");
  for(i = 0; i < NSHADOW; i++){
    char s[2];
    s[0] = want[i];
    s[1] = 0;
    ok = ok && expect_at(i*BSIZE, s);
  }
  check(ok, "public file holds exactly the committed cycles' bytes");
#ifdef SHADOW_TEST
  check(shadowtest(ST_PAGES, 0, 0) == pages, "no shadow pages leaked");
#endif
  unlink(F);
}

// Ordinary operations keep the log busy while a speculation commits
// repeatedly, so publication often has to wait for a group to drain. This
// is the path where an early bug let ordinary operations join the
// "exclusive" publication transaction (see step5-verification.md).
static void
test_concurrent_ordinary_ops(void)
{
  int done[2], i, ok = 1, wok;
  char c;

  printf(1, "-- commits interleaved with a busy ordinary writer\n");
  mkfile();
  mkfile_n("cg", 3, 4);
  pipe(done);
  if(fork() == 0){
    int k, good = 1;
    for(k = 0; k < 300; k++){
      c = 'a' + k % 26;
      good = good && write_at_f("cg", (k % 4)*BSIZE + 7, &c, 1) == 1;
    }
    send(done[1], good);
    exit();
  }
  for(i = 0; i < 25 && ok; i++){
    c = 'A' + i % 26;
    ok = xbegin() == 0 && write_at((i % 5)*BSIZE + 3, &c, 1) == 1 &&
         write_at(9*BSIZE + 3, &c, 1) == 1 && xcommit() == 0;
  }
  wok = receive(done[0]);
  wait();
  check(ok, "25 commits succeed while an ordinary writer runs");
  check(wok, "300 ordinary writes succeed concurrently");
  check(expect_at(9*BSIZE + 3, "Y") && file_size(F) == FSZ && file_size("cg") == 4*BSIZE,
        "final committed bytes and sizes correct");
  close(done[0]); close(done[1]);
  unlink("cg");
  unlink(F);
}

#ifdef SHADOW_TEST
static int
wait_held(int point)
{
  int i;
  for(i = 0; i < 2000; i++){
    if(shadowtest(ST_PUBHELD, 0, 0) == point)
      return 1;
    sleep(1);
  }
  return 0;
}

// While the committer is paused after installing ONE of two blocks into the
// cache, an outside reader with an already-open descriptor reads both blocks
// in one read(). It must block and then see both blocks new, never a mix.
// Nothing may print while the committer is paused: console writes need
// begin_op(), which waits for the exclusive publication.
static void
test_live_reader(void)
{
  int o2p[2], p2o[2], r2p[2], p2r[2], held, ownerok;
  char cls = '?', c;

  printf(1, "-- live outside reader during publication (test build)\n");
  mkfile();
  shadowtest(ST_PUBHOLD, 0, 0);
  pipe(o2p); pipe(p2o); pipe(r2p); pipe(p2r);
  if(fork() == 0){                                  // reader (outsider)
    int fd = open(F, O_RDONLY), i, nnew = 0, nold = 0;
    send(r2p[1], fd >= 0);
    receive(p2r[0]);
    if(read(fd, buf, 2*BSIZE) == 2*BSIZE)
      for(i = 0; i < 2*BSIZE; i++){
        nnew += buf[i] == 'L';
        nold += buf[i] == pat(0, i);
      }
    c = nnew == 2*BSIZE ? 'N' : nold == 2*BSIZE ? 'O' : 'M';
    write(r2p[1], &c, 1);
    exit();
  }
  if(fork() == 0){                                  // owner/committer
    int ok = xbegin() == 0 && fill_block(0, 'L') == BSIZE && fill_block(1, 'L') == BSIZE;
    receive(p2o[0]);
    int c = (xcommit() == 0);
    send(o2p[1], ok && c);
    exit();
  }
  check(receive(r2p[0]), "reader opened the file before the commit");
  check(wait_pages(2), "owner wrote two private blocks");
  shadowtest(ST_PUBHOLD, 0, 2);
  send(p2o[1], 1);
  held = wait_held(2);
  send(p2r[1], 1);                                   // reader read()s now
  sleep(20);
  shadowtest(ST_PUBHOLD, 0, 0);                      // release the committer
  ownerok = receive(o2p[0]);
  read(r2p[0], &cls, 1);
  wait();
  wait();
  check(held, "committer paused after installing the first block");
  check(ownerok, "commit completed after the pause");
  check(cls == 'N', "reader saw all-new bytes, never a partial or undurable view");
  check(block_is(0, 'L') && block_is(1, 'L'), "both blocks published");
  close(o2p[0]); close(o2p[1]); close(p2o[0]); close(p2o[1]);
  close(r2p[0]); close(r2p[1]); close(p2r[0]); close(p2r[1]);
  unlink(F);
}

// D23: kill at hold point 1 (before the point of no return) -> no public
// change; kill at hold point 2 (after it) -> the commit completes.
static void
test_owner_kill_during_commit(int point)
{
  int o2p[2], p2o[2], pid, held, i, ok;

  printf(1, "-- owner killed during commit at hold point %d (D23)\n", point);
  mkfile();
  shadowtest(ST_PUBHOLD, 0, 0);
  pipe(o2p); pipe(p2o);
  if((pid = fork()) == 0){
    int good = xbegin() == 0 && fill_block(0, 'K') == BSIZE && fill_block(1, 'K') == BSIZE;
    if(!good) exit();
    receive(p2o[0]);
    xcommit();
    exit();
  }
  check(wait_pages(2), "owner wrote two private blocks");
  shadowtest(ST_PUBHOLD, 0, point);
  send(p2o[1], 1);
  held = wait_held(point);
  kill(pid);                     // also ends the pause
  wait();
  check(held, "committer reached the hold point");
  if(point == 1){
    check(expect_pattern(0, 2*BSIZE), "killed before the point of no return: no public change");
    for(ok = 0, i = 0; i < 500 && !ok; i++){
      ok = write_at(0, "free", 4) == 4;
      if(!ok)
        sleep(1);
    }
    check(ok, "cancelled speculation's copies released after owner exit");
  } else {
    check(block_is(0, 'K') && block_is(1, 'K'),
          "killed after the point of no return: commit completed and is visible");
    check(write_at(0, "free", 4) == 4, "copies released after the committed owner died");
  }
  close(o2p[0]); close(o2p[1]); close(p2o[0]); close(p2o[1]);
  unlink(F);
}

// ---- crash modes (driven by tests/run-spec-tests.py --suite crash)
static void
crash_setup(void)
{
  mkfile_n("cra", 1, 2);
  mkfile_n("crb", 2, 2);
  printf(1, "CRASHSETUP OK\n");
}

static void
crash_commit(int point)
{
  int i;
  if(xbegin() != 0){
    printf(1, "FAIL: crash xbegin\n");
    return;
  }
  for(i = 0; i < BSIZE; i++) tmp[i] = newpat(1, i);
  write_at_f("cra", 0, tmp, BSIZE);
  for(i = 0; i < BSIZE; i++) tmp[i] = newpat(1, BSIZE + i);
  write_at_f("cra", BSIZE, tmp, BSIZE);
  for(i = 0; i < BSIZE; i++) tmp[i] = newpat(2, BSIZE + i);
  write_at_f("crb", BSIZE, tmp, BSIZE);
  shadowtest(ST_CRASH, 0, point);
  printf(1, "CALLER: calling xcommit (3 blocks, 2 files, crash point %d)\n", point);
  i = xcommit();
  printf(1, "CALLER: xcommit returned %d\n", i);
}

// Classify a block: 'O' all old, 'N' all new, 'M' anything else.
static char
classify(char *name, int seed, int blk, int changes)
{
  int i, nold = 0, nnew = 0;
  if(read_at_f(name, blk*BSIZE, buf, BSIZE) != BSIZE)
    return 'M';
  for(i = 0; i < BSIZE; i++){
    nold += buf[i] == pat(seed, blk*BSIZE + i);
    nnew += changes && buf[i] == newpat(seed, blk*BSIZE + i);
  }
  return nold == BSIZE ? 'O' : nnew == BSIZE ? 'N' : 'M';
}

static void
crash_verify(char *expect)
{
  char a0 = classify("cra", 1, 0, 1), a1 = classify("cra", 1, 1, 1);
  char b0 = classify("crb", 2, 0, 0), b1 = classify("crb", 2, 1, 1);
  char want = expect[0] == 'n' ? 'N' : 'O';
  int fd;

  printf(1, "RECOVERED cra=[%c%c] crb=[%c%c] (expected all %s)\n", a0, a1, b0, b1, expect);
  check(a0 == want && a1 == want && b1 == want, "speculation recovered completely");
  check(b0 == 'O', "unchanged block untouched");
  check(file_size("cra") == 2*BSIZE && file_size("crb") == 2*BSIZE, "sizes intact");
  fd = open("crpost", O_CREATE|O_RDWR);
  check(fd >= 0 && write(fd, "ok", 2) == 2, "filesystem usable after recovery");
  close(fd);
  unlink("crpost");
  if(failures == 0)
    printf(1, "CRASHVERIFY OK %s\n", expect);
}
#endif

int
main(int argc, char **argv)
{
#ifdef SHADOW_TEST
  if(argc == 2 && strcmp(argv[1], "crashsetup") == 0){
    crash_setup();
    exit();
  }
  if(argc == 3 && strcmp(argv[1], "crash") == 0){
    crash_commit(atoi(argv[2]));
    exit();
  }
  if(argc == 3 && strcmp(argv[1], "crashverify") == 0){
    crash_verify(argv[2]);
    exit();
  }
#endif
  if(argc != 1){
    printf(1, "usage: spcommit (crash modes need a SHADOW_TEST=1 build)\n");
    exit();
  }
  test_abort();
  test_commit_publishes();
  test_child_changes();
  test_permissions();
  test_conflict();
  test_disjoint();
  test_empty_and_capacity();
  test_outsider_policy();
  test_reuse();
  test_concurrent_ordinary_ops();
#ifdef SHADOW_TEST
  test_live_reader();
  test_owner_kill_during_commit(1);
  test_owner_kill_during_commit(2);
  check(shadowtest(ST_PUBWRITES, 0, 0) == 0,
        "no log_write() by a speculative process outside its own publication");
#endif
  if(failures == 0)
    printf(1, "SPCOMMIT TESTS PASSED\n");
  else
    printf(1, "SPCOMMIT TESTS FAILED: %d\n", failures);
  exit();
}
