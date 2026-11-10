//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "mmu.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

// An outside unlink cannot retire an inode held by a speculative descriptor,
// even before its first write. The shared cache reference protects its identity.
int
file_spec_busy(struct inode *ip)
{
  struct file *f;
  int busy = 0, id = spec_current();
  acquire(&ftable.lock);
  for(f = ftable.file; f < &ftable.file[NFILE]; f++)
    if(f->ref && f->type == FD_INODE && f->ip == ip && f->spec && f->spec != id)
      busy = 1;
  release(&ftable.lock);
  return busy;
}

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      f->spec = 0;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    struct shadow_save *save = 0;
    struct spec_context *sc = 0;
    int unlinked = 0;
    begin_op();
    if(spec_current() && (sc = spec_pin(spec_current())) != 0){
      ilock(ff.ip);
      unlinked = !fs_failed() && ff.ip->nlink == 0;
      iunlock(ff.ip);
      if(unlinked){
        save = shadow_savepoint();
        if(!save) fs_fail();
      }
      spec_unpin(sc);
    }
    iput(ff.ip);
    if(unlinked && fs_failed()){
      if(save){ shadow_restore(save); inode_invalidate(); }
      spec_abort_only();
    }
    if(save) shadow_save_free(save);
    end_op();
  }
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    fs_enter();
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    fs_leave();
    return 0;
  }
  return -1;
}

// Decision D21: a speculating process may read/write a non-device inode only
// through a file object opened by its own speculation. A file object opened
// earlier may be shared with an outside process, and moving its offset would
// be a visible effect that survives abort. Devices (the console) are exempt.
// Caller holds f->ip->lock.
static int
spec_file_ok(struct file *f)
{
  int id = spec_current();
  return id == 0 || f->ip->type == T_DEV || f->spec == id;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){
    int gated = f->ip->type != T_DEV;
    if(gated) fs_enter();
    ilock(f->ip);
    if(!spec_file_ok(f)){
      iunlock(f->ip);
      if(gated) fs_leave();
      return -1;
    }
    if((r = readi(f->ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    if(gated) fs_leave();
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  int r;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;

    // A speculative write never enters the log, so it needs no transaction
    // or chunking. Partial private writes return the byte count (D22).
    if(spec_current() != 0){
      fs_enter();
      ilock(f->ip);
      if(!spec_file_ok(f)){
        iunlock(f->ip);
        fs_leave();
        return -1;
      }
      if((r = writei(f->ip, addr, f->off, n)) > 0)
        f->off += r;
      iunlock(f->ip);
      fs_leave();
      return r;
    }

    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r < 0)
        break;
      i += r;
      // Short write: an outside write reached a privately copied block (D20).
      if(r != n1)
        break;
    }
    if(i == n)
      return n;
    return i > 0 ? i : -1;   // xv6 short-write convention
  }
  panic("filewrite");
}

