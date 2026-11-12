struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct rtcdate;
struct spinlock;
struct sleeplock;
struct spec_context;
struct stat;
struct superblock;
struct shadow_save;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
struct buf*     bread_original(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
struct buf*     fs_bread(uint, uint);
void            fs_brelse(struct buf*);
int             fs_log_write(struct buf*);
void            fs_enter(void);
void            fsviewinit(void);
void            fs_leave(void);
int             fs_failed(void);
void            fs_fail(void);
void            inode_invalidate(void);
int             file_spec_busy(struct inode*);
void            fs_finish(struct spec_context*, int);
int             fs_syscall(int, int (*)(void));

// console.c
void            consoleinit(void);
void            cprintf(char*, ...);
void            consoleintr(int(*)(void));
void            panic(char*) __attribute__((noreturn));
int             spec_console_write(int, char*, int);
int             console_ask(int, char*, char*);
void            spec_console_take(struct spec_context*, char**, uint*);
void            spec_console_emit(char*, uint);
void            spec_console_discard(struct spec_context*);

// exec.c
int             exec(char*, char**);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, char*, int n);
int             filestat(struct file*, struct stat*);
int             filewrite(struct file*, char*, int n);

// fs.c
void            readsb(int dev, struct superblock *sb);
int             dirlink(struct inode*, char*, uint);
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   ialloc(uint, short);
struct inode*   idup(struct inode*);
void            iinit(int dev);
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, char*, uint, uint);
struct inode*   iget_ref(uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, char*, uint, uint);

// ide.c
void            ideinit(void);
void            ideintr(void);
void            iderw(struct buf*);

// ioapic.c
void            ioapicenable(int irq, int cpu);
extern uchar    ioapicid;
void            ioapicinit(void);

// kalloc.c
char*           kalloc(void);
void            kfree(char*);
void            kinit1(void*, void*);
void            kinit2(void*, void*);

// kbd.c
void            kbdintr(void);

// lapic.c
void            cmostime(struct rtcdate *r);
int             lapicid(void);
extern volatile uint*    lapic;
void            lapiceoi(void);
void            lapicinit(void);
void            lapicstartap(uchar, uint);
void            microdelay(int);

// log.c
void            initlog(int dev);
void            log_write(struct buf*);
void            begin_op();
void            begin_op_exclusive(void);
void            end_op();

// mp.c
extern int      ismp;
void            mpinit(void);

// picirq.c
void            picenable(int);
void            picinit(void);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, char*, int);
int             pipewrite(struct pipe*, char*, int);
int             pipe_has_external_reader(struct pipe*, int);
void            spec_pipe_take(struct spec_context*, char**, uint*);
void            spec_pipe_deliver(char*, uint);
void            spec_pipe_discard(struct spec_context*);

//PAGEBREAK: 16
// proc.c
int             cpuid(void);
void            exit(void);
int             fork(void);
int             growproc(int);
int             kill(int);
int             spec_begin(void);
int             spec_commit(void);
int             spec_abort(void);
void            spec_abort_only(void);
int             spec_ask(char*);
int             spec_frozen_allows(int);
struct spec_context* spec_pin(int);
struct spec_context* spec_pin_cleanup(int);
void            spec_unpin(struct spec_context*);
int             spec_wait(struct spec_context*, void*, struct spinlock*);
int             spec_current(void);
struct cpu*     mycpu(void);
struct proc*    myproc();
void            pinit(void);
void            procdump(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            setproc(struct proc*);
void            sleep(void*, struct spinlock*);
void            userinit(void);
int             wait(void);
void            wakeup(void*);
void            yield(void);

// shadow.c: read/write take kernel or argptr-validated user addresses; the
// caller holds no cache buffer or spinlock (an inode sleeplock is allowed).
void            shadowinit(void);
int             shadow_read(int, uint, uint, uint, void*, uint);
int             shadow_write(int, uint, uint, uint, uint, const void*, uint);
int             shadow_block_busy(uint, uint);
int             shadow_inode_busy(uint, uint);
// shadow_publish() results (Step 5 commit)
#define SP_OK          0   // published durably (or nothing to publish)
#define SP_UNSUPPORTED (-1) // private state that commit cannot publish
#define SP_CONFLICT    (-2) // D24: a public block changed since it was copied
#define SP_KILLED      (-3) // D23: owner killed before the point of no return
int             shadow_publish(struct spec_context*);
int             shadow_report(struct spec_context*, char*, int);
int             shadow_count(struct spec_context*);
void            shadow_discard(struct spec_context*);
#define SH_META 0xffffffffU
struct shadow_save* shadow_savepoint(void);
void            shadow_restore(struct shadow_save*);
void            shadow_save_free(struct shadow_save*);
int             shadow_inode_new(struct spec_context*, uint);
#ifdef SHADOW_TEST
int             shadow_test(int, int, int);
void            shadow_note_public_write(void);
void            shadow_crash_point(int);
#define SPC_BEFORE_LOG   1  // commit(): nothing written to the log yet
#define SPC_BEFORE_HEAD  2  // log blocks written, commit record not yet
#define SPC_AFTER_HEAD   3  // commit record written, not installed
#define SPC_MID_INSTALL  4  // first of several blocks installed home
// Step 10: output-release window (after the durable point).
#define SPC_AFTER_DURABLE 5 // filesystem durable, no held output released
#define SPC_MID_PIPE      6 // first held pipe record delivered, rest not
#define SPC_BEFORE_CONS   7 // pipe output released, console not yet
#define SPC_MID_CONS      8 // first 16 held console bytes emitted, rest not
int             shadow_test_fail(int);
int             kfreepages(void);
#endif

// swtch.S
void            swtch(struct context**, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
void            getcallerpcs(void*, uint*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            pushcli(void);
void            popcli(void);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);

// syscall.c
int             argint(int, int*);
int             argptr(int, char**, int);
int             argstr(int, char**);
int             fetchint(uint, int*);
int             fetchstr(uint, char**);
void            syscall(void);

// timer.c
void            timerinit(void);

// trap.c
void            idtinit(void);
extern uint     ticks;
void            tvinit(void);
extern struct spinlock tickslock;

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartputc(int);

// vm.c
void            seginit(void);
void            kvmalloc(void);
pde_t*          setupkvm(void);
char*           uva2ka(pde_t*, char*);
int             allocuvm(pde_t*, uint, uint);
int             deallocuvm(pde_t*, uint, uint);
void            freevm(pde_t*);
void            inituvm(pde_t*, char*, uint);
int             loaduvm(pde_t*, char*, struct inode*, uint, uint);
pde_t*          copyuvm(pde_t*, uint);
void            switchuvm(struct proc*);
void            switchkvm(void);
int             copyout(pde_t*, uint, void*, uint);
void            clearpteu(pde_t *pgdir, char *uva);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
