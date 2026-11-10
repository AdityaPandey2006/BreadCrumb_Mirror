#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "mmu.h"
#include "proc.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int exclusive;   // Step 5 (D26): EXCL_NONE, EXCL_WAITING or EXCL_OWNED
  int dev;
  struct logheader lh;
};
struct log log;

// log.exclusive states. WAITING closes admission while a publisher waits for
// the current group to drain; OWNED means the publisher's transaction is the
// only one in the log. Only the OWNED transaction's own commit releases it:
// a group commit that finishes while a publisher is WAITING must not.
#define EXCL_NONE    0
#define EXCL_WAITING 1
#define EXCL_OWNED   2

static void recover_from_log(void);
static void commit();

void
initlog(int dev)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  struct superblock sb;
  initlock(&log.lock, "log");
  readsb(dev, &sb);
  log.start = sb.logstart;
  log.size = sb.nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    brelse(lbuf);
    brelse(dbuf);
#ifdef SHADOW_TEST
    if(log.exclusive == EXCL_OWNED && tail == 0 && log.lh.n > 1)
      shadow_crash_point(SPC_MID_INSTALL);
#endif
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();
  install_trans(); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void
begin_op(void)
{
  fs_enter();
  if(spec_current() && !myproc()->fs_publication)
    return;  // private filesystem operations never join the public log
  acquire(&log.lock);
  while(1){
    if(log.committing || log.exclusive){
      // exclusive: a speculative publication is waiting for or owns the log.
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  if(spec_current() && !myproc()->fs_publication){
    fs_leave();
    return;
  }

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    if(log.exclusive == EXCL_OWNED)
      log.exclusive = EXCL_NONE;  // the publication's own commit is done
    wakeup(&log);
    release(&log.lock);
  }
  myproc()->fs_publication = 0;
  fs_leave();
}

// Step 5: begin a transaction that is the only one in its log group, for
// publishing a speculation (decision D26). It closes admission to new
// operations, waits for every outstanding operation and any running commit
// to finish, then reserves the log alone. The matching end_op() is
// therefore the last one, and it runs commit() itself: when that end_op()
// returns, this transaction's commit record (write_head) is on disk. The
// caller must hold no inode or buffer locks when calling this.
void
begin_op_exclusive(void)
{
  fs_enter();
  myproc()->fs_publication = 1;
  acquire(&log.lock);
  while(log.exclusive != EXCL_NONE)    // one publication at a time
    sleep(&log, &log.lock);
  log.exclusive = EXCL_WAITING;        // begin_op() now waits
  while(log.outstanding > 0 || log.committing)
    sleep(&log, &log.lock);
  if(log.lh.n != 0)
    panic("begin_op_exclusive: log not empty");
  log.exclusive = EXCL_OWNED;
  log.outstanding = 1;
  release(&log.lock);
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
#ifdef SHADOW_TEST
    if(log.exclusive == EXCL_OWNED)
      shadow_crash_point(SPC_BEFORE_LOG);
#endif
    write_log();     // Write modified blocks from cache to log
#ifdef SHADOW_TEST
    if(log.exclusive == EXCL_OWNED)
      shadow_crash_point(SPC_BEFORE_HEAD);
#endif
    write_head();    // Write header to disk -- the real commit
#ifdef SHADOW_TEST
    if(log.exclusive == EXCL_OWNED)
      shadow_crash_point(SPC_AFTER_HEAD);
#endif
    install_trans(); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache with B_DIRTY.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");
  // Step 5 invariant: a publication's transaction is alone in the log.
  if (log.exclusive == EXCL_OWNED && log.outstanding != 1)
    panic("log_write: exclusive publication shared");
#ifdef SHADOW_TEST
  // Step-4 evidence: a speculative process must never dirty a public buffer
  // (data, inode, bitmap or indirect block) or add it to the log, except
  // in its own exclusive publication transaction (Step 5 commit).
  if(spec_current() != 0 && log.exclusive != EXCL_OWNED)
    shadow_note_public_write();
#endif

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n)
    log.lh.n++;
  b->flags |= B_DIRTY; // prevent eviction
  release(&log.lock);
}

