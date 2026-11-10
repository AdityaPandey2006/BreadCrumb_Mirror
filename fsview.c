// A coarse, reentrant filesystem gate serializes metadata views and publication.
// Pipes and blocking device reads do not take it. No spinlock spans disk I/O.
#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "stat.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "buf.h"
#include "syscall.h"
#include "fcntl.h"

static struct {
  struct spinlock lock;
  int owner, depth;
} gate;

void fsviewinit(void) { initlock(&gate.lock, "filesystem-view"); }

void
fs_enter(void)
{
  int pid = myproc()->pid;
  acquire(&gate.lock);
  while(gate.owner && gate.owner != pid)
    sleep(&gate, &gate.lock);
  if(!gate.owner){
    gate.owner = pid;
    myproc()->fs_error = 0;
  }
  gate.depth++;
  release(&gate.lock);
}

void
fs_leave(void)
{
  acquire(&gate.lock);
  if(gate.owner != myproc()->pid || gate.depth < 1)
    panic("filesystem gate");
  if(--gate.depth == 0){
    gate.owner = 0;
    wakeup(&gate);
  }
  release(&gate.lock);
}

int fs_failed(void) { return myproc()->fs_error; }
void fs_fail(void) { myproc()->fs_error = 1; }

struct buf*
fs_bread(uint dev, uint block)
{
  struct buf *b, *original;
  int r, id = spec_current();
  if(id == 0 || myproc()->fs_publication)
    return bread_original(dev, block);
  if(fs_failed() || (b = (struct buf*)kalloc()) == 0){
    fs_fail();
    return 0;
  }
  memset(b, 0, sizeof(*b));
  b->spec = id;
  b->dev = dev;
  b->blockno = block;
  b->inum = SH_META;
  r = shadow_read(id, dev, block, 0, b->data, BSIZE);
  if(r < 0){
    kfree((char*)b);
    fs_fail();
    return 0;
  }
  if(r == 0){
    original = bread_original(dev, block);
    memmove(b->data, original->data, BSIZE);
    brelse(original);
  }
  return b;  // private buffer is exclusively owned by this call, not the cache
}

void
fs_brelse(struct buf *b)
{
  if(b->spec)
    kfree((char*)b);
  else
    brelse(b);
}

int
fs_log_write(struct buf *b)
{
  if(!b->spec){
    log_write(b);
    return 0;
  }
  if(fs_failed() || shadow_write(b->spec, b->dev, b->blockno, b->inum,
                                0, b->data, BSIZE) != 1){
    fs_fail();
    return -1;
  }
  return 0;
}

// Identify newly allocated inode identities before discarding their orig images.
// Abort closes new-file descriptors; existing-file handles survive. A cwd made
// inside an aborted attempt falls back to root. No private cache fields survive.
void
fs_finish(struct spec_context *sc, int committed)
{
  struct proc *p = myproc();
  struct file *f;
  int fd;
  uint closemask = 0;
  if(!committed){
    for(fd = 0; fd < NOFILE; fd++){
      f = p->ofile[fd];
      if(f && f->type == FD_INODE && f->spec == sc->id &&
         shadow_inode_new(sc, f->ip->inum))
        closemask |= 1U << fd;
    }
    if(shadow_inode_new(sc, p->cwd->inum)){
      iput(p->cwd);
      p->cwd = iget_ref(ROOTDEV, ROOTINO);
    }
  }
  inode_invalidate();
  // Clear tags so historical handles do not obstruct future outside unlinks.
  for(fd = 0; fd < NOFILE; fd++){
    f = p->ofile[fd];
    if(f && f->type == FD_INODE){
      if(f->spec == sc->id) f->spec = 0;
      if(closemask & (1U << fd)){
        p->ofile[fd] = 0;
        fileclose(f);
      }
    }
  }
  inode_invalidate();
}

// Namespace syscalls are checkpointed as units. Allocation/capacity failure
// cannot leave an allocated inode without its name, or half an unlink/link.
int
fs_syscall(int num, int (*call)(void))
{
  struct shadow_save *save = 0;
  int r, mode = 0, mutate = num == SYS_link || num == SYS_unlink ||
                  num == SYS_mkdir || num == SYS_mknod;
  if(num == SYS_open && argint(1, &mode) == 0 && (mode & O_CREATE))
    mutate = 1;
  fs_enter();
  if(spec_current() && mutate){
    save = shadow_savepoint();
    if(save == 0){
      fs_leave();
      return -1;
    }
  }
  r = call();
  if(r < 0 || fs_failed()){
    if(save){
      shadow_restore(save);
      inode_invalidate();
    }
    r = -1;
  }
  if(save)
    shadow_save_free(save);
  fs_leave();
  return r;
}
