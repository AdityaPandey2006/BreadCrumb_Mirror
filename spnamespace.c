// Step 6: ordinary syscalls exercise private metadata and namespace changes.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"
#include "param.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

static char buf[BSIZE];
static int failures;
static int compare(char *a, char *b, int n) {
  while(n--) if(*a++ != *b++) return 1;
  return 0;
}
static void check(int ok, char *s) {
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", s);
  if(!ok) failures++;
}
static void send(int fd, int ok) { char c = ok ? 'P' : 'F'; write(fd, &c, 1); }
static int receive(int fd) { char c = 0; return read(fd, &c, 1) == 1 && c == 'P'; }
static int exists(char *path) { struct stat st; return stat(path, &st) == 0; }
static int size(char *path) { struct stat st; return stat(path, &st) == 0 ? st.size : -1; }
static int make(char *path, char *text) {
  int fd = open(path, O_CREATE|O_RDWR), n = strlen(text), ok;
  if(fd < 0) return 0;
  ok = write(fd, text, n) == n;
  close(fd); return ok;
}
static int content(char *path, char *text) {
  char data[32];
  int fd = open(path, O_RDONLY), n = strlen(text), r;
  if(fd < 0 || n > sizeof(data)) return 0;
  r = read(fd, data, sizeof(data)); close(fd);
  return r == n && compare(data, text, n) == 0;
}
static void skip(int fd, int n) {
  int m;
  while(n > 0){ m = n > BSIZE ? BSIZE : n; if(read(fd, buf, m) != m) break; n -= m; }
}
static void clean(void) {
  unlink("nnew"); unlink("nbase"); unlink("nlink"); unlink("ndir/item");
  unlink("ndir"); unlink("nfull"); unlink("nfail"); unlink("nlarge");
}

static void create_delete(int commit)
{
  int p2c[2], c2p[2], pid, ok;
  clean(); make("nbase", "original");
  pipe(p2c); pipe(c2p);
  pid = fork();
  if(pid == 0){
    close(p2c[1]); close(c2p[0]);
    receive(p2c[0]);
    send(c2p[1], content("nbase", "original") && !exists("nnew"));
    receive(p2c[0]);
    send(c2p[1], commit ? !exists("nbase") && content("nnew", "private") :
                          content("nbase", "original") && !exists("nnew"));
    exit();
  }
  close(p2c[0]); close(c2p[1]);
  check(xbegin() == 0, "begin create/delete attempt");
  check(make("nnew", "private"), "create and write new private file");
  check(unlink("nbase") == 0 && !exists("nbase") && content("nnew", "private"),
        "inside sees deletion and new contents");
  send(p2c[1], 1);
  check(receive(c2p[0]), "outside sees old name and no new name before completion");
  ok = commit ? xcommit() == 0 : xabort() == 0;
  check(ok, commit ? "commit publishes namespace/data together" : "abort discards namespace/data");
  send(p2c[1], 1);
  check(receive(c2p[0]), "outside sees the correct final namespace");
  wait(); close(p2c[1]); close(c2p[0]); clean();
}

static void extension(int commit)
{
  int fd, i, old = NDIRECT*BSIZE, p[2], q[2];
  clean();
  memset(buf, 'A', BSIZE);
  fd = open("nlarge", O_CREATE|O_RDWR);
  for(i = 0; i < NDIRECT; i++) write(fd, buf, BSIZE);
  close(fd);
  pipe(p); pipe(q);
  if(fork() == 0){
    receive(p[0]); send(q[1], size("nlarge") == old); exit();
  }
  check(xbegin() == 0, "begin direct-to-indirect extension");
  fd = open("nlarge", O_RDWR); skip(fd, old);
  memset(buf, 'B', BSIZE);
  check(write(fd, buf, BSIZE) == BSIZE && write(fd, "C", 1) == 1,
        "allocate private indirect block and two new data blocks");
  close(fd);
  check(size("nlarge") == old+BSIZE+1, "private stat reflects extended size");
  send(p[1], 1); check(receive(q[0]), "outside size stays original"); wait();
  check((commit ? xcommit() : xabort()) == 0, "finish extension");
  check(size("nlarge") == (commit ? old+BSIZE+1 : old), "final size matches commit/abort");
  if(commit){
    fd = open("nlarge", O_RDONLY); skip(fd, old);
    check(read(fd, buf, BSIZE) == BSIZE && buf[0] == 'B' && buf[BSIZE-1] == 'B',
          "published indirect data is readable");
    check(read(fd, buf, 1) == 1 && buf[0] == 'C', "last extended byte published");
    close(fd);
  }
  close(p[0]); close(p[1]); close(q[0]); close(q[1]); clean();
}

static void directories(void)
{
  clean();
  check(xbegin() == 0 && mkdir("ndir") == 0 && chdir("ndir") == 0,
        "create and enter a private directory");
  check(make("item", "data") && content("item", "data"), "relative lookup uses private cwd");
  check(xabort() == 0 && !exists("ndir") && exists("README"),
        "abort removes new directory and restores usable root cwd");
  check(xbegin() == 0 && mkdir("ndir") == 0 && make("ndir/item", "data") && xcommit() == 0,
        "commit directory and its child");
  check(content("ndir/item", "data"), "committed child reachable through directory");
  check(xbegin() == 0 && unlink("ndir/item") == 0 && unlink("ndir") == 0 && xabort() == 0,
        "abort directory deletion");
  check(content("ndir/item", "data"), "aborted directory deletion preserves child");
  check(xbegin() == 0 && unlink("ndir/item") == 0 && unlink("ndir") == 0 && xcommit() == 0,
        "commit directory deletion");
  check(!exists("ndir"), "deleted directory absent publicly"); clean();
}

static void handles_links(void)
{
  int fd, alias, outside[2], gate[2];
  char data[16];
  struct stat st;
  clean(); make("nbase", "original");
  check(xbegin() == 0 && link("nbase", "nlink") == 0 && content("nlink", "original"),
        "private hard link resolves to original data");
  check(xabort() == 0 && !exists("nlink"), "abort removes private hard link");
  check(xbegin() == 0 && link("nbase", "nlink") == 0 && xcommit() == 0,
        "commit hard link");
  fd = open("nbase", O_RDONLY); fstat(fd, &st); close(fd);
  check(st.nlink == 2 && content("nlink", "original"), "public nlink and alias agree");
  unlink("nlink");
  check(xbegin() == 0, "begin new-descriptor abort");
  fd = open("nnew", O_CREATE|O_RDWR); alias = dup(fd);
  write(fd, "new", 3);
  check(xabort() == 0 && read(fd, data, 1) == -1 && read(alias, data, 1) == -1,
        "abort invalidates all aliases of a newly created file");
  check(xbegin() == 0, "begin new-descriptor commit");
  fd = open("nnew", O_CREATE|O_RDWR); write(fd, "new", 3);
  check(xcommit() == 0 && write(fd, "!", 1) == 1, "committed new descriptor remains usable");
  close(fd);
  pipe(outside); pipe(gate);
  if(fork() == 0){
    fd = open("nbase", O_RDONLY);
    send(outside[1], fd >= 0); receive(gate[0]);
    send(outside[1], read(fd, data, sizeof(data)) == 8 && compare(data, "original", 8) == 0);
    close(fd); exit();
  }
  receive(outside[0]);
  check(xbegin() == 0 && unlink("nbase") == 0 && xcommit() == 0 && !exists("nbase"),
        "commit unlink while an outside descriptor remains open");
  send(gate[1], 1); check(receive(outside[0]), "outside open descriptor retains unlinked contents");
  wait(); close(outside[0]); close(outside[1]); close(gate[0]); close(gate[1]); clean();
}

static void capacity(void)
{
  int fd, i, r;
  clean(); memset(buf, 'X', BSIZE);
  fd = open("nfull", O_CREATE|O_RDWR);
  for(i = 0; i < NSHADOW+1; i++) write(fd, buf, BSIZE);
  close(fd);
  check(xbegin() == 0, "begin capacity rollback");
  fd = open("nfull", O_RDWR);
  memset(buf, 'Y', BSIZE);
  for(i = 0; i < NSHADOW; i++) write(fd, buf, BSIZE);
  close(fd);
  check(open("nfail", O_CREATE|O_RDWR) == -1 && !exists("nfail"),
        "full table creation fails without leaving a private inode/name");
  check(unlink("nfull") == -1 && exists("nfull"), "full table unlink rolls back completely");
  check(xabort() == 0 && !exists("nfail") && size("nfull") == (NSHADOW+1)*BSIZE,
        "capacity failures leave public metadata unchanged");
  check(xbegin() == 0, "begin partial extension");
  fd = open("nfail", O_CREATE|O_RDWR);
  for(i = 0; i < NSHADOW+2; i++){
    r = write(fd, buf, BSIZE);
    if(r < 0) break;
  }
  check(i > 0 && i < NSHADOW && size("nfail") == i*BSIZE,
        "metadata counts toward cap; failed extension preserves completed chunks");
  close(fd);
  check(xcommit() == 0 && size("nfail") == i*BSIZE, "partial extension remains consistently committable");
#ifdef SHADOW_TEST
  check(xbegin() == 0, "begin injected allocation failure");
  shadowtest(ST_FAIL, 0, 0);
  check(open("fault", O_CREATE|O_RDWR) == -1 && !exists("fault"),
        "allocation failure rolls back namespace operation");
  check(xabort() == 0 && shadowtest(ST_PAGES, 0, 0) == 0, "failure cleanup leaks no shadow pages");
#endif
  clean();
}

static void owner_exit(void)
{
  int p[2], q[2], pid;
  pipe(p); pipe(q); clean();
  pid = fork();
  if(pid == 0){
    xbegin(); send(q[1], make("nnew", "doomed")); receive(p[0]); exit();
  }
  check(receive(q[0]) && !exists("nnew"), "owner's new file stays private");
  kill(pid); wait();
  check(!exists("nnew") && make("nnew", "public"), "owner death releases allocation and namespace shadows");
  close(p[0]); close(p[1]); close(q[0]); close(q[1]); clean();
}

static void close_failure(void)
{
  int fd, held, i;
  clean(); make("nbase", "original");
  fd = open("nfull", O_CREATE|O_RDWR); memset(buf, 'A', BSIZE);
  for(i = 0; i < NSHADOW; i++) write(fd, buf, BSIZE);
  close(fd);
  check(xbegin() == 0, "begin unlinked-close capacity test");
  held = open("nbase", O_RDWR);
  check(unlink("nbase") == 0, "unlink keeps an open private descriptor alive");
  fd = open("nfull", O_RDWR); memset(buf, 'B', BSIZE);
  for(i = 0; i < NSHADOW; i++) if(write(fd, buf, BSIZE) != BSIZE) break;
  close(fd);
  close(held);  // its bitmap cleanup cannot fit; descriptor cannot be restored
  check(xcommit() == -1, "failed final-close cleanup makes the attempt abort-only");
  check(xabort() == 0 && content("nbase", "original"), "abort restores consistent allocation after close failure");
  clean();
}

// Same-image crash verification includes namespace, inode size, bitmap and data.
static void crash_setup(void) { clean(); make("nbase", "original"); printf(1,"NSSETUP OK\n"); }
static void crash_run(int point) {
#ifdef SHADOW_TEST
  xbegin(); make("nnew", "private"); unlink("nbase");
  shadowtest(ST_CRASH,0,point); xcommit(); printf(1,"unexpected commit reply\n");
#endif
}
static void crash_verify(int newer) {
  check(newer ? !exists("nbase") && content("nnew","private") :
                content("nbase","original") && !exists("nnew"), "recovered whole namespace and contents");
  check(make("reuse","ok"), "allocator remains usable after recovery"); unlink("reuse");
  if(!failures) printf(1,"NSVERIFY OK\n");
}
int main(int argc, char **argv) {
  if(argc == 2 && strcmp(argv[1],"crashsetup") == 0){ crash_setup(); exit(); }
  if(argc == 3 && strcmp(argv[1],"crash") == 0){ crash_run(atoi(argv[2])); exit(); }
  if(argc == 3 && strcmp(argv[1],"crashverify") == 0){ crash_verify(strcmp(argv[2],"new") == 0); exit(); }
  create_delete(0); create_delete(1);
  extension(0); extension(1);
  directories(); handles_links(); capacity(); owner_exit(); close_failure();
  printf(1,failures ? "NAMESPACE TESTS FAILED: %d\n" : "NAMESPACE TESTS PASSED\n",failures);
  exit();
}
