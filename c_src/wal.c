/************ Begin file wal.c *********************************************/
/*
** 2010 February 1
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains the implementation of a write-ahead log (WAL) used in 
** "journal_mode=WAL" mode.
**
** WRITE-AHEAD LOG (WAL) FILE FORMAT
**
** A WAL file consists of a header followed by zero or more "frames".
** Each frame records the revised content of a single page from the
** database file.  All changes to the database are recorded by writing
** frames into the WAL.  Transactions commit when a frame is written that
** contains a commit marker.  A single WAL can and usually does record 
** multiple transactions.  Periodically, the content of the WAL is
** transferred back into the database file in an operation called a
** "checkpoint".
**
** A single WAL file can be used multiple times.  In other words, the
** WAL can fill up with frames and then be checkpointed and then new
** frames can overwrite the old ones.  A WAL always grows from beginning
** toward the end.  Checksums and counters attached to each frame are
** used to determine which frames within the WAL are valid and which
** are leftovers from prior checkpoints.
**
** The WAL header is 32 bytes in size and consists of the following eight
** big-endian 32-bit unsigned integer values:
**
**     0: Magic number.  0x377f0682 or 0x377f0683
**     4: File format version.  Currently 3007000
**     8: Database page size.  Example: 1024
**    12: Checkpoint sequence number
**    20: Salt-1, random integer incremented with each checkpoint
**    24: Salt-2, a different random integer changing with each ckpt
**    28: 0
**    32: Checksum-1 (first part of checksum for first 24 bytes of header).
**    36: Checksum-2 (second part of checksum for first 24 bytes of header).
**
** Immediately following the wal-header are zero or more frames. Each
** frame consists of a 24-byte frame-header followed by a <page-size> bytes
** of page data. The frame-header is six big-endian 32-bit unsigned 
** integer values, as follows:
**
**     0: Page number.
**     4: For commit records, the size of the database image in pages 
**        after the commit. For all other records, zero.
**     8: Salt-1 (copied from the header)
**    12: Salt-2 (copied from the header)
**    16: Checksum-1.
**    20: Checksum-2.
**
** A frame is considered valid if and only if the following conditions are
** true:
**
**    (1) The salt-1 and salt-2 values in the frame-header match
**        salt values in the wal-header
**
**    (2) The checksum values in the final 8 bytes of the frame-header
**        exactly match the checksum computed consecutively on the
**        WAL header and the first 8 bytes and the content of all frames
**        up to and including the current frame.
**
** The checksum is computed using 32-bit big-endian integers if the
** magic number in the first 4 bytes of the WAL is 0x377f0683 and it
** is computed using little-endian if the magic number is 0x377f0682.
** The checksum values are always stored in the frame header in a
** big-endian format regardless of which byte order is used to compute
** the checksum.  The checksum is computed by interpreting the input as
** an even number of unsigned 32-bit integers: x[0] through x[N].  The
** algorithm used for the checksum is as follows:
** 
**   for i from 0 to n-1 step 2:
**     s0 += x[i] + s1;
**     s1 += x[i+1] + s0;
**   endfor
**
** Note that s0 and s1 are both weighted checksums using fibonacci weights
** in reverse order (the largest fibonacci weight occurs on the first element
** of the sequence being summed.)  The s1 value spans all 32-bit 
** terms of the sequence whereas s0 omits the final term.
**
** On a checkpoint, the WAL is first VFS.xSync-ed, then valid content of the
** WAL is transferred into the database, then the database is VFS.xSync-ed.
** The VFS.xSync operations serve as write barriers - all writes launched
** before the xSync must complete before any write that launches after the
** xSync begins.
**
** After each checkpoint, the salt-1 value is incremented and the salt-2
** value is randomized.  This prevents old and new frames in the WAL from
** being considered valid at the same time and being checkpointing together
** following a crash.
**
** READER ALGORITHM
**
** To read a page from the database (call it page number P), a reader
** first checks the WAL to see if it contains page P.  If so, then the
** last valid instance of page P that is a followed by a commit frame
** or is a commit frame itself becomes the value read.  If the WAL
** contains no copies of page P that are valid and which are a commit
** frame or are followed by a commit frame, then page P is read from
** the database file.
**
** To start a read transaction, the reader records the index of the last
** valid frame in the WAL.  The reader uses this recorded "mxFrame" value
** for all subsequent read operations.  New transactions can be appended
** to the WAL, but as long as the reader uses its original mxFrame value
** and ignores the newly appended content, it will see a consistent snapshot
** of the database from a single point in time.  This technique allows
** multiple concurrent readers to view different versions of the database
** content simultaneously.
**
** The reader algorithm in the previous paragraphs works correctly, but 
** because frames for page P can appear anywhere within the WAL, the
** reader has to scan the entire WAL looking for page P frames.  If the
** WAL is large (multiple megabytes is typical) that scan can be slow,
** and read performance suffers.  To overcome this problem, a separate
** data structure called the wal-index is maintained to expedite the
** search for frames of a particular page.
** 
** WAL-INDEX FORMAT
**
** Conceptually, the wal-index is shared memory, though VFS implementations
** might choose to implement the wal-index using a mmapped file.  Because
** the wal-index is shared memory, SQLite does not support journal_mode=WAL 
** on a network filesystem.  All users of the database must be able to
** share memory.
**
** The wal-index is transient.  After a crash, the wal-index can (and should
** be) reconstructed from the original WAL file.  In fact, the VFS is required
** to either truncate or zero the header of the wal-index when the last
** connection to it closes.  Because the wal-index is transient, it can
** use an architecture-specific format; it does not have to be cross-platform.
** Hence, unlike the database and WAL file formats which store all values
** as big endian, the wal-index can store multi-byte values in the native
** byte order of the host computer.
**
** The purpose of the wal-index is to answer this question quickly:  Given
** a page number P and a maximum frame index M, return the index of the 
** last frame in the wal before frame M for page P in the WAL, or return
** NULL if there are no frames for page P in the WAL prior to M.
**
** The wal-index consists of a header region, followed by an one or
** more index blocks.  
**
** The wal-index header contains the total number of frames within the WAL
** in the mxFrame field.
**
** Each index block except for the first contains information on 
** HASHTABLE_NPAGE frames. The first index block contains information on
** HASHTABLE_NPAGE_ONE frames. The values of HASHTABLE_NPAGE_ONE and 
** HASHTABLE_NPAGE are selected so that together the wal-index header and
** first index block are the same size as all other index blocks in the
** wal-index.
**
** Each index block contains two sections, a page-mapping that contains the
** database page number associated with each wal frame, and a hash-table 
** that allows readers to query an index block for a specific page number.
** The page-mapping is an array of HASHTABLE_NPAGE (or HASHTABLE_NPAGE_ONE
** for the first index block) 32-bit page numbers. The first entry in the 
** first index-block contains the database page number corresponding to the
** first frame in the WAL file. The first entry in the second index block
** in the WAL file corresponds to the (HASHTABLE_NPAGE_ONE+1)th frame in
** the log, and so on.
**
** The last index block in a wal-index usually contains less than the full
** complement of HASHTABLE_NPAGE (or HASHTABLE_NPAGE_ONE) page-numbers,
** depending on the contents of the WAL file. This does not change the
** allocated size of the page-mapping array - the page-mapping array merely
** contains unused entries.
**
** Even without using the hash table, the last frame for page P
** can be found by scanning the page-mapping sections of each index block
** starting with the last index block and moving toward the first, and
** within each index block, starting at the end and moving toward the
** beginning.  The first entry that equals P corresponds to the frame
** holding the content for that page.
**
** The hash table consists of HASHTABLE_NSLOT 16-bit unsigned integers.
** HASHTABLE_NSLOT = 2*HASHTABLE_NPAGE, and there is one entry in the
** hash table for each page number in the mapping section, so the hash 
** table is never more than half full.  The expected number of collisions 
** prior to finding a match is 1.  Each entry of the hash table is an
** 1-based index of an entry in the mapping section of the same
** index block.   Let K be the 1-based index of the largest entry in
** the mapping section.  (For index blocks other than the last, K will
** always be exactly HASHTABLE_NPAGE (4096) and for the last index block
** K will be (mxFrame%HASHTABLE_NPAGE).)  Unused slots of the hash table
** contain a value of 0.
**
** To look for page P in the hash table, first compute a hash iKey on
** P as follows:
**
**      iKey = (P * 383) % HASHTABLE_NSLOT
**
** Then start scanning entries of the hash table, starting with iKey
** (wrapping around to the beginning when the end of the hash table is
** reached) until an unused hash slot is found. Let the first unused slot
** be at index iUnused.  (iUnused might be less than iKey if there was
** wrap-around.) Because the hash table is never more than half full,
** the search is guaranteed to eventually hit an unused entry.  Let 
** iMax be the value between iKey and iUnused, closest to iUnused,
** where aHash[iMax]==P.  If there is no iMax entry (if there exists
** no hash slot such that aHash[i]==p) then page P is not in the
** current index block.  Otherwise the iMax-th mapping entry of the
** current index block corresponds to the last entry that references 
** page P.
**
** A hash search begins with the last index block and moves toward the
** first index block, looking for entries corresponding to page P.  On
** average, only two or three slots in each index block need to be
** examined in order to either find the last entry for page P, or to
** establish that no such entry exists in the block.  Each index block
** holds over 4000 entries.  So two or three index blocks are sufficient
** to cover a typical 10 megabyte WAL file, assuming 1K pages.  8 or 10
** comparisons (on average) suffice to either locate a frame in the
** WAL or to establish that the frame does not exist in the WAL.  This
** is much faster than scanning the entire 10MB WAL.
**
** Note that entries are added in order of increasing K.  Hence, one
** reader might be using some value K0 and a second reader that started
** at a later time (after additional transactions were added to the WAL
** and to the wal-index) might be using a different value K1, where K1>K0.
** Both readers can use the same hash table and mapping section to get
** the correct result.  There may be entries in the hash table with
** K>K0 but to the first reader, those entries will appear to be unused
** slots in the hash table and so the first reader will get an answer as
** if no values greater than K0 had ever been inserted into the hash table
** in the first place - which is what reader one wants.  Meanwhile, the
** second reader using K1 will see additional values that were inserted
** later, which is exactly what reader two wants.  
**
** When a rollback occurs, the value of K is decreased. Hash table entries
** that correspond to frames greater than the new K value are removed
** from the hash table at this point.
*/
#ifndef SQLITE_OMIT_WAL


/*
** Trace output macros
*/
#if defined(SQLITE_TEST) && defined(SQLITE_DEBUG)
SQLITE_PRIVATE int sqlite3WalTrace = 0;
# define WALTRACE(X)  if(sqlite3WalTrace) sqlite3DebugPrintf X
#else
# define WALTRACE(X)
#endif

#if defined(_TESTDBG_)
# define DBG(X)  printf X
#else
# define DBG(X)
#endif

/*
** The maximum (and only) versions of the wal and wal-index formats
** that may be interpreted by this version of SQLite.
**
** If a client begins recovering a WAL file and finds that (a) the checksum
** values in the wal-header are correct and (b) the version field is not
** WAL_MAX_VERSION, recovery fails and SQLite returns SQLITE_CANTOPEN.
**
** Similarly, if a client successfully reads a wal-index header (i.e. the 
** checksum test is successful) and finds that the version field is not
** WALINDEX_MAX_VERSION, then no read-transaction is opened and SQLite
** returns SQLITE_CANTOPEN.
*/
#define WAL_MAX_VERSION      13007000
#define WALINDEX_MAX_VERSION 13007000

/*
** Indices of various locking bytes.   WAL_NREADER is the number
** of available reader locks and should be at least 3.
*/
#define WAL_WRITE_LOCK         0
#define WAL_ALL_BUT_WRITE      1
#define WAL_CKPT_LOCK          1
#define WAL_RECOVER_LOCK       2
#define WAL_READ_LOCK(I)       (3+(I))
#define WAL_NREADER            (SQLITE_SHM_NLOCK-3)



/*
** A copy of the following object occurs in the wal-index immediately
** following the second copy of the WalIndexHdr.  This object stores
** information used by checkpoint.
**
** nBackfill is the number of frames in the WAL that have been written
** back into the database. (We call the act of moving content from WAL to
** database "backfilling".)  The nBackfill number is never greater than
** WalIndexHdr.mxFrame.  nBackfill can only be increased by threads
** holding the WAL_CKPT_LOCK lock (which includes a recovery thread).
** However, a WAL_WRITE_LOCK thread can move the value of nBackfill from
** mxFrame back to zero when the WAL is reset.
**
** There is one entry in aReadMark[] for each reader lock.  If a reader
** holds read-lock K, then the value in aReadMark[K] is no greater than
** the mxFrame for that reader.  The value READMARK_NOT_USED (0xffffffff)
** for any aReadMark[] means that entry is unused.  aReadMark[0] is 
** a special case; its value is never used and it exists as a place-holder
** to avoid having to offset aReadMark[] indexs by one.  Readers holding
** WAL_READ_LOCK(0) always ignore the entire WAL and read all content
** directly from the database.
**
** The value of aReadMark[K] may only be changed by a thread that
** is holding an exclusive lock on WAL_READ_LOCK(K).  Thus, the value of
** aReadMark[K] cannot changed while there is a reader is using that mark
** since the reader will be holding a shared lock on WAL_READ_LOCK(K).
**
** The checkpointer may only transfer frames from WAL to database where
** the frame numbers are less than or equal to every aReadMark[] that is
** in use (that is, every aReadMark[j] for which there is a corresponding
** WAL_READ_LOCK(j)).  New readers (usually) pick the aReadMark[] with the
** largest value and will increase an unused aReadMark[] to mxFrame if there
** is not already an aReadMark[] equal to mxFrame.  The exception to the
** previous sentence is when nBackfill equals mxFrame (meaning that everything
** in the WAL has been backfilled into the database) then new readers
** will choose aReadMark[0] which has value 0 and hence such reader will
** get all their all content directly from the database file and ignore 
** the WAL.
**
** Writers normally append new frames to the end of the WAL.  However,
** if nBackfill equals mxFrame (meaning that all WAL content has been
** written back into the database) and if no readers are using the WAL
** (in other words, if there are no WAL_READ_LOCK(i) where i>0) then
** the writer will first "reset" the WAL back to the beginning and start
** writing new content beginning at frame 1.
**
** We assume that 32-bit loads are atomic and so no locks are needed in
** order to read from any aReadMark[] entries.
*/

#define READMARK_NOT_USED  0xffffffff


/* A block of WALINDEX_LOCK_RESERVED bytes beginning at
** WALINDEX_LOCK_OFFSET is reserved for locks. Since some systems
** only support mandatory file-locks, we do not read or write data
** from the region of the file on which locks are applied.
*/
#define WALINDEX_LOCK_OFFSET   (sizeof(WalIndexHdr)*2 + sizeof(WalCkptInfo))
#define WALINDEX_LOCK_RESERVED 16
#define WALINDEX_HDR_SIZE      (WALINDEX_LOCK_OFFSET+WALINDEX_LOCK_RESERVED)

/* Size of header before each frame in wal */
#define WAL_FRAME_HDRSIZE 144

/* Size of write ahead log header, including checksum. */
/* #define WAL_HDRSIZE 24 */
#define WAL_HDRSIZE 40

/* WAL magic value. Either this value, or the same value with the least
** significant bit also set (WAL_MAGIC | 0x00000001) is stored in 32-bit
** big-endian format in the first 4 bytes of a WAL file.
**
** If the LSB is set, then the checksums for each frame within the WAL
** file are calculated by treating all data as an array of 32-bit 
** big-endian words. Otherwise, they are calculated by interpreting 
** all data as 32-bit little-endian words.
*/
#define WAL_MAGIC 0x377f0682

/*
** Return the offset of frame iFrame in the write-ahead log file, 
** assuming a database page size of szPage bytes. The offset returned
** is to the start of the write-ahead log frame-header.
*/
#define walFrameOffset(iFrame, szPage) (                               \
  WAL_HDRSIZE + ((iFrame)-1)*(i64)((szPage)+WAL_FRAME_HDRSIZE)         \
)


