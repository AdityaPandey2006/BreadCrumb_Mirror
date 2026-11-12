// Regression tests for exclusive approval sessions and ordinary readers.
#include "types.h"
#include "user.h"
#include "fcntl.h"

static void
result(char who, int yes)
{
  char b[12] = "A_RESULT_0\n";
  b[0]=who; b[9]=yes ? '1' : '0';
  write(1,b,11);              // one ordinary write, after completion
}

static void
ask(char who)
{
  int r;
  if(xbegin() != 0) exit();
  write(1,who == 'A' ? "HELD_PAIR_A\n" : "HELD_PAIR_B\n",12);
  r=xask(who == 'A' ? "PAIR_A" : "PAIR_B");
  if(r == 1){
    if(xcommit() != 0){xabort(); r=-1;}
  } else {
    if(xcommit() != -1) r=-1;
    if(xabort() != 0) r=-1;
  }
  result(who,r == 1);
  exit();
}

int
main(int argc, char **argv)
{
  int a,b,p[2],pid,r;
  char ready, bytes[2];
  if(argc > 1 && strcmp(argv[1],"direct") == 0){
    unlink("directf");
    if(xbegin() != 0) exit();
    pid=open("directf",O_CREATE|O_WRONLY);
    if(pid < 0) exit();
    write(pid,"NEW\n",4); close(pid);
    write(1,"DIRECT_HELD_SECRET\n",19);
    r=xcommit();
    if(r == 0) printf(1,"DIRECT_COMMIT_OK\n");
    else if(xabort() == 0) printf(1,"DIRECT_ABORT_OK\n");
    else printf(1,"FAIL: direct completion\n");
    exit();
  }
  if(argc > 1 && strcmp(argv[1],"overflow") == 0){
    char name[]="ov00";
    if(xbegin() != 0) exit();
    for(a=0; a<40; a++){
      name[2]='0'+a/10; name[3]='0'+a%10;
      pid=open(name,O_CREATE|O_WRONLY);
      if(pid < 0) break;
      close(pid);
    }
    r=xcommit();
    if(r == -1 && xabort() == 0) printf(1,"REPORT_OVERFLOW_REFUSED\n");
    else printf(1,"FAIL: oversized review was approved\n");
    exit();
  }
  if(argc > 1 && strcmp(argv[1],"reader") == 0){
    pipe(p);
    pid=fork();
    if(pid == 0){
      close(p[0]); write(p[1],"r",1); close(p[1]);
      r=read(0,bytes,2);
      write(1,r == 2 && bytes[0] == 'z' ? "READER_Z_OK\n" : "FAIL: reader stole approval\n",
            r == 2 && bytes[0] == 'z' ? 12 : 27);
      exit();
    }
    close(p[1]); read(p[0],&ready,1); close(p[0]);
    if(xbegin() != 0) exit();
    r=xask("READER_OWNER");
    if(r == 1 && xcommit() == 0) write(1,"READ_OWNER_OK\n",14);
    else { xabort(); write(1,"FAIL: owner answer\n",19); }
    wait();
    printf(1,"READER_DONE\n");
    exit();
  }
  a=fork();
  if(a == 0) ask('A');
  if(argc > 1 && strcmp(argv[1],"kill") == 0) sleep(15);
  b=fork();
  if(b == 0) ask('B');
  if(argc > 1 && strcmp(argv[1],"kill") == 0){sleep(30); kill(a);}
  wait(); wait();
  printf(1,"PAIR_DONE\n");
  exit();
}
