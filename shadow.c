// Private block storage. Step 4: readi()/writei() route speculative regular-
// file data through this module; it never writes the public cache or log.
#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"

enum shadowstate { SH_LOADING, SH_READY };

// One kalloc page per entry. Unlinked/free entries are FREE implicitly.
// Linked allocation avoids a fixed pointer array and keeps wait channels stable.
// dev, blockno and inum are immutable while the entry is linked anywhere.
struct shadow {
  struct shadow *next;          // per-context list, under resource_lock
  struct shadow *gnext;         // global index, under shadowmap.lock
  uint dev;
  uint blockno;
  uint inum;                    // owning inode, or 0 for raw test blocks
  enum shadowstate state;
  uchar data[BSIZE];
};
typedef char shadow_fits_page[(sizeof(struct shadow) <= PGSIZE) ? 1 : -1];

// Global index of every linked entry (LOADING or READY) in every context, used
// only for the Step-4 outside-writer refusal (decision D20). The lock is a
// leaf: it is taken after resource_lock, or alone, and nothing is acquired
// while it is held. Checking it never pins a context, so it cannot make an
// owner's xcommit()/xabort() fail its sole-reference test.
static struct {
  struct spinlock lock;
  struct shadow *head;
} shadowmap;

void
shadowinit(void)
{
  initlock(&shadowmap.lock, "shadowmap");
}

static void
index_add(struct shadow *s)
{
  acquire(&shadowmap.lock);
  s->gnext = shadowmap.head;
  shadowmap.head = s;
  release(&shadowmap.lock);
}

static void
index_remove(struct shadow *s)
{
  struct shadow **link;
  acquire(&shadowmap.lock);
  for(link = &shadowmap.head; *link && *link != s; link = &(*link)->gnext)
    ;
  if(*link == 0)
    panic("shadow index");
  *link = s->gnext;
  release(&shadowmap.lock);
}

static int
index_busy(uint dev, uint key, int byinum)
{
  struct shadow *s;
  int busy = 0;
  acquire(&shadowmap.lock);
  for(s = shadowmap.head; s && !busy; s = s->gnext)
    if(s->dev == dev && (byinum ? s->inum == key : s->blockno == key))
      busy = 1;
  release(&shadowmap.lock);
  return busy;
}

// Does any speculation hold a private copy (LOADING or READY) of this block?
// May be called while holding that block's buffer lock: it takes only the
// leaf shadowmap.lock and never sleeps. The public writer must hold the
// buffer lock across check-and-write so first-copy cannot interleave.
int
shadow_block_busy(uint dev, uint blockno)
{
  return index_busy(dev, blockno, 0);
}

// Does any speculation hold a private copy of any data block of this inode?
// Caller holds the inode lock, which every first-copy of a file block holds.
int
shadow_inode_busy(uint dev, uint inum)
{
  return inum != 0 && index_busy(dev, inum, 1);
}

#ifdef SHADOW_TEST
static volatile uint fail_alloc;
static volatile uint hold_loader;
static volatile uint waiting;
static volatile uint pages;
static volatile uint pubwrites;

void
shadow_note_public_write(void)
{
  __sync_fetch_and_add(&pubwrites, 1);
}
#endif

static struct shadow*
find(struct spec_context *sc, uint dev, uint blockno)
{
  struct shadow *s;
  for(s = sc->resources; s; s = s->next)
    if(s->dev == dev && s->blockno == blockno)
      return s;
  return 0;
}

static char*
entry_alloc(void)
{
  char *p;
#ifdef SHADOW_TEST
  if(__sync_bool_compare_and_swap(&fail_alloc, 1, 0))
    return 0;
#endif
  p = kalloc();
  if(p){
    memset(p, 0, PGSIZE);
#ifdef SHADOW_TEST
    __sync_fetch_and_add(&pages, 1);
#endif
  }
  return p;
}

static void
entry_free(struct shadow *s)
{
#ifdef SHADOW_TEST
  __sync_fetch_and_sub(&pages, 1);
#endif
  kfree((char*)s);
}

int
shadow_count(struct spec_context *sc)
{
  int n = 0;
  struct shadow *s;
  acquire(&sc->resource_lock);
  for(s = sc->resources; s; s = s->next)
    n++;
  release(&sc->resource_lock);
  return n;
}

// Only completion's exclusive reference or the final reclaimer may call this.
// No other user may hold an entry reference; free outside both spinlocks.
void
shadow_discard(struct spec_context *sc)
{
  struct shadow *s, *next;
  acquire(&sc->resource_lock);
  s = sc->resources;
  sc->resources = 0;
  release(&sc->resource_lock);
  for(; s; s = next){
    next = s->next;
    index_remove(s);
    entry_free(s);
  }
}