/*
** Candidate values for Wal.exclusiveMode.
*/
#define WAL_NORMAL_MODE     0
#define WAL_EXCLUSIVE_MODE  1     
#define WAL_HEAPMEMORY_MODE 2

/*
** Possible values for WAL.readOnly
*/
#define WAL_RDWR        0    /* Normal read/write connection */
#define WAL_RDONLY      1    /* The WAL file is readonly */
#define WAL_SHM_RDONLY  2    /* The SHM file is readonly */

/*
** Each page of the wal-index mapping contains a hash-table made up of
** an array of HASHTABLE_NSLOT elements of the following type.
*/
typedef u16 ht_slot;

/*
** This structure is used to implement an iterator that loops through
** all frames in the WAL in database page order. Where two or more frames
** correspond to the same database page, the iterator visits only the 
** frame most recently written to the WAL (in other words, the frame with
** the largest index).
**
** The internals of this structure are only accessed by:
**
**   walIteratorInit() - Create a new iterator,
**   walIteratorNext() - Step an iterator,
**   walIteratorFree() - Free an iterator.
**
** This functionality is used by the checkpoint code (see walCheckpoint()).
*/
struct WalIterator {
  int iPrior;                     /* Last result returned from the iterator */
  int nSegment;                   /* Number of entries in aSegment[] */
  struct WalSegment {
    int iNext;                    /* Next slot in aIndex[] not yet returned */
    ht_slot *aIndex;              /* i0, i1, i2... such that aPgno[iN] ascend */
    u32 *aPgno;                   /* Array of page numbers. */
    int nEntry;                   /* Nr. of entries in aPgno[] and aIndex[] */
    int iZero;                    /* Frame number associated with aPgno[0] */
  } aSegment[1];                  /* One for every 32KB page in the wal-index */
};

/*
** Define the parameters of the hash tables in the wal-index file. There
** is a hash-table following every HASHTABLE_NPAGE page numbers in the
** wal-index.
**
** Changing any of these constants will alter the wal-index format and
** create incompatibilities.
*/
#define HASHTABLE_NPAGE      4096                 /* Must be power of 2 */
#define HASHTABLE_HASH_1     383                  /* Should be prime */
#define HASHTABLE_NSLOT      (HASHTABLE_NPAGE*2)  /* Must be a power of 2 */

/* 
** The block of page numbers associated with the first hash-table in a
** wal-index is smaller than usual. This is so that there is a complete
** hash-table on each aligned 32KB page of the wal-index.
*/
#define HASHTABLE_NPAGE_ONE  (HASHTABLE_NPAGE - (WALINDEX_HDR_SIZE/sizeof(u32)))

/* The wal-index is divided into pages of WALINDEX_PGSZ bytes each. */
#define WALINDEX_PGSZ   (sizeof(ht_slot)*HASHTABLE_NSLOT + HASHTABLE_NPAGE*sizeof(u32) )

/*
** Obtain a pointer to the iPage'th page of the wal-index. The wal-index
** is broken into pages of WALINDEX_PGSZ bytes. Wal-index pages are
** numbered from zero.
**
** If this call is successful, *ppPage is set to point to the wal-index
** page and SQLITE_OK is returned. If an error (an OOM or VFS error) occurs,
** then an SQLite error code is returned and *ppPage is set to 0.
*/
static int walIndexPage(Wal *pWal, int iPage, u32 **ppPage){
  int rc = SQLITE_OK;

  /* Enlarge the pWal->apWiData[] array if required */
  if( pWal->nWiData<=iPage ){
    int nByte = sizeof(u32*)*(iPage+1);
    u32 **apNew;
    apNew = (u32 **)sqlite3_realloc((void *)pWal->apWiData, nByte);
    if( !apNew ){
      *ppPage = 0;
      return SQLITE_NOMEM;
    }
    memset((void*)&apNew[pWal->nWiData], 0,
           sizeof(u32*)*(iPage+1-pWal->nWiData));
    pWal->apWiData = apNew;
    pWal->nWiData = iPage+1;
  }

  /* Request a pointer to the required page from the VFS */
  if( pWal->apWiData[iPage]==0 ){
    // if( pWal->exclusiveMode==WAL_HEAPMEMORY_MODE ){
      pWal->apWiData[iPage] = (u32 *)sqlite3MallocZero(WALINDEX_PGSZ);
      if( !pWal->apWiData[iPage] ) rc = SQLITE_NOMEM;
    // }
    // else{
    //   rc = sqlite3OsShmMap(pWal->pDbFd, iPage, WALINDEX_PGSZ, 
    //       pWal->writeLock, (void **)&pWal->apWiData[iPage]
    //   );
    //   if( rc==SQLITE_READONLY ){
    //     pWal->readOnly |= WAL_SHM_RDONLY;
    //     rc = SQLITE_OK;
    //   }
    // }
  }

  *ppPage = pWal->apWiData[iPage];
  assert( iPage==0 || *ppPage || rc!=SQLITE_OK );
  return rc;
}

/*
** Return a pointer to the WalCkptInfo structure in the wal-index.
*/
static WalCkptInfo *walCkptInfo(Wal *pWal){
  assert( pWal->nWiData>0 && pWal->apWiData[0] );
  return (WalCkptInfo*)&(pWal->apWiData[0][sizeof(WalIndexHdr)/2]);
}

/*
** Return a pointer to the WalIndexHdr structure in the wal-index.
*/
static WalIndexHdr *walIndexHdr(Wal *pWal){
  assert( pWal->nWiData>0 && pWal->apWiData[0] );
  return (WalIndexHdr*)pWal->apWiData[0];
}

/*
** The argument to this macro must be of type u32. On a little-endian
** architecture, it returns the u32 value that results from interpreting
** the 4 bytes as a big-endian value. On a big-endian architecture, it
** returns the value that would be produced by interpreting the 4 bytes
** of the input value as a little-endian integer.
*/
#define BYTESWAP32(x) ( \
    (((x)&0x000000FF)<<24) + (((x)&0x0000FF00)<<8)  \
  + (((x)&0x00FF0000)>>8)  + (((x)&0xFF000000)>>24) \
)

/*
** Generate or extend an 8 byte checksum based on the data in 
** array aByte[] and the initial values of aIn[0] and aIn[1] (or
** initial values of 0 and 0 if aIn==NULL).
**
** The checksum is written back into aOut[] before returning.
**
** nByte must be a positive multiple of 8.
*/
static void walChecksumBytes(
  int nativeCksum, /* True for native byte-order, false for non-native */
  u8 *a,           /* Content to be checksummed */
  int nByte,       /* Bytes of content in a[].  Must be a multiple of 8. */
  const u32 *aIn,  /* Initial checksum value input */
  u32 *aOut        /* OUT: Final checksum value output */
){
  u32 s1, s2;
  u32 *aData = (u32 *)a;
  u32 *aEnd = (u32 *)&a[nByte];

  if( aIn ){
    s1 = aIn[0];
    s2 = aIn[1];
  }else{
    s1 = s2 = 0;
  }

  assert( nByte>=8 );
  assert( (nByte&0x00000007)==0 );

  if( nativeCksum ){
    do {
      s1 += *aData++ + s2;
      s2 += *aData++ + s1;
    }while( aData<aEnd );
  }else{
    do {
      s1 += BYTESWAP32(aData[0]) + s2;
      s2 += BYTESWAP32(aData[1]) + s1;
      aData += 2;
    }while( aData<aEnd );
  }

  aOut[0] = s1;
  aOut[1] = s2;
}

static void walShmBarrier(Wal *pWal){
  if( pWal->exclusiveMode!=WAL_HEAPMEMORY_MODE ){
    sqlite3OsShmBarrier(pWal->pDbFd);
  }
}

/*
** Write the header information in pWal->hdr into the wal-index.
**
** The checksum on pWal->hdr is updated before it is written.
*/
static void walIndexWriteHdr(Wal *pWal){
  WalIndexHdr *aHdr = walIndexHdr(pWal);
  const int nCksum = offsetof(WalIndexHdr, aCksum);

  assert( pWal->writeLock );
  pWal->hdr.isInit = 1;
  pWal->hdr.iVersion = WALINDEX_MAX_VERSION;
  walChecksumBytes(1, (u8*)&pWal->hdr, nCksum, 0, pWal->hdr.aCksum);
  memcpy((void *)&aHdr[1], (void *)&pWal->hdr, sizeof(WalIndexHdr));
  walShmBarrier(pWal);
  memcpy((void *)&aHdr[0], (void *)&pWal->hdr, sizeof(WalIndexHdr));
}

/*
** ActorDB modification - added more data (actor index, actor path, writeNumber, writeTermNumber)
** Also checksum is not cummulative from beginning of wal. 

** This function encodes a single frame header and writes it to a buffer
** supplied by the caller. A frame-header is made up of a series of 
** 4-byte big-endian integers, as follows:
**
**     0: Page number.
**     4: For commit records, the size of the database image in pages 
**        after the commit. For all other records, zero.
**     8: writeNumber (custom)
**    16: writeTermNumber (custom)
**    24: actor index (actor index in thread)
**    28: Actorname
**    128: Salt-1 (copied from the wal-header)
**    132: Salt-2 (copied from the wal-header)
**    136: Checksum-1.
**    140: Checksum-2.
*/
static void walEncodeFrame(
  Wal *pWal,                      /* The write-ahead log */
  u32 iPage,                      /* Database page number for frame */
  u32 nTruncate,                  /* New db size (or 0 for non-commit frames) */
  u8 *aData,                      /* Pointer to page data */
  u8 *aFrame                      /* OUT: Write encoded frame here */
){
  int nativeCksum;                /* True for native byte-order checksums */
  u32 aCksum[2] = {0, 0};
  db_connection *conn = NULL;

  if (pWal->thread)
    conn = pWal->thread->curConn;
  
  sqlite3Put4byte(&aFrame[0], iPage);
  sqlite3Put4byte(&aFrame[4], nTruncate);
  if (conn)
  {
    writeUInt64(&aFrame[8], conn->writeNumber);
    writeUInt64(&aFrame[16], conn->writeTermNumber);
    sqlite3Put4byte(&aFrame[24], conn->connindex);
    memcpy((char*)&aFrame[28],conn->dbpath,100);
  }
  else
  {
    memset((void *)&aFrame[8], 0, 120);
  }
  // DBG(("Writing %d npage %d, writing truncate %d\r\n",pWal->hdr.bigEndCksum,iPage,nTruncate));
  // DBG(("Writing filename %s, actorindex %d, wn %llu, wtn %llu\r\n",conn->dbpath,conn->connindex,conn->writeNumber,conn->writeTermNumber));
  // DBG(("Writing salt %d %d\r\n",pWal->hdr.aSalt[0],pWal->hdr.aSalt[1]));
  memcpy(&aFrame[128], pWal->hdr.aSalt, 8);

  nativeCksum = (pWal->hdr.bigEndCksum==SQLITE_BIGENDIAN);
  walChecksumBytes(nativeCksum, aFrame, 136, aCksum, aCksum);
  walChecksumBytes(nativeCksum, aData, pWal->szPage, aCksum, aCksum);

  sqlite3Put4byte(&aFrame[136], aCksum[0]);
  sqlite3Put4byte(&aFrame[140], aCksum[1]);
}

/*
** Check to see if the frame with header in aFrame[] and content
** in aData[] is valid.  If it is a valid frame, fill *piPage and
** *pnTruncate and return true.  Return if the frame is not valid.
*/
static int walDecodeFrame(
  wal_file *pWal,                      /* The write-ahead log */
  u32 *piPage,                    /* OUT: Database page number for frame */
  u32 *pnTruncate,                /* OUT: New db size (or 0 if not commit) */
  char *filename,                 /* OUT: to pre allocated buffer filename in wal */
  u32  *actorIndex,               /* OUT: actor index */
  u64  *writeNumber,              /* OUT: writeNumber */
  u64  *writeTermNumber,          /* OUT: writeTermNumber */
  u8 *aData,                      /* Pointer to page data (for checksum) */
  u8 *aFrame                      /* Frame data */
){
  int nativeCksum;                /* True for native byte-order checksums */
  u32 aCksum[2] = {0, 0};
  u32 pgno;                       /* Page number of the frame */

  /* A frame is only valid if the salt values in the frame-header
  ** match the salt values in the wal-header. 
  */
  if( memcmp(&pWal->aSalt, &aFrame[128], 8)!=0 ){
  	// DBG(("Salt does not match %d %d, %d %d\r\n",pWal->aSalt[0],pWal->aSalt[1],(int)aFrame[128],(int)aFrame[132]));
    return 0;
  }

  /* A frame is only valid if the page number is creater than zero.
  */
  pgno = sqlite3Get4byte(&aFrame[0]);
  if( pgno==0 ){
  	// DBG(("Page number zero\r\n"));
    return 0;
  } 

  /* A frame is only valid if a checksum of the WAL header,
  ** all prior frams, the first 16 bytes of this frame-header, 
  ** and the frame-data matches the checksum in the last 8 
  ** bytes of this frame-header.
  */
  nativeCksum = (pWal->bigEndCksum==SQLITE_BIGENDIAN);
  // walChecksumBytes(nativeCksum, aFrame, 24, aCksum, aCksum);
  // walChecksumBytes(nativeCksum, aData, pWal->szPage, aCksum, aCksum);
  walChecksumBytes(nativeCksum, aFrame, 136, aCksum, aCksum);
  walChecksumBytes(nativeCksum, aData, pWal->szPage, aCksum, aCksum);
  if( aCksum[0]!=sqlite3Get4byte(&aFrame[136]) 
   || aCksum[1]!=sqlite3Get4byte(&aFrame[140]) 
  ){
    /* Checksum failed. */
	*writeNumber = readUInt64(&aFrame[8]);
	*writeTermNumber = readUInt64(&aFrame[16]);
	*actorIndex = sqlite3Get4byte(&aFrame[24]);
	*piPage = pgno;
  	*pnTruncate = sqlite3Get4byte(&aFrame[4]);
	if (filename != NULL)
		memcpy(filename,&aFrame[28],100);
	// DBG(("Pagen %d %d, trunc %d\r\n",pWal->bigEndCksum,*piPage,*pnTruncate));
	// DBG(("Actor index %d, wn %llu, wtn %llu\r\n",*actorIndex, *writeNumber, *writeTermNumber));
	// DBG(("NAME %s\r\n",filename));
 //    DBG(("Checksum failed %d %d %d %d\r\n",aCksum[0],aCksum[1],sqlite3Get4byte(&aFrame[136]),sqlite3Get4byte(&aFrame[140])));
    return 0;
  }

  *writeNumber = readUInt64(&aFrame[8]);
  *writeTermNumber = readUInt64(&aFrame[16]);
  *actorIndex = sqlite3Get4byte(&aFrame[24]);
  if (filename != NULL)
  	memcpy(filename,&aFrame[28],100);

  /* If we reach this point, the frame is valid.  Return the page number
  ** and the new database size.
  */
  *piPage = pgno;
  *pnTruncate = sqlite3Get4byte(&aFrame[4]);
  return 1;
}
    

