struct buf {
  int flags;
  uint dev;
  uint blockno;
  int spec;           // nonzero only for an ephemeral private filesystem buffer
  uint inum;          // owner for private data; SH_META for metadata
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  uchar data[BSIZE];
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