// Return 1 if copied/updated, 0 for a read with no private version, -1 on
// invalid input, capacity/allocation failure, or cancellation. Never public-write.
// data may be a user address already validated by argptr() (as readi/writei
// already accept): xv6 maps all user memory, so the copy cannot fault.
static int
access(int id, uint dev, uint blockno, uint inum, uint off, void *data, uint n,
       int write)
{
  struct spec_context *sc, *active;
  struct shadow *s, *candidate = 0;
  struct buf *b;
  int result = -1, count;

  if(dev != ROOTDEV || blockno >= FSSIZE || off > BSIZE || n > BSIZE-off ||
     data == 0 || n == 0)
    return -1;
  if((sc = spec_pin(id)) == 0)
    return -1;
  acquire(&sc->resource_lock);
  for(;;){
    if(myproc()->killed)
      goto out;
    s = find(sc, dev, blockno);
    if(s && s->state == SH_LOADING){
#ifdef SHADOW_TEST
      __sync_fetch_and_add(&waiting, 1);
#endif
      // Cancellation check and sleep admission are atomic under ptable.lock.
      result = spec_wait(sc, sc, &sc->resource_lock);
#ifdef SHADOW_TEST
      __sync_fetch_and_sub(&waiting, 1);
#endif
      if(result < 0){
        result = -1;
        goto out;
      }
      continue;  // entry may have disappeared after failed initialization
    }
    if(s){
      if(write)
        memmove(s->data+off, data, n);
      else
        memmove(data, s->data+off, n);
      result = 1;
      goto out;
    }
    if(!write){
      result = 0;
      goto out;
    }
    count = 0;
    for(s = sc->resources; s; s = s->next)
      count++;
    if(count >= NSHADOW){
      result = -1;
      goto out;
    }
    if(candidate == 0){
      release(&sc->resource_lock);
      candidate = (struct shadow*)entry_alloc();
      acquire(&sc->resource_lock);
      if(candidate == 0){
        result = -1;
        goto out;
      }
      // Recheck after allocation: another member may have reserved this block.
      continue;
    }
    s = candidate;
    candidate = 0;
    s->dev = dev;
    s->blockno = blockno;
    s->inum = inum;
    s->state = SH_LOADING;
    s->next = sc->resources;
    sc->resources = s;
    // Indexed before the source read: a public writer holding this block's
    // buffer lock either sees the reservation, or finishes before we copy.
    index_add(s);
    release(&sc->resource_lock);

#ifdef SHADOW_TEST
    while(__sync_val_compare_and_swap(&hold_loader, 0, 0) && !myproc()->killed)
      yield();
#endif
    // Explicit original view: bypasses speculative lookup by construction.
    // Never call while holding resource_lock or a source buffer lock.
    b = bread_original(dev, blockno);
    memmove(s->data, b->data, BSIZE);
    brelse(b);
    // A pin keeps memory alive but is not permission to admit work after abort.
    active = spec_pin(id);
    acquire(&sc->resource_lock);
    if(active == 0 || myproc()->killed){
      struct shadow **link;
      for(link = (struct shadow**)&sc->resources; *link != s; link = &(*link)->next)
        ;
      *link = s->next;
      release(&sc->resource_lock);
      index_remove(s);
      entry_free(s);
      wakeup(sc);
      if(active)
        spec_unpin(active);
      spec_unpin(sc);
      return -1;
    }
    memmove(s->data+off, data, n);
    s->state = SH_READY;
    release(&sc->resource_lock);
    wakeup(sc);
    spec_unpin(active);
    spec_unpin(sc);
    return 1;
  }
out:
  release(&sc->resource_lock);
  if(candidate)
    entry_free(candidate);
  spec_unpin(sc);
  return result;
}

int
shadow_read(int id, uint dev, uint blockno, uint off, void *dst, uint n)
{
  return access(id, dev, blockno, 0, off, dst, n, 0);
}

// inum records the owning inode on first copy (0 for raw test blocks).
int
shadow_write(int id, uint dev, uint blockno, uint inum, uint off,
             const void *src, uint n)
{
  return access(id, dev, blockno, inum, off, (void*)src, n, 1);
}

#ifdef SHADOW_TEST
#include "shadowtest.h"
// Test-only bounded interface: no arbitrary block numbers or user pointers.
int
shadow_test(int op, int slot, int value)
{
  struct spec_context *sc;
  struct buf *b;
  uchar byte;
  int r;
  uint block;
  if(op == ST_PAGES)
    return __sync_val_compare_and_swap(&pages, 0, 0);
  if(op == ST_WAITERS)
    return __sync_val_compare_and_swap(&waiting, 0, 0);
  if(op == ST_PUBWRITES)
    return __sync_val_compare_and_swap(&pubwrites, 0, 0);
  if(op == ST_FAIL){
    __sync_lock_test_and_set(&fail_alloc, 1);
    return 0;
  }
  if(op == ST_HOLD){
    __sync_lock_test_and_set(&hold_loader, value != 0);
    return 0;
  }
  if(slot < 0 || slot > NSHADOW || value < 0 || value > 255)
    return -1;
  block = FSSIZE-NSHADOW-4+slot;
  if(op == ST_ORIGINAL){
    b = bread_original(ROOTDEV, block);
    r = b->data[0];
    brelse(b);
    return r;
  }
  if(op == ST_COUNT){
    sc = spec_pin(myproc()->spec);
    if(sc == 0)
      return -1;
    r = shadow_count(sc);
    spec_unpin(sc);
    return r;
  }
  byte = value;
  if(op == ST_WRITE)
    return shadow_write(myproc()->spec, ROOTDEV, block, 0, 0, &byte, 1);
  if(op == ST_READ){
    r = shadow_read(myproc()->spec, ROOTDEV, block, 0, &byte, 1);
    return r == 1 ? byte : (r == 0 ? ST_ABSENT : -1);
  }
  if(op == ST_COPY){
    // No-op modification at byte 511 verifies first-copy preserves byte zero.
    b = bread_original(ROOTDEV, block);
    byte = b->data[BSIZE-1];
    brelse(b);
    return shadow_write(myproc()->spec, ROOTDEV, block, 0, BSIZE-1, &byte, 1);
  }
  return -1;
}
#endif