/*
** Set or release locks on the WAL.  Locks are either shared or exclusive.
** A lock cannot be moved directly between shared and exclusive - it must go
** through the unlocked state first.
**
** In locking_mode=EXCLUSIVE, all of these routines become no-ops.
*/
static int walLockShared(Wal *pWal, int lockIdx){
  int rc;
  if( pWal->exclusiveMode ) return SQLITE_OK;
  rc = sqlite3OsShmLock(pWal->pDbFd, lockIdx, 1,
                        SQLITE_SHM_LOCK | SQLITE_SHM_SHARED);
  WALTRACE(("WAL%p: acquire SHARED-%s %s\n", pWal,
            walLockName(lockIdx), rc ? "failed" : "ok"));
  VVA_ONLY( pWal->lockError = (u8)(rc!=SQLITE_OK && rc!=SQLITE_BUSY); )
  return rc;
}
static void walUnlockShared(Wal *pWal, int lockIdx){
  if( pWal->exclusiveMode ) return;
  (void)sqlite3OsShmLock(pWal->pDbFd, lockIdx, 1,
                         SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED);
  WALTRACE(("WAL%p: release SHARED-%s\n", pWal, walLockName(lockIdx)));
}
static int walLockExclusive(Wal *pWal, int lockIdx, int n){
  int rc;
  if( pWal->exclusiveMode ) return SQLITE_OK;
  rc = sqlite3OsShmLock(pWal->pDbFd, lockIdx, n,
                        SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE);
  WALTRACE(("WAL%p: acquire EXCLUSIVE-%s cnt=%d %s\n", pWal,
            walLockName(lockIdx), n, rc ? "failed" : "ok"));
  VVA_ONLY( pWal->lockError = (u8)(rc!=SQLITE_OK && rc!=SQLITE_BUSY); )
  return rc;
}
static void walUnlockExclusive(Wal *pWal, int lockIdx, int n){
  if( pWal->exclusiveMode ) return;
  (void)sqlite3OsShmLock(pWal->pDbFd, lockIdx, n,
                         SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE);
  WALTRACE(("WAL%p: release EXCLUSIVE-%s cnt=%d\n", pWal,
             walLockName(lockIdx), n));
}

/*
** Compute a hash on a page number.  The resulting hash value must land
** between 0 and (HASHTABLE_NSLOT-1).  The walHashNext() function advances
** the hash to the next value in the event of a collision.
*/
static int walHash(u32 iPage){
  assert( iPage>0 );
  assert( (HASHTABLE_NSLOT & (HASHTABLE_NSLOT-1))==0 );
  return (iPage*HASHTABLE_HASH_1) & (HASHTABLE_NSLOT-1);
}
static int walNextHash(int iPriorHash){
  return (iPriorHash+1)&(HASHTABLE_NSLOT-1);
}

/* 
** Return pointers to the hash table and page number array stored on
** page iHash of the wal-index. The wal-index is broken into 32KB pages
** numbered starting from 0.
**
** Set output variable *paHash to point to the start of the hash table
** in the wal-index file. Set *piZero to one less than the frame 
** number of the first frame indexed by this hash table. If a
** slot in the hash table is set to N, it refers to frame number 
** (*piZero+N) in the log.
**
** Finally, set *paPgno so that *paPgno[1] is the page number of the
** first frame indexed by the hash table, frame (*piZero+1).
*/
static int walHashGet(
  Wal *pWal,                      /* WAL handle */
  int iHash,                      /* Find the iHash'th table */
  ht_slot **paHash,      /* OUT: Pointer to hash index */
  u32 **paPgno,          /* OUT: Pointer to page number array */
  u32 *piZero                     /* OUT: Frame associated with *paPgno[0] */
){
  int rc;                         /* Return code */
  u32 *aPgno;

  rc = walIndexPage(pWal, iHash, &aPgno);
  assert( rc==SQLITE_OK || iHash>0 );

  if( rc==SQLITE_OK ){
    u32 iZero;
    ht_slot *aHash;

    aHash = (ht_slot *)&aPgno[HASHTABLE_NPAGE];
    if( iHash==0 ){
      aPgno = &aPgno[WALINDEX_HDR_SIZE/sizeof(u32)];
      iZero = 0;
    }else{
      iZero = HASHTABLE_NPAGE_ONE + (iHash-1)*HASHTABLE_NPAGE;
    }
  
    *paPgno = &aPgno[-1];
    *paHash = aHash;
    *piZero = iZero;
  }
  return rc;
}

/*
** Return the number of the wal-index page that contains the hash-table
** and page-number array that contain entries corresponding to WAL frame
** iFrame. The wal-index is broken up into 32KB pages. Wal-index pages 
** are numbered starting from 0.
*/
static int walFramePage(u32 iFrame){
  int iHash = (iFrame+HASHTABLE_NPAGE-HASHTABLE_NPAGE_ONE-1) / HASHTABLE_NPAGE;
  assert( (iHash==0 || iFrame>HASHTABLE_NPAGE_ONE)
       && (iHash>=1 || iFrame<=HASHTABLE_NPAGE_ONE)
       && (iHash<=1 || iFrame>(HASHTABLE_NPAGE_ONE+HASHTABLE_NPAGE))
       && (iHash>=2 || iFrame<=HASHTABLE_NPAGE_ONE+HASHTABLE_NPAGE)
       && (iHash<=2 || iFrame>(HASHTABLE_NPAGE_ONE+2*HASHTABLE_NPAGE))
  );
  return iHash;
}

/*
** Return the page number associated with frame iFrame in this WAL.
*/
static u32 walFramePgno(Wal *pWal, u32 iFrame){
  int iHash = walFramePage(iFrame);
  if( iHash==0 ){
    return pWal->apWiData[0][WALINDEX_HDR_SIZE/sizeof(u32) + iFrame - 1];
  }
  return pWal->apWiData[iHash][(iFrame-1-HASHTABLE_NPAGE_ONE)%HASHTABLE_NPAGE];
}

/*
** Remove entries from the hash table that point to WAL slots greater
** than pWal->hdr.mxFrame.
**
** This function is called whenever pWal->hdr.mxFrame is decreased due
** to a rollback or savepoint.
**
** At most only the hash table containing pWal->hdr.mxFrame needs to be
** updated.  Any later hash tables will be automatically cleared when
** pWal->hdr.mxFrame advances to the point where those hash tables are
** actually needed.
*/
static void walCleanupHash(Wal *pWal){
  ht_slot *aHash = 0;    /* Pointer to hash table to clear */
  u32 *aPgno = 0;        /* Page number array for hash table */
  u32 iZero = 0;                  /* frame == (aHash[x]+iZero) */
  int iLimit = 0;                 /* Zero values greater than this */
  int nByte;                      /* Number of bytes to zero in aPgno[] */
  int i;                          /* Used to iterate through aHash[] */

  assert( pWal->writeLock );
  testcase( pWal->hdr.mxFrame==HASHTABLE_NPAGE_ONE-1 );
  testcase( pWal->hdr.mxFrame==HASHTABLE_NPAGE_ONE );
  testcase( pWal->hdr.mxFrame==HASHTABLE_NPAGE_ONE+1 );

  if( pWal->hdr.mxFrame==0 ) return;

  /* Obtain pointers to the hash-table and page-number array containing 
  ** the entry that corresponds to frame pWal->hdr.mxFrame. It is guaranteed
  ** that the page said hash-table and array reside on is already mapped.
  */
  assert( pWal->nWiData>walFramePage(pWal->hdr.mxFrame) );
  assert( pWal->apWiData[walFramePage(pWal->hdr.mxFrame)] );
  walHashGet(pWal, walFramePage(pWal->hdr.mxFrame), &aHash, &aPgno, &iZero);

  /* Zero all hash-table entries that correspond to frame numbers greater
  ** than pWal->hdr.mxFrame.
  */
  iLimit = pWal->hdr.mxFrame - iZero;
  assert( iLimit>0 );
  for(i=0; i<HASHTABLE_NSLOT; i++){
    if( aHash[i]>iLimit ){
      aHash[i] = 0;
    }
  }
  
  /* Zero the entries in the aPgno array that correspond to frames with
  ** frame numbers greater than pWal->hdr.mxFrame. 
  */
  nByte = (int)((char *)aHash - (char *)&aPgno[iLimit+1]);
  memset((void *)&aPgno[iLimit+1], 0, nByte);

#ifdef SQLITE_ENABLE_EXPENSIVE_ASSERT
  /* Verify that the every entry in the mapping region is still reachable
  ** via the hash table even after the cleanup.
  */
  if( iLimit ){
    int i;           /* Loop counter */
    int iKey;        /* Hash key */
    for(i=1; i<=iLimit; i++){
      for(iKey=walHash(aPgno[i]); aHash[iKey]; iKey=walNextHash(iKey)){
        if( aHash[iKey]==i ) break;
      }
      assert( aHash[iKey]==i );
    }
  }
#endif /* SQLITE_ENABLE_EXPENSIVE_ASSERT */
}


/*
** Set an entry in the wal-index that will map database page number
** pPage into WAL frame iFrame.
*/
static int walIndexAppend(Wal *pWal, u32 iFrame, u32 iPage){
  int rc;                         /* Return code */
  u32 iZero = 0;                  /* One less than frame number of aPgno[1] */
  u32 *aPgno = 0;        /* Page number array */
  ht_slot *aHash = 0;    /* Hash table */

  rc = walHashGet(pWal, walFramePage(iFrame), &aHash, &aPgno, &iZero);

  /* Assuming the wal-index file was successfully mapped, populate the
  ** page number array and hash table entry.
  */
  if( rc==SQLITE_OK ){
    int iKey;                     /* Hash table key */
    int idx;                      /* Value to write to hash-table slot */
    int nCollide;                 /* Number of hash collisions */

    idx = iFrame - iZero;
    assert( idx <= HASHTABLE_NSLOT/2 + 1 );
    
    /* If this is the first entry to be added to this hash-table, zero the
    ** entire hash table and aPgno[] array before proceeding. 
    */
    if( idx==1 ){
      int nByte = (int)((u8 *)&aHash[HASHTABLE_NSLOT] - (u8 *)&aPgno[1]);
      memset((void*)&aPgno[1], 0, nByte);
    }

    /* If the entry in aPgno[] is already set, then the previous writer
    ** must have exited unexpectedly in the middle of a transaction (after
    ** writing one or more dirty pages to the WAL to free up memory). 
    ** Remove the remnants of that writers uncommitted transaction from 
    ** the hash-table before writing any new entries.
    */
    if( aPgno[idx] ){
      walCleanupHash(pWal);
      assert( !aPgno[idx] );
    }

    /* Write the aPgno[] array entry and the hash-table slot. */
    nCollide = idx;
    for(iKey=walHash(iPage); aHash[iKey]; iKey=walNextHash(iKey)){
      if( (nCollide--)==0 ) return SQLITE_CORRUPT_BKPT;
    }
    aPgno[idx] = iPage;
    aHash[iKey] = (ht_slot)idx;

#ifdef SQLITE_ENABLE_EXPENSIVE_ASSERT
    /* Verify that the number of entries in the hash table exactly equals
    ** the number of entries in the mapping region.
    */
    {
      int i;           /* Loop counter */
      int nEntry = 0;  /* Number of entries in the hash table */
      for(i=0; i<HASHTABLE_NSLOT; i++){ if( aHash[i] ) nEntry++; }
      assert( nEntry==idx );
    }

    /* Verify that the every entry in the mapping region is reachable
    ** via the hash table.  This turns out to be a really, really expensive
    ** thing to check, so only do this occasionally - not on every
    ** iteration.
    */
    if( (idx&0x3ff)==0 ){
      int i;           /* Loop counter */
      for(i=1; i<=idx; i++){
        for(iKey=walHash(aPgno[i]); aHash[iKey]; iKey=walNextHash(iKey)){
          if( aHash[iKey]==i ) break;
        }
        assert( aHash[iKey]==i );
      }
    }
#endif /* SQLITE_ENABLE_EXPENSIVE_ASSERT */
  }


  return rc;
}


/*
** Recover the wal-index by reading the write-ahead log file. 
**
** This routine first tries to establish an exclusive lock on the
** wal-index to prevent other threads/processes from doing anything
** with the WAL or wal-index while recovery is running.  The
** WAL_RECOVER_LOCK is also held so that other threads will know
** that this thread is running recovery.  If unable to establish
** the necessary locks, this routine returns SQLITE_BUSY.
*/
static int walIndexRecover(Wal *pWal)
{
	// Replaced by read_thread_wal (and the code moved there)
  return SQLITE_OK;
}

// Returns: 0 nothing to do
// 			1 still have work to do
int checkpoint_continue(db_thread *thread)
{
	int i,rc;
	wal_file *wFile = thread->walFile;
	wal_file *nextToLast = thread->walFile;
	Wal *conWal;

	// No prev wal files, we don't have anything to do.
	if (thread->walFile->prev == NULL)
		return 0;

	// Move to last.
	while (wFile->prev != NULL)
	{
		nextToLast = wFile;
		wFile = wFile->prev;
	}
		

	for (i = 0; i < thread->nconns; i++)
	{
		if (!thread->conns[i].db)
			continue;

		DBG(("Checkpoint actor %d for wal %llu\r\n",i, wFile->walIndex));

		conWal = thread->conns[i].wal;
		while (conWal != NULL && conWal->walIndex > wFile->walIndex)
		{
			conWal = conWal->prev;
		}	

		if (conWal == NULL)
			continue;
		if (conWal->walIndex == wFile->walIndex)
		{
			assert(conWal->prev == NULL);
			// just call api function. It will call sqlite3WalCheckpoint, which will move to last
			// wal file in linked list and checkpoint that.
			rc = sqlite3_wal_checkpoint_v2(thread->conns[i].db,NULL,SQLITE_CHECKPOINT_FULL,NULL,NULL);
			assert(rc == SQLITE_OK);
			break;
		}
	}
	if (i == thread->nconns)
	{
		sqlite3OsCloseFree(wFile->pWalFd);
		sqlite3OsDelete(thread->vfs,wFile->filename,0);
		free(wFile->filename);
		free(wFile);
		nextToLast->prev = NULL;
		// If there were more than 2 wal files, we still have work to do.
		if (thread->walFile->prev != NULL)
			return 1;
		return 0;
	}
	return 1;
}

