// Small, checked file edits and buffered printing, with owner/user approval.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define LIMIT 60

static void
fail(char *why)
{
  xabort();
  printf(2, "SPECEDIT: aborted: %s\n", why);
  exit();
}

static int
hex(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int
main(int argc, char **argv)
{
  char text[LIMIT], expected[LIMIT], actual[LIMIT], *encoded, *path = 0;
  struct stat st;
  int printing, appending, len, i, a, b, fd, n, total, size;

  printing = argc == 3 && strcmp(argv[1], "print") == 0;
  appending = argc == 4 && strcmp(argv[1], "append") == 0;
  if(!printing && !appending && !(argc == 4 && strcmp(argv[1], "write") == 0)){
    printf(2, "SPECEDIT: usage: specedit print HEX | write|append PATH HEX\n");
    exit();
  }
  encoded = argv[argc-1];
  len = strlen(encoded);
  if(len < 2 || len % 2 || len/2 + 1 > LIMIT){
    printf(2, "SPECEDIT: invalid text size\n");
    exit();
  }
  for(i = 0; i < len; i += 2){
    a = hex(encoded[i]); b = hex(encoded[i+1]);
    if(a < 0 || b < 0){
      printf(2, "SPECEDIT: invalid text encoding\n");
      exit();
    }
    text[i/2] = (a << 4) | b;
  }
  len /= 2;
  text[len++] = '\n';
  if(xbegin() < 0){
    printf(2, "SPECEDIT: could not begin speculation\n");
    exit();
  }
  if(printing){
    if(write(1, text, len) != len)
      fail("console buffer full");
  } else {
    path = argv[2];
    // Host validation also checks this; keep the standalone utility confined.
    if(strlen(path) < 7 || strlen(path) > 20)
      fail("invalid demo path");
    for(i = 0; i < 6; i++)
      if(path[i] != "/demo/"[i]) fail("invalid demo path");
    if(path[6] < 'a' || path[6] > 'z') fail("invalid demo filename");
    for(i = 7; path[i]; i++)
      if(!((path[i] >= 'a' && path[i] <= 'z') ||
           (path[i] >= '0' && path[i] <= '9') || path[i] == '_' || path[i] == '.'))
        fail("invalid demo filename");
    fd = open(path, O_RDWR);
    if(fd < 0)
      fail("file does not exist");
    if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size > LIMIT){
      close(fd);
      fail("target must be a regular file of at most 60 bytes");
    }
    if(!appending && st.nlink != 1){
      close(fd);
      fail("replacement requires a file with one hard link");
    }
    size = appending ? st.size + len : len;
    if(size > LIMIT){
      close(fd);
      fail("appended file would exceed 60 bytes");
    }
    for(total = 0; total < st.size; total += n){
      n = read(fd, expected + total, st.size - total);
      if(n <= 0){ close(fd); fail("original read failed"); }
    }
    if(!appending){
      close(fd);
      // xv6 has no truncate syscall. Replace the single-link file privately;
      // abort restores its original inode/name/data, commit publishes the new.
      if(unlink(path) < 0)
        fail("cannot replace file");
      fd = open(path, O_CREATE | O_WRONLY);
      if(fd < 0)
        fail("cannot create replacement within shadow limits");
    }
    memmove(expected + (appending ? st.size : 0), text, len);
    n = write(fd, text, len);
    close(fd);
    if(n != len)
      fail("write failed or exceeded shadow capacity");
    fd = open(path, O_RDONLY);
    if(fd < 0)
      fail("cannot verify edited file");
    if(fstat(fd, &st) < 0 || st.size != size){
      close(fd); fail("edited size verification failed");
    }
    for(total = 0; total < size; total += n){
      n = read(fd, actual + total, size - total);
      if(n <= 0){ close(fd); fail("edited read failed"); }
    }
    close(fd);
    for(i = 0; i < size; i++)
      if(actual[i] != expected[i]) fail("edited contents verification failed");
  }
  if(xask(printing ? "Release buffered print output" :
          appending ? "Append text to existing file" : "Replace contents of existing file") != 1)
    fail("user declined or review failed; effects discarded");
  if(xcommit() < 0)
    fail("commit refused");
  printf(1, "SPECEDIT: committed: %s\n", argv[1]);
  exit();
}
