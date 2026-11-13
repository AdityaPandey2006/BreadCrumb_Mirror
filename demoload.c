// Bootstrap small demo files from hex data without interpreting shell syntax.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

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
  char data[24], old[60];
  struct stat st;
  int fd, len, i, a, b, n, total;

  if(argc != 3 || strlen(argv[1]) < 7 || strlen(argv[1]) > 20)
    goto bad;
  for(i = 0; i < 6; i++)
    if(argv[1][i] != "/demo/"[i]) goto bad;
  if(argv[1][6] < 'a' || argv[1][6] > 'z') goto bad;
  for(i = 7; argv[1][i]; i++)
    if(!((argv[1][i] >= 'a' && argv[1][i] <= 'z') ||
         (argv[1][i] >= '0' && argv[1][i] <= '9') ||
         argv[1][i] == '_' || argv[1][i] == '.')) goto bad;
  len = strcmp(argv[2], "-") == 0 ? 0 : strlen(argv[2]);
  if(len > 48 || len % 2)
    goto bad;
  for(i = 0; i < len; i += 2){
    a = hex(argv[2][i]);
    b = hex(argv[2][i+1]);
    if(a < 0 || b < 0) goto bad;
    data[i/2] = (a << 4) | b;
  }
  fd = open(argv[1], O_CREATE | O_RDWR);
  if(fd < 0) goto bad;
  if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size + len/2 > 60){
    close(fd);
    goto bad;
  }
  // xv6 has no seek: read through the existing bytes to append the next chunk.
  for(total = 0; total < st.size; total += n){
    n = read(fd, old, st.size - total);
    if(n <= 0){ close(fd); goto bad; }
  }
  n = write(fd, data, len/2);
  close(fd);
  if(n != len/2) goto bad;
  printf(1, "DEMOLOAD: ok\n");
  exit();
bad:
  printf(2, "DEMOLOAD: failed\n");
  exit();
}