// ActorDB combines wal files for multiple actors (i.e. multiple sqlite dbs).
// Thus an individual actor should not be recovering index. This is a job for thread
// on which actor is running.
// 1. Read wal files from lowest number to highest.
// 2. For every wal entry, check if actor is opened. If it is not open it and add it to actor table.
// 3. Add wal entries to wal index (if they end with a commit)
int read_thread_wal(db_thread *thread)
{
  // File structure for path is generally: actors (d), shards (d), nodename (d), wal.1 (f), wal.2 (f), ..
  // Find wal.X files, make a link list of youngest to oldest.
  DIR *dir;
  struct dirent *ent;
  int flags = (SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_WAL);
  int rc = SQLITE_OK;
  char filename[MAX_PATHNAME];
  sqlite3_file *pWalFd = NULL;
  wal_file *prevWal = NULL;
  wal_file *curWal = NULL;
  u64 writeNumber = 0, writeTermNumber = 0;

  thread->vfs = sqlite3_vfs_find(0);

  // control threads have no path
  if (thread->pathlen == 0)
    return SQLITE_OK;

  if ((dir = opendir(thread->path)) == NULL)
  {
    DBG(("Can not open: %s\r\n",thread->path));
    return SQLITE_NOTFOUND;
  }

  while((ent = readdir(dir)) != NULL)
  {
    wal_file *walInfo;
    if (ent->d_type != DT_REG)
      continue;

    if (!(strlen(ent->d_name) > 4 && ent->d_name[0] == 'w' && ent->d_name[1] == 'a' && ent->d_name[2] == 'l' && ent->d_name[3] == '.'))
      continue;

    DBG(("OPENING : %s/%s\r\n",thread->path,ent->d_name));
    
    snprintf(filename,MAX_PATHNAME,"%s/%s",thread->path,ent->d_name);
    pWalFd = sqlite3MallocZero(thread->vfs->szOsFile);
    rc = sqlite3OsOpen(thread->vfs, filename, pWalFd, (SQLITE_OPEN_TRANSIENT_DB|SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE), &flags);
    if (rc != SQLITE_OK || flags&SQLITE_OPEN_READONLY || read_wal_hdr(thread->vfs,pWalFd,&walInfo) != SQLITE_OK)
    {
      DBG(("Removing wal %d\r\n",rc));
      sqlite3OsClose(pWalFd);
      sqlite3_free(pWalFd);
      remove(filename);
      continue;
    }

    walInfo->filename = malloc(strlen(filename)+1);
    memset(walInfo->filename,0,sizeof(strlen(filename)+1));
    strcpy(walInfo->filename,filename);
    walInfo->pWalFd = pWalFd;
    walInfo->szPage = SQLITE_DEFAULT_PAGE_SIZE;
    walInfo->bigEndCksum = SQLITE_BIGENDIAN;
    walInfo->aSalt[0] = 123456789;
    walInfo->aSalt[1] = 987654321;
    pWalFd = NULL;

    // We have a valid wal file structure.
    // Place walinfo structure in the right place in linked list.
    // Linked list is from newest to oldest.
    if (!thread->walFile)
      thread->walFile = walInfo;
    else
    {
      prevWal = thread->walFile->prev;
      curWal = thread->walFile;
      // Go back to the end or until we find a wal file with lower index
      while (prevWal != NULL && prevWal->walIndex > walInfo->walIndex)
      {
        prevWal = prevWal->prev;
        curWal = prevWal;
      }
      curWal->prev = walInfo;
      // Place current wal in the middle
      if (prevWal != NULL)
        walInfo->prev = prevWal;
    }
  } // endwhile
  closedir(dir);

  // no wal files exist, start new one
  if (!thread->walFile)
  {
    snprintf(filename,MAX_PATHNAME,"%s/wal.0",thread->path);
    thread->walFile = new_wal_file(filename,thread->vfs);
  }

  memset(filename,0,MAX_PATHNAME);
  // Now go through wal files, read frames and open dbs.
  curWal = thread->walFile;
  // Go to oldest.
  while (curWal->prev != NULL)
    curWal = curWal->prev;

  // strcpy(filename,thread->path);
  sprintf(filename,"%s/",thread->path);

  
  while(1)
  {
    i64 nSize;
    u8 *aFrame = 0;               /* Malloc'd buffer to load entire frame */
    int szFrame;                  /* Number of bytes in buffer aFrame[] */
    u8 *aData;                    /* Pointer to data part of aFrame buffer */
    int iFrame;
    int iOffset;
    int isValid;
    db_connection *curConn = thread->curConn;
    char prevDone = 1;
    u32 actorIndex;
    short curPathLen;
    int i;

    rc = sqlite3OsFileSize(curWal->pWalFd, &nSize);
    if( rc!=SQLITE_OK )
      return rc;

    if (nSize > WAL_HDRSIZE)
    {
      /* Malloc a buffer to read frames into. */
      szFrame = curWal->szPage + WAL_FRAME_HDRSIZE;
      aFrame = (u8 *)sqlite3_malloc(szFrame);
      if( !aFrame )
      {
        rc = SQLITE_NOMEM;
        break;
      }
      aData = &aFrame[WAL_FRAME_HDRSIZE];

      /* Read all frames from the log file. */
      iFrame = 0;
      for(iOffset=WAL_HDRSIZE; (iOffset+szFrame)<=nSize; iOffset+=szFrame)
      {
        u32 pgno;                   /* Database page number for frame */
        u32 nTruncate;              /* dbsize field from frame header */
        // DBG(("Reading wal %llu, frame %d\r\n",curWal->walIndex,iFrame));

        /* Read and decode the next log frame. */
        iFrame++;
        rc = sqlite3OsRead(curWal->pWalFd, aFrame, szFrame, iOffset);
        if( rc!=SQLITE_OK )
        {
        	DBG(("Can not read file\r\n"));
        	break;
        }
        isValid = walDecodeFrame(curWal, &pgno, &nTruncate,filename+thread->pathlen+1,&actorIndex, &writeNumber,&writeTermNumber, aData, aFrame);
        if( !isValid )
        {
        	DBG(("Frame INVALID! %s\r\n",filename));
        	break;
        }

        // DBG(("Frame belongs to %s, truncate %d, pgno %d\r\n",filename,nTruncate,pgno));

        if (curConn != NULL && strncmp(filename+thread->pathlen+1,curConn->dbpath,100) == 0)
        {
          // continue
          curConn->nPages++;
        }
        else
        {
          // open new
          if (!prevDone)
          {
            // previous transaction was not finished, we must undo index for it
            sqlite3WalUndo(curConn->wal, NULL, NULL);
          }

          if (thread->nconns <= actorIndex)
          {
            int newsize = sizeof(db_connection)*thread->nconns*actorIndex*1.2;
            db_connection *newcons = malloc(newsize);
            memset(newcons,0,newsize);
            memcpy(newcons,thread->conns,thread->nconns*sizeof(db_connection));
            thread->nconns = actorIndex*1.2;
          }
          if (curConn != NULL)
          {
          	sqlite3WalEndReadTransaction(curConn->wal);
          	curConn->wal->init = 0;
          }
          	
          curConn = &thread->conns[actorIndex];
          if (curConn->db == NULL)
          {
            curPathLen = strlen(filename+thread->pathlen+1)+1;
            if (curPathLen >= 100)
            {
            	rc = SQLITE_ERROR;
            	break;
            }
            memset(curConn->dbpath,0,100);
            strcpy(curConn->dbpath,filename+thread->pathlen+1);

            rc = sqlite3_open(filename,&(curConn->db));
            if (rc != SQLITE_OK)
            {
              break;
            }
            curConn->wal = malloc(sizeof(struct Wal));
            memset(curConn->wal,0,sizeof(struct Wal));
            curConn->connindex = actorIndex;
            curConn->wal->thread = thread;
		    curConn->wal->pWalFd = curWal->pWalFd;
		    curConn->wal->exclusiveMode = WAL_HEAPMEMORY_MODE;
		    curConn->wal->padToSectorBoundary = 1;
		    curConn->wal->syncHeader = 0;
		    curConn->wal->readLock = -1;

		    sqlite3WalBeginReadTransaction(curConn->wal,&rc);
            sqlite3HashInsert(&thread->walHash, curConn->dbpath, curConn);
            curConn->nPages = 1;
            curConn->nPrevPages = 0;
          }
          else
          	sqlite3WalBeginReadTransaction(curConn->wal,&rc);
        }

        rc = walIndexAppend(curConn->wal, iFrame, pgno);
        if( rc!=SQLITE_OK ) break;

        /* If nTruncate is non-zero, this is a commit record. */
        if( nTruncate )
        {
          WalCkptInfo *pInfo;
          prevDone = 1;
          thread->walFile->mxFrame = iFrame;
          thread->walFile->lastCommit = iFrame;
          curConn->wal->hdr.mxFrame = iFrame;
          curConn->wal->hdr.nPage = nTruncate;
          curConn->wal->hdr.szPage = (u16)((curWal->szPage&0xff00) | (curWal->szPage>>16));
          testcase( curWal->szPage<=32768 );
          testcase( curWal->szPage>=65536 );

          sqlite3WalBeginWriteTransaction(curConn->wal);
          walIndexWriteHdr(curConn->wal);
          pInfo = walCkptInfo(curConn->wal);
          pInfo->nBackfill = 0;
          pInfo->aReadMark[0] = 0;
          for(i=1; i<WAL_NREADER; i++) pInfo->aReadMark[i] = READMARK_NOT_USED;
          if( curConn->wal->hdr.mxFrame ) pInfo->aReadMark[1] = curConn->wal->hdr.mxFrame;
      	  sqlite3WalEndWriteTransaction(curConn->wal);

          // aFrameCksum[0] = pWal->hdr.aFrameCksum[0];
          // aFrameCksum[1] = pWal->hdr.aFrameCksum[1];
        }
        else
          prevDone = 0;
      }
      // open new
      if (!prevDone && curConn != NULL)
      {
        // previous transaction was not finished, we must undo index for it
        sqlite3WalUndo(curConn->wal, NULL, NULL);
      }
      if (curConn != NULL)
      {
      	sqlite3WalEndReadTransaction(curConn->wal);
      	curConn->wal->init = 0;
      }
      	
      sqlite3_free(aFrame);
    }
    
    // Start with newwest file
    prevWal = thread->walFile;
    // Move back until reaching next wal from current
    while (prevWal->prev != curWal && prevWal->prev != NULL)
      prevWal = prevWal->prev;

    if (prevWal->prev != curWal)
      break;
    curWal = prevWal;
  }


  return SQLITE_OK;
}

wal_file *new_wal_file(char* filename,sqlite3_vfs *vfs)
{
	sqlite3_file *pWalFd = sqlite3MallocZero(vfs->szOsFile);
	wal_file *walFile = NULL;
	int flags;
	int rc;

    rc = sqlite3OsOpen(vfs, filename, pWalFd, (SQLITE_OPEN_TRANSIENT_DB|SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE), &flags);
    if (rc != SQLITE_OK)
    {
      DBG(("Can not open wal file %s\r\n",filename));fflush(stdout);
      sqlite3_free(pWalFd);
      return NULL;
    }
	
    walFile = sqlite3MallocZero(sizeof(wal_file));
    walFile->filename = malloc(strlen(filename)+1);
    memset(walFile->filename,0,sizeof(strlen(filename)+1));
    strcpy(walFile->filename,filename);
    walFile->pWalFd = pWalFd;
    walFile->szPage = SQLITE_DEFAULT_PAGE_SIZE;
    walFile->bigEndCksum = SQLITE_BIGENDIAN;
    walFile->aSalt[0] = 123456789;
    walFile->aSalt[1] = 987654321;
    return walFile;
}

int read_wal_hdr(sqlite3_vfs *vfs, sqlite3_file *pWalFd, wal_file **outWalFile)
{
  wal_file walFile;
  i64 nSize = 0;
  int rc;
  
  *outWalFile = NULL;
  memset(&walFile,0,sizeof(wal_file));

  rc = sqlite3OsFileSize(pWalFd, &nSize);
  if( rc!=SQLITE_OK )
    return rc;

  if (nSize >= WAL_HDRSIZE)
  {
    u8 aBuf[WAL_HDRSIZE];         /* Buffer to load WAL header into */
    int szPage;                   /* Page size according to the log */
    u32 magic;                    /* Magic value read from WAL header */
    u32 version;                  /* Magic value read from WAL header */
    u32 aFrameCksum[2];

    /* Read in the WAL header. */
    rc = sqlite3OsRead(pWalFd, aBuf, WAL_HDRSIZE, 0);
    if( rc!=SQLITE_OK )
      return SQLITE_CANTOPEN_BKPT;

    /* If the database page size is not a power of two, or is greater than
    ** SQLITE_MAX_PAGE_SIZE, conclude that the WAL file contains no valid 
    ** data. Similarly, if the 'magic' value is invalid, ignore the whole
    ** WAL file.
    */
    magic = sqlite3Get4byte(&aBuf[0]);
    szPage = sqlite3Get4byte(&aBuf[8]);
    if( (magic&0xFFFFFFFE)!=WAL_MAGIC 
     || szPage&(szPage-1) 
     || szPage>SQLITE_MAX_PAGE_SIZE 
     || szPage<512 
    ){
      return SQLITE_CANTOPEN_BKPT;
    }
    walFile.bigEndCksum = SQLITE_BIGENDIAN; //(u8)(magic&0x00000001);
    walFile.szPage = szPage;
    // pWal->szPage
    // pWal->bigEndCksum
    // pWal->nCkpt = sqlite3Get4byte(&aBuf[12]);
    walFile.walIndex = readUInt64(&aBuf[12]);
    walFile.aSalt[0] = sqlite3Get4byte(&aBuf[20]);
    walFile.aSalt[1] = sqlite3Get4byte(&aBuf[24]);

    /* Verify that the WAL header checksum is correct */
    walChecksumBytes(walFile.bigEndCksum==SQLITE_BIGENDIAN, 
        aBuf, WAL_HDRSIZE-2*4, 0, aFrameCksum);
    
    if( aFrameCksum[0]!=sqlite3Get4byte(&aBuf[32])
     || aFrameCksum[1]!=sqlite3Get4byte(&aBuf[36])
    ){
      return SQLITE_CANTOPEN_BKPT;
    }

    /* Verify that the version number on the WAL format is one that
    ** are able to understand */
    version = sqlite3Get4byte(&aBuf[4]);
    if( version!=WAL_MAX_VERSION ){
      return SQLITE_CANTOPEN_BKPT;
    }
  }
  else
    return SQLITE_CANTOPEN_BKPT;

  *outWalFile = malloc(sizeof(wal_file));
  memcpy(*outWalFile,&walFile,sizeof(wal_file));
  return SQLITE_OK;
}


/*
** Close an open wal-index.
*/
static void walIndexClose(Wal *pWal, int isDelete){
  if( pWal->exclusiveMode==WAL_HEAPMEMORY_MODE ){
    int i;
    for(i=0; i<pWal->nWiData; i++){
      sqlite3_free((void *)pWal->apWiData[i]);
      pWal->apWiData[i] = 0;
    }
  }else{
    sqlite3OsShmUnmap(pWal->pDbFd, isDelete);
  }
}



/*
** Find the smallest page number out of all pages held in the WAL that
** has not been returned by any prior invocation of this method on the
** same WalIterator object.   Write into *piFrame the frame index where
** that page was last written into the WAL.  Write into *piPage the page
** number.
**
** Return 0 on success.  If there are no pages in the WAL with a page
** number larger than *piPage, then return 1.
*/
static int walIteratorNext(
  WalIterator *p,               /* Iterator */
  u32 *piPage,                  /* OUT: The page number of the next page */
  u32 *piFrame                  /* OUT: Wal frame index of next page */
){
  u32 iMin;                     /* Result pgno must be greater than iMin */
  u32 iRet = 0xFFFFFFFF;        /* 0xffffffff is never a valid page number */
  int i;                        /* For looping through segments */

  iMin = p->iPrior;
  assert( iMin<0xffffffff );
  for(i=p->nSegment-1; i>=0; i--){
    struct WalSegment *pSegment = &p->aSegment[i];
    while( pSegment->iNext<pSegment->nEntry ){
      u32 iPg = pSegment->aPgno[pSegment->aIndex[pSegment->iNext]];
      if( iPg>iMin ){
        if( iPg<iRet ){
          iRet = iPg;
          *piFrame = pSegment->iZero + pSegment->aIndex[pSegment->iNext];
        }
        break;
      }
      pSegment->iNext++;
    }
  }

  *piPage = p->iPrior = iRet;
  return (iRet==0xFFFFFFFF);
}

/*
** This function merges two sorted lists into a single sorted list.
**
** aLeft[] and aRight[] are arrays of indices.  The sort key is
** aContent[aLeft[]] and aContent[aRight[]].  Upon entry, the following
** is guaranteed for all J<K:
**
**        aContent[aLeft[J]] < aContent[aLeft[K]]
**        aContent[aRight[J]] < aContent[aRight[K]]
**
** This routine overwrites aRight[] with a new (probably longer) sequence
** of indices such that the aRight[] contains every index that appears in
** either aLeft[] or the old aRight[] and such that the second condition
** above is still met.
**
** The aContent[aLeft[X]] values will be unique for all X.  And the
** aContent[aRight[X]] values will be unique too.  But there might be
** one or more combinations of X and Y such that
**
**      aLeft[X]!=aRight[Y]  &&  aContent[aLeft[X]] == aContent[aRight[Y]]
**
** When that happens, omit the aLeft[X] and use the aRight[Y] index.
*/
static void walMerge(
  const u32 *aContent,            /* Pages in wal - keys for the sort */
  ht_slot *aLeft,                 /* IN: Left hand input list */
  int nLeft,                      /* IN: Elements in array *paLeft */
  ht_slot **paRight,              /* IN/OUT: Right hand input list */
  int *pnRight,                   /* IN/OUT: Elements in *paRight */
  ht_slot *aTmp                   /* Temporary buffer */
){
  int iLeft = 0;                  /* Current index in aLeft */
  int iRight = 0;                 /* Current index in aRight */
  int iOut = 0;                   /* Current index in output buffer */
  int nRight = *pnRight;
  ht_slot *aRight = *paRight;

  assert( nLeft>0 && nRight>0 );
  while( iRight<nRight || iLeft<nLeft ){
    ht_slot logpage;
    Pgno dbpage;

    if( (iLeft<nLeft) 
     && (iRight>=nRight || aContent[aLeft[iLeft]]<aContent[aRight[iRight]])
    ){
      logpage = aLeft[iLeft++];
    }else{
      logpage = aRight[iRight++];
    }
    dbpage = aContent[logpage];

    aTmp[iOut++] = logpage;
    if( iLeft<nLeft && aContent[aLeft[iLeft]]==dbpage ) iLeft++;

    assert( iLeft>=nLeft || aContent[aLeft[iLeft]]>dbpage );
    assert( iRight>=nRight || aContent[aRight[iRight]]>dbpage );
  }

  *paRight = aLeft;
  *pnRight = iOut;
  memcpy(aLeft, aTmp, sizeof(aTmp[0])*iOut);
}

