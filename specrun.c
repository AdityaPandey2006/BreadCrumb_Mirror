// specrun: run a short command list inside one speculation, then decide.
//
//   specrun commit    STEP [STEP ...]
//   specrun abort     STEP [STEP ...]
//   specrun keep PATH STEP [STEP ...]   commit only if PATH still exists in
//                                       the private view, otherwise abort
//   specrun ask       STEP [STEP ...]   show a summary of the changes and ask
//                                       the user at the console (xask, D63)
// Each STEP is ONE shell argument whose words are separated by commas
// (xv6 sh allows only 10 arguments and interprets ';', '>' and '|'):
//   CMD,ARG,...[,into,FILE]         run CMD; "into,FILE" redirects its stdout
//   CMD,ARG,...,pipe,CMD,ARG,...    two commands joined by a pipe whose both
//                                   ends belong to the speculation (internal)
//   pause,N                         owner sleeps N ticks (lets an outside
//                                   observer look at the live speculation)
// Example: specrun abort rm,a echo,hi,into,b echo,LEAK
//
// Every command runs in a forked member of the speculation (fork + exec keep
// membership); steps run one after another. Redirection files and pipes are
// created by members after xbegin, as decision D21 requires. Console output
// of the commands is held until commit and discarded on abort.
//
// The "keep" guard is a deliberately simple rule-based policy: before
// deciding, the owner reads the speculation's own private state (reading
// your own writes is not an externalization). The barrier only contains the
// effects until that decision; it does not itself judge whether a request is
// trustworthy or detect prompt injection.
//
// The "ask" mode hands the decision to the human at the console. After every
// step has finished, the owner compares each path the steps touch (rm/mkdir
// arguments and "into" targets) in the public view before xbegin with its own
// private view, and passes that summary to xask(). The kernel prints it
// immediately, freezes the attempt and reads y/n. The summary is written by
// the owner, so it is only as trustworthy as specrun itself; held console and
// pipe output is not part of it and stays held until commit.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAXCMD 8
#define MAXW   12
#define MAXPATHS 16

static char *cmds[MAXCMD][MAXW];
static char *into[MAXCMD];
static int pipeto[MAXCMD];     // 1: stdout of this command feeds the next one
static int ncmd;

static char *paths[MAXPATHS];  // paths the steps may change (ask mode)
static struct stat before[MAXPATHS];
static int existed[MAXPATHS];
static int npaths;
static char summary[1024];
static int slen;

static void
addpath(char *path)
{
  int i;
  for(i = 0; i < npaths; i++)
    if(strcmp(paths[i], path) == 0)
      return;
  if(npaths < MAXPATHS)
    paths[npaths++] = path;
}

static void
put(char *s)
{
  while(*s && slen < (int)sizeof(summary) - 1)
    summary[slen++] = *s++;
  summary[slen] = 0;
}

static void
putnum(uint n)
{
  char buf[12];
  int i = sizeof(buf) - 1;
  buf[i] = 0;
  do {
    buf[--i] = '0' + n % 10;
    n /= 10;
  } while(n && i > 0);
  put(buf + i);
}

// Compare the public "before" state with the owner's private view (reading
// your own writes is not an externalization).
static void
summarize(void)
{
  struct stat st;
  int i, now, changes = 0;

  put("proposed changes (summary by specrun):");
  for(i = 0; i < npaths; i++){
    now = stat(paths[i], &st) == 0;
    if(!existed[i] && !now)
      continue;
    if(existed[i] && now && st.ino == before[i].ino && st.size == before[i].size)
      continue;   // size-only check: a same-size rewrite is not detected
    changes++;
    put("\n  ");
    if(!existed[i]){
      put("create ");
      put(paths[i]);
      put(" (");
      putnum(st.size);
      put(" bytes)");
    } else if(!now){
      put("delete ");
      put(paths[i]);
    } else {
      put("modify ");
      put(paths[i]);
      put(" (");
      putnum(before[i].size);
      put(" -> ");
      putnum(st.size);
      put(" bytes)");
    }
  }
  if(changes == 0)
    put("\n  no file changes detected");
  put("\n  console/pipe output stays held until commit");
}

static void
say(char *msg, char *arg)
{
  // Called only while NOT speculative, so it is visible immediately.
  printf(1, "SPECRUN: %s%s\n", msg, arg ? arg : "");
}

