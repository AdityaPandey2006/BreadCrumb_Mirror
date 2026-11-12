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
#include "stat.h"
#include "file.h"

extern struct superblock sb;

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
  uchar data[BSIZE];            // private bytes
  uchar orig[BSIZE];            // public bytes at first copy (D24 validation)
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
  return inum != 0 && (index_busy(dev, inum, 1) ||
                       index_busy(dev, IBLOCK(inum, sb), 0));
}

#ifdef SHADOW_TEST
#include "shadowtest.h"
static volatile uint fail_alloc;
static volatile uint hold_loader;
static volatile uint waiting;
static volatile uint pages;
static volatile uint pubwrites;
static volatile uint pubhold;     // ST_PUBHOLD: hold point to pause at
static volatile uint pubheld;     // hold point a committer is paused at
static volatile uint crashpoint;  // ST_CRASH: SPC_* point to halt at
static volatile uint fail_console; // ST_FAIL STF_CONSOLE: next console page
static volatile uint fail_pipe;    // ST_FAIL STF_PIPE: next held-pipe page

// Test-only: consume a pending injected failure for the console (STF_CONSOLE)
// or pipe (STF_PIPE) held-output page. Returns 1 if the caller must fail.
int
shadow_test_fail(int which)
{
  if(which == STF_CONSOLE)
    return __sync_bool_compare_and_swap(&fail_console, 1, 0);
  if(which == STF_PIPE)
    return __sync_bool_compare_and_swap(&fail_pipe, 1, 0);
  return 0;
}

void
shadow_note_public_write(void)
{
  __sync_fetch_and_add(&pubwrites, 1);
}

// Test-only crash injection for the speculation's own exclusive commit.
// Halts the machine; disk writes already issued are complete (bwrite is
// synchronous). The harness then reboots the SAME disk image.
void
shadow_crash_point(int where)
{
  if(__sync_val_compare_and_swap(&crashpoint, where, 0) != (uint)where)
    return;
  cprintf("SPEC CRASH POINT %d\n", where);
  panic("spec crash point (test-only)");
}

// Test-only pause inside publication. A kill also ends the pause, so tests
// can kill the owner deterministically on either side of the point of no
// return. While paused the committer holds the exclusive log and the
// affected inode locks: other processes' begin_op()/console writes wait.
static void
pub_hold_point(uint where)
{
  if(__sync_val_compare_and_swap(&pubhold, where, where) != where)
    return;
  __sync_lock_test_and_set(&pubheld, where);
  while(__sync_val_compare_and_swap(&pubhold, where, where) == where &&
        !myproc()->killed)
    yield();
  __sync_lock_test_and_set(&pubhold, 0);
  __sync_lock_test_and_set(&pubheld, 0);
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

struct shadow_save {
  struct spec_context *sc;
  struct shadow *head;
};

// Operation-local savepoints, not nested user speculations. The filesystem gate
// excludes other filesystem users while an operation copies/restores its view.
struct shadow_save*
shadow_savepoint(void)
{
  struct shadow_save *save;
  struct shadow *s, *copy;
  struct spec_context *sc = spec_pin(spec_current());
  if(!sc)
    return 0;
  save = (struct shadow_save*)kalloc();
  if(!save){ spec_unpin(sc); return 0; }
  save->sc = sc;
  save->head = 0;
  for(s = sc->resources; s; s = s->next){
    if(s->state != SH_READY || (copy = (struct shadow*)entry_alloc()) == 0){
      shadow_save_free(save);
      return 0;
    }
    memmove(copy, s, sizeof(*copy));
    copy->next = save->head;
    save->head = copy;
  }
  return save;
}

void
shadow_save_free(struct shadow_save *save)
{
  struct shadow *s, *next;
  for(s = save->head; s; s = next){
    next = s->next;
    entry_free(s);
  }
  spec_unpin(save->sc);
  kfree((char*)save);
}

void
shadow_restore(struct shadow_save *save)
{
  struct shadow *s;
  shadow_discard(save->sc);
  acquire(&save->sc->resource_lock);
  save->sc->resources = save->head;
  save->head = 0;
  for(s = save->sc->resources; s; s = s->next)
    index_add(s);
  release(&save->sc->resource_lock);
  myproc()->fs_error = 0;
}

int
shadow_inode_new(struct spec_context *sc, uint inum)
{
  struct shadow *s;
  int fresh = 0;
  acquire(&sc->resource_lock);
  s = find(sc, ROOTDEV, IBLOCK(inum, sb));
  if(s && s->state == SH_READY)
    fresh = ((struct dinode*)s->orig)[inum % IPB].type == 0;
  release(&sc->resource_lock);
  return fresh;
}

// Approval report: caller owns the filesystem gate and is the sole participant.
// No resource operation is in flight (refs==1). Existing shadow images remain
// stable; only the bounded text page is new storage, not another file copy.
struct report_text { char *p; int n, cap, failed; };

static void
rp(struct report_text *r, char *s)
{
  while(*s){
    if(r->n >= r->cap-1){ r->failed = 1; return; }
    r->p[r->n++] = *s++;
  }
  r->p[r->n] = 0;
}

static void
rn(struct report_text *r, uint n)
{
  char b[12]; int i = 0;
  do { b[i++] = '0'+n%10; n /= 10; } while(n);
  while(i){ char c[2] = {b[--i], 0}; rp(r, c); }
}

static void
rbytes(struct report_text *r, uchar *p, int n)
{
  static char hex[] = "0123456789abcdef";
  int i;
  rp(r, "\"");
  for(i = 0; i < n; i++){
    uint c = p[i];
    char b[5] = {'\\', 'x', hex[c>>4], hex[c&15], 0};
    if(c >= 32 && c < 127 && c != '"' && c != '\\'){
      b[0] = c; b[1] = 0;
    }
    rp(r, b);
  }
  rp(r, "\"");
}

static void
report_image(struct spec_context *sc, uint block, int after, uint off,
             void *dst, uint n)
{
  struct shadow *s = find(sc, ROOTDEV, block);
  struct buf *b;
  if(s){
    memmove(dst, (after ? s->data : s->orig)+off, n);
    return;
  }
  b = bread_original(ROOTDEV, block);
  memmove(dst, b->data+off, n);
  brelse(b);
}

static void
report_inode(struct spec_context *sc, uint ino, int after, struct dinode *d)
{
  report_image(sc, IBLOCK(ino, sb), after, (ino%IPB)*sizeof(*d), d, sizeof(*d));
}

// Identify a data/indirect block without invoking bmap() or allocating blocks.
static int
report_owner(struct spec_context *sc, uint block, int after,
             uint *ino, uint *offset, int *indirect)
{
  struct dinode d;
  struct shadow *s;
  struct buf *bp;
  uint *ptrs;
  uint i, j, addr;
  for(i = 1; i < sb.ninodes; i++){
    report_inode(sc, i, after, &d);
    if(d.type != T_FILE && d.type != T_DIR) continue;
    for(j = 0; j < NDIRECT; j++)
      if(d.addrs[j] && d.addrs[j] == block){
        *ino=i; *offset=j*BSIZE; *indirect=0; return d.type;
      }
    if(d.addrs[NDIRECT]){
      if(d.addrs[NDIRECT] == block){
        *ino=i; *offset=NDIRECT*BSIZE; *indirect=1; return d.type;
      }
      s = find(sc, ROOTDEV, d.addrs[NDIRECT]);
      bp = s ? 0 : bread_original(ROOTDEV, d.addrs[NDIRECT]);
      ptrs = (uint*)(s ? (after ? s->data : s->orig) : bp->data);
      for(j = 0; j < NINDIRECT; j++){
        addr = ptrs[j];
        if(addr && addr == block){
          if(bp) brelse(bp);
          *ino=i; *offset=(NDIRECT+j)*BSIZE; *indirect=0; return d.type;
        }
      }
      if(bp) brelse(bp);
    }
  }
  return 0;
}

static void
report_name(struct report_text *r, char *name)
{
  int i;
  for(i=0; i<DIRSIZ && name[i]; i++){
    uchar c = name[i];
    if(c >= 33 && c < 127 && c != '\\' && c != '"'){
      char b[2] = {c, 0}; rp(r,b);
    } else rbytes(r, &c, 1);
  }
}

int
shadow_report(struct spec_context *sc, char *dst, int cap)
{
  struct report_text r = {dst, 0, cap, 0};
  struct shadow *s;
  struct dinode old, now;
  struct dirent a, b;
  uint i, changed=0, count, first, last, ino, off, oldino, oldoff;
  int kind, ind, oldind, oldkind;
  if(cap < 1) return -1;
  dst[0] = 0;
  // Refuse unsupported raw test blocks BEFORE freezing the owner.
  for(s=sc->resources; s; s=s->next)
    if(s->state != SH_READY || s->inum == 0 || s->dev != ROOTDEV) return -1;
  rp(&r, "Kernel change report (saved original -> private):\n");
  for(s=sc->resources; s; s=s->next){
    count=0; first=BSIZE; last=0;
    for(i=0; i<BSIZE; i++) if(s->orig[i] != s->data[i]){
      if(!count) first=i;
      last=i; count++;
    }
    if(!count) continue;
    changed++;
    rp(&r, "Block "); rn(&r,s->blockno); rp(&r, ": ");
    if(s->blockno >= sb.inodestart && s->blockno < sb.bmapstart){
      rp(&r,"inode metadata\n");
      for(i=0; i<IPB; i++){
        memmove(&old,s->orig+i*sizeof(old),sizeof(old));
        memmove(&now,s->data+i*sizeof(now),sizeof(now));
        if(memcmp(&old,&now,sizeof(old)) == 0) continue;
        ino=(s->blockno-sb.inodestart)*IPB+i;
        rp(&r,"  inode ");rn(&r,ino);
        rp(&r,old.type == 0 ? " created" : now.type == 0 ? " freed" : " modified");
        rp(&r,"; type ");rn(&r,old.type);rp(&r," -> ");rn(&r,now.type);
        rp(&r,"; size ");rn(&r,old.size);rp(&r," -> ");rn(&r,now.size);
        rp(&r,"; links ");rn(&r,old.nlink);rp(&r," -> ");rn(&r,now.nlink);rp(&r,"\n");
      }
    } else if(s->blockno >= sb.bmapstart &&
              s->blockno < sb.bmapstart + (sb.size+BPB-1)/BPB){
      rp(&r,"allocation bitmap\n");
    } else {
      ino=off=oldino=oldoff=0; ind=oldind=0;
      kind=report_owner(sc,s->blockno,1,&ino,&off,&ind);
      oldkind=report_owner(sc,s->blockno,0,&oldino,&oldoff,&oldind);
      if(!kind){kind=oldkind;ino=oldino;off=oldoff;ind=oldind;}
      if(kind){
        rp(&r,ind ? "indirect pointers for inode " : kind==T_DIR ? "directory inode " : "file inode ");
        rn(&r,ino);rp(&r," at offset ");rn(&r,off);rp(&r,"\n");
      } else rp(&r,"unmapped data or metadata (raw byte changes listed below)\n");
      if(kind == T_DIR && !ind){
        for(i=0; i<BSIZE; i+=sizeof(a)){
          memset(&a,0,sizeof(a));memmove(&b,s->data+i,sizeof(b));
          if(oldkind == T_DIR && !oldind) memmove(&a,s->orig+i,sizeof(a));
          if(a.inum==b.inum && memcmp(a.name,b.name,DIRSIZ)==0) continue;
          if(a.inum){rp(&r,"  delete ");report_name(&r,a.name);rp(&r,"\n");}
          if(b.inum){
            report_inode(sc,b.inum,1,&now);
            rp(&r,"  create ");report_name(&r,b.name);rp(&r," (");rn(&r,now.size);rp(&r," bytes)\n");
          }
        }
      }
    }
    rp(&r,"  ");rn(&r,count);rp(&r," changed bytes; offsets ");rn(&r,first);rp(&r,"..");rn(&r,last);
    rp(&r,"; preview at ");rn(&r,first);rp(&r,":\n    before ");
    rbytes(&r,s->orig+first,(BSIZE-first < 24 ? BSIZE-first : 24));
    rp(&r,"\n    after  ");rbytes(&r,s->data+first,(BSIZE-first < 24 ? BSIZE-first : 24));rp(&r,"\n");
  }
  if(!changed) rp(&r,"No filesystem block changes.\n");
  rp(&r,"Changed blocks: ");rn(&r,changed);rp(&r,". Every changed block listed; byte previews limited to 24 bytes.\n");
  rp(&r,"Held console bytes: ");rn(&r,sc->console_len);
  rp(&r,". Held external-pipe storage bytes (includes record headers): ");rn(&r,sc->pipe_len);
  rp(&r,". Payloads remain hidden until durable commit.\n");
  // A missing/truncated report can never authorize publication.
  return r.failed ? -1 : r.n;
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
    memmove(s->orig, b->data, BSIZE);
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

// Step 5: publish a COMMITTING context's private blocks in one exclusive,
// durable log transaction. Called by spec_complete() with the context's
// exclusive completion reference, no spinlock held, and no member or
// resource operation in flight (so the entry list cannot change).
//
//   begin_op_exclusive()                 the transaction is alone in the log
//   iget + ilock every affected inode    in inode-number order (all T_FILE)
//   validate: each public block == the bytes it was copied from     (D24)
//   last kill check                      (D23) -- point of no return after it
//   install: bread_original, copy, log_write, brelse, per block
//   end_op()                             runs commit(): durable at write_head
//   iunlock all; iput all in a separate ordinary transaction
//
// The inode locks are held from before the first public change until after
// the commit record is on disk, so a live reader (readi holds the inode lock)
// sees all-old or all-new contents and never sees undurable bytes. Holding
// them across end_op() is safe only because the transaction is exclusive:
// no other operation in it can be waiting for these locks.
// Returns SP_OK, SP_UNSUPPORTED, SP_CONFLICT or SP_KILLED (defs.h).
int
shadow_publish(struct spec_context *sc)
{
  struct shadow *e[NSHADOW], *s;
  struct buf *b;
  int n = 0, i, r = SP_OK;

  acquire(&sc->resource_lock);
  for(s = sc->resources; s; s = s->next){
    if(n == NSHADOW || s->state != SH_READY || s->inum == 0 || s->dev != ROOTDEV){
      release(&sc->resource_lock);
      return SP_UNSUPPORTED;
    }
    e[n++] = s;
  }
  release(&sc->resource_lock);
  if(n == 0) return SP_OK;

  // spec_complete holds the filesystem gate: readers, namespace lookups and
  // ordinary mutation cannot observe a partially published metadata/data view.
  begin_op_exclusive();
  for(i = 0; i < n && r == SP_OK; i++){
    b = bread_original(e[i]->dev, e[i]->blockno);
    if(memcmp(b->data, e[i]->orig, BSIZE) != 0)
      r = SP_CONFLICT;
    brelse(b);
  }
#ifdef SHADOW_TEST
  if(r == SP_OK) pub_hold_point(1);
#endif
  if(r == SP_OK && myproc()->killed) r = SP_KILLED;
  if(r == SP_OK){
    // D23 point of no return; metadata and data share this one transaction.
    for(i = 0; i < n; i++){
      b = bread_original(e[i]->dev, e[i]->blockno);
      memmove(b->data, e[i]->data, BSIZE);
      log_write(b);
      brelse(b);
#ifdef SHADOW_TEST
      if(i == 0) pub_hold_point(2);
#endif
    }
  }
  end_op();
  inode_invalidate();
  return r;
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
  if(op == ST_PUBHOLD){
    __sync_lock_test_and_set(&pubhold, value);
    return 0;
  }
  if(op == ST_PUBHELD)
    return __sync_val_compare_and_swap(&pubheld, 0, 0);
  if(op == ST_CRASH){
    __sync_lock_test_and_set(&crashpoint, value);
    return 0;
  }
  if(op == ST_FAIL){
    if(value == STF_CONSOLE)
      __sync_lock_test_and_set(&fail_console, 1);
    else if(value == STF_PIPE)
      __sync_lock_test_and_set(&fail_pipe, 1);
    else if(value == 0 || value == STF_SHADOW)
      __sync_lock_test_and_set(&fail_alloc, 1);
    else
      return -1;
    return 0;
  }
  if(op == ST_FREEPAGES)
    return kfreepages();
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