/*
** Sort the elements in list aList using aContent[] as the sort key.
** Remove elements with duplicate keys, preferring to keep the
** larger aList[] values.
**
** The aList[] entries are indices into aContent[].  The values in
** aList[] are to be sorted so that for all J<K:
**
**      aContent[aList[J]] < aContent[aList[K]]
**
** For any X and Y such that
**
**      aContent[aList[X]] == aContent[aList[Y]]
**
** Keep the larger of the two values aList[X] and aList[Y] and discard
** the smaller.
*/
static void walMergesort(
  const u32 *aContent,            /* Pages in wal */
  ht_slot *aBuffer,               /* Buffer of at least *pnList items to use */
  ht_slot *aList,                 /* IN/OUT: List to sort */
  int *pnList                     /* IN/OUT: Number of elements in aList[] */
){
  struct Sublist {
    int nList;                    /* Number of elements in aList */
    ht_slot *aList;               /* Pointer to sub-list content */
  };

  const int nList = *pnList;      /* Size of input list */
  int nMerge = 0;                 /* Number of elements in list aMerge */
  ht_slot *aMerge = 0;            /* List to be merged */
  int iList;                      /* Index into input list */
  int iSub = 0;                   /* Index into aSub array */
  struct Sublist aSub[13];        /* Array of sub-lists */

  memset(aSub, 0, sizeof(aSub));
  assert( nList<=HASHTABLE_NPAGE && nList>0 );
  assert( HASHTABLE_NPAGE==(1<<(ArraySize(aSub)-1)) );

  for(iList=0; iList<nList; iList++){
    nMerge = 1;
    aMerge = &aList[iList];
    for(iSub=0; iList & (1<<iSub); iSub++){
      struct Sublist *p = &aSub[iSub];
      assert( p->aList && p->nList<=(1<<iSub) );
      assert( p->aList==&aList[iList&~((2<<iSub)-1)] );
      walMerge(aContent, p->aList, p->nList, &aMerge, &nMerge, aBuffer);
    }
    aSub[iSub].aList = aMerge;
    aSub[iSub].nList = nMerge;
  }

  for(iSub++; iSub<ArraySize(aSub); iSub++){
    if( nList & (1<<iSub) ){
      struct Sublist *p = &aSub[iSub];
      assert( p->nList<=(1<<iSub) );
      assert( p->aList==&aList[nList&~((2<<iSub)-1)] );
      walMerge(aContent, p->aList, p->nList, &aMerge, &nMerge, aBuffer);
    }
  }
  assert( aMerge==aList );
  *pnList = nMerge;

#ifdef SQLITE_DEBUG
  {
    int i;
    for(i=1; i<*pnList; i++){
      assert( aContent[aList[i]] > aContent[aList[i-1]] );
    }
  }
#endif
}

/* 
** Free an iterator allocated by walIteratorInit().
*/
static void walIteratorFree(WalIterator *p){
  sqlite3ScratchFree(p);
}

/*
** Construct a WalInterator object that can be used to loop over all 
** pages in the WAL in ascending order. The caller must hold the checkpoint
** lock.
**
** On success, make *pp point to the newly allocated WalInterator object
** return SQLITE_OK. Otherwise, return an error code. If this routine
** returns an error, the value of *pp is undefined.
**
** The calling routine should invoke walIteratorFree() to destroy the
** WalIterator object when it has finished with it.
*/
static int walIteratorInit(Wal *pWal, WalIterator **pp){
  WalIterator *p;                 /* Return value */
  int nSegment;                   /* Number of segments to merge */
  u32 iLast;                      /* Last frame in log */
  int nByte;                      /* Number of bytes to allocate */
  int i;                          /* Iterator variable */
  ht_slot *aTmp;                  /* Temp space used by merge-sort */
  int rc = SQLITE_OK;             /* Return Code */

  /* This routine only runs while holding the checkpoint lock. And
  ** it only runs if there is actually content in the log (mxFrame>0).
  */
  assert( pWal->ckptLock && pWal->hdr.mxFrame>0 );
  iLast = pWal->hdr.mxFrame;

  /* Allocate space for the WalIterator object. */
  nSegment = walFramePage(iLast) + 1;
  nByte = sizeof(WalIterator) 
        + (nSegment-1)*sizeof(struct WalSegment)
        + iLast*sizeof(ht_slot);
  p = (WalIterator *)sqlite3ScratchMalloc(nByte);
  if( !p ){
    return SQLITE_NOMEM;
  }
  memset(p, 0, nByte);
  p->nSegment = nSegment;

  /* Allocate temporary space used by the merge-sort routine. This block
  ** of memory will be freed before this function returns.
  */
  aTmp = (ht_slot *)sqlite3ScratchMalloc(
      sizeof(ht_slot) * (iLast>HASHTABLE_NPAGE?HASHTABLE_NPAGE:iLast)
  );
  if( !aTmp ){
    rc = SQLITE_NOMEM;
  }

  for(i=0; rc==SQLITE_OK && i<nSegment; i++){
    ht_slot *aHash;
    u32 iZero;
    u32 *aPgno;

    rc = walHashGet(pWal, i, &aHash, &aPgno, &iZero);
    if( rc==SQLITE_OK ){
      int j;                      /* Counter variable */
      int nEntry;                 /* Number of entries in this segment */
      ht_slot *aIndex;            /* Sorted index for this segment */

      aPgno++;
      if( (i+1)==nSegment ){
        nEntry = (int)(iLast - iZero);
      }else{
        nEntry = (int)((u32*)aHash - (u32*)aPgno);
      }
      aIndex = &((ht_slot *)&p->aSegment[p->nSegment])[iZero];
      iZero++;
  
      for(j=0; j<nEntry; j++){
        aIndex[j] = (ht_slot)j;
      }
      walMergesort((u32 *)aPgno, aTmp, aIndex, &nEntry);
      p->aSegment[i].iZero = iZero;
      p->aSegment[i].nEntry = nEntry;
      p->aSegment[i].aIndex = aIndex;
      p->aSegment[i].aPgno = (u32 *)aPgno;
    }
  }
  sqlite3ScratchFree(aTmp);

  if( rc!=SQLITE_OK ){
    walIteratorFree(p);
  }
  *pp = p;
  return rc;
}

/*
** Attempt to obtain the exclusive WAL lock defined by parameters lockIdx and
** n. If the attempt fails and parameter xBusy is not NULL, then it is a
** busy-handler function. Invoke it and retry the lock until either the
** lock is successfully obtained or the busy-handler returns 0.
*/
static int walBusyLock(
  Wal *pWal,                      /* WAL connection */
  int (*xBusy)(void*),            /* Function to call when busy */
  void *pBusyArg,                 /* Context argument for xBusyHandler */
  int lockIdx,                    /* Offset of first byte to lock */
  int n                           /* Number of bytes to lock */
){
  int rc;
  do {
    rc = walLockExclusive(pWal, lockIdx, n);
  }while( xBusy && rc==SQLITE_BUSY && xBusy(pBusyArg) );
  return rc;
}

/*
** The cache of the wal-index header must be valid to call this function.
** Return the page-size in bytes used by the database.
*/
static int walPagesize(Wal *pWal){
  return (pWal->hdr.szPage&0xfe00) + ((pWal->hdr.szPage&0x0001)<<16);
}

/*
** Copy as much content as we can from the WAL back into the database file
** in response to an sqlite3_wal_checkpoint() request or the equivalent.
**
** The amount of information copies from WAL to database might be limited
** by active readers.  This routine will never overwrite a database page
** that a concurrent reader might be using.
**
** All I/O barrier operations (a.k.a fsyncs) occur in this routine when
** SQLite is in WAL-mode in synchronous=NORMAL.  That means that if 
** checkpoints are always run by a background thread or background 
** process, foreground threads will never block on a lengthy fsync call.
**
** Fsync is called on the WAL before writing content out of the WAL and
** into the database.  This ensures that if the new content is persistent
** in the WAL and can be recovered following a power-loss or hard reset.
**
** Fsync is also called on the database file if (and only if) the entire
** WAL content is copied into the database file.  This second fsync makes
** it safe to delete the WAL since the new content will persist in the
** database file.
**
** This routine uses and updates the nBackfill field of the wal-index header.
** This is the only routine that will increase the value of nBackfill.  
** (A WAL reset or recovery will revert nBackfill to zero, but not increase
** its value.)
**
** The caller must be holding sufficient locks to ensure that no other
** checkpoint is running (in any other thread or process) at the same
** time.
*/
static int walCheckpoint(
  Wal *pWal,                      /* Wal connection */
  int eMode,                      /* One of PASSIVE, FULL or RESTART */
  int (*xBusyCall)(void*),        /* Function to call when busy */
  void *pBusyArg,                 /* Context argument for xBusyHandler */
  int sync_flags,                 /* Flags for OsSync() (or 0) */
  u8 *zBuf                        /* Temporary buffer to use */
){
  int rc;                         /* Return code */
  int szPage;                     /* Database page-size */
  WalIterator *pIter = 0;         /* Wal iterator context */
  u32 iDbpage = 0;                /* Next database page to write */
  u32 iFrame = 0;                 /* Wal frame containing data for iDbpage */
  u32 mxSafeFrame;                /* Max frame that can be backfilled */
  u32 mxPage;                     /* Max database page to write */
  int i;                          /* Loop counter */
  WalCkptInfo *pInfo;    /* The checkpoint status information */
  int (*xBusy)(void*) = 0;        /* Function to call when waiting for locks */
  db_connection *con = pWal->thread->curConn;

  szPage = walPagesize(pWal);
  testcase( szPage<=32768 );
  testcase( szPage>=65536 );
  pInfo = walCkptInfo(pWal);
  if( pInfo->nBackfill>=pWal->hdr.mxFrame ) return SQLITE_OK;

  /* Allocate the iterator */
  rc = walIteratorInit(pWal, &pIter);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  assert( pIter );

  if( eMode!=SQLITE_CHECKPOINT_PASSIVE ) xBusy = xBusyCall;

  /* Compute in mxSafeFrame the index of the last frame of the WAL that is
  ** safe to write into the database.  Frames beyond mxSafeFrame might
  ** overwrite database pages that are in use by active readers and thus
  ** cannot be backfilled from the WAL.
  */
  mxSafeFrame = pWal->hdr.mxFrame;
  mxPage = pWal->hdr.nPage;
  for(i=1; i<WAL_NREADER; i++){
    u32 y = pInfo->aReadMark[i];
    if( mxSafeFrame>y ){
      assert( y<=pWal->hdr.mxFrame );
      rc = walBusyLock(pWal, xBusy, pBusyArg, WAL_READ_LOCK(i), 1);
      if( rc==SQLITE_OK ){
        pInfo->aReadMark[i] = (i==1 ? mxSafeFrame : READMARK_NOT_USED);
        walUnlockExclusive(pWal, WAL_READ_LOCK(i), 1);
      }else if( rc==SQLITE_BUSY ){
        mxSafeFrame = y;
        xBusy = 0;
      }else{
        goto walcheckpoint_out;
      }
    }
  }

  if( pInfo->nBackfill<mxSafeFrame
   && (rc = walBusyLock(pWal, xBusy, pBusyArg, WAL_READ_LOCK(0), 1))==SQLITE_OK
  ){
    i64 nSize;                    /* Current size of database file */
    u32 nBackfill = pInfo->nBackfill;

    /* Sync the WAL to disk */
    if( sync_flags ){
      rc = sqlite3OsSync(pWal->pWalFd, sync_flags);
    }

    /* If the database may grow as a result of this checkpoint, hint
    ** about the eventual size of the db file to the VFS layer.
    */
    if( rc==SQLITE_OK ){
      i64 nReq = ((i64)mxPage * szPage);
      rc = sqlite3OsFileSize(pWal->pDbFd, &nSize);
      if( rc==SQLITE_OK && nSize<nReq ){
        sqlite3OsFileControlHint(pWal->pDbFd, SQLITE_FCNTL_SIZE_HINT, &nReq);
      }
    }

    /* Iterate through the contents of the WAL, copying data to the db file. */
    while( rc==SQLITE_OK && 0==walIteratorNext(pIter, &iDbpage, &iFrame) ){
      i64 iOffset;
      assert( walFramePgno(pWal, iFrame)==iDbpage );
      if( iFrame<=nBackfill || iFrame>mxSafeFrame || iDbpage>mxPage ) continue;
      iOffset = walFrameOffset(iFrame, szPage) + WAL_FRAME_HDRSIZE;
      con->nPages--;
      /* testcase( IS_BIG_INT(iOffset) ); // requires a 4GiB WAL file */
      rc = sqlite3OsRead(pWal->pWalFd, zBuf, szPage, iOffset);
      if( rc!=SQLITE_OK ) break;
      iOffset = (iDbpage-1)*(i64)szPage;
      testcase( IS_BIG_INT(iOffset) );
      rc = sqlite3OsWrite(pWal->pDbFd, zBuf, szPage, iOffset);
      if( rc!=SQLITE_OK ) break;
    }

    /* If work was actually accomplished... */
    if( rc==SQLITE_OK ){
      if( mxSafeFrame==walIndexHdr(pWal)->mxFrame ){
        i64 szDb = pWal->hdr.nPage*(i64)szPage;
        testcase( IS_BIG_INT(szDb) );
        rc = sqlite3OsTruncate(pWal->pDbFd, szDb);
        if( rc==SQLITE_OK && sync_flags ){
          rc = sqlite3OsSync(pWal->pDbFd, sync_flags);
        }
      }
      if( rc==SQLITE_OK ){
        pInfo->nBackfill = mxSafeFrame;
      }
    }

    /* Release the reader lock held while backfilling */
    walUnlockExclusive(pWal, WAL_READ_LOCK(0), 1);
  }

  if( rc==SQLITE_BUSY ){
    /* Reset the return code so as not to report a checkpoint failure
    ** just because there are active readers.  */
    rc = SQLITE_OK;
  }

  /* If this is an SQLITE_CHECKPOINT_RESTART operation, and the entire wal
  ** file has been copied into the database file, then block until all
  ** readers have finished using the wal file. This ensures that the next
  ** process to write to the database restarts the wal file.
  */
  if( rc==SQLITE_OK && eMode!=SQLITE_CHECKPOINT_PASSIVE ){
    assert( pWal->writeLock );
    if( pInfo->nBackfill<pWal->hdr.mxFrame ){
      rc = SQLITE_BUSY;
    }else if( eMode==SQLITE_CHECKPOINT_RESTART ){
      assert( mxSafeFrame==pWal->hdr.mxFrame );
      rc = walBusyLock(pWal, xBusy, pBusyArg, WAL_READ_LOCK(1), WAL_NREADER-1);
      if( rc==SQLITE_OK ){
        walUnlockExclusive(pWal, WAL_READ_LOCK(1), WAL_NREADER-1);
      }
    }
  }

 walcheckpoint_out:
  walIteratorFree(pIter);
  return rc;
}

/*
** If the WAL file is currently larger than nMax bytes in size, truncate
** it to exactly nMax bytes. If an error occurs while doing so, ignore it.
*/
// static void walLimitSize(Wal *pWal, i64 nMax){
//   i64 sz;
//   int rx;
//   sqlite3BeginBenignMalloc();
//   rx = sqlite3OsFileSize(pWal->pWalFd, &sz);
//   if( rx==SQLITE_OK && (sz > nMax ) ){
//     rx = sqlite3OsTruncate(pWal->pWalFd, nMax);
//   }
//   sqlite3EndBenignMalloc();
//   if( rx ){
//     sqlite3_log(rx, "cannot limit WAL size: %s", pWal->zWalName);
//   }
// }