static int
run1(int i, int in, int out, int *pp)
{
  int pid = fork(), fd;
  if(pid != 0)
    return pid;
  if(in >= 0){
    close(0);
    dup(in);
  }
  if(out >= 0){
    close(1);
    dup(out);
  }
  if(pp){                       // drop both original pipe ends (EOF for the reader)
    close(pp[0]);
    close(pp[1]);
  }
  if(into[i]){
    close(1);
    if((fd = open(into[i], O_CREATE | O_WRONLY)) != 1)
      exit();
  }
  exec(cmds[i][0], cmds[i]);
  printf(2, "specrun: exec %s failed\n", cmds[i][0]);
  exit();
}

int
main(int argc, char *argv[])
{
  int nargs = 0, i, a, p[2], rc, commit, ask = 0, answer = -1;
  char *mode, *keep = 0;
  struct stat st;

  if(argc < 3){
    printf(2, "usage: specrun commit|abort|ask|keep PATH STEP [: STEP ...]\n");
    exit();
  }
  mode = argv[1];
  a = 2;
  if(strcmp(mode, "keep") == 0){
    keep = argv[2];
    a = 3;
  } else if(strcmp(mode, "ask") == 0){
    ask = 1;
  } else if(strcmp(mode, "commit") != 0 && strcmp(mode, "abort") != 0){
    printf(2, "specrun: unknown mode %s\n", mode);
    exit();
  }
  for(; a < argc && ncmd < MAXCMD; a++){
    char *w = argv[a], *s;
    nargs = 0;
    for(;;){                            // split this step at commas, in place
      for(s = w; *s && *s != ','; s++)
        ;
      int last = *s == 0;
      *s = 0;
      if(strcmp(w, "pipe") == 0 && nargs && ncmd < MAXCMD - 1){
        cmds[ncmd][nargs] = 0;
        pipeto[ncmd++] = 1;
        nargs = 0;
      } else if(strcmp(w, "into") == 0 && !last){
        w = s + 1;
        for(s = w; *s && *s != ','; s++)
          ;
        last = *s == 0;
        *s = 0;
        into[ncmd] = w;
      } else if(*w && nargs < MAXW - 1)
        cmds[ncmd][nargs++] = w;
      if(last)
        break;
      w = s + 1;
    }
    if(nargs){
      cmds[ncmd][nargs] = 0;
      ncmd++;
    }
  }

  if(ask){
    for(i = 0; i < ncmd; i++){
      if(into[i])
        addpath(into[i]);
      if(strcmp(cmds[i][0], "rm") == 0 || strcmp(cmds[i][0], "mkdir") == 0)
        for(a = 1; cmds[i][a]; a++)
          addpath(cmds[i][a]);
    }
    for(i = 0; i < npaths; i++)
      existed[i] = stat(paths[i], &before[i]) == 0;   // public view
  }

  if(xbegin() != 0){
    say("xbegin failed (already speculative or no free context)", 0);
    exit();
  }
  for(i = 0; i < ncmd; i++){
    if(strcmp(cmds[i][0], "pause") == 0 && cmds[i][1]){
      sleep(atoi(cmds[i][1]));
      continue;
    }
    if(pipeto[i] && i + 1 < ncmd && pipe(p) == 0){
      run1(i, -1, p[1], p);
      run1(i + 1, p[0], -1, p);
      close(p[0]);
      close(p[1]);
      wait();
      wait();
      i++;
      continue;
    }
    if(run1(i, -1, -1, 0) > 0)
      wait();
  }
  if(ask){
    summarize();
    answer = xask(summary);     // all members have exited; owner is alone
    commit = answer == 1;
  } else if(keep)
    commit = stat(keep, &st) == 0;      // private view: our own effects
  else
    commit = strcmp(mode, "commit") == 0;
  rc = commit ? xcommit() : xabort();
  if(commit && rc == 0)
    say("committed: changes published; held output released above", 0);
  else if(commit){
    xabort();
    say("commit refused (conflict, first committer wins); attempt aborted", 0);
  } else if(ask && answer == 0)
    say("aborted by user: private changes and held output discarded", 0);
  else if(ask)
    say("ask failed (refused or interrupted); attempt aborted", 0);
  else if(keep)
    say("aborted by guard: missing in the private view: ", keep);
  else
    say("aborted: private changes and held output discarded", 0);
  exit();
}
