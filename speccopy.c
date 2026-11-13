// Bounded, exact copy in one owner-controlled, human-reviewed speculation.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define COPYMAX 60

static void
fail(char *message)
{
  xabort();
  printf(2, "SPECCOPY: aborted: %s\n", message);
  exit();
}

int
main(int argc, char **argv)
{
  char buf[COPYMAX], check[COPYMAX], extra;
  struct stat st;
  int fd, n, total, size, answer, i;

  if(argc != 3){
    printf(2, "usage: speccopy SOURCE DESTINATION\n");
    exit();
  }
  if(xbegin() < 0){
    printf(2, "SPECCOPY: could not begin speculation\n");
    exit();
  }
  fd = open(argv[1], O_RDONLY);
  if(fd < 0)
    fail("cannot open source");
  if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size > COPYMAX){
    close(fd);
    fail("source must be a regular text file of at most 60 bytes");
  }
  size = st.size;
  for(total = 0; total < size; total += n){
    n = read(fd, buf + total, size - total);
    if(n <= 0){
      close(fd);
      fail("source read failed");
    }
  }
  n = read(fd, &extra, 1);
  close(fd);
  if(n != 0)
    fail("source changed size during read");
  // Keep demonstrations readable; contents are data, never shell commands.
  for(i = 0; i < size; i++)
    if((buf[i] < 32 || buf[i] > 126) && buf[i] != '\n' && buf[i] != '\t')
      fail("source is not plain ASCII text");
  fd = open(argv[2], O_RDONLY);
  if(fd >= 0){
    close(fd);
    fail("destination already exists");
  }
  fd = open(argv[2], O_CREATE | O_WRONLY);
  if(fd < 0)
    fail("cannot create destination within speculation limits");
  n = write(fd, buf, size);
  close(fd);
  if(n != size)
    fail("write failed or exceeded shadow capacity");
  fd = open(argv[2], O_RDONLY);
  if(fd < 0)
    fail("cannot verify destination");
  if(fstat(fd, &st) < 0 || st.size != size){
    close(fd);
    fail("destination size verification failed");
  }
  for(total = 0; total < size; total += n){
    n = read(fd, check + total, size - total);
    if(n <= 0){
      close(fd);
      fail("destination read failed");
    }
  }
  close(fd);
  for(i = 0; i < size; i++)
    if(buf[i] != check[i])
      fail("destination contents verification failed");
  answer = xask("Copy source contents into a new file");
  if(answer != 1){
    xabort();
    printf(1, "SPECCOPY: aborted: destination was not created\n");
    exit();
  }
  if(xcommit() < 0)
    fail("commit refused");
  printf(1, "SPECCOPY: committed: copied %d bytes from %s to %s\n",
         size, argv[1], argv[2]);
  exit();
}