/*
** Try to read the wal-index header.  Return 0 on success and 1 if
** there is a problem.
**
** The wal-index is in shared memory.  Another thread or process might
** be writing the header at the same time this procedure is trying to
** read it, which might result in inconsistency.  A dirty read is detected
** by verifying that both copies of the header are the same and also by
** a checksum on the header.
**
** If and only if the read is consistent and the header is different from
** pWal->hdr, then pWal->hdr is updated to the content of the new header
** and *pChanged is set to 1.
**
** If the checksum cannot be verified return non-zero. If the header
** is read successfully and the checksum verified, return zero.
*/
static int walIndexTryHdr(Wal *pWal, int *pChanged){
  u32 aCksum[2];                  /* Checksum on the header content */
  WalIndexHdr h1, h2;             /* Two copies of the header content */
  WalIndexHdr *aHdr;     /* Header in shared memory */

  /* The first page of the wal-index must be mapped at this point. */
  assert( pWal->nWiData>0 && pWal->apWiData[0] );

  /* Read the header. This might happen concurrently with a write to the
  ** same area of shared memory on a different CPU in a SMP,
  ** meaning it is possible that an inconsistent snapshot is read
  ** from the file. If this happens, return non-zero.
  **
  ** There are two copies of the header at the beginning of the wal-index.
  ** When reading, read [0] first then [1].  Writes are in the reverse order.
  ** Memory barriers are used to prevent the compiler or the hardware from
  ** reordering the reads and writes.
  */
  aHdr = walIndexHdr(pWal);
  memcpy(&h1, (void *)&aHdr[0], sizeof(h1));
  walShmBarrier(pWal);
  memcpy(&h2, (void *)&aHdr[1], sizeof(h2));

  if( memcmp(&h1, &h2, sizeof(h1))!=0 ){
    return 1;   /* Dirty read */
  }  
  if( h1.isInit==0 ){
    return 1;   /* Malformed header - probably all zeros */
  }
  walChecksumBytes(1, (u8*)&h1, sizeof(h1)-sizeof(h1.aCksum), 0, aCksum);
  if( aCksum[0]!=h1.aCksum[0] || aCksum[1]!=h1.aCksum[1] ){
    return 1;   /* Checksum does not match */
  }

  if( memcmp(&pWal->hdr, &h1, sizeof(WalIndexHdr)) ){
    *pChanged = 1;
    memcpy(&pWal->hdr, &h1, sizeof(WalIndexHdr));
    pWal->szPage = (pWal->hdr.szPage&0xfe00) + ((pWal->hdr.szPage&0x0001)<<16);
    testcase( pWal->szPage<=32768 );
    testcase( pWal->szPage>=65536 );
  }

  /* The header was successfully read. Return zero. */
  return 0;
}

/*
** Read the wal-index header from the wal-index and into pWal->hdr.
** If the wal-header appears to be corrupt, try to reconstruct the
** wal-index from the WAL before returning.
**
** Set *pChanged to 1 if the wal-index header value in pWal->hdr is
** changed by this operation.  If pWal->hdr is unchanged, set *pChanged
** to 0.
**
** If the wal-index header is successfully read, return SQLITE_OK. 
** Otherwise an SQLite error code.
*/
static int walIndexReadHdr(Wal *pWal, int *pChanged){
  int rc;                         /* Return code */
  int badHdr;                     /* True if a header read failed */
  u32 *page0;            /* Chunk of wal-index containing header */

  /* Ensure that page 0 of the wal-index (the page that contains the 
  ** wal-index header) is mapped. Return early if an error occurs here.
  */
  assert( pChanged );
  rc = walIndexPage(pWal, 0, &page0);
  if( rc!=SQLITE_OK ){
    return rc;
  };
  assert( page0 || pWal->writeLock==0 );

  /* If the first page of the wal-index has been mapped, try to read the
  ** wal-index header immediately, without holding any lock. This usually
  ** works, but may fail if the wal-index header is corrupt or currently 
  ** being modified by another thread or process.
  */
  badHdr = (page0 ? walIndexTryHdr(pWal, pChanged) : 1);

  /* If the first attempt failed, it might have been due to a race
  ** with a writer.  So get a WRITE lock and try again.
  */
  assert( badHdr==0 || pWal->writeLock==0 );
  if( badHdr ){
    if( pWal->readOnly & WAL_SHM_RDONLY ){
      if( SQLITE_OK==(rc = walLockShared(pWal, WAL_WRITE_LOCK)) ){
        walUnlockShared(pWal, WAL_WRITE_LOCK);
        rc = SQLITE_READONLY_RECOVERY;
      }
    }else if( SQLITE_OK==(rc = walLockExclusive(pWal, WAL_WRITE_LOCK, 1)) ){
      pWal->writeLock = 1;
      if( SQLITE_OK==(rc = walIndexPage(pWal, 0, &page0)) ){
        badHdr = walIndexTryHdr(pWal, pChanged);
        if( badHdr ){
          /* If the wal-index header is still malformed even while holding
          ** a WRITE lock, it can only mean that the header is corrupted and
          ** needs to be reconstructed.  So run recovery to do exactly that.
          */
          rc = walIndexRecover(pWal);
          *pChanged = 1;
        }
      }
      pWal->writeLock = 0;
      walUnlockExclusive(pWal, WAL_WRITE_LOCK, 1);
    }
  }
  
  // Force changed to 1 if this is first time we have reached this point.
  // Wal was initialized by read_thread_wal, but data structures outside wal
  // don't know that. Normally this would occur here (in walIndexRecover).
  if (!pWal->init)
  {
  	pWal->init = 1;
  	badHdr = 1;
  	*pChanged = 1;
  }

  /* If the header is read successfully, check the version number to make
  ** sure the wal-index was not constructed with some future format that
  ** this version of SQLite cannot understand.
  */
  if( badHdr==0 && pWal->hdr.iVersion!=WALINDEX_MAX_VERSION ){
    rc = SQLITE_CANTOPEN_BKPT;
  }

  return rc;
}

/*
** This is the value that walTryBeginRead returns when it needs to
** be retried.
*/
#define WAL_RETRY  (-1)

/*
** Attempt to start a read transaction.  This might fail due to a race or
** other transient condition.  When that happens, it returns WAL_RETRY to
** indicate to the caller that it is safe to retry immediately.
**
** On success return SQLITE_OK.  On a permanent failure (such an
** I/O error or an SQLITE_BUSY because another process is running
** recovery) return a positive error code.
**
** The useWal parameter is true to force the use of the WAL and disable
** the case where the WAL is bypassed because it has been completely
** checkpointed.  If useWal==0 then this routine calls walIndexReadHdr() 
** to make a copy of the wal-index header into pWal->hdr.  If the 
** wal-index header has changed, *pChanged is set to 1 (as an indication 
** to the caller that the local paget cache is obsolete and needs to be 
** flushed.)  When useWal==1, the wal-index header is assumed to already
** be loaded and the pChanged parameter is unused.
**
** The caller must set the cnt parameter to the number of prior calls to
** this routine during the current read attempt that returned WAL_RETRY.
** This routine will start taking more aggressive measures to clear the
** race conditions after multiple WAL_RETRY returns, and after an excessive
** number of errors will ultimately return SQLITE_PROTOCOL.  The
** SQLITE_PROTOCOL return indicates that some other process has gone rogue
** and is not honoring the locking protocol.  There is a vanishingly small
** chance that SQLITE_PROTOCOL could be returned because of a run of really
** bad luck when there is lots of contention for the wal-index, but that
** possibility is so small that it can be safely neglected, we believe.
**
** On success, this routine obtains a read lock on 
** WAL_READ_LOCK(pWal->readLock).  The pWal->readLock integer is
** in the range 0 <= pWal->readLock < WAL_NREADER.  If pWal->readLock==(-1)
** that means the Wal does not hold any read lock.  The reader must not
** access any database page that is modified by a WAL frame up to and
** including frame number aReadMark[pWal->readLock].  The reader will
** use WAL frames up to and including pWal->hdr.mxFrame if pWal->readLock>0
** Or if pWal->readLock==0, then the reader will ignore the WAL
** completely and get all content directly from the database file.
** If the useWal parameter is 1 then the WAL will never be ignored and
** this routine will always set pWal->readLock>0 on success.
** When the read transaction is completed, the caller must release the
** lock on WAL_READ_LOCK(pWal->readLock) and set pWal->readLock to -1.
**
** This routine uses the nBackfill and aReadMark[] fields of the header
** to select a particular WAL_READ_LOCK() that strives to let the
** checkpoint process do as much work as possible.  This routine might
** update values of the aReadMark[] array in the header, but if it does
** so it takes care to hold an exclusive lock on the corresponding
** WAL_READ_LOCK() while changing values.
*/
static int walTryBeginRead(Wal *pWal, int *pChanged, int useWal, int cnt){
  WalCkptInfo *pInfo;    /* Checkpoint information in wal-index */
  u32 mxReadMark;                 /* Largest aReadMark[] value */
  int mxI;                        /* Index of largest aReadMark[] value */
  int i;                          /* Loop counter */
  int rc = SQLITE_OK;             /* Return code  */

  assert( pWal->readLock<0 );     /* Not currently locked */

  /* Take steps to avoid spinning forever if there is a protocol error.
  **
  ** Circumstances that cause a RETRY should only last for the briefest
  ** instances of time.  No I/O or other system calls are done while the
  ** locks are held, so the locks should not be held for very long. But 
  ** if we are unlucky, another process that is holding a lock might get
  ** paged out or take a page-fault that is time-consuming to resolve, 
  ** during the few nanoseconds that it is holding the lock.  In that case,
  ** it might take longer than normal for the lock to free.
  **
  ** After 5 RETRYs, we begin calling sqlite3OsSleep().  The first few
  ** calls to sqlite3OsSleep() have a delay of 1 microsecond.  Really this
  ** is more of a scheduler yield than an actual delay.  But on the 10th
  ** an subsequent retries, the delays start becoming longer and longer, 
  ** so that on the 100th (and last) RETRY we delay for 323 milliseconds.
  ** The total delay time before giving up is less than 10 seconds.
  */
  if( cnt>5 ){
    int nDelay = 1;                      /* Pause time in microseconds */
    if( cnt>100 ){
      VVA_ONLY( pWal->lockError = 1; )
      return SQLITE_PROTOCOL;
    }
    if( cnt>=10 ) nDelay = (cnt-9)*(cnt-9)*39;
    sqlite3OsSleep(pWal->pVfs, nDelay);
  }

  if( !useWal ){
    rc = walIndexReadHdr(pWal, pChanged);
    if( rc==SQLITE_BUSY ){
      /* If there is not a recovery running in another thread or process
      ** then convert BUSY errors to WAL_RETRY.  If recovery is known to
      ** be running, convert BUSY to BUSY_RECOVERY.  There is a race here
      ** which might cause WAL_RETRY to be returned even if BUSY_RECOVERY
      ** would be technically correct.  But the race is benign since with
      ** WAL_RETRY this routine will be called again and will probably be
      ** right on the second iteration.
      */
      if( pWal->apWiData[0]==0 ){
        /* This branch is taken when the xShmMap() method returns SQLITE_BUSY.
        ** We assume this is a transient condition, so return WAL_RETRY. The
        ** xShmMap() implementation used by the default unix and win32 VFS 
        ** modules may return SQLITE_BUSY due to a race condition in the 
        ** code that determines whether or not the shared-memory region 
        ** must be zeroed before the requested page is returned.
        */
        rc = WAL_RETRY;
      }else if( SQLITE_OK==(rc = walLockShared(pWal, WAL_RECOVER_LOCK)) ){
        walUnlockShared(pWal, WAL_RECOVER_LOCK);
        rc = WAL_RETRY;
      }else if( rc==SQLITE_BUSY ){
        rc = SQLITE_BUSY_RECOVERY;
      }
    }
    if( rc!=SQLITE_OK ){
      return rc;
    }
  }

  pInfo = walCkptInfo(pWal);
  if( !useWal && pInfo->nBackfill==pWal->hdr.mxFrame ){
    /* The WAL has been completely backfilled (or it is empty).
    ** and can be safely ignored.
    */
    rc = walLockShared(pWal, WAL_READ_LOCK(0));
    walShmBarrier(pWal);
    if( rc==SQLITE_OK ){
      if( memcmp((void *)walIndexHdr(pWal), &pWal->hdr, sizeof(WalIndexHdr)) ){
        /* It is not safe to allow the reader to continue here if frames
        ** may have been appended to the log before READ_LOCK(0) was obtained.
        ** When holding READ_LOCK(0), the reader ignores the entire log file,
        ** which implies that the database file contains a trustworthy
        ** snapshot. Since holding READ_LOCK(0) prevents a checkpoint from
        ** happening, this is usually correct.
        **
        ** However, if frames have been appended to the log (or if the log 
        ** is wrapped and written for that matter) before the READ_LOCK(0)
        ** is obtained, that is not necessarily true. A checkpointer may
        ** have started to backfill the appended frames but crashed before
        ** it finished. Leaving a corrupt image in the database file.
        */
        walUnlockShared(pWal, WAL_READ_LOCK(0));
        return WAL_RETRY;
      }
      pWal->readLock = 0;
      return SQLITE_OK;
    }else if( rc!=SQLITE_BUSY ){
      return rc;
    }
  }

  /* If we get this far, it means that the reader will want to use
  ** the WAL to get at content from recent commits.  The job now is
  ** to select one of the aReadMark[] entries that is closest to
  ** but not exceeding pWal->hdr.mxFrame and lock that entry.
  */
  mxReadMark = 0;
  mxI = 0;
  for(i=1; i<WAL_NREADER; i++){
    u32 thisMark = pInfo->aReadMark[i];
    if( mxReadMark<=thisMark && thisMark<=pWal->hdr.mxFrame ){
      assert( thisMark!=READMARK_NOT_USED );
      mxReadMark = thisMark;
      mxI = i;
    }
  }
  /* There was once an "if" here. The extra "{" is to preserve indentation. */
  {
    if( (pWal->readOnly & WAL_SHM_RDONLY)==0
     && (mxReadMark<pWal->hdr.mxFrame || mxI==0)
    ){
      for(i=1; i<WAL_NREADER; i++){
        rc = walLockExclusive(pWal, WAL_READ_LOCK(i), 1);
        if( rc==SQLITE_OK ){
          mxReadMark = pInfo->aReadMark[i] = pWal->hdr.mxFrame;
          mxI = i;
          walUnlockExclusive(pWal, WAL_READ_LOCK(i), 1);
          break;
        }else if( rc!=SQLITE_BUSY ){
          return rc;
        }
      }
    }
    if( mxI==0 ){
      assert( rc==SQLITE_BUSY || (pWal->readOnly & WAL_SHM_RDONLY)!=0 );
      return rc==SQLITE_BUSY ? WAL_RETRY : SQLITE_READONLY_CANTLOCK;
    }

    rc = walLockShared(pWal, WAL_READ_LOCK(mxI));
    if( rc ){
      return rc==SQLITE_BUSY ? WAL_RETRY : rc;
    }
    /* Now that the read-lock has been obtained, check that neither the
    ** value in the aReadMark[] array or the contents of the wal-index
    ** header have changed.
    **
    ** It is necessary to check that the wal-index header did not change
    ** between the time it was read and when the shared-lock was obtained
    ** on WAL_READ_LOCK(mxI) was obtained to account for the possibility
    ** that the log file may have been wrapped by a writer, or that frames
    ** that occur later in the log than pWal->hdr.mxFrame may have been
    ** copied into the database by a checkpointer. If either of these things
    ** happened, then reading the database with the current value of
    ** pWal->hdr.mxFrame risks reading a corrupted snapshot. So, retry
    ** instead.
    **
    ** This does not guarantee that the copy of the wal-index header is up to
    ** date before proceeding. That would not be possible without somehow
    ** blocking writers. It only guarantees that a dangerous checkpoint or 
    ** log-wrap (either of which would require an exclusive lock on
    ** WAL_READ_LOCK(mxI)) has not occurred since the snapshot was valid.
    */
    walShmBarrier(pWal);
    if( pInfo->aReadMark[mxI]!=mxReadMark
     || memcmp((void *)walIndexHdr(pWal), &pWal->hdr, sizeof(WalIndexHdr))
    ){
      walUnlockShared(pWal, WAL_READ_LOCK(mxI));
      return WAL_RETRY;
    }else{
      assert( mxReadMark<=pWal->hdr.mxFrame );
      pWal->readLock = (i16)mxI;
    }
  }
  return rc;
}


