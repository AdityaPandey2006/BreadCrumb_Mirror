// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#ifdef SHADOW_TEST
#include "shadowtest.h"
#endif

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  uint *argp;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("lapicid %d: panic: ", lapicid());
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c == BACKSPACE){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | 0x0700;  // black on white

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} input;

#define C(x)  ((x)-'@')  // Control-x

void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while((c = getc()) >= 0){
    switch(c){
    case C('P'):  // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'):  // Kill line.
      while(input.e != input.w &&
            input.buf[(input.e-1) % INPUT_BUF] != '\n'){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      if(input.e != input.w){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        c = (c == '\r') ? '\n' : c;
        input.buf[input.e++ % INPUT_BUF] = c;
        consputc(c);
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          input.w = input.e;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

// Approval reservations use cons.lock only to protect ownership. sleep()
// releases that spinlock; no filesystem gate is held while a human decides.
static int approval_owner;
static int approval_waiters;

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(approval_owner || approval_waiters || input.r == input.w){
      if(myproc()->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D')){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

static void
ask_puts(char *s)
{
  while(*s) consputc(*s++ & 0xff);
}

static void
ask_num(uint n)
{
  char buf[12];
  int i = 0;
  do { buf[i++] = '0' + n % 10; n /= 10; } while(n);
  while(i) consputc(buf[--i]);
}

// Owner notes are labels, not a trusted audit. Escape controls and newlines
// so neither a note nor file bytes can forge the host's framed prompt.
static void
ask_note(char *s)
{
  static char hex[] = "0123456789abcdef";
  uint c;
  while(*s){
    c = *s++ & 0xff;
    if(c >= 32 && c < 127 && c != '\\') consputc(c);
    else {
      ask_puts("\\x"); consputc(hex[c >> 4]); consputc(hex[c & 15]);
    }
  }
}

// Reserve the entire exchange, not just printing. Other approval callers and
// ordinary console readers sleep until this exact request has its answer.
int
console_ask(int spec, char *note, char *report)
{
  char ans[4];
  int c, len = 0, result = -1, pid = myproc()->pid;

  acquire(&cons.lock);
  approval_waiters++;
  while(approval_owner && !myproc()->killed)
    sleep(&approval_owner, &cons.lock);
  approval_waiters--;
  if(myproc()->killed){
    wakeup(&input.r);
    release(&cons.lock);
    return -1;
  }
  approval_owner = pid;
  // Discard complete AND partially typed old input while printing atomically.
  input.r = input.w = input.e;
  ask_puts("\n[review "); ask_num(spec); ask_puts(" BEGIN]\n[speculation ");
  ask_num(spec); ask_puts("] "); ask_note(note); ask_puts("\n");
  ask_puts(report);
  ask_puts("[approval "); ask_num(spec); ask_puts("] commit? [y/n] ");
  for(;;){
    while(input.r == input.w && !myproc()->killed)
      sleep(&input.r, &cons.lock);
    if(myproc()->killed) goto done;
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D') || c == '\n') break;
    if(c == ' ' || c == '\t') continue;
    if(len < (int)sizeof(ans))
      ans[len] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
    if(len < 4) len++;          // saturate: a long answer can never become yes
  }
  result = (len == 1 && ans[0] == 'y') ||
           (len == 3 && ans[0] == 'y' && ans[1] == 'e' && ans[2] == 's');
done:
  input.r = input.w = input.e;
  approval_owner = 0;
  wakeup(&approval_owner);
  wakeup(&input.r);
  release(&cons.lock);
  return result;
}

int
spec_console_write(int spec, char *buf, int n)
{
  struct spec_context *sc;
  char *page = 0;
  int written = 0;
  int remaining;

  if(n <= 0)
    return 0;

  sc = spec_pin(spec);
  if(sc == 0)
    return -1;

  acquire(&sc->resource_lock);
  if(sc->state != SPEC_ACTIVE){
    release(&sc->resource_lock);
    spec_unpin(sc);
    return -1;
  }

  if(sc->console_buf == 0){
    release(&sc->resource_lock);
#ifdef SHADOW_TEST
    if(shadow_test_fail(STF_CONSOLE))
      page = 0;
    else
#endif
    page = kalloc();
    if(page == 0){
      spec_unpin(sc);
      return -1;
    }
    acquire(&sc->resource_lock);
    if(sc->state != SPEC_ACTIVE){
      release(&sc->resource_lock);
      kfree(page);
      spec_unpin(sc);
      return -1;
    }
    if(sc->console_buf == 0){
      sc->console_buf = page;
      sc->console_len = 0;
      sc->console_cap = PGSIZE;
      page = 0;
    }
  }

  remaining = sc->console_cap - sc->console_len;
  if(remaining > 0){
    written = n < remaining ? n : remaining;
    memmove(sc->console_buf + sc->console_len, buf, written);
    sc->console_len += written;
  } else {
    written = -1;
  }

  release(&sc->resource_lock);
  if(page)
    kfree(page);
  spec_unpin(sc);

  return written;
}

// Durable commit, part 1: detach the held console page from the context.
// The caller owns *bufp afterwards and must pass it to spec_console_emit().
void
spec_console_take(struct spec_context *sc, char **bufp, uint *lenp)
{
  acquire(&sc->resource_lock);
  *bufp = sc->console_buf;
  *lenp = sc->console_len;
  sc->console_buf = 0;
  sc->console_len = 0;
  sc->console_cap = 0;
  release(&sc->resource_lock);
}

// Durable commit, part 2 (D37): emit the detached bytes, then free the page.
// Runs after the filesystem gate is released and after held pipe output.
void
spec_console_emit(char *buf, uint len)
{
  uint i;

  if(buf == 0)
    return;
#ifdef SHADOW_TEST
  shadow_crash_point(SPC_BEFORE_CONS);
#endif
  acquire(&cons.lock);
  for(i = 0; i < len; i++){
    consputc(buf[i] & 0xff);
#ifdef SHADOW_TEST
    if(i == 15 && len > 16){
      release(&cons.lock);              // panic's cprintf needs cons.lock
      shadow_crash_point(SPC_MID_CONS); // first 16 bytes out, rest held
      acquire(&cons.lock);
    }
#endif
  }
  release(&cons.lock);
  kfree(buf);
}

void
spec_console_discard(struct spec_context *sc)
{
  char *buf = 0;

  acquire(&sc->resource_lock);
  if(sc->console_buf){
    buf = sc->console_buf;
    sc->console_buf = 0;
    sc->console_len = 0;
    sc->console_cap = 0;
  }
  release(&sc->resource_lock);

  if(buf)
    kfree(buf);
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;
  int spec = spec_current();

  if(spec != 0)
    return spec_console_write(spec, buf, n);

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}

