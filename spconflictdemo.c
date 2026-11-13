// Two independent owners stage edits before either gets permission to commit.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static int ready[2][2], go[2][2], done[2][2];
static char *path = "/demo/conflict.txt";
static char *versions[] = {"WINNER_A\n", "WINNER_B\n"};

static int
matches(char *expected)
{
  char data[16];
  int fd = open(path, O_RDONLY), n, i;
  if(fd < 0) return 0;
  n = read(fd, data, sizeof(data));
  close(fd);
  if(n != strlen(expected)) return 0;
  for(i = 0; i < n; i++)
    if(data[i] != expected[i]) return 0;
  return 1;
}

static void
participant(int id)
{
  int j, fd, ok, answer, rc, aborted;
  char signal, result = 'F';
  for(j = 0; j < 2; j++){
    close(ready[j][1]); close(go[j][1]); close(done[j][0]);
    if(j != id){ close(ready[j][0]); close(go[j][0]); close(done[j][1]); }
  }
  ok = xbegin() == 0;
  fd = ok ? open(path, O_RDWR) : -1;
  if(fd < 0) ok = 0;
  else {
    if(write(fd, versions[id], 9) != 9) ok = 0;
    close(fd);
  }
  // The parent filled this pipe before fork. Consuming a byte lets its next
  // write complete, providing readiness without speculative outgoing signals.
  if(read(ready[id][0], &signal, 1) != 1) ok = 0;
  // Keep the reader open until the parent finishes its readiness write.
  // Closing it immediately could make that blocked write fail with -1.
  if(read(go[id][0], &signal, 1) != 1) ok = 0;
  close(ready[id][0]);
  close(go[id][0]);
  if(!ok){
    xabort();
  } else if(signal == 'C'){
    // Approval is necessary but does not override conflict detection.
    answer = xask("A committed first. Approve B's competing edit to test conflict rejection");
    if(answer == 1){
      rc = xcommit();
      answer = xcommit();
      aborted = xabort();
      if(rc < 0 && answer < 0 && aborted == 0){
        printf(1, "CONFLICT: B rejected: A already committed; B aborted\n");
        result = 'R';
      }
    } else {
      aborted = xabort();
      if(answer == 0 && aborted == 0){
        printf(1, "CONFLICT: B declined; its commit was not attempted\n");
        result = 'D';
      }
    }
  } else {
    answer = xask(id == 0 ? "Commit A's private edit? B has staged a competing edit" :
                          "A was declined. Commit B's private edit instead?");
    if(answer == 1 && xcommit() == 0){
      printf(1, "CONFLICT: %s committed\n", id == 0 ? "A" : "B");
      result = 'W';
    } else {
      aborted = xabort();
      if(answer == 0 && aborted == 0){
        printf(1, "CONFLICT: %s declined and aborted\n", id == 0 ? "A" : "B");
        result = 'D';
      }
    }
  }
  // The participant is no longer speculative, so this status is immediate.
  write(done[id][1], &result, 1);
  close(done[id][1]);
  exit();
}

int
main(void)
{
  char fill[512], results[2] = {'F', 'F'}, signal;
  int i, fd, pids[2], ok = 1;
  struct stat st;
  if(stat(path, &st) == 0){
    printf(2, "CONFLICT: refused: /demo/conflict.txt exists; delete it before repeating\n");
    exit();
  }
  fd = open(path, O_CREATE | O_WRONLY);
  if(fd < 0 || write(fd, "ORIGINAL\n", 9) != 9){
    if(fd >= 0) close(fd);
    printf(2, "CONFLICT: FAIL: fixture creation\n");
    exit();
  }
  close(fd);
  memset(fill, 'P', sizeof(fill));
  for(i = 0; i < 2; i++){
    if(pipe(ready[i]) < 0 || pipe(go[i]) < 0 || pipe(done[i]) < 0 ||
       write(ready[i][1], fill, sizeof(fill)) != sizeof(fill)){
      printf(2, "CONFLICT: FAIL: coordination pipes\n");
      exit();
    }
  }
  for(i = 0; i < 2; i++){
    pids[i] = fork();
    if(pids[i] == 0) participant(i);
    if(pids[i] < 0){
      if(i) { kill(pids[0]); wait(); }
      printf(2, "CONFLICT: FAIL: fork\n");
      exit();
    }
  }
  for(i = 0; i < 2; i++){
    close(ready[i][0]); close(go[i][0]); close(done[i][1]);
    if(write(ready[i][1], "R", 1) != 1) ok = 0;
    close(ready[i][1]);
  }
  if(!matches("ORIGINAL\n")) ok = 0;
  if(!ok){
    kill(pids[0]); kill(pids[1]);
    wait(); wait();
    printf(2, "CONFLICT: FAIL: staging or original visibility\n");
    exit();
  }
  printf(1, "CONFLICT: both edits staged; public file still ORIGINAL\n");
  for(i = 0; i < 2; i++){
    signal = i == 1 && results[0] == 'W' ? 'C' : 'G';
    if(write(go[i][1], &signal, 1) != 1) ok = 0;
    close(go[i][1]);
    if(read(done[i][0], &results[i], 1) != 1) ok = 0;
    close(done[i][0]);
  }
  wait(); wait();
  if(results[0] == 'W'){
    ok = ok && (results[1] == 'R' || results[1] == 'D') && matches(versions[0]);
    printf(1, "CONFLICT: final contents: WINNER_A\n");
  } else if(results[0] == 'D' && results[1] == 'W'){
    ok = ok && matches(versions[1]);
    printf(1, "CONFLICT: final contents: WINNER_B\n");
  } else if(results[0] == 'D' && results[1] == 'D'){
    ok = ok && matches("ORIGINAL\n");
    printf(1, "CONFLICT: both declined; final contents: ORIGINAL\n");
  } else ok = 0;
  printf(1, "CONFLICT: %s\n", ok ? "VERIFIED" : "FAIL");
  exit();
}