/*
** Information about the current state of the WAL file and where
** the next fsync should occur - passed from sqlite3WalFrames() into
** walWriteToLog().
*/
typedef struct WalWriter {
  Wal *pWal;                   /* The complete WAL information */
  sqlite3_file *pFd;           /* The WAL file to which we write */
  sqlite3_int64 iSyncPoint;    /* Fsync at this offset */
  int syncFlags;               /* Flags for the fsync */
  int szPage;                  /* Size of one page */
} WalWriter;

/*
** Write iAmt bytes of content into the WAL file beginning at iOffset.
** Do a sync when crossing the p->iSyncPoint boundary.
**
** In other words, if iSyncPoint is in between iOffset and iOffset+iAmt,
** first write the part before iSyncPoint, then sync, then write the
** rest.
*/
static int walWriteToLog(
  WalWriter *p,              /* WAL to write to */
  void *pContent,            /* Content to be written */
  int iAmt,                  /* Number of bytes to write */
  sqlite3_int64 iOffset      /* Start writing at this offset */
){
  int rc;
  if( iOffset<p->iSyncPoint && iOffset+iAmt>=p->iSyncPoint ){
    int iFirstAmt = (int)(p->iSyncPoint - iOffset);
    rc = sqlite3OsWrite(p->pFd, pContent, iFirstAmt, iOffset);
    if( rc ) return rc;
    iOffset += iFirstAmt;
    iAmt -= iFirstAmt;
    pContent = (void*)(iFirstAmt + (char*)pContent);
    assert( p->syncFlags & (SQLITE_SYNC_NORMAL|SQLITE_SYNC_FULL) );
    rc = sqlite3OsSync(p->pFd, p->syncFlags & SQLITE_SYNC_MASK);
    if( iAmt==0 || rc ) return rc;
  }
  rc = sqlite3OsWrite(p->pFd, pContent, iAmt, iOffset);
  return rc;
}

/*
** Write out a single frame of the WAL
*/
static int walWriteOneFrame(
  WalWriter *p,               /* Where to write the frame */
  PgHdr *pPage,               /* The page of the frame to be written */
  int nTruncate,              /* The commit flag.  Usually 0.  >0 for commit */
  sqlite3_int64 iOffset       /* Byte offset at which to write */
){
  int rc;                         /* Result code from subfunctions */
  void *pData;                    /* Data actually written */
  u8 aFrame[WAL_FRAME_HDRSIZE];   /* Buffer to assemble frame-header in */
#if defined(SQLITE_HAS_CODEC)
  if( (pData = sqlite3PagerCodec(pPage))==0 ) return SQLITE_NOMEM;
#else
  pData = pPage->pData;
#endif
  walEncodeFrame(p->pWal, pPage->pgno, nTruncate, pData, aFrame);
  rc = walWriteToLog(p, aFrame, sizeof(aFrame), iOffset);
  if( rc ) return rc;
  /* Write the page data */
  rc = walWriteToLog(p, pData, p->szPage, iOffset+sizeof(aFrame));
  return rc;
}






int sqlite3WalOpen(
    sqlite3_vfs *pVfs, /* vfs module to open wal and wal-index */
    sqlite3_file *pDbFd, /* The open database file */
    const char *zWalName, /* Name of the WAL file */
    int bNoShm, /* True to run in heap-memory mode */
    i64 mxWalSize, /* Truncate WAL to this size on reset */
    Wal **ppWal, /* OUT: Allocated Wal handle */
    void *walData
){
    int iDC;
    Wal *pRet = NULL;

    if (zWalName != NULL)
		pRet = ((db_thread*)walData)->curConn->wal;
	if (!pRet)
	{
		pRet = malloc(sizeof(Wal));
		memset(pRet,0,sizeof(Wal));
	}

    // if(!pRet)
    // {
    //     return SQLITE_NOMEM;
    // }
    pRet->pVfs = pVfs;
    pRet->pDbFd = pDbFd;
    pRet->thread = (db_thread*)walData;
    pRet->pWalFd = pRet->thread->walFile->pWalFd;
    // pRet->thread->curConn->pWal = pRet;

    // No one is accessing this db other than current thread.
    pRet->exclusiveMode = WAL_HEAPMEMORY_MODE;
    pRet->padToSectorBoundary = 1;
    pRet->syncHeader = 0;
    pRet->readLock = -1;

    iDC = sqlite3OsDeviceCharacteristics(pDbFd);
    if( iDC & SQLITE_IOCAP_SEQUENTIAL ){ pRet->syncHeader = 0; }
    if( iDC & SQLITE_IOCAP_POWERSAFE_OVERWRITE ){
      pRet->padToSectorBoundary = 0;
    }
    *ppWal = pRet;

    return SQLITE_OK;
}

int sqlite3WalClose(
    Wal *pWal, /* Wal to close */
    int sync_flags, /* Flags to pass to OsSync() (or 0) */
    int nBuf,
    u8 *zBuf /* Buffer of at least nBuf bytes */
){
    if (pWal)
    {
      walIndexClose(pWal, 0);
      sqlite3WalClose(pWal->prev,sync_flags,nBuf,zBuf);
      sqlite3_free((void *)pWal->apWiData);
      sqlite3_free(pWal);
    }
    return SQLITE_OK;
}

/* Write a frame or frames to the log. */
int sqlite3WalFrames(
    Wal **pWal, /* Wal handle to write to, can be moved */
    int szPage, /* Database page-size in bytes */
    PgHdr *pList, /* List of dirty pages to write */
    Pgno nTruncate, /* Database size after this commit */
    int isCommit, /* True if this is a commit */
    int sync_flags /* Flags to pass to OsSync() (or 0) */
){
  int rc;                         /* Used to catch return codes */
  u32 iFrame;                     /* Next frame address */
  PgHdr *p;                       /* Iterator to run through pList with. */
  PgHdr *pLast = 0;               /* Last frame in list */
  int nExtra = 0;                 /* Number of extra copies of last page */
  int szFrame;                    /* The size of a single frame */
  i64 iOffset;                    /* Next byte to write in WAL file */
  WalWriter w;                    /* The writer */
  db_connection *con = (*pWal)->thread->curConn;
  
  assert( pList );
  /* If this frame set completes a transaction, then nTruncate>0.  If
  ** nTruncate==0 then this frame set does not complete the transaction. */
  assert( (isCommit!=0)==(nTruncate!=0) );

  // Do we need to create new wal file?
  if ((*pWal)->thread->walFile->mxFrame > 1024*3)
  {
  	char filename[MAX_PATHNAME];
  	snprintf(filename,MAX_PATHNAME,"%s/wal.%llu",(*pWal)->thread->path,(*pWal)->thread->walFile->walIndex+1);
  	DBG(("Creating new wal!\r\n"));
  	wal_file *nw = new_wal_file(filename,(*pWal)->thread->vfs);
  	nw->walIndex = (*pWal)->thread->walFile->walIndex+1;
  	nw->prev = (*pWal)->thread->walFile;
  	(*pWal)->thread->walFile = nw;
  }

  // Do we need to create a new wal structure for newer file?
  if ((*pWal)->thread->walFile->walIndex > (*pWal)->walIndex)
  {
  	DBG(("OPENING INTO NEW WAL FILE %d\r\n",(*pWal)->thread->curConn->connindex));
    Wal *newWal;
    int changed;
    sqlite3WalOpen((*pWal)->pVfs, (*pWal)->pDbFd, NULL, 1, 0, &newWal,(void*)(*pWal)->thread);
    sqlite3WalEndReadTransaction(*pWal);
    sqlite3WalEndWriteTransaction(*pWal);
    newWal->prev = *pWal;
    newWal->pWalFd = (*pWal)->thread->walFile->pWalFd;
    newWal->walIndex = (*pWal)->thread->walFile->walIndex;
    
    (*pWal)->thread->curConn->wal = newWal;
    *pWal = newWal;

    sqlite3WalBeginReadTransaction(*pWal,&changed);
    sqlite3WalBeginWriteTransaction(*pWal);
  }

  iFrame = (*pWal)->thread->walFile->mxFrame;

  (*pWal)->szPage = szPage;
  if( iFrame==0 ){
    u8 aWalHdr[WAL_HDRSIZE];      /* Buffer to assemble wal-header in */
    u32 aCksum[2];                /* Checksum for wal-header */

    sqlite3Put4byte(&aWalHdr[0], (WAL_MAGIC | SQLITE_BIGENDIAN));
    sqlite3Put4byte(&aWalHdr[4], WAL_MAX_VERSION);
    sqlite3Put4byte(&aWalHdr[8], szPage);
    writeUInt64(&aWalHdr[12], (*pWal)->thread->walFile->walIndex);
    // if( (*pWal)->nCkpt==0 )
    // {
      (*pWal)->hdr.aSalt[0] = 123456789;
      (*pWal)->hdr.aSalt[1] = 987654321;
      sqlite3Put4byte(&aWalHdr[20],123456789);
      sqlite3Put4byte(&aWalHdr[24], 987654321);
    // }
    // memcpy(&aWalHdr[20], (*pWal)->hdr.aSalt, 8);
    sqlite3Put4byte(&aWalHdr[28], 0);
    walChecksumBytes((u8)((WAL_MAGIC | SQLITE_BIGENDIAN)&0x00000001) == SQLITE_BIGENDIAN, aWalHdr, WAL_HDRSIZE-2*4, 0, aCksum);
    sqlite3Put4byte(&aWalHdr[32], aCksum[0]);
    sqlite3Put4byte(&aWalHdr[36], aCksum[1]);
    
    (*pWal)->hdr.bigEndCksum = SQLITE_BIGENDIAN;
    (*pWal)->hdr.aFrameCksum[0] = aCksum[0];
    (*pWal)->hdr.aFrameCksum[1] = aCksum[1];
    (*pWal)->truncateOnCommit = 1;

    rc = sqlite3OsWrite((*pWal)->pWalFd, aWalHdr, sizeof(aWalHdr), 0);
    WALTRACE(("WAL%p: wal-header write %s\n", *pWal, rc ? "failed" : "ok"));
    if( rc!=SQLITE_OK ){
      return rc;
    }

    /* Sync the header (unless SQLITE_IOCAP_SEQUENTIAL is true or unless
    ** all syncing is turned off by PRAGMA synchronous=OFF).  Otherwise
    ** an out-of-order write following a WAL restart could result in
    ** database corruption.  See the ticket:
    **
    **     http://localhost:591/sqlite/info/ff5be73dee
    */
    if( (*pWal)->syncHeader && sync_flags ){
      rc = sqlite3OsSync((*pWal)->pWalFd, sync_flags & SQLITE_SYNC_MASK);
      if( rc ) return rc;
    }
  }
  else if ((*pWal)->hdr.aSalt[0] != 123456789)
  {
  	(*pWal)->hdr.bigEndCksum = SQLITE_BIGENDIAN;
	(*pWal)->truncateOnCommit = 1;
	(*pWal)->hdr.aSalt[0] = 123456789;
	(*pWal)->hdr.aSalt[1] = 987654321;
  }

  /* Setup information needed to write frames into the WAL */
  w.pWal = *pWal;
  w.pFd = (*pWal)->pWalFd;
  w.iSyncPoint = 0;
  w.syncFlags = sync_flags;
  w.szPage = szPage;
  iOffset = walFrameOffset(iFrame+1, szPage);
  szFrame = szPage + WAL_FRAME_HDRSIZE;

  /* Write all frames into the log file exactly once */
  for(p=pList; p; p=p->pDirty){
    int nDbSize;   /* 0 normally.  Positive == commit flag */
    iFrame++;
    con->nPages++;
    assert( iOffset==walFrameOffset(iFrame, szPage) );
    nDbSize = (isCommit && p->pDirty==0) ? nTruncate : 0;
    rc = walWriteOneFrame(&w, p, nDbSize, iOffset);
    if( rc ) return rc;
    pLast = p;
    iOffset += szFrame;
  }


  /* If this is the end of a transaction, then we might need to pad
  ** the transaction and/or sync the WAL file.
  **
  ** Padding and syncing only occur if this set of frames complete a
  ** transaction and if PRAGMA synchronous=FULL.  If synchronous==NORMAL
  ** or synchronous==OFF, then no padding or syncing are needed.
  **
  ** If SQLITE_IOCAP_POWERSAFE_OVERWRITE is defined, then padding is not
  ** needed and only the sync is done.  If padding is needed, then the
  ** final frame is repeated (with its commit mark) until the next sector
  ** boundary is crossed.  Only the part of the WAL prior to the last
  ** sector boundary is synced; the part of the last frame that extends
  ** past the sector boundary is written after the sync.
  */
  if( isCommit && (sync_flags & WAL_SYNC_TRANSACTIONS)!=0 ){
    if( (*pWal)->padToSectorBoundary ){
      int sectorSize = sqlite3SectorSize((*pWal)->pWalFd);
      w.iSyncPoint = ((iOffset+sectorSize-1)/sectorSize)*sectorSize;
      while( iOffset<w.iSyncPoint ){
        rc = walWriteOneFrame(&w, pLast, nTruncate, iOffset);
        if( rc ) return rc;
        iOffset += szFrame;
        nExtra++;
      }
    }else{
      rc = sqlite3OsSync(w.pFd, sync_flags & SQLITE_SYNC_MASK);
    }
  }

  /* If this frame set completes the first transaction in the WAL and
  ** if PRAGMA journal_size_limit is set, then truncate the WAL to the
  ** journal size limit, if possible.
  */
  // if( isCommit && pWal->truncateOnCommit && pWal->mxWalSize>=0 ){
  //   i64 sz = pWal->mxWalSize;
  //   if( walFrameOffset(iFrame+nExtra+1, szPage)>pWal->mxWalSize ){
  //     sz = walFrameOffset(iFrame+nExtra+1, szPage);
  //   }
  //   walLimitSize(pWal, sz);
  //   pWal->truncateOnCommit = 0;
  // }

  /* Append data to the wal-index. It is not necessary to lock the 
  ** wal-index to do this as the SQLITE_SHM_WRITE lock held on the wal-index
  ** guarantees that there are no other writers, and no data that may
  ** be in use by existing readers is being overwritten.
  */
  iFrame = (*pWal)->thread->walFile->mxFrame;
  for(p=pList; p && rc==SQLITE_OK; p=p->pDirty){
    iFrame++;
    rc = walIndexAppend(*pWal, iFrame, p->pgno);
  }
  while( rc==SQLITE_OK && nExtra>0 ){
    iFrame++;
    nExtra--;
    rc = walIndexAppend(*pWal, iFrame, pLast->pgno);
  }

  if( rc==SQLITE_OK ){
    /* Update the private copy of the header. */
    (*pWal)->hdr.szPage = (u16)((szPage&0xff00) | (szPage>>16));
    testcase( szPage<=32768 );
    testcase( szPage>=65536 );
    (*pWal)->hdr.mxFrame = iFrame;
    (*pWal)->thread->walFile->mxFrame = iFrame;
    if( isCommit ){
      (*pWal)->hdr.iChange++;
      (*pWal)->hdr.nPage = nTruncate;
      (*pWal)->thread->walFile->lastCommit = iFrame;
    }
    /* If this is a commit, update the wal-index header too. */
    if( isCommit ){
      walIndexWriteHdr(*pWal);
      (*pWal)->iCallback = iFrame;
    }
  }

  return rc;
}

