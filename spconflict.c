// Step 9: Concurrency, conflict handling, and multi-speculation validation.
// Exercises data conflicts (first-committer-wins, abort-only loser),
// metadata false conflicts between different files, outside writer/unlink refusals,
// concurrent shared participants, live-peer commit rejection, kill cancellation,
// context reuse, pending output discard on conflict, and multi-CPU race safety.

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "param.h"
#include "fs.h"

static int failures;

static void
check(int ok, char *msg)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", msg);
  if(!ok)
    failures++;
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

static int
exists(char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static int
make(char *path, char *text)
{
  int fd = open(path, O_CREATE|O_RDWR);
  int n = strlen(text);
  int ok;
  if(fd < 0)
    return 0;
  ok = (write(fd, text, n) == n);
  close(fd);
  return ok;
}

static int
content(char *path, char *text)
{
  char data[64];
  int fd = open(path, O_RDONLY);
  int n = strlen(text);
  int r;
  if(fd < 0 || n >= sizeof(data))
    return 0;
  memset(data, 0, sizeof(data));
  r = read(fd, data, sizeof(data) - 1);
  close(fd);
  if(r < n)
    return 0;
  for(int i = 0; i < n; i++){
    if(data[i] != text[i])
      return 0;
  }
  return 1;
}

static void
clean(void)
{
  unlink("cf_data");
  unlink("cf_meta_a");
  unlink("cf_meta_b");
  unlink("cf_race");
  unlink("cf_part");
  unlink("cf_live");
  unlink("cf_kill");
  unlink("cf_stress");
  unlink("cf_out_test");
}

// 1. Two speculations modifying the same data block (First-committer-wins).
static void
test_data_conflict(void)
{
  int a2p[2], b2p[2], p2a[2], p2b[2];
  int pid_a, pid_b;

  printf(1, "-- 1. Data conflict between independent speculations\n");
  clean();
  make("cf_data", "ORIGINAL_DATA_BLOCK_0");

  pipe(a2p); pipe(b2p); pipe(p2a); pipe(p2b);

  pid_a = fork();
  if(pid_a == 0){
    close(a2p[0]); close(p2a[1]);
    int ok = (xbegin() == 0);
    int fd = open("cf_data", O_RDWR);
    if(fd >= 0){
      if(write(fd, "AAAA_DATA", 9) != 9) ok = 0;
      close(fd);
    } else ok = 0;
    // Wait for parent signal to commit
    char sig;
    read(p2a[0], &sig, 1);
    int c = (xcommit() == 0); // commit makes process non-speculative
    send(a2p[1], ok && c);
    exit();
  }

  pid_b = fork();
  if(pid_b == 0){
    close(b2p[0]); close(p2b[1]);
    int ok = (xbegin() == 0);
    int fd = open("cf_data", O_RDWR);
    if(fd >= 0){
      if(write(fd, "BBBB_DATA", 9) != 9) ok = 0;
      close(fd);
    } else ok = 0;
    // Wait for parent signal to attempt commit
    char sig;
    read(p2b[0], &sig, 1);
    int c1 = xcommit();        // should fail with -1 (conflict)
    int c2 = xcommit();        // should fail with -1 (abort-only)
    int a = (xabort() == 0);   // abort succeeds and makes process non-speculative
    send(b2p[1], ok && (c1 == -1) && (c2 == -1) && a);
    exit();
  }

  close(a2p[1]); close(p2a[0]);
  close(b2p[1]); close(p2b[0]);

  // Give children time to stage their shadows
  sleep(5);

  write(p2a[1], "C", 1);       // A commits first
  check(receive(a2p[0]), "first committer (A) succeeds");

  write(p2b[1], "C", 1);       // B tries to commit
  check(receive(b2p[0]), "second committer (B) rejected with -1 and abort-only");

  wait(); wait();
  check(content("cf_data", "AAAA_DATA"), "public block contains A's changes, none of B");

  close(a2p[0]); close(p2a[1]);
  close(b2p[0]); close(p2b[1]);
  clean();
}

// 2. Metadata false conflicts between different files (Directory/dinode block sharing).
static void
test_metadata_false_conflict(void)
{
  int a2p[2], b2p[2], p2a[2], p2b[2];
  int pid_a, pid_b;

  printf(1, "-- 2. Metadata false conflict (directory / dinode sharing)\n");
  clean();
  pipe(a2p); pipe(b2p); pipe(p2a); pipe(p2b);

  pid_a = fork();
  if(pid_a == 0){
    close(a2p[0]); close(p2a[1]);
    int ok = (xbegin() == 0) && make("cf_meta_a", "FILE_A_CONTENT");
    char sig;
    read(p2a[0], &sig, 1);
    int c = (xcommit() == 0);
    send(a2p[1], ok && c);
    exit();
  }

  pid_b = fork();
  if(pid_b == 0){
    close(b2p[0]); close(p2b[1]);
    int ok = (xbegin() == 0) && make("cf_meta_b", "FILE_B_CONTENT");
    char sig;
    read(p2b[0], &sig, 1);
    int c1 = xcommit();
    int c2 = xcommit();
    int a = (xabort() == 0);
    send(b2p[1], ok && (c1 == -1) && (c2 == -1) && a);
    exit();
  }

  close(a2p[1]); close(p2a[0]);
  close(b2p[1]); close(p2b[0]);

  sleep(5);

  write(p2a[1], "C", 1);       // A commits first
  check(receive(a2p[0]), "spec A commits directory and inode block modifications");

  write(p2b[1], "C", 1);       // B commits second (shares directory block)
  check(receive(b2p[0]), "spec B suffers metadata conflict, becomes abort-only");

  wait(); wait();
  check(exists("cf_meta_a") && content("cf_meta_a", "FILE_A_CONTENT"), "file A published successfully");
  check(!exists("cf_meta_b"), "conflicted file B was discarded cleanly");

  close(a2p[0]); close(p2a[1]);
  close(b2p[0]); close(p2b[1]);
  clean();
}

// 3. Outside writers or allocators racing with private changes (Refusal).
static void
test_outside_writer_refusal(void)
{
  int p2c[2], c2p[2], pid;

  printf(1, "-- 3. Outside writer and unlink refusal during active speculation\n");
  clean();
  make("cf_race", "ORIGINAL_RACE_CONTENT");
  pipe(p2c); pipe(c2p);

  pid = fork();
  if(pid == 0){
    close(p2c[1]); close(c2p[0]);
    int ok = (xbegin() == 0);
    int fd = open("cf_race", O_RDWR);
    if(fd >= 0){
      if(write(fd, "SPEC_MUTATION", 13) != 13) ok = 0;
      close(fd);
    } else ok = 0;
    // Wait for outside parent to perform refusal tests
    char sig;
    read(p2c[0], &sig, 1);
    int c = (xcommit() == 0);
    send(c2p[1], ok && c);
    exit();
  }

  close(p2c[0]); close(c2p[1]);
  sleep(5);

  // Outside process attempts to write to shadowed block 0
  int out_fd = open("cf_race", O_RDWR);
  check(out_fd >= 0, "outside process opened cf_race");
  int wr = write(out_fd, "OUTSIDE_OVERWRITE", 17);
  check(wr <= 0, "outside write to shadowed block refused (returned <= 0)");
  close(out_fd);

  // Outside process attempts to unlink shadowed file
  check(unlink("cf_race") == -1, "outside unlink of shadowed file refused (-1)");

  write(p2c[1], "C", 1);      // signal spec child to commit
  check(receive(c2p[0]), "spec child committed successfully");
  wait();

  check(content("cf_race", "SPEC_MUTATION"), "cf_race has committed spec data");

  // Now that spec is committed, outside write and unlink must succeed
  out_fd = open("cf_race", O_RDWR);
  check(out_fd >= 0 && write(out_fd, "AFTER_COMMIT", 12) == 12, "outside write succeeds after commit");
  close(out_fd);
  check(unlink("cf_race") == 0, "outside unlink succeeds after commit");

  close(p2c[1]); close(c2p[0]);
  clean();
}

// 4. Shared participants reading/writing and exiting.
static void
test_shared_participants(void)
{
  int pid1, pid2;

  printf(1, "-- 4. Concurrent participants in shared speculation\n");
  clean();
  check(xbegin() == 0, "owner begins shared speculation");
  check(make("cf_part", "INITIAL_DATA"), "create file inside speculation");

  pid1 = fork();
  if(pid1 == 0){
    int fd = open("cf_part", O_RDWR);
    if(fd >= 0){
      write(fd, "PART1_BYTES", 11);
      close(fd);
    }
    exit();
  }

  pid2 = fork();
  if(pid2 == 0){
    int fd = open("cf_part", O_RDWR);
    if(fd >= 0){
      char buf[16];
      read(fd, buf, 11);
      close(fd);
    }
    exit();
  }

  wait(); wait();

  // Owner reads back within speculation
  check(content("cf_part", "PART1_BYTES"), "owner sees child 1 modification");
  check(xcommit() == 0, "owner commits shared participants' work");
  check(content("cf_part", "PART1_BYTES"), "committed file retains shared changes");
  clean();
}

// 5. Commit/abort attempts with live peers.
static void
test_live_peer_rejection(void)
{
  int pid;

  printf(1, "-- 5. Commit and abort rejection while peers are live\n");
  clean();
  check(xbegin() == 0, "owner begins");

  pid = fork();
  if(pid == 0){
    sleep(10);
    exit();
  }

  check(xcommit() == -1, "xcommit() returns -1 while peer child is alive");
  check(xabort() == -1, "xabort() returns -1 while peer child is alive");

  wait();

  check(xcommit() == 0, "xcommit() succeeds after child exits and is reaped");
  clean();
}

// 6. Kill and cancellation around commit boundary.
static void
test_owner_kill_cancellation(void)
{
  int pid;

  printf(1, "-- 6. Owner kill and cancellation cleanup\n");
  clean();

  pid = fork();
  if(pid == 0){
    check(xbegin() == 0, "spec child begins");
    make("cf_kill", "DOOMED_SPEC_DATA");
    for(;;){
      sleep(10);
    }
  }

  sleep(5);
  kill(pid);
  wait();

  check(!exists("cf_kill"), "doomed file was discarded on owner kill");
  check(xbegin() == 0 && xabort() == 0, "new speculation can start after killed owner cleanup");
  clean();
}

// 7. Repeated context allocation/reuse and cleanup.
static void
test_context_reuse(void)
{
  int i;
  printf(1, "-- 7. Repeated context allocation, reuse and reclamation\n");
  clean();

  for(i = 0; i < 15; i++){
    if(xbegin() != 0){
      check(0, "xbegin failed in reuse commit loop");
      return;
    }
    make("cf_stress", "REUSE_DATA");
    if(xcommit() != 0){
      check(0, "xcommit failed in reuse loop");
      return;
    }
    unlink("cf_stress");
  }

  for(i = 0; i < 15; i++){
    if(xbegin() != 0){
      check(0, "xbegin failed in reuse abort loop");
      return;
    }
    make("cf_stress", "ABORT_DATA");
    if(xabort() != 0){
      check(0, "xabort failed in reuse abort loop");
      return;
    }
    if(exists("cf_stress")){
      check(0, "aborted file still exists in reuse loop");
      return;
    }
  }

  check(1, "passed 30 consecutive commit/abort cycles without leaking contexts");
  clean();
}

// 8. Conflicts with pending console/pipe effects (Zero leakage).
static void
test_conflict_pending_output(void)
{
  int ext_p[2], a2p[2], b2p[2], p2a[2], p2b[2];
  int pid_a, pid_b;

  printf(1, "-- 8. Conflict with pending console and external pipe output\n");
  clean();
  make("cf_out_test", "BASE_CONTENT_0000");

  pipe(ext_p);
  pipe(a2p); pipe(b2p); pipe(p2a); pipe(p2b);

  pid_a = fork();
  if(pid_a == 0){
    close(a2p[0]); close(p2a[1]);
    int ok = (xbegin() == 0);
    // Write secret to external pipe
    write(ext_p[1], "LEAK_CONFLICT_PIPE_444\n", 23);
    // Write secret to console
    write(1, "LEAK_CONFLICT_CONSOLE_555\n", 26);
    // Write to shared file
    int fd = open("cf_out_test", O_RDWR);
    if(fd >= 0){
      if(write(fd, "SPEC_A_DATA", 11) != 11) ok = 0;
      close(fd);
    } else ok = 0;

    char sig;
    read(p2a[0], &sig, 1);
    int c = xcommit();         // will fail (-1)
    int a = xabort();          // discard held buffers
    close(ext_p[1]);
    send(a2p[1], ok && (c == -1) && (a == 0));
    exit();
  }

  pid_b = fork();
  if(pid_b == 0){
    close(b2p[0]); close(p2b[1]);
    int ok = (xbegin() == 0);
    int fd = open("cf_out_test", O_RDWR);
    if(fd >= 0){
      if(write(fd, "SPEC_B_WIN", 10) != 10) ok = 0;
      close(fd);
    } else ok = 0;

    char sig;
    read(p2b[0], &sig, 1);
    int c = (xcommit() == 0);  // B commits first!
    send(b2p[1], ok && c);
    exit();
  }

  close(a2p[1]); close(p2a[0]);
  close(b2p[1]); close(p2b[0]);
  close(ext_p[1]);             // parent closes write end

  sleep(5);

  write(p2b[1], "C", 1);       // B commits first
  check(receive(b2p[0]), "spec B commits first");

  write(p2a[1], "C", 1);       // A attempts commit, suffers conflict, aborts
  check(receive(a2p[0]), "spec A suffers conflict, aborts and discards held output");

  wait(); wait();

  // Verify external pipe received 0 bytes
  char pipe_buf[32];
  int r = read(ext_p[0], pipe_buf, sizeof(pipe_buf));
  check(r == 0, "external pipe reader received 0 bytes (EOF, zero leakage on conflict)");
  close(ext_p[0]);

  check(content("cf_out_test", "SPEC_B_WIN"), "file contains winning spec B's data");

  close(a2p[0]); close(p2a[1]);
  close(b2p[0]); close(p2b[1]);
  clean();
}

// 9. Deadlock and race safety under multi-CPU concurrency.
static void
test_multicpu_concurrency(void)
{
  int pids[4];
  int i;

  printf(1, "-- 9. Multi-process concurrency stress\n");
  clean();

  for(i = 0; i < 4; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      char fname[16];
      fname[0] = 'c'; fname[1] = 'f'; fname[2] = '_'; fname[3] = 'p';
      fname[4] = '0' + i; fname[5] = 0;
      if(xbegin() == 0){
        make(fname, "CONCURRENT_DATA");
        sleep(2);
        xcommit();
      }
      exit();
    }
  }

  for(i = 0; i < 4; i++)
    wait();

  int count = 0;
  for(i = 0; i < 4; i++){
    char fname[16];
    fname[0] = 'c'; fname[1] = 'f'; fname[2] = '_'; fname[3] = 'p';
    fname[4] = '0' + i; fname[5] = 0;
    if(exists(fname))
      count++;
    unlink(fname);
  }

  check(count >= 1, "at least one concurrent speculation committed successfully without deadlock");
  clean();
}

int
main(int argc, char *argv[])
{
  printf(1, "SPCONFLICT TESTS STARTING\n");

  test_data_conflict();
  test_metadata_false_conflict();
  test_outside_writer_refusal();
  test_shared_participants();
  test_live_peer_rejection();
  test_owner_kill_cancellation();
  test_context_reuse();
  test_conflict_pending_output();
  test_multicpu_concurrency();

  if(failures == 0){
    printf(1, "SPCONFLICT: ALL TESTS PASSED\n");
    exit();
  } else {
    printf(1, "SPCONFLICT: %d TESTS FAILED\n", failures);
    exit();
  }
}
