#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
  int spec_refs;  // active references from uncommitted speculative pipe buffers
};

struct spec_pipe_record {
  struct pipe *pipe;
  uint len;
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *p;

  p = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((p = (struct pipe*)kalloc()) == 0)
    goto bad;
  p->readopen = 1;
  p->writeopen = 1;
  p->nwrite = 0;
  p->nread = 0;
  p->spec_refs = 0;
  initlock(&p->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = p;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = p;
  return 0;

//PAGEBREAK: 20
 bad:
  if(p)
    kfree((char*)p);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *p, int writable)
{
  acquire(&p->lock);
  if(writable){
    p->writeopen = 0;
    wakeup(&p->nread);
  } else {
    p->readopen = 0;
    wakeup(&p->nwrite);
  }
  if(p->readopen == 0 && p->writeopen == 0 && p->spec_refs == 0){
    release(&p->lock);
    kfree((char*)p);
  } else
    release(&p->lock);
}

// Staging write for speculative processes directed towards external readers.
// Buffers in context memory with short-write semantics; never sleeps on external readers.
static int
spec_pipe_write(struct pipe *p, char *addr, int n)
{
  struct spec_context *sc;
  int id = spec_current();
  if(id == 0)
    return -1;

  sc = spec_pin(id);
  if(!sc)
    return -1;

  acquire(&sc->resource_lock);
  acquire(&p->lock);
  if(p->readopen == 0 || myproc()->killed){
    release(&p->lock);
    release(&sc->resource_lock);
    spec_unpin(sc);
    return -1;
  }
  release(&p->lock);

  if(sc->pipe_buf == 0){
#ifdef SHADOW_TEST
    if(shadow_test_fail(STF_PIPE))
      sc->pipe_buf = 0;
    else
#endif
    sc->pipe_buf = kalloc();
    if(sc->pipe_buf == 0){
      release(&sc->resource_lock);
      spec_unpin(sc);
      return -1;
    }
    sc->pipe_len = 0;
    sc->pipe_cap = PGSIZE;
  }

  uint overhead = sizeof(struct spec_pipe_record);
  if(sc->pipe_len + overhead >= sc->pipe_cap){
    release(&sc->resource_lock);
    spec_unpin(sc);
    return -1;
  }

  uint avail = sc->pipe_cap - sc->pipe_len - overhead;
  uint to_write = n;
  if(to_write > avail)
    to_write = avail;

  if(to_write == 0){
    release(&sc->resource_lock);
    spec_unpin(sc);
    return -1;
  }

  struct spec_pipe_record *rec = (struct spec_pipe_record*)(sc->pipe_buf + sc->pipe_len);
  rec->pipe = p;
  rec->len = to_write;
  memmove((char*)(rec + 1), addr, to_write);
  sc->pipe_len += overhead + to_write;

  acquire(&p->lock);
  p->spec_refs++;
  release(&p->lock);

  release(&sc->resource_lock);
  spec_unpin(sc);
  return to_write;
}

// Durable commit, part 1: detach the held pipe records from the context so
// that context cleanup (spec_pipe_discard) no longer sees them. The caller
// owns *bufp afterwards and must pass it to spec_pipe_deliver().
void
spec_pipe_take(struct spec_context *sc, char **bufp, uint *lenp)
{
  acquire(&sc->resource_lock);
  *bufp = sc->pipe_buf;
  *lenp = sc->pipe_len;
  sc->pipe_buf = 0;
  sc->pipe_len = 0;
  sc->pipe_cap = 0;
  release(&sc->resource_lock);
}

// Durable commit, part 2 (D42, Step 10 fix): deliver detached records into
// the outside pipes. This may sleep while an outside pipe is full, so the
// caller must hold NO filesystem gate, inode lock or spinlock: the outside
// reader may need the filesystem (e.g. console printf -> begin_op) before it
// reads again. A kill of the releasing process stops delivery; the rest is
// dropped (partial delivery, documented with the crash window).
void
spec_pipe_deliver(char *buf, uint total_len)
{
  if(buf == 0)
    return;
  uint off = 0;
#ifdef SHADOW_TEST
  int records = 0;
#endif
  while(off < total_len){
    struct spec_pipe_record *rec = (struct spec_pipe_record*)(buf + off);
    struct pipe *p = rec->pipe;
    uint len = rec->len;
    char *data = buf + off + sizeof(struct spec_pipe_record);
    off += sizeof(struct spec_pipe_record) + len;

    acquire(&p->lock);
    uint i;
    for(i = 0; i < len; i++){
      while(p->nwrite == p->nread + PIPESIZE){
        if(p->readopen == 0 || myproc()->killed)
          break;
        wakeup(&p->nread);
        sleep(&p->nwrite, &p->lock);
      }
      if(p->readopen == 0 || myproc()->killed)
        break;
      p->data[p->nwrite++ % PIPESIZE] = data[i];
    }
    wakeup(&p->nread);

    p->spec_refs--;
    int should_free = (p->readopen == 0 && p->writeopen == 0 && p->spec_refs == 0);
    release(&p->lock);
    if(should_free)
      kfree((char*)p);
#ifdef SHADOW_TEST
    if(++records == 1 && off < total_len)
      shadow_crash_point(SPC_MID_PIPE);   // first record out, rest held
#endif
  }

  kfree(buf);
}

// Discard held speculative pipe output on abort, cancellation, or owner kill.
void
spec_pipe_discard(struct spec_context *sc)
{
  acquire(&sc->resource_lock);
  if(sc->pipe_buf == 0){
    release(&sc->resource_lock);
    return;
  }
  char *buf = sc->pipe_buf;
  uint total_len = sc->pipe_len;
  sc->pipe_buf = 0;
  sc->pipe_len = 0;
  sc->pipe_cap = 0;
  release(&sc->resource_lock);

  uint off = 0;
  while(off < total_len){
    struct spec_pipe_record *rec = (struct spec_pipe_record*)(buf + off);
    struct pipe *p = rec->pipe;
    uint len = rec->len;
    off += sizeof(struct spec_pipe_record) + len;

    acquire(&p->lock);
    p->spec_refs--;
    wakeup(&p->nread);
    int should_free = (p->readopen == 0 && p->writeopen == 0 && p->spec_refs == 0);
    release(&p->lock);
    if(should_free)
      kfree((char*)p);
  }

  kfree(buf);
}

//PAGEBREAK: 40
int
pipewrite(struct pipe *p, char *addr, int n)
{
  int spec = spec_current();
  if(spec != 0 && pipe_has_external_reader(p, spec)){
    return spec_pipe_write(p, addr, n);
  }

  int i;

  acquire(&p->lock);
  if(p->readopen == 0 || myproc()->killed){
    release(&p->lock);
    return -1;
  }
  for(i = 0; i < n; i++){
    while(p->nwrite == p->nread + PIPESIZE){  //DOC: pipewrite-full
      if(p->readopen == 0 || myproc()->killed){
        release(&p->lock);
        return -1;
      }
      wakeup(&p->nread);
      sleep(&p->nwrite, &p->lock);  //DOC: pipewrite-sleep
    }
    p->data[p->nwrite++ % PIPESIZE] = addr[i];
  }
  wakeup(&p->nread);  //DOC: pipewrite-wakeup1
  release(&p->lock);
  return n;
}

int
piperead(struct pipe *p, char *addr, int n)
{
  int i;

  acquire(&p->lock);
  while(p->nread == p->nwrite && (p->writeopen || p->spec_refs > 0)){  //DOC: pipe-empty
    if(myproc()->killed){
      release(&p->lock);
      return -1;
    }
    sleep(&p->nread, &p->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(p->nread == p->nwrite)
      break;
    addr[i] = p->data[p->nread++ % PIPESIZE];
  }
  wakeup(&p->nwrite);  //DOC: piperead-wakeup
  release(&p->lock);
  return i;
}