/* Read a page from the write-ahead log, if it is present. */
int sqlite3WalFindFrame(
    Wal *pWal, /* WAL handle */
    Pgno pgno, /* Database page number to read data for */
    u32 *piRead, /* OUT: Frame number (or zero) */
    u64 *walIndex /* OUT: What wal file it is in */
){
    u32 iRead = 0;                  /* If !=0, WAL frame to return data from */
    u32 iLast = pWal->hdr.mxFrame;  /* Last page in WAL for this reader */
    int iHash;                      /* Used to loop through N hash tables */
    int rc;
    // DBG(("Wal find frame %d, last %d\r\n",pgno, iLast));

    if( iLast==0 || pWal->readLock==0 ){
      if (iLast == 0 && pWal->prev)
        return sqlite3WalFindFrame(pWal->prev,pgno,piRead,walIndex);
      else
        *piRead = 0;
      
      return SQLITE_OK;
    }

    for(iHash=walFramePage(iLast); iHash>=0 && iRead==0; iHash--)
    {
      ht_slot *aHash;      /* Pointer to hash table */
      u32 *aPgno;          /* Pointer to array of page numbers */
      u32 iZero;           /* Frame number corresponding to aPgno[0] */
      int iKey;            /* Hash slot index */
      int nCollide;        /* Number of hash collisions remaining */

      rc = walHashGet(pWal, iHash, &aHash, &aPgno, &iZero);
      if( rc!=SQLITE_OK )
      {
        if (pWal->prev)
          return sqlite3WalFindFrame(pWal->prev,pgno,piRead,walIndex);
        else
          return rc;
      }

      nCollide = HASHTABLE_NSLOT;
      for(iKey=walHash(pgno); aHash[iKey]; iKey=walNextHash(iKey))
      {
        u32 iFrame = aHash[iKey] + iZero;
        if( iFrame<=iLast && aPgno[aHash[iKey]]==pgno )
        {
          /* assert( iFrame>iRead ); -- not true if there is corruption */
          iRead = iFrame;
        }
        if( (nCollide--)==0 )
        {
          return SQLITE_CORRUPT_BKPT;
        }
      }
    }

    *walIndex = pWal->walIndex;
    *piRead = iRead;
    return SQLITE_OK;
}

// Called immediately after sqlite3WalFindFrame
// We added walIndex
int sqlite3WalReadFrame(
    Wal *pWal, /* WAL handle */
    u32 iRead, /* Frame to read */
    u64 walIndex, /* Which WAL frame belongs to */
    int nOut, /* Size of buffer pOut in bytes */
    u8 *pOut /* Buffer to write page data to */
){
  int sz;
  i64 iOffset;
  sz = pWal->hdr.szPage;
  sz = (sz&0xfe00) + ((sz&0x0001)<<16);
  testcase( sz<=32768 );
  testcase( sz>=65536 );

  // Wal always points to first wal, but reads are always from last to first.
  // So if walIndex different, it must be one of the next ones.
  if (walIndex != pWal->walIndex)
  {
    assert(walIndex < pWal->walIndex);
    return sqlite3WalReadFrame(pWal->prev,iRead,walIndex,nOut,pOut);
  }
  else
  {
    iOffset = walFrameOffset(iRead, sz) + WAL_FRAME_HDRSIZE;
    /* testcase( IS_BIG_INT(iOffset) ); // requires a 4GiB WAL */
    return sqlite3OsRead(pWal->pWalFd, pOut, (nOut>sz ? sz : nOut), iOffset);
  }
}

/* Copy pages from the log to the database file */
int sqlite3WalCheckpoint(
    Wal *pWalTop, /* Write-ahead log connection */
    int eMode, /* One of PASSIVE, FULL and RESTART */
    int (*xBusy)(void*), /* Function to call when busy */
    void *pBusyArg, /* Context argument for xBusyHandler */
    int sync_flags, /* Flags to sync db file with (or 0) */
    int nBuf, /* Size of buffer nBuf */
    u8 *zBuf, /* Temporary buffer to use */
    int *pnLog, /* OUT: Number of frames in WAL */
    int *pnCkpt /* OUT: Number of backfilled frames in WAL */
){
  Wal *pWal;
  int rc = SQLITE_OK;
  int isChanged = 0;              /* True if a new wal-index header is loaded */
  int eMode2 = eMode;             /* Mode to pass to walCheckpoint() */

  // Only checkpoint prev wals (if they are prev they are finished)
  if (pWalTop->prev)
  {
    // If more than 1 wal behind, move back
    // Only checkpoint the first file even if more than 1 behind.
    if (pWalTop->prev->prev)
    {
    	return sqlite3WalCheckpoint(pWalTop->prev,eMode,xBusy,pBusyArg,sync_flags,nBuf,zBuf,pnLog,pnCkpt);	
    }
    else
    {
      int rch;
      pWal = pWalTop->prev;
      sqlite3WalBeginReadTransaction(pWal,&rch);
      sqlite3WalBeginWriteTransaction(pWal);
    }
  }
  else
  {
  	return SQLITE_OK;
  }
    


//  rc = walLockExclusive(pWal, WAL_CKPT_LOCK, 1);
//  if( rc ){
    /* Usually this is SQLITE_BUSY meaning that another thread or process
    ** is already running a checkpoint, or maybe a recovery.  But it might
    ** also be SQLITE_IOERR. */
//    return rc;
//  }
  pWal->ckptLock = 1;

  /* Read the wal-index header. */
  if( rc==SQLITE_OK ){
    rc = walIndexReadHdr(pWal, &isChanged);
    if( isChanged && pWal->pDbFd->pMethods->iVersion>=3 ){
      sqlite3OsUnfetch(pWal->pDbFd, 0, 0);
    }
  }

  /* Copy data from the log to the database file. */
  if( rc==SQLITE_OK ){
    if( pWal->hdr.mxFrame && walPagesize(pWal)!=nBuf ){
      rc = SQLITE_CORRUPT_BKPT;
    }else{
      rc = walCheckpoint(pWal, eMode2, xBusy, pBusyArg, sync_flags, zBuf);
    }

    /* If no error occurred, set the output variables. */
    if( rc==SQLITE_OK || rc==SQLITE_BUSY ){
      if( pnLog ) *pnLog = (int)pWal->hdr.mxFrame;
      if( pnCkpt ) *pnCkpt = (int)(walCkptInfo(pWal)->nBackfill);
    }
  }

  if( isChanged ){
    /* If a new wal-index header was loaded before the checkpoint was 
    ** performed, then the pager-cache associated with pWal is now
    ** out of date. So zero the cached wal-index header to ensure that
    ** next time the pager opens a snapshot on this database it knows that
    ** the cache needs to be reset.
    */
    memset(&pWal->hdr, 0, sizeof(WalIndexHdr));
  }


  sqlite3WalEndWriteTransaction(pWal);
  walUnlockExclusive(pWal, WAL_CKPT_LOCK, 1);
  pWal->ckptLock = 0;

  if (rc == SQLITE_OK)
  {
    sqlite3WalClose(pWal, sync_flags, nBuf,zBuf);
    pWalTop->prev = NULL;
  }
  return (rc==SQLITE_OK && eMode!=eMode2 ? SQLITE_BUSY : rc);
}

/* Undo any frames written (but not committed) to the log */
int sqlite3WalUndo(Wal *pWal, int (*xUndo)(void *, Pgno), void *pUndoCtx)
{
  int rc = SQLITE_OK;
  // DBG(("sqlite3WalUndo, mxframe %d, threadmax %d\r\n",pWal->hdr.mxFrame, pWal->thread->walFile->mxFrame));
  if( ALWAYS(pWal->writeLock) )
  {
    Pgno iMax = pWal->hdr.mxFrame;
    Pgno iFrame;
  
    /* Restore the clients cache of the wal-index header to the state it
    ** was in before the client began writing to the database. 
    */
    memcpy(&pWal->hdr, (void *)walIndexHdr(pWal), sizeof(WalIndexHdr));

    if (xUndo != NULL)
    {
      // We now have multiple dbs in wal. We cant rely on mxFrame+1 because it might belong to another db.
      // Use last commit number.
      // This means that when a write fails, it must be rollbacked immediately. Before another db might write to wal.
      for(iFrame=pWal->thread->walFile->lastCommit+1;  // pWal->hdr.mxFrame+1
        ALWAYS(rc==SQLITE_OK) && iFrame<=iMax;
        iFrame++
      ){
        /* This call cannot fail. Unless the page for which the page number
        ** is passed as the second argument is (a) in the cache and 
        ** (b) has an outstanding reference, then xUndo is either a no-op
        ** (if (a) is false) or simply expels the page from the cache (if (b)
        ** is false).
        **
        ** If the upper layer is doing a rollback, it is guaranteed that there
        ** are no outstanding references to any page other than page 1. And
        ** page 1 is never written to the log until the transaction is
        ** committed. As a result, the call to xUndo may not fail.
        */
        assert( walFramePgno(pWal, iFrame)!=1 );
        rc = xUndo(pUndoCtx, walFramePgno(pWal, iFrame));
      }
    }
    if( iMax!=pWal->hdr.mxFrame ) walCleanupHash(pWal);
    // If we can move back thread wal do so (no writes to other actors came after it)
    if (pWal->walIndex == pWal->thread->walFile->walIndex &&
        iMax == pWal->thread->walFile->mxFrame)
    {
      pWal->thread->walFile->mxFrame = pWal->thread->walFile->lastCommit;
      assert(iMax > pWal->hdr.mxFrame);
    }
    pWal->thread->curConn->nPages -= (iMax - pWal->hdr.mxFrame);
  }
  return rc;
}

/* Return an integer that records the current (uncommitted) write
** position in the WAL */
void sqlite3WalSavepoint(Wal *pWal, u32 *aWalData)
{
  assert( pWal->writeLock );
  aWalData[0] = pWal->hdr.mxFrame;
  aWalData[1] = pWal->hdr.aFrameCksum[0];
  aWalData[2] = pWal->hdr.aFrameCksum[1];
  aWalData[3] = pWal->nCkpt;
}

/* Move the write position of the WAL back to iFrame. Called in
** response to a ROLLBACK TO command. */
int sqlite3WalSavepointUndo(Wal *pWal, u32 *aWalData)
{
  int rc = SQLITE_OK;

  assert( pWal->writeLock );
  assert( aWalData[3]!=pWal->nCkpt || aWalData[0]<=pWal->hdr.mxFrame );

  if( aWalData[3]!=pWal->nCkpt ){
    /* This savepoint was opened immediately after the write-transaction
    ** was started. Right after that, the writer decided to wrap around
    ** to the start of the log. Update the savepoint values to match.
    */
    aWalData[0] = 0;
    aWalData[3] = pWal->nCkpt;
  }

  if( aWalData[0]<pWal->hdr.mxFrame ){
    pWal->hdr.mxFrame = aWalData[0];
    pWal->hdr.aFrameCksum[0] = aWalData[1];
    pWal->hdr.aFrameCksum[1] = aWalData[2];
    walCleanupHash(pWal);

    // If we can move back thread wal do so (no writes to other actors came after it)
    if (pWal->walIndex == pWal->thread->walFile->walIndex &&
        pWal->hdr.mxFrame == pWal->thread->walFile->mxFrame)
      pWal->thread->walFile->mxFrame = pWal->hdr.mxFrame;
  }

  return rc;
}





/* If the WAL is not empty, return the size of the database. */
Pgno sqlite3WalDbsize(Wal *pWal){
  if( pWal && ALWAYS(pWal->readLock>=0) ){
    return pWal->hdr.nPage;
  }
  return 0;
}

void sqlite3WalLimit(Wal *pWal, i64 iLimit)
{
}

/*
** Begin a read transaction on the database.
**
** This routine used to be called sqlite3OpenSnapshot() and with good reason:
** it takes a snapshot of the state of the WAL and wal-index for the current
** instant in time.  The current thread will continue to use this snapshot.
** Other threads might append new content to the WAL and wal-index but
** that extra content is ignored by the current thread.
**
** If the database contents have changes since the previous read
** transaction, then *pChanged is set to 1 before returning.  The
** Pager layer will use this to know that is cache is stale and
** needs to be flushed.
*/
int sqlite3WalBeginReadTransaction(Wal *pWal, int *pChanged){
  int rc;                         /* Return code */
  int cnt = 0;                    /* Number of TryBeginRead attempts */

  do{
    rc = walTryBeginRead(pWal, pChanged, 0, ++cnt);
  }while( rc==WAL_RETRY );
  // DBG(("START READ TRANSACTION result=%d, changed %d\r\n",rc,*pChanged));
  testcase( (rc&0xff)==SQLITE_BUSY );
  testcase( (rc&0xff)==SQLITE_IOERR );
  testcase( rc==SQLITE_PROTOCOL );
  testcase( rc==SQLITE_OK );
  return rc;
}

/*
** Finish with a read transaction.  All this does is release the
** read-lock.
*/
void sqlite3WalEndReadTransaction(Wal *pWal){
  sqlite3WalEndWriteTransaction(pWal);
  if( pWal->readLock>=0 ){
    walUnlockShared(pWal, WAL_READ_LOCK(pWal->readLock));
    pWal->readLock = -1;
  }
}

int sqlite3WalCallback(Wal *pWal)
{
    return SQLITE_OK;
}

int sqlite3WalExclusiveMode(Wal *pWal, int op)
{
    return SQLITE_OK;
}

int sqlite3WalHeapMemory(Wal *pWal)
{
    return pWal != NULL;
}

/* 
** This function starts a write transaction on the WAL.
**
** A read transaction must have already been started by a prior call
** to sqlite3WalBeginReadTransaction().
**
** If another thread or process has written into the database since
** the read transaction was started, then it is not possible for this
** thread to write as doing so would cause a fork.  So this routine
** returns SQLITE_BUSY in that case and no write transaction is started.
**
** There can only be a single writer active at a time.
*/
int sqlite3WalBeginWriteTransaction(Wal *pWal){
  int rc; 

  /* Cannot start a write transaction without first holding a read
  ** transaction. */
  assert( pWal->readLock>=0 );

  if( pWal->readOnly ){
    return SQLITE_READONLY;
  }

  /* Only one writer allowed at a time.  Get the write lock.  Return
  ** SQLITE_BUSY if unable.
  */
  rc = walLockExclusive(pWal, WAL_WRITE_LOCK, 1);
  if( rc ){
    return rc;
  }
  pWal->writeLock = 1;

  /* If another connection has written to the database file since the
  ** time the read transaction on this connection was started, then
  ** the write is disallowed.
  */
  // if( memcmp(&pWal->hdr, (void *)walIndexHdr(pWal), sizeof(WalIndexHdr))!=0 ){
  //   walUnlockExclusive(pWal, WAL_WRITE_LOCK, 1);
  //   pWal->writeLock = 0;
  //   rc = SQLITE_BUSY_SNAPSHOT;
  // }

  // DBG(("BEGIN WRITE %d\r\n",rc));

  return rc;
}


/*
** End a write transaction.  The commit has already been done.  This
** routine merely releases the lock.
*/
int sqlite3WalEndWriteTransaction(Wal *pWal){
  if( pWal->writeLock ){
    walUnlockExclusive(pWal, WAL_WRITE_LOCK, 1);
    pWal->writeLock = 0;
    pWal->truncateOnCommit = 0;
  }
  return SQLITE_OK;
}


// New function. It adds thread pointer to wal structure.
SQLITE_API int sqlite3_wal_data(
  sqlite3 *db,
  void *pArg
  ){

  int rt = SQLITE_NOTFOUND;
  int i;
  for(i=0; i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ){
      Pager *pPager = sqlite3BtreePager(pBt);
      if (pPager->pWal)
      {
          pPager->pWal->thread = (db_thread*)pArg;
          rt = SQLITE_OK;
      }
      else
      {
        pPager->walData = pArg;
        rt = SQLITE_OK;
      }
    }
  }
  return rt;
}

void writeUInt64(unsigned char* buf, unsigned long long num)
{
  buf[0] = num >> 56;
  buf[1] = num >> 48;
  buf[2] = num >> 40;
  buf[3] = num >> 32;
  buf[4] = num >> 24;
  buf[5] = num >> 16;
  buf[6] = num >> 8;
  buf[7] = num;
}

u64 readUInt64(u8* buf)
{
  return ((u64)buf[0] << 56) + ((u64)buf[1] << 48) + ((u64)buf[2] << 40) + ((u64)buf[3] << 32) + 
       ((u64)buf[4] << 24) + ((u64)buf[5] << 16)  + ((u64)buf[6] << 8) + buf[7];
}

#endif /* #ifndef SQLITE_OMIT_WAL */

/************** End of wal.c ***********************************************/