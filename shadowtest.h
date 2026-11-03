// Test-only operations, available only in make SHADOW_TEST=1 builds.
#define ST_WRITE     1
#define ST_READ      2
#define ST_COPY      3
#define ST_COUNT     4
#define ST_ORIGINAL  5
#define ST_PAGES     6
#define ST_FAIL      7
#define ST_HOLD      8
#define ST_WAITERS   9
#define ST_PUBWRITES 10  // log_write() calls made by speculative processes
#define ST_PUBHOLD   11  // value 1: pause commit before the point of no return;
                         // 2: after the first public block; 0: release
#define ST_PUBHELD   12  // which hold point a committer is paused at (0 none)
#define ST_CRASH     13  // value N: halt at crash point N of the next
                         // speculative publication (SPC_* in defs.h)
#define ST_ABSENT  256
