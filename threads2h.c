// btree version threads2h pthread rw lock version
// 24 JAN 2014

// author: karl malbrain, malbrain@cal.berkeley.edu

/*
This work, including the source code, documentation
and related data, is placed into the public domain.

The orginal author is Karl Malbrain.

THIS SOFTWARE IS PROVIDED AS-IS WITHOUT WARRANTY
OF ANY KIND, NOT EVEN THE IMPLIED WARRANTY OF
MERCHANTABILITY. THE AUTHOR OF THIS SOFTWARE,
ASSUMES _NO_ RESPONSIBILITY FOR ANY CONSEQUENCE
RESULTING FROM THE USE, MODIFICATION, OR
REDISTRIBUTION OF THIS SOFTWARE.
*/

// Please see the project home page for documentation
// code.google.com/p/high-concurrency-btree

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE

#ifdef linux
#define _GNU_SOURCE
#endif

#ifdef unix
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <process.h>
#include <intrin.h>
#endif

#include <memory.h>
#include <string.h>

typedef unsigned long long	uid;

#ifndef unix
typedef unsigned long long	off64_t;
typedef unsigned short		ushort;
typedef unsigned int		uint;
#endif

#define BT_latchtable	128					// number of latch manager slots

#define BT_ro 0x6f72	// ro
#define BT_rw 0x7772	// rw

#define BT_maxbits		24					// maximum page size in bits
#define BT_minbits		9					// minimum page size in bits
#define BT_minpage		(1 << BT_minbits)	// minimum page size
#define BT_maxpage		(1 << BT_maxbits)	// maximum page size

/*
There are five lock types for each node in three independent sets: 
1. (set 1) AccessIntent: Sharable. Going to Read the node. Incompatible with NodeDelete. 
2. (set 1) NodeDelete: Exclusive. About to release the node. Incompatible with AccessIntent. 
3. (set 2) ReadLock: Sharable. Read the node. Incompatible with WriteLock. 
4. (set 2) WriteLock: Exclusive. Modify the node. Incompatible with ReadLock and other WriteLocks. 
5. (set 3) ParentModification: Exclusive. Change the node's parent keys. Incompatible with another ParentModification. 
*/

typedef enum{
	BtLockAccess,
	BtLockDelete,
	BtLockRead,
	BtLockWrite,
	BtLockParent
} BtLock;

//	mode & definition for latch implementation

enum {
	Mutex = 1,
	Write = 2,
	Pending = 4,
	Share = 8
} LockMode;

// exclusive is set for write access
// share is count of read accessors
// grant write lock when share == 0

typedef struct {
	volatile ushort mutex:1;
	volatile ushort exclusive:1;
	volatile ushort pending:1;
	volatile ushort share:13;
} BtSpinLatch;

//  hash table entries

typedef struct {
	BtSpinLatch latch[1];
	volatile ushort slot;		// Latch table entry at head of chain
} BtHashEntry;

//	latch manager table structure

typedef struct {
#ifdef unix
	pthread_rwlock_t lock[1];
#else
	SRWLOCK srw[1];
#endif
} BtLatch;

typedef struct {
	BtLatch readwr[1];			// read/write page lock
	BtLatch access[1];			// Access Intent/Page delete
	BtLatch parent[1];			// adoption of foster children
	BtSpinLatch busy[1];		// slot is being moved between chains
	volatile ushort next;		// next entry in hash table chain
	volatile ushort prev;		// prev entry in hash table chain
	volatile ushort pin;		// number of outstanding locks
	volatile ushort hash;		// hash slot entry is under
	volatile uid page_no;		// latch set page number
} BtLatchSet;

//	Define the length of the page and key pointers

#define BtId 6

//	Page key slot definition.

//	If BT_maxbits is 15 or less, you can save 4 bytes
//	for each key stored by making the first two uints
//	into ushorts.  You can also save 4 bytes by removing
//	the tod field from the key.

//	Keys are marked dead, but remain on the page until
//	it cleanup is called. The fence key (highest key) for
//	the page is always present, even after cleanup.

typedef struct {
	uint off:BT_maxbits;		// page offset for key start
	uint dead:1;				// set for deleted key
	uint tod;					// time-stamp for key
	unsigned char id[BtId];		// id associated with key
} BtSlot;

//	The key structure occupies space at the upper end of
//	each page.  It's a length byte followed by the value
//	bytes.

typedef struct {
	unsigned char len;
	unsigned char key[1];
} *BtKey;

//	The first part of an index page.
//	It is immediately followed
//	by the BtSlot array of keys.

typedef struct Page {
	uint cnt;					// count of keys in page
	uint act;					// count of active keys
	uint min;					// next key offset
	unsigned char bits;			// page size in bits
	unsigned char lvl:6;		// level of page
	unsigned char kill:1;		// page is being deleted
	unsigned char dirty:1;		// page has deleted keys
	unsigned char right[BtId];	// page number to right
	BtSlot table[0];			// array of key slots
} *BtPage;

//	The memory mapping pool table buffer manager entry

typedef struct {
	unsigned long long int lru;	// number of times accessed
	uid  basepage;				// mapped base page number
	char *map;					// mapped memory pointer
	ushort slot;				// slot index in this array
	ushort pin;					// mapped page pin counter
	void *hashprev;				// previous pool entry for the same hash idx
	void *hashnext;				// next pool entry for the same hash idx
#ifndef unix
	HANDLE hmap;
#endif
} BtPool;

//	structure for latch manager on ALLOC_page

typedef struct {
	struct Page alloc[2];		// next & free page_nos in right ptr
	BtSpinLatch lock[1];		// allocation area lite latch
	ushort latchdeployed;		// highest number of latch entries deployed
	ushort nlatchpage;			// number of latch pages at BT_latch
	ushort latchtotal;			// number of page latch entries
	ushort latchhash;			// number of latch hash table slots
	ushort latchvictim;			// next latch entry to examine
	BtHashEntry table[0];		// the hash table
} BtLatchMgr;

//	The object structure for Btree access

typedef struct {
	uint page_size;				// page size	
	uint page_bits;				// page size in bits	
	uint seg_bits;				// seg size in pages in bits
	uint mode;					// read-write mode
#ifdef unix
	char *pooladvise;			// bit maps for pool page advisements
	int idx;
#else
	HANDLE idx;
#endif
	ushort poolcnt;				// highest page pool node in use
	ushort poolmax;				// highest page pool node allocated
	ushort poolmask;			// total number of pages in mmap segment - 1
	ushort hashsize;			// size of Hash Table for pool entries
	volatile uint evicted;		// last evicted hash table slot
	ushort *hash;				// pool index for hash entries
	BtSpinLatch *latch;			// latches for hash table slots
	BtLatchMgr *latchmgr;		// mapped latch page from allocation page
	BtLatchSet *latchsets;		// mapped latch set from latch pages
	BtPool *pool;				// memory pool page segments
#ifndef unix
	HANDLE halloc;				// allocation and latch table handle
#endif
} BtMgr;

typedef struct {
	BtMgr *mgr;			// buffer manager for thread
	BtPage cursor;		// cached frame for start/next (never mapped)
	BtPage frame;		// spare frame for the page split (never mapped)
	BtPage zero;		// page frame for zeroes at end of file
	BtPage page;		// current page
	uid page_no;		// current page number	
	uid cursor_page;	// current cursor page number	
	BtLatchSet *set;	// current page latch set
	BtPool *pool;		// current page pool
	unsigned char *mem;	// frame, cursor, page memory buffer
	int found;			// last delete or insert was found
	int err;			// last error
} BtDb;

typedef enum {
	BTERR_ok = 0,
	BTERR_struct,
	BTERR_ovflw,
	BTERR_lock,
	BTERR_map,
	BTERR_wrt,
	BTERR_hash
} BTERR;

// B-Tree functions
extern void bt_close (BtDb *bt);
extern BtDb *bt_open (BtMgr *mgr);
extern BTERR  bt_insertkey (BtDb *bt, unsigned char *key, uint len, uint lvl, uid id, uint tod);
extern BTERR  bt_deletekey (BtDb *bt, unsigned char *key, uint len, uint lvl);
extern uid bt_findkey    (BtDb *bt, unsigned char *key, uint len);
extern uint bt_startkey  (BtDb *bt, unsigned char *key, uint len);
extern uint bt_nextkey   (BtDb *bt, uint slot);

//	manager functions
extern BtMgr *bt_mgr (char *name, uint mode, uint bits, uint poolsize, uint segsize, uint hashsize);
void bt_mgrclose (BtMgr *mgr);

//  Helper functions to return slot values

extern BtKey bt_key (BtDb *bt, uint slot);
extern uid bt_uid (BtDb *bt, uint slot);
extern uint bt_tod (BtDb *bt, uint slot);

//  BTree page number constants
#define ALLOC_page		0	// allocation & lock manager hash table
#define ROOT_page		1	// root of the btree
#define LEAF_page		2	// first page of leaves
#define LATCH_page		3	// pages for lock manager

//	Number of levels to create in a new BTree

#define MIN_lvl			2

//  The page is allocated from low and hi ends.
//  The key offsets and row-id's are allocated
//  from the bottom, while the text of the key
//  is allocated from the top.  When the two
//  areas meet, the page is split into two.

//  A key consists of a length byte, two bytes of
//  index number (0 - 65534), and up to 253 bytes
//  of key value.  Duplicate keys are discarded.
//  Associated with each key is a 48 bit row-id.

//  The b-tree root is always located at page 1.
//	The first leaf page of level zero is always
//	located on page 2.

//	The b-tree pages are linked with next
//	pointers to facilitate enumerators,
//	and provide for concurrency.

//	When to root page fills, it is split in two and
//	the tree height is raised by a new root at page
//	one with two keys.

//	Deleted keys are marked with a dead bit until
//	page cleanup The fence key for a node is always
//	present, even after deletion and cleanup.

//  Groups of pages called segments from the btree are optionally
//  cached with a memory mapped pool. A hash table is used to keep
//  track of the cached segments.  This behaviour is controlled
//  by the cache block size parameter to bt_open.

//  To achieve maximum concurrency one page is locked at a time
//  as the tree is traversed to find leaf key in question. The right
//  page numbers are used in cases where the page is being split,
//	or consolidated.

//  Page 0 is dedicated to lock for new page extensions,
//	and chains empty pages together for reuse.

//	The ParentModification lock on a node is obtained to prevent resplitting
//	or deleting a node before its fence is posted into its upper level.

//	Empty pages are chained together through the ALLOC page and reused.

//	Access macros to address slot and key values from the page

#define slotptr(page, slot) (page->table + slot-1)
#define keyptr(page, slot) ((BtKey)((unsigned char*)(page) + slotptr(page, slot)->off))

void bt_putid(unsigned char *dest, uid id)
{
int i = BtId;

	while( i-- )
		dest[i] = (unsigned char)id, id >>= 8;
}

uid bt_getid(unsigned char *src)
{
uid id = 0;
int i;

	for( i = 0; i < BtId; i++ )
		id <<= 8, id |= *src++; 

	return id;
}

//	Latch Manager

//	wait until write lock mode is clear
//	and add 1 to the share count

void bt_spinreadlock(BtSpinLatch *latch)
{
ushort prev;

  do {
	//	obtain latch mutex
#ifdef unix
	if( __sync_fetch_and_or((ushort *)latch, Mutex) & Mutex )
		continue;
#else
	if( prev = _InterlockedOr16((ushort *)latch, Mutex) & Mutex )
		continue;
#endif
	//  see if exclusive request is granted or pending

	if( prev = !(latch->exclusive | latch->pending) )
#ifdef unix
		__sync_fetch_and_add((ushort *)latch, Share);
#else
		_InterlockedExchangeAdd16 ((ushort *)latch, Share);
#endif

#ifdef unix
	__sync_fetch_and_and ((ushort *)latch, ~Mutex);
#else
	_InterlockedAnd16((ushort *)latch, ~Mutex);
#endif

	if( prev )
		return;
#ifdef  unix
  } while( sched_yield(), 1 );
#else
  } while( SwitchToThread(), 1 );
#endif
}

//	wait for other read and write latches to relinquish

void bt_spinwritelock(BtSpinLatch *latch)
{
  do {
#ifdef  unix
	if( __sync_fetch_and_or((ushort *)latch, Mutex | Pending) & Mutex )
		continue;
#else
	if( _InterlockedOr16((ushort *)latch, Mutex | Pending) & Mutex )
		continue;
#endif
	if( !(latch->share | latch->exclusive) ) {
#ifdef unix
		__sync_fetch_and_or((ushort *)latch, Write);
		__sync_fetch_and_and ((ushort *)latch, ~(Mutex | Pending));
#else
		_InterlockedOr16((ushort *)latch, Write);
		_InterlockedAnd16((ushort *)latch, ~(Mutex | Pending));
#endif
		return;
	}

#ifdef unix
	__sync_fetch_and_and ((ushort *)latch, ~Mutex);
#else
	_InterlockedAnd16((ushort *)latch, ~Mutex);
#endif

#ifdef  unix
  } while( sched_yield(), 1 );
#else
  } while( SwitchToThread(), 1 );
#endif
}

//	try to obtain write lock

//	return 1 if obtained,
//		0 otherwise

int bt_spinwritetry(BtSpinLatch *latch)
{
ushort prev;

#ifdef unix
	if( prev = __sync_fetch_and_or((ushort *)latch, Mutex), prev & Mutex )
		return 0;
#else
	if( prev = _InterlockedOr16((ushort *)latch, Mutex), prev & Mutex )
		return 0;
#endif
	//	take write access if all bits are clear

	if( !prev )
#ifdef unix
		__sync_fetch_and_or ((ushort *)latch, Write);
#else
		_InterlockedOr16((ushort *)latch, Write);
#endif

#ifdef unix
	__sync_fetch_and_and ((ushort *)latch, ~Mutex);
#else
	_InterlockedAnd16((ushort *)latch, ~Mutex);
#endif
	return !prev;
}

//	clear write mode

void bt_spinreleasewrite(BtSpinLatch *latch)
{
#ifdef unix
	__sync_fetch_and_and ((ushort *)latch, ~Write);
#else
	_InterlockedAnd16((ushort *)latch, ~Write);
#endif
}

//	decrement reader count

void bt_spinreleaseread(BtSpinLatch *latch)
{
#ifdef unix
	__sync_fetch_and_add((ushort *)latch, -Share);
#else
	_InterlockedExchangeAdd16 ((ushort *)latch, -Share);
#endif
}

void bt_readlock(BtLatch *latch)
{
#ifdef unix
	pthread_rwlock_rdlock (latch->lock);
#else
	AcquireSRWLockShared (latch->srw);
#endif
}

//	wait for other read and write latches to relinquish

void bt_writelock(BtLatch *latch)
{
#ifdef unix
	pthread_rwlock_wrlock (latch->lock);
#else
	AcquireSRWLockExclusive (latch->srw);
#endif
}

//	try to obtain write lock

//	return 1 if obtained,
//		0 if already write or read locked

int bt_writetry(BtLatch *latch)
{
int result = 0;

#ifdef unix
	result = !pthread_rwlock_trywrlock (latch->lock);
#else
	result = TryAcquireSRWLockExclusive (latch->srw);
#endif
	return result;
}

//	clear write mode

void bt_releasewrite(BtLatch *latch)
{
#ifdef unix
	pthread_rwlock_unlock (latch->lock);
#else
	ReleaseSRWLockExclusive (latch->srw);
#endif
}

//	decrement reader count

void bt_releaseread(BtLatch *latch)
{
#ifdef unix
	pthread_rwlock_unlock (latch->lock);
#else
	ReleaseSRWLockShared (latch->srw);
#endif
}

void bt_initlockset (BtLatchSet *set, int reuse)
{
#ifdef unix
pthread_rwlockattr_t rwattr[1];

	if( reuse ) {
		pthread_rwlock_destroy (set->readwr->lock);
		pthread_rwlock_destroy (set->access->lock);
		pthread_rwlock_destroy (set->parent->lock);
	}

	pthread_rwlockattr_init (rwattr);
	pthread_rwlockattr_setkind_np (rwattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlockattr_setpshared (rwattr, PTHREAD_PROCESS_SHARED);

	pthread_rwlock_init (set->readwr->lock, rwattr);
	pthread_rwlock_init (set->access->lock, rwattr);
	pthread_rwlock_init (set->parent->lock, rwattr);
	pthread_rwlockattr_destroy (rwattr);
#else
	InitializeSRWLock (set->readwr->srw);
	InitializeSRWLock (set->access->srw);
	InitializeSRWLock (set->parent->srw);
#endif
}

//	link latch table entry into latch hash table

void bt_latchlink (BtDb *bt, ushort hashidx, ushort victim, uid page_no)
{
BtLatchSet *set = bt->mgr->latchsets + victim;

	if( set->next = bt->mgr->latchmgr->table[hashidx].slot )
		bt->mgr->latchsets[set->next].prev = victim;

	bt->mgr->latchmgr->table[hashidx].slot = victim;
	set->page_no = page_no;
	set->hash = hashidx;
	set->prev = 0;
}

//	release latch pin

void bt_unpinlatch (BtLatchSet *set)
{
#ifdef unix
	__sync_fetch_and_add(&set->pin, -1);
#else
	_InterlockedDecrement16 (&set->pin);
#endif
}

//	find existing latchset or inspire new one
//	return with latchset pinned

BtLatchSet *bt_pinlatch (BtDb *bt, uid page_no)
{
ushort hashidx = page_no % bt->mgr->latchmgr->latchhash;
ushort slot, avail = 0, victim, idx;
BtLatchSet *set;

	//  obtain read lock on hash table entry

	bt_spinreadlock(bt->mgr->latchmgr->table[hashidx].latch);

	if( slot = bt->mgr->latchmgr->table[hashidx].slot ) do
	{
		set = bt->mgr->latchsets + slot;
		if( page_no == set->page_no )
			break;
	} while( slot = set->next );

	if( slot ) {
#ifdef unix
		__sync_fetch_and_add(&set->pin, 1);
#else
		_InterlockedIncrement16 (&set->pin);
#endif
	}

    bt_spinreleaseread (bt->mgr->latchmgr->table[hashidx].latch);

	if( slot )
		return set;

  //  try again, this time with write lock

  bt_spinwritelock(bt->mgr->latchmgr->table[hashidx].latch);

  if( slot = bt->mgr->latchmgr->table[hashidx].slot ) do
  {
	set = bt->mgr->latchsets + slot;
	if( page_no == set->page_no )
		break;
	if( !set->pin && !avail )
		avail = slot;
  } while( slot = set->next );

  //  found our entry, or take over an unpinned one

  if( slot || (slot = avail) ) {
	set = bt->mgr->latchsets + slot;
#ifdef unix
	__sync_fetch_and_add(&set->pin, 1);
#else
	_InterlockedIncrement16 (&set->pin);
#endif
	set->page_no = page_no;
	bt_spinreleasewrite(bt->mgr->latchmgr->table[hashidx].latch);
	return set;
  }

	//  see if there are any unused entries
#ifdef unix
	victim = __sync_fetch_and_add (&bt->mgr->latchmgr->latchdeployed, 1) + 1;
#else
	victim = _InterlockedIncrement16 (&bt->mgr->latchmgr->latchdeployed);
#endif

	if( victim < bt->mgr->latchmgr->latchtotal ) {
		set = bt->mgr->latchsets + victim;
#ifdef unix
		__sync_fetch_and_add(&set->pin, 1);
#else
		_InterlockedIncrement16 (&set->pin);
#endif
		bt_initlockset (set, 0);
		bt_latchlink (bt, hashidx, victim, page_no);
		bt_spinreleasewrite (bt->mgr->latchmgr->table[hashidx].latch);
		return set;
	}

#ifdef unix
	victim = __sync_fetch_and_add (&bt->mgr->latchmgr->latchdeployed, -1);
#else
	victim = _InterlockedDecrement16 (&bt->mgr->latchmgr->latchdeployed);
#endif
  //  find and reuse previous lock entry

  while( 1 ) {
#ifdef unix
	victim = __sync_fetch_and_add(&bt->mgr->latchmgr->latchvictim, 1);
#else
	victim = _InterlockedIncrement16 (&bt->mgr->latchmgr->latchvictim) - 1;
#endif
	//	we don't use slot zero

	if( victim %= bt->mgr->latchmgr->latchtotal )
		set = bt->mgr->latchsets + victim;
	else
		continue;

	//	take control of our slot
	//	from other threads

	if( set->pin || !bt_spinwritetry (set->busy) )
		continue;

	idx = set->hash;

	// try to get write lock on hash chain
	//	skip entry if not obtained
	//	or has outstanding locks

	if( !bt_spinwritetry (bt->mgr->latchmgr->table[idx].latch) ) {
		bt_spinreleasewrite (set->busy);
		continue;
	}

	if( set->pin ) {
		bt_spinreleasewrite (set->busy);
		bt_spinreleasewrite (bt->mgr->latchmgr->table[idx].latch);
		continue;
	}

	//  unlink our available victim from its hash chain

	if( set->prev )
		bt->mgr->latchsets[set->prev].next = set->next;
	else
		bt->mgr->latchmgr->table[idx].slot = set->next;

	if( set->next )
		bt->mgr->latchsets[set->next].prev = set->prev;

	bt_spinreleasewrite (bt->mgr->latchmgr->table[idx].latch);
#ifdef unix
	__sync_fetch_and_add(&set->pin, 1);
#else
	_InterlockedIncrement16 (&set->pin);
#endif
	bt_initlockset (set, 1);
	bt_latchlink (bt, hashidx, victim, page_no);
	bt_spinreleasewrite (bt->mgr->latchmgr->table[hashidx].latch);
	bt_spinreleasewrite (set->busy);
	return set;
  }
}

void bt_mgrclose (BtMgr *mgr)
{
BtPool *pool;
uint slot;

	// release mapped pages
	//	note that slot zero is never used

	for( slot = 1; slot < mgr->poolmax; slot++ ) {
		pool = mgr->pool + slot;
		if( pool->slot )
#ifdef unix
			munmap (pool->map, (mgr->poolmask+1) << mgr->page_bits);
#else
		{
			FlushViewOfFile(pool->map, 0);
			UnmapViewOfFile(pool->map);
			CloseHandle(pool->hmap);
		}
#endif
	}

#ifdef unix
	munmap (mgr->latchsets, mgr->latchmgr->nlatchpage * mgr->page_size);
	munmap (mgr->latchmgr, mgr->page_size);
#else
	FlushViewOfFile(mgr->latchmgr, 0);
	UnmapViewOfFile(mgr->latchmgr);
	CloseHandle(mgr->halloc);
#endif
#ifdef unix
	close (mgr->idx);
	free (mgr->pool);
	free (mgr->hash);
	free (mgr->latch);
	free (mgr->pooladvise);
	free (mgr);
#else
	FlushFileBuffers(mgr->idx);
	CloseHandle(mgr->idx);
	GlobalFree (mgr->pool);
	GlobalFree (mgr->hash);
	GlobalFree (mgr->latch);
	GlobalFree (mgr);
#endif
}

//	close and release memory

void bt_close (BtDb *bt)
{
#ifdef unix
	if ( bt->mem )
		free (bt->mem);
#else
	if ( bt->mem)
		VirtualFree (bt->mem, 0, MEM_RELEASE);
#endif
	free (bt);
}

//  open/create new btree buffer manager

//	call with file_name, BT_openmode, bits in page size (e.g. 16),
//		size of mapped page pool (e.g. 8192)

BtMgr *bt_mgr (char *name, uint mode, uint bits, uint poolmax, uint segsize, uint hashsize)
{
uint lvl, attr, cacheblk, last, slot, idx;
uint nlatchpage, latchhash;
BtLatchMgr *latchmgr;
off64_t size;
uint amt[1];
BtMgr* mgr;
BtKey key;
int flag;

#ifndef unix
SYSTEM_INFO sysinfo[1];
#endif

	// determine sanity of page size and buffer pool

	if( bits > BT_maxbits )
		bits = BT_maxbits;
	else if( bits < BT_minbits )
		bits = BT_minbits;

	if( !poolmax )
		return NULL;	// must have buffer pool

#ifdef unix
	mgr = calloc (1, sizeof(BtMgr));

	mgr->idx = open ((char*)name, O_RDWR | O_CREAT, 0666);

	if( mgr->idx == -1 )
		return free(mgr), NULL;
	
	cacheblk = 4096;	// minimum mmap segment size for unix

#else
	mgr = GlobalAlloc (GMEM_FIXED|GMEM_ZEROINIT, sizeof(BtMgr));
	attr = FILE_ATTRIBUTE_NORMAL;
	mgr->idx = CreateFile(name, GENERIC_READ| GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, attr, NULL);

	if( mgr->idx == INVALID_HANDLE_VALUE )
		return GlobalFree(mgr), NULL;

	// normalize cacheblk to multiple of sysinfo->dwAllocationGranularity
	GetSystemInfo(sysinfo);
	cacheblk = sysinfo->dwAllocationGranularity;
#endif

#ifdef unix
	latchmgr = malloc (BT_maxpage);
	*amt = 0;

	// read minimum page size to get root info

	if( size = lseek (mgr->idx, 0L, 2) ) {
		if( pread(mgr->idx, latchmgr, BT_minpage, 0) == BT_minpage )
			bits = latchmgr->alloc->bits;
		else
			return free(mgr), free(latchmgr), NULL;
	} else if( mode == BT_ro )
		return free(latchmgr), bt_mgrclose (mgr), NULL;
#else
	latchmgr = VirtualAlloc(NULL, BT_maxpage, MEM_COMMIT, PAGE_READWRITE);
	size = GetFileSize(mgr->idx, amt);

	if( size || *amt ) {
		if( !ReadFile(mgr->idx, (char *)latchmgr, BT_minpage, amt, NULL) )
			return bt_mgrclose (mgr), NULL;
		bits = latchmgr->alloc->bits;
	} else if( mode == BT_ro )
		return bt_mgrclose (mgr), NULL;
#endif

	mgr->page_size = 1 << bits;
	mgr->page_bits = bits;

	mgr->poolmax = poolmax;
	mgr->mode = mode;

	if( cacheblk < mgr->page_size )
		cacheblk = mgr->page_size;

	//  mask for partial memmaps

	mgr->poolmask = (cacheblk >> bits) - 1;

	//	see if requested size of pages per memmap is greater

	if( (1 << segsize) > mgr->poolmask )
		mgr->poolmask = (1 << segsize) - 1;

	mgr->seg_bits = 0;

	while( (1 << mgr->seg_bits) <= mgr->poolmask )
		mgr->seg_bits++;

	mgr->hashsize = hashsize;

#ifdef unix
	mgr->pool = calloc (poolmax, sizeof(BtPool));
	mgr->hash = calloc (hashsize, sizeof(ushort));
	mgr->latch = calloc (hashsize, sizeof(BtSpinLatch));
	mgr->pooladvise = calloc (poolmax, (mgr->poolmask + 8) / 8);
#else
	mgr->pool = GlobalAlloc (GMEM_FIXED|GMEM_ZEROINIT, poolmax * sizeof(BtPool));
	mgr->hash = GlobalAlloc (GMEM_FIXED|GMEM_ZEROINIT, hashsize * sizeof(ushort));
	mgr->latch = GlobalAlloc (GMEM_FIXED|GMEM_ZEROINIT, hashsize * sizeof(BtSpinLatch));
#endif

	if( size || *amt )
		goto mgrlatch;

	// initialize an empty b-tree with latch page, root page, page of leaves
	// and page(s) of latches

	memset (latchmgr, 0, 1 << bits);
	nlatchpage = BT_latchtable / (mgr->page_size / sizeof(BtLatchSet)) + 1; 
	bt_putid(latchmgr->alloc->right, MIN_lvl+1+nlatchpage);
	latchmgr->alloc->bits = mgr->page_bits;

	latchmgr->nlatchpage = nlatchpage;
	latchmgr->latchtotal = nlatchpage * (mgr->page_size / sizeof(BtLatchSet));

	//  initialize latch manager

	latchhash = (mgr->page_size - sizeof(BtLatchMgr)) / sizeof(BtHashEntry);

	//	size of hash table = total number of latchsets

	if( latchhash > latchmgr->latchtotal )
		latchhash = latchmgr->latchtotal;

	latchmgr->latchhash = latchhash;

#ifdef unix
	if( write (mgr->idx, latchmgr, mgr->page_size) < mgr->page_size )
		return bt_mgrclose (mgr), NULL;
#else
	if( !WriteFile (mgr->idx, (char *)latchmgr, mgr->page_size, amt, NULL) )
		return bt_mgrclose (mgr), NULL;

	if( *amt < mgr->page_size )
		return bt_mgrclose (mgr), NULL;
#endif

	memset (latchmgr, 0, 1 << bits);
	latchmgr->alloc->bits = mgr->page_bits;

	for( lvl=MIN_lvl; lvl--; ) {
		slotptr(latchmgr->alloc, 1)->off = mgr->page_size - 3;
		bt_putid(slotptr(latchmgr->alloc, 1)->id, lvl ? MIN_lvl - lvl + 1 : 0);		// next(lower) page number
		key = keyptr(latchmgr->alloc, 1);
		key->len = 2;			// create stopper key
		key->key[0] = 0xff;
		key->key[1] = 0xff;
		latchmgr->alloc->min = mgr->page_size - 3;
		latchmgr->alloc->lvl = lvl;
		latchmgr->alloc->cnt = 1;
		latchmgr->alloc->act = 1;
#ifdef unix
		if( write (mgr->idx, latchmgr, mgr->page_size) < mgr->page_size )
			return bt_mgrclose (mgr), NULL;
#else
		if( !WriteFile (mgr->idx, (char *)latchmgr, mgr->page_size, amt, NULL) )
			return bt_mgrclose (mgr), NULL;

		if( *amt < mgr->page_size )
			return bt_mgrclose (mgr), NULL;
#endif
	}

	// clear out latch manager locks
	//	and rest of pages to round out segment

	memset(latchmgr, 0, mgr->page_size);
	last = MIN_lvl + 1;

	while( last <= ((MIN_lvl + 1 + nlatchpage) | mgr->poolmask) ) {
#ifdef unix
		pwrite(mgr->idx, latchmgr, mgr->page_size, last << mgr->page_bits);
#else
		SetFilePointer (mgr->idx, last << mgr->page_bits, NULL, FILE_BEGIN);
		if( !WriteFile (mgr->idx, (char *)latchmgr, mgr->page_size, amt, NULL) )
			return bt_mgrclose (mgr), NULL;
		if( *amt < mgr->page_size )
			return bt_mgrclose (mgr), NULL;
#endif
		last++;
	}

mgrlatch:
#ifdef unix
	flag = PROT_READ | PROT_WRITE;
	mgr->latchmgr = mmap (0, mgr->page_size, flag, MAP_SHARED, mgr->idx, ALLOC_page * mgr->page_size);
	if( mgr->latchmgr == MAP_FAILED )
		return bt_mgrclose (mgr), NULL;
	mgr->latchsets = (BtLatchSet *)mmap (0, mgr->latchmgr->nlatchpage * mgr->page_size, flag, MAP_SHARED, mgr->idx, LATCH_page * mgr->page_size);
	if( mgr->latchsets == MAP_FAILED )
		return bt_mgrclose (mgr), NULL;
#else
	flag = PAGE_READWRITE;
	mgr->halloc = CreateFileMapping(mgr->idx, NULL, flag, 0, (BT_latchtable / (mgr->page_size / sizeof(BtLatchSet)) + 1 + LATCH_page) * mgr->page_size, NULL);
	if( !mgr->halloc )
		return bt_mgrclose (mgr), NULL;

	flag = FILE_MAP_WRITE;
	mgr->latchmgr = MapViewOfFile(mgr->halloc, flag, 0, 0, (BT_latchtable / (mgr->page_size / sizeof(BtLatchSet)) + 1 + LATCH_page) * mgr->page_size);
	if( !mgr->latchmgr )
		return GetLastError(), bt_mgrclose (mgr), NULL;

	mgr->latchsets = (void *)((char *)mgr->latchmgr + LATCH_page * mgr->page_size);
#endif

#ifdef unix
	free (latchmgr);
#else
	VirtualFree (latchmgr, 0, MEM_RELEASE);
#endif
	return mgr;
}

//	open BTree access method
//	based on buffer manager

BtDb *bt_open (BtMgr *mgr)
{
BtDb *bt = malloc (sizeof(*bt));

	memset (bt, 0, sizeof(*bt));
	bt->mgr = mgr;
#ifdef unix
	bt->mem = malloc (3 *mgr->page_size);
#else
	bt->mem = VirtualAlloc(NULL, 3 * mgr->page_size, MEM_COMMIT, PAGE_READWRITE);
#endif
	bt->frame = (BtPage)bt->mem;
	bt->zero = (BtPage)(bt->mem + 1 * mgr->page_size);
	bt->cursor = (BtPage)(bt->mem + 2 * mgr->page_size);

	memset (bt->zero, 0, mgr->page_size);
	return bt;
}

//  compare two keys, returning > 0, = 0, or < 0
//  as the comparison value

int keycmp (BtKey key1, unsigned char *key2, uint len2)
{
uint len1 = key1->len;
int ans;

	if( ans = memcmp (key1->key, key2, len1 > len2 ? len2 : len1) )
		return ans;

	if( len1 > len2 )
		return 1;
	if( len1 < len2 )
		return -1;

	return 0;
}

//	Buffer Pool mgr

// find segment in pool
// must be called with hashslot idx locked
//	return NULL if not there
//	otherwise return node

BtPool *bt_findpool(BtDb *bt, uid page_no, uint idx)
{
BtPool *pool;
uint slot;

	// compute start of hash chain in pool

	if( slot = bt->mgr->hash[idx] ) 
		pool = bt->mgr->pool + slot;
	else
		return NULL;

	page_no &= ~bt->mgr->poolmask;

	while( pool->basepage != page_no )
	  if( pool = pool->hashnext )
		continue;
	  else
		return NULL;

	return pool;
}

// add segment to hash table

void bt_linkhash(BtDb *bt, BtPool *pool, uid page_no, int idx)
{
BtPool *node;
uint slot;

	pool->hashprev = pool->hashnext = NULL;
	pool->basepage = page_no & ~bt->mgr->poolmask;
	pool->lru = 1;

	if( slot = bt->mgr->hash[idx] ) {
		node = bt->mgr->pool + slot;
		pool->hashnext = node;
		node->hashprev = pool;
	}

	bt->mgr->hash[idx] = pool->slot;
}

//	find best segment to evict from buffer pool

BtPool *bt_findlru (BtDb *bt, uint hashslot)
{
unsigned long long int target = ~0LL;
BtPool *pool = NULL, *node;

	if( !hashslot )
		return NULL;

	node = bt->mgr->pool + hashslot;

	//  scan pool entries under hash table slot

	do {
	  if( node->pin )
		continue;
	  if( node->lru > target )
		continue;
	  target = node->lru;
	  pool = node;
	} while( node = node->hashnext );

	return pool;
}

//  map new buffer pool segment to virtual memory

BTERR bt_mapsegment(BtDb *bt, BtPool *pool, uid page_no)
{
off64_t off = (page_no & ~bt->mgr->poolmask) << bt->mgr->page_bits;
off64_t limit = off + ((bt->mgr->poolmask+1) << bt->mgr->page_bits);
int flag;

#ifdef unix
	flag = PROT_READ | ( bt->mgr->mode == BT_ro ? 0 : PROT_WRITE );
	pool->map = mmap (0, (bt->mgr->poolmask+1) << bt->mgr->page_bits, flag, MAP_SHARED, bt->mgr->idx, off);
	if( pool->map == MAP_FAILED )
		return bt->err = BTERR_map;

	// clear out madvise issued bits
	memset (bt->mgr->pooladvise + pool->slot * ((bt->mgr->poolmask + 8) / 8), 0, (bt->mgr->poolmask + 8)/8);
#else
	flag = ( bt->mgr->mode == BT_ro ? PAGE_READONLY : PAGE_READWRITE );
	pool->hmap = CreateFileMapping(bt->mgr->idx, NULL, flag, (DWORD)(limit >> 32), (DWORD)limit, NULL);
	if( !pool->hmap )
		return bt->err = BTERR_map;

	flag = ( bt->mgr->mode == BT_ro ? FILE_MAP_READ : FILE_MAP_WRITE );
	pool->map = MapViewOfFile(pool->hmap, flag, (DWORD)(off >> 32), (DWORD)off, (bt->mgr->poolmask+1) << bt->mgr->page_bits);
	if( !pool->map )
		return bt->err = BTERR_map;
#endif
 	return bt->err = 0;
}

//	calculate page within pool

BtPage bt_page (BtDb *bt, BtPool *pool, uid page_no)
{
uint subpage = (uint)(page_no & bt->mgr->poolmask); // page within mapping
BtPage page;

	page = (BtPage)(pool->map + (subpage << bt->mgr->page_bits));
#ifdef unix
	{
	uint idx = subpage / 8;
	uint bit = subpage % 8;

		if( ~((bt->mgr->pooladvise + pool->slot * ((bt->mgr->poolmask + 8)/8))[idx] >> bit) & 1 ) {
		  madvise (page, bt->mgr->page_size, MADV_WILLNEED);
		  (bt->mgr->pooladvise + pool->slot * ((bt->mgr->poolmask + 8)/8))[idx] |= 1 << bit;
		}
	}
#endif
	return page;
}

//  release pool pin

void bt_unpinpool (BtPool *pool)
{
#ifdef unix
	__sync_fetch_and_add(&pool->pin, -1);
#else
	_InterlockedDecrement16 (&pool->pin);
#endif
}

//	find or place requested page in segment-pool
//	return pool table entry, incrementing pin

BtPool *bt_pinpool(BtDb *bt, uid page_no)
{
BtPool *pool, *node, *next;
uint slot, idx, victim;

	//	lock hash table chain

	idx = (uint)(page_no >> bt->mgr->seg_bits) % bt->mgr->hashsize;
	bt_spinreadlock (&bt->mgr->latch[idx]);

	//	look up in hash table

	if( pool = bt_findpool(bt, page_no, idx) ) {
#ifdef unix
		__sync_fetch_and_add(&pool->pin, 1);
#else
		_InterlockedIncrement16 (&pool->pin);
#endif
		bt_spinreleaseread (&bt->mgr->latch[idx]);
		pool->lru++;
		return pool;
	}

	//	upgrade to write lock

	bt_spinreleaseread (&bt->mgr->latch[idx]);
	bt_spinwritelock (&bt->mgr->latch[idx]);

	// try to find page in pool with write lock

	if( pool = bt_findpool(bt, page_no, idx) ) {
#ifdef unix
		__sync_fetch_and_add(&pool->pin, 1);
#else
		_InterlockedIncrement16 (&pool->pin);
#endif
		bt_spinreleasewrite (&bt->mgr->latch[idx]);
		pool->lru++;
		return pool;
	}

	// allocate a new pool node
	// and add to hash table

#ifdef unix
	slot = __sync_fetch_and_add(&bt->mgr->poolcnt, 1);
#else
	slot = _InterlockedIncrement16 (&bt->mgr->poolcnt) - 1;
#endif

	if( ++slot < bt->mgr->poolmax ) {
		pool = bt->mgr->pool + slot;
		pool->slot = slot;

		if( bt_mapsegment(bt, pool, page_no) )
			return NULL;

		bt_linkhash(bt, pool, page_no, idx);
#ifdef unix
		__sync_fetch_and_add(&pool->pin, 1);
#else
		_InterlockedIncrement16 (&pool->pin);
#endif
		bt_spinreleasewrite (&bt->mgr->latch[idx]);
		return pool;
	}

	// pool table is full
	//	find best pool entry to evict

#ifdef unix
	__sync_fetch_and_add(&bt->mgr->poolcnt, -1);
#else
	_InterlockedDecrement16 (&bt->mgr->poolcnt);
#endif

	while( 1 ) {
#ifdef unix
		victim = __sync_fetch_and_add(&bt->mgr->evicted, 1);
#else
		victim = _InterlockedIncrement (&bt->mgr->evicted) - 1;
#endif
		victim %= bt->mgr->hashsize;

		// try to get write lock
		//	skip entry if not obtained

		if( !bt_spinwritetry (&bt->mgr->latch[victim]) )
			continue;

		//  if pool entry is empty
		//	or any pages are pinned
		//	skip this entry

		if( !(pool = bt_findlru(bt, bt->mgr->hash[victim])) ) {
			bt_spinreleasewrite (&bt->mgr->latch[victim]);
			continue;
		}

		// unlink victim pool node from hash table

		if( node = pool->hashprev )
			node->hashnext = pool->hashnext;
		else if( node = pool->hashnext )
			bt->mgr->hash[victim] = node->slot;
		else
			bt->mgr->hash[victim] = 0;

		if( node = pool->hashnext )
			node->hashprev = pool->hashprev;

		bt_spinreleasewrite (&bt->mgr->latch[victim]);

		//	remove old file mapping
#ifdef unix
		munmap (pool->map, (bt->mgr->poolmask+1) << bt->mgr->page_bits);
#else
		FlushViewOfFile(pool->map, 0);
		UnmapViewOfFile(pool->map);
		CloseHandle(pool->hmap);
#endif
		pool->map = NULL;

		//  create new pool mapping
		//  and link into hash table

		if( bt_mapsegment(bt, pool, page_no) )
			return NULL;

		bt_linkhash(bt, pool, page_no, idx);
#ifdef unix
		__sync_fetch_and_add(&pool->pin, 1);
#else
		_InterlockedIncrement16 (&pool->pin);
#endif
		bt_spinreleasewrite (&bt->mgr->latch[idx]);
		return pool;
	}
}

// place write, read, or parent lock on requested page_no.

void bt_lockpage(BtLock mode, BtLatchSet *set)
{
	switch( mode ) {
	case BtLockRead:
		bt_readlock (set->readwr);
		break;
	case BtLockWrite:
		bt_writelock (set->readwr);
		break;
	case BtLockAccess:
		bt_readlock (set->access);
		break;
	case BtLockDelete:
		bt_writelock (set->access);
		break;
	case BtLockParent:
		bt_writelock (set->parent);
		break;
	}
}

// remove write, read, or parent lock on requested page

void bt_unlockpage(BtLock mode, BtLatchSet *set)
{
	switch( mode ) {
	case BtLockRead:
		bt_releaseread (set->readwr);
		break;
	case BtLockWrite:
		bt_releasewrite (set->readwr);
		break;
	case BtLockAccess:
		bt_releaseread (set->access);
		break;
	case BtLockDelete:
		bt_releasewrite (set->access);
		break;
	case BtLockParent:
		bt_releasewrite (set->parent);
		break;
	}
}

//	allocate a new page and write page into it

uid bt_newpage(BtDb *bt, BtPage page)
{
BtLatchSet *set;
BtPool *pool;
uid new_page;
BtPage pmap;
int reuse;

	//	lock allocation page

	bt_spinwritelock(bt->mgr->latchmgr->lock);

	// use empty chain first
	// else allocate empty page

	if( new_page = bt_getid(bt->mgr->latchmgr->alloc[1].right) ) {
		if( pool = bt_pinpool (bt, new_page) )
			pmap = bt_page (bt, pool, new_page);
		else
			return 0;
		bt_putid(bt->mgr->latchmgr->alloc[1].right, bt_getid(pmap->right));
		bt_unpinpool (pool);
		reuse = 1;
	} else {
		new_page = bt_getid(bt->mgr->latchmgr->alloc->right);
		bt_putid(bt->mgr->latchmgr->alloc->right, new_page+1);
		reuse = 0;
	}
#ifdef unix
	if ( pwrite(bt->mgr->idx, page, bt->mgr->page_size, new_page << bt->mgr->page_bits) < bt->mgr->page_size )
		return bt->err = BTERR_wrt, 0;

	// if writing first page of pool block, zero last page in the block

	if ( !reuse && bt->mgr->poolmask > 0 && (new_page & bt->mgr->poolmask) == 0 )
	{
		// use zero buffer to write zeros
		memset(bt->zero, 0, bt->mgr->page_size);
		if ( pwrite(bt->mgr->idx,bt->zero, bt->mgr->page_size, (new_page | bt->mgr->poolmask) << bt->mgr->page_bits) < bt->mgr->page_size )
			return bt->err = BTERR_wrt, 0;
	}
#else
	//	bring new page into pool and copy page.
	//	this will extend the file into the new pages.

	if( pool = bt_pinpool (bt, new_page) )
		pmap = bt_page (bt, pool, new_page);
	else
		return 0;

	memcpy(pmap, page, bt->mgr->page_size);
	bt_unpinpool (pool);
#endif
	// unlock allocation latch and return new page no

	bt_spinreleasewrite(bt->mgr->latchmgr->lock);
	return new_page;
}

//  find slot in page for given key at a given level

int bt_findslot (BtDb *bt, unsigned char *key, uint len)
{
uint diff, higher = bt->page->cnt, low = 1, slot;
uint good = 0;

	//	make stopper key an infinite fence value

	if( bt_getid (bt->page->right) )
		higher++;
	else
		good++;

	//	low is the next candidate, higher is already
	//	tested as .ge. the given key, loop ends when they meet

	while( diff = higher - low ) {
		slot = low + ( diff >> 1 );
		if( keycmp (keyptr(bt->page, slot), key, len) < 0 )
			low = slot + 1;
		else
			higher = slot, good++;
	}

	//	return zero if key is on right link page

 	return good ? higher : 0;
}

//  find and load page at given level for given key
//	leave page rd or wr locked as requested

int bt_loadpage (BtDb *bt, unsigned char *key, uint len, uint lvl, uint lock)
{
uid page_no = ROOT_page, prevpage = 0;
BtLatchSet *set, *prevset;
uint drill = 0xff, slot;
uint mode, prevmode;
BtPool *prevpool;

  //  start at root of btree and drill down

  bt->set = NULL;

  do {
	// determine lock mode of drill level
	mode = (lock == BtLockWrite) && (drill == lvl) ? BtLockWrite : BtLockRead; 

	bt->set = bt_pinlatch (bt, page_no);
	bt->page_no = page_no;

	// pin page contents

	if( bt->pool = bt_pinpool (bt, page_no) )
		bt->page = bt_page (bt, bt->pool, page_no);
	else
		return 0;

 	// obtain access lock using lock chaining with Access mode

	if( page_no > ROOT_page )
	  bt_lockpage(BtLockAccess, bt->set);

	//	release & unpin parent page

	if( prevpage ) {
	  bt_unlockpage(prevmode, prevset);
	  bt_unpinlatch (prevset);
	  bt_unpinpool (prevpool);
	  prevpage = 0;
	}

 	// obtain read lock using lock chaining

	bt_lockpage(mode, bt->set);

	if( page_no > ROOT_page )
	  bt_unlockpage(BtLockAccess, bt->set);

	// re-read and re-lock root after determining actual level of root

	if( bt->page->lvl != drill) {
		if ( bt->page_no != ROOT_page )
			return bt->err = BTERR_struct, 0;
			
		drill = bt->page->lvl;

		if( lock == BtLockWrite && drill == lvl ) {
		  bt_unlockpage(mode, bt->set);
		  bt_unpinlatch (bt->set);
		  bt_unpinpool (bt->pool);
		  continue;
		}
	}

	//  find key on page at this level
	//  and descend to requested level

	if( !bt->page->kill && (slot = bt_findslot (bt, key, len)) ) {
	  if( drill == lvl )
		return slot;

	  while( slotptr(bt->page, slot)->dead )
		if( slot++ < bt->page->cnt )
			continue;
		else {
			page_no = bt_getid(bt->page->right);
			goto slideright;
		}

	  page_no = bt_getid(slotptr(bt->page, slot)->id);
	  drill--;
	}

	//  or slide right into next page
	//  (slide left from deleted page)

	else
		page_no = bt_getid(bt->page->right);

	//  continue down / right using overlapping locks
	//  to protect pages being killed or split.

slideright:
	prevpage = bt->page_no;
	prevpool = bt->pool;
	prevset = bt->set;
	prevmode = mode;
  } while( page_no );

  // return error on end of right chain

  bt->err = BTERR_struct;
  return 0;	// return error
}

//  find and delete key on page by marking delete flag bit
//  when page becomes empty, delete it

BTERR bt_deletekey (BtDb *bt, unsigned char *key, uint len, uint lvl)
{
unsigned char lowerkey[256], higherkey[256];
BtLatchSet *rset, *set;
BtPool *pool, *rpool;
uid page_no, right;
uint slot, tod;
BtPage rpage;
BtKey ptr;

	if( slot = bt_loadpage (bt, key, len, lvl, BtLockWrite) )
		ptr = keyptr(bt->page, slot);
	else
		return bt->err;

	// if key is found delete it, otherwise ignore request

	if( bt->found = !keycmp (ptr, key, len) )
		if( bt->found = slotptr(bt->page, slot)->dead == 0 ) {
			slotptr(bt->page,slot)->dead = 1;
			if( slot < bt->page->cnt )
				bt->page->dirty = 1;
			bt->page->act--;
		}

	// return if page is not empty, or it has no right sibling

	right = bt_getid(bt->page->right);
	page_no = bt->page_no;
	pool = bt->pool;
	set = bt->set;

	if( !right || bt->page->act ) {
		bt_unlockpage(BtLockWrite, set);
		bt_unpinlatch (set);
		bt_unpinpool (pool);
		return bt->err;
	}

	// obtain Parent lock over write lock

	bt_lockpage(BtLockParent, set);

	// keep copy of key to delete

	ptr = keyptr(bt->page, bt->page->cnt);
	memcpy(lowerkey, ptr, ptr->len + 1);

	// lock and map right page

	if( rpool = bt_pinpool (bt, right) )
		rpage = bt_page (bt, rpool, right);
	else
		return bt->err;

	rset = bt_pinlatch (bt, right);
	bt_lockpage(BtLockWrite, rset);

	// pull contents of next page into current empty page 

	memcpy (bt->page, rpage, bt->mgr->page_size);

	//	keep copy of key to update

	ptr = keyptr(rpage, rpage->cnt);
	memcpy(higherkey, ptr, ptr->len + 1);

	//  Mark right page as deleted and point it to left page
	//	until we can post updates at higher level.

	bt_putid(rpage->right, page_no);
	rpage->kill = 1;
	rpage->cnt = 0;

	bt_unlockpage(BtLockWrite, rset);
	bt_unlockpage(BtLockWrite, set);

	//  delete old lower key to consolidated node

	if( bt_deletekey (bt, lowerkey + 1, *lowerkey, lvl + 1) )
		return bt->err;

	//  redirect higher key directly to consolidated node

	tod = (uint)time(NULL);

	if( bt_insertkey (bt, higherkey+1, *higherkey, lvl + 1, page_no, tod) )
		return bt->err;

	//	add killed right block to free chain
	//	lock latch mgr

	bt_spinwritelock(bt->mgr->latchmgr->lock);

	//	store free chain in allocation page second right
	bt_putid(rpage->right, bt_getid(bt->mgr->latchmgr->alloc[1].right));
	bt_putid(bt->mgr->latchmgr->alloc[1].right, right);

	// unlock latch mgr and unpin right page

	bt_spinreleasewrite(bt->mgr->latchmgr->lock);
	bt_unpinlatch (rset);
	bt_unpinpool (rpool);

	// 	remove ParentModify lock

	bt_unlockpage(BtLockParent, set);
	bt_unpinlatch (set);
	bt_unpinpool (pool);
	return 0;
}

//	find key in leaf level and return row-id

uid bt_findkey (BtDb *bt, unsigned char *key, uint len)
{
uint  slot;
BtKey ptr;
uid id;

	if( slot = bt_loadpage (bt, key, len, 0, BtLockRead) )
		ptr = keyptr(bt->page, slot);
	else
		return 0;

	// if key exists, return row-id
	//	otherwise return 0

	if( ptr->len == len && !memcmp (ptr->key, key, len) )
		id = bt_getid(slotptr(bt->page,slot)->id);
	else
		id = 0;

	bt_unlockpage (BtLockRead, bt->set);
	bt_unpinlatch (bt->set);
	bt_unpinpool (bt->pool);
	return id;
}

//	check page for space available,
//	clean if necessary and return
//	=0 - page needs splitting
//	>0 - go ahead at returned slot

uint bt_cleanpage(BtDb *bt, uint amt, uint slot)
{
uint nxt = bt->mgr->page_size;
BtPage page = bt->page;
uint cnt = 0, idx = 0;
uint max = page->cnt;
uint newslot;
BtKey key;

	if( page->min >= (max+1) * sizeof(BtSlot) + sizeof(*page) + amt + 1 )
		return slot;

	//	skip cleanup if nothing to reclaim

	if( !page->dirty )
		return 0;

	memcpy (bt->frame, page, bt->mgr->page_size);

	// skip page info and set rest of page to zero

	memset (page+1, 0, bt->mgr->page_size - sizeof(*page));
	page->dirty = 0;
	page->act = 0;

	// always leave fence key in list

	while( cnt++ < max ) {
		if( cnt == slot )
			newslot = idx + 1;
		else if( cnt < max && slotptr(bt->frame,cnt)->dead )
			continue;

		// copy key
		key = keyptr(bt->frame, cnt);
		nxt -= key->len + 1;
		memcpy ((unsigned char *)page + nxt, key, key->len + 1);

		// copy slot
		memcpy(slotptr(page, ++idx)->id, slotptr(bt->frame, cnt)->id, BtId);
		if( !(slotptr(page, idx)->dead = slotptr(bt->frame, cnt)->dead) )
			page->act++;
		slotptr(page, idx)->tod = slotptr(bt->frame, cnt)->tod;
		slotptr(page, idx)->off = nxt;
	}
	page->min = nxt;
	page->cnt = idx;

	if( page->min >= (idx+1) * sizeof(BtSlot) + sizeof(*page) + amt + 1 )
		return newslot;

	return 0;
}

// split the root and raise the height of the btree

BTERR bt_splitroot(BtDb *bt,  unsigned char *newkey, unsigned char *oldkey, uid page_no2)
{
uint nxt = bt->mgr->page_size;
BtPage root = bt->page;
uid new_page;

	//  Obtain an empty page to use, and copy the current
	//  root contents into it which is the lower half of
	//	the old root.

	if( !(new_page = bt_newpage(bt, root)) )
		return bt->err;

	// preserve the page info at the bottom
	// and set rest to zero

	memset(root+1, 0, bt->mgr->page_size - sizeof(*root));

	// insert first key on newroot page

	nxt -= *newkey + 1;
	memcpy ((unsigned char *)root + nxt, newkey, *newkey + 1);
	bt_putid(slotptr(root, 1)->id, new_page);
	slotptr(root, 1)->off = nxt;
	
	// insert second key on newroot page
	// and increase the root height

	nxt -= *oldkey + 1;
	memcpy ((unsigned char *)root + nxt, oldkey, *oldkey + 1);
	bt_putid(slotptr(root, 2)->id, page_no2);
	slotptr(root, 2)->off = nxt;

	bt_putid(root->right, 0);
	root->min = nxt;		// reset lowest used offset and key count
	root->cnt = 2;
	root->act = 2;
	root->lvl++;

	// release and unpin root (bt->page)

	bt_unlockpage(BtLockWrite, bt->set);
	bt_unpinlatch (bt->set);
	bt_unpinpool (bt->pool);
	return 0;
}

//  split already locked full node
//	return unlocked.

BTERR bt_splitpage (BtDb *bt)
{
uint cnt = 0, idx = 0, max, nxt = bt->mgr->page_size;
unsigned char oldkey[256], lowerkey[256];
uid page_no = bt->page_no, right;
BtLatchSet *nset, *set = bt->set;
BtPool *pool = bt->pool;
BtPage page = bt->page;
uint lvl = page->lvl;
uid new_page;
BtKey key;
uint tod;

	//  split higher half of keys to bt->frame
	//	the last key (fence key) might be dead

	tod = (uint)time(NULL);

	memset (bt->frame, 0, bt->mgr->page_size);
	max = (int)page->cnt;
	cnt = max / 2;
	idx = 0;

	while( cnt++ < max ) {
		key = keyptr(page, cnt);
		nxt -= key->len + 1;
		memcpy ((unsigned char *)bt->frame + nxt, key, key->len + 1);
		memcpy(slotptr(bt->frame,++idx)->id, slotptr(page,cnt)->id, BtId);
		if( !(slotptr(bt->frame, idx)->dead = slotptr(page, cnt)->dead) )
			bt->frame->act++;
		slotptr(bt->frame, idx)->tod = slotptr(page, cnt)->tod;
		slotptr(bt->frame, idx)->off = nxt;
	}

	// remember existing fence key for new page to the right

	memcpy (oldkey, key, key->len + 1);

	bt->frame->bits = bt->mgr->page_bits;
	bt->frame->min = nxt;
	bt->frame->cnt = idx;
	bt->frame->lvl = lvl;

	// link right node

	if( page_no > ROOT_page ) {
		right = bt_getid (page->right);
		bt_putid(bt->frame->right, right);
	}

	//	get new free page and write frame to it.

	if( !(new_page = bt_newpage(bt, bt->frame)) )
		return bt->err;

	//	update lower keys to continue in old page

	memcpy (bt->frame, page, bt->mgr->page_size);
	memset (page+1, 0, bt->mgr->page_size - sizeof(*page));
	nxt = bt->mgr->page_size;
	page->act = 0;
	cnt = 0;
	idx = 0;

	//  assemble page of smaller keys
	//	(they're all active keys)

	while( cnt++ < max / 2 ) {
		key = keyptr(bt->frame, cnt);
		nxt -= key->len + 1;
		memcpy ((unsigned char *)page + nxt, key, key->len + 1);
		memcpy(slotptr(page,++idx)->id, slotptr(bt->frame,cnt)->id, BtId);
		slotptr(page, idx)->tod = slotptr(bt->frame, cnt)->tod;
		slotptr(page, idx)->off = nxt;
		page->act++;
	}

	// remember fence key for old page

	memcpy(lowerkey, key, key->len + 1);
	bt_putid(page->right, new_page);
	page->min = nxt;
	page->cnt = idx;

	// if current page is the root page, split it

	if( page_no == ROOT_page )
		return bt_splitroot (bt, lowerkey, oldkey, new_page);

	// obtain Parent/Write locks
	// for left and right node pages

	nset = bt_pinlatch (bt, new_page);

	bt_lockpage (BtLockParent, nset);
	bt_lockpage (BtLockParent, set);

	//  release wr lock on left page
	//  (keep the SMO in sequence)

	bt_unlockpage (BtLockWrite, set);

	// insert new fence for reformulated left block

	if( bt_insertkey (bt, lowerkey+1, *lowerkey, lvl + 1, page_no, tod) )
		return bt->err;

	// fix old fence for newly allocated right block page

	if( bt_insertkey (bt, oldkey+1, *oldkey, lvl + 1, new_page, tod) )
		return bt->err;

	// release Parent locks

	bt_unlockpage (BtLockParent, nset);
	bt_unlockpage (BtLockParent, set);
	bt_unpinlatch (nset);
	bt_unpinlatch (set);
	bt_unpinpool (pool);
	return 0;
}

//  Insert new key into the btree at requested level.
//  Level zero pages are leaf pages. Page is unlocked at exit.

BTERR bt_insertkey (BtDb *bt, unsigned char *key, uint len, uint lvl, uid id, uint tod)
{
uint slot, idx;
BtPage page;
BtKey ptr;

  while( 1 ) {
	if( slot = bt_loadpage (bt, key, len, lvl, BtLockWrite) )
		ptr = keyptr(bt->page, slot);
	else
	{
		if ( !bt->err )
			bt->err = BTERR_ovflw;
		return bt->err;
	}

	// if key already exists, update id and return

	page = bt->page;

	if( bt->found = !keycmp (ptr, key, len) ) {
		slotptr(page, slot)->dead = 0;
		slotptr(page, slot)->tod = tod;
		bt_putid(slotptr(page,slot)->id, id);
		bt_unlockpage(BtLockWrite, bt->set);
		bt_unpinlatch(bt->set);
		bt_unpinpool (bt->pool);
		return bt->err;
	}

	// check if page has enough space

	if( slot = bt_cleanpage (bt, len, slot) )
		break;

	if( bt_splitpage (bt) )
		return bt->err;
  }

  // calculate next available slot and copy key into page

  page->min -= len + 1; // reset lowest used offset
  ((unsigned char *)page)[page->min] = len;
  memcpy ((unsigned char *)page + page->min +1, key, len );

  for( idx = slot; idx < page->cnt; idx++ )
	if( slotptr(page, idx)->dead )
		break;

  // now insert key into array before slot
  // preserving the fence slot

  if( idx == page->cnt )
	idx++, page->cnt++;

  page->act++;

  while( idx > slot )
	*slotptr(page, idx) = *slotptr(page, idx -1), idx--;

  bt_putid(slotptr(page,slot)->id, id);
  slotptr(page, slot)->off = page->min;
  slotptr(page, slot)->tod = tod;
  slotptr(page, slot)->dead = 0;

  bt_unlockpage (BtLockWrite, bt->set);
  bt_unpinlatch (bt->set);
  bt_unpinpool (bt->pool);
  return 0;
}

//  cache page of keys into cursor and return starting slot for given key

uint bt_startkey (BtDb *bt, unsigned char *key, uint len)
{
uint slot;

	// cache page for retrieval
	if( slot = bt_loadpage (bt, key, len, 0, BtLockRead) )
		memcpy (bt->cursor, bt->page, bt->mgr->page_size);
	bt->cursor_page = bt->page_no;
	bt_unlockpage(BtLockRead, bt->set);
	bt_unpinlatch (bt->set);
	bt_unpinpool (bt->pool);
	return slot;
}

//  return next slot for cursor page
//  or slide cursor right into next page

uint bt_nextkey (BtDb *bt, uint slot)
{
BtPool *pool;
BtPage page;
uid right;

  do {
	right = bt_getid(bt->cursor->right);
	while( slot++ < bt->cursor->cnt )
	  if( slotptr(bt->cursor,slot)->dead )
		continue;
	  else if( right || (slot < bt->cursor->cnt))
		return slot;
	  else
		break;

	if( !right )
		break;

	bt->cursor_page = right;

	if( pool = bt_pinpool (bt, right) )
		page = bt_page (bt, pool, right);
	else
		return 0;

	bt->set = bt_pinlatch (bt, right);
    bt_lockpage(BtLockRead, bt->set);

	memcpy (bt->cursor, page, bt->mgr->page_size);

	bt_unlockpage(BtLockRead, bt->set);
	bt_unpinlatch (bt->set);
	bt_unpinpool (pool);
	slot = 0;
  } while( 1 );

  return bt->err = 0;
}

BtKey bt_key(BtDb *bt, uint slot)
{
	return keyptr(bt->cursor, slot);
}

uid bt_uid(BtDb *bt, uint slot)
{
	return bt_getid(slotptr(bt->cursor,slot)->id);
}

uint bt_tod(BtDb *bt, uint slot)
{
	return slotptr(bt->cursor,slot)->tod;
}

#ifdef STANDALONE

void bt_latchaudit (BtDb *bt)
{
ushort idx, hashidx;
BtLatchSet *set;
BtPool *pool;
BtPage page;
uid page_no;

#ifdef unix
	for( idx = 1; idx < bt->mgr->latchmgr->latchdeployed; idx++ ) {
		set = bt->mgr->latchsets + idx;
		if( *(ushort *)set->readwr || *(ushort *)set->access || *(ushort *)set->parent ) {
			fprintf(stderr, "latchset %d locked for page %6x\n", idx, set->page_no);
			*(ushort *)set->readwr = 0;
			*(ushort *)set->access = 0;
			*(ushort *)set->parent = 0;
		}
		if( set->pin ) {
			fprintf(stderr, "latchset %d pinned\n", idx);
			set->pin = 0;
		}
	}

	for( hashidx = 0; hashidx < bt->mgr->latchmgr->latchhash; hashidx++ ) {
	  if( *(uint *)bt->mgr->latchmgr->table[hashidx].latch )
		fprintf(stderr, "latchmgr locked\n");
	  if( idx = bt->mgr->latchmgr->table[hashidx].slot ) do {
		set = bt->mgr->latchsets + idx;
		if( *(uint *)set->readwr || *(ushort *)set->access || *(ushort *)set->parent )
			fprintf(stderr, "latchset %d locked\n", idx);
		if( set->hash != hashidx )
			fprintf(stderr, "latchset %d wrong hashidx\n", idx);
		if( set->pin )
			fprintf(stderr, "latchset %d pinned\n", idx);
	  } while( idx = set->next );
	}
	page_no = bt_getid(bt->mgr->latchmgr->alloc[1].right);

	while( page_no ) {
		fprintf(stderr, "free: %.6x\n", (uint)page_no);
		pool = bt_pinpool (bt, page_no);
		page = bt_page (bt, pool, page_no);
	    page_no = bt_getid(page->right);
		bt_unpinpool (pool);
	}
#endif
}

typedef struct {
	char type, idx;
	char *infile;
	BtMgr *mgr;
	int num;
} ThreadArg;

//  standalone program to index file of keys
//  then list them onto std-out

#ifdef unix
void *index_file (void *arg)
#else
uint __stdcall index_file (void *arg)
#endif
{
int line = 0, found = 0, cnt = 0;
uid next, page_no = LEAF_page;	// start on first page of leaves
unsigned char key[256];
ThreadArg *args = arg;
int ch, len = 0, slot;
time_t tod[1];
BtPool *pool;
BtPage page;
BtKey ptr;
BtDb *bt;
FILE *in;

	bt = bt_open (args->mgr);
	time (tod);

	switch(args->type | 0x20)
	{
	case 'a':
		fprintf(stderr, "started latch mgr audit\n");
		bt_latchaudit (bt);
		fprintf(stderr, "finished latch mgr audit\n");
		break;

	case 'w':
		fprintf(stderr, "started indexing for %s\n", args->infile);
		if( in = fopen (args->infile, "rb") )
		  while( ch = getc(in), ch != EOF )
			if( ch == '\n' )
			{
			  line++;

			  if( args->num == 1 )
		  		sprintf((char *)key+len, "%.9d", 1000000000 - line), len += 9;

			  else if( args->num )
		  		sprintf((char *)key+len, "%.9d", line + args->idx * args->num), len += 9;

			  if( bt_insertkey (bt, key, len, 0, line, *tod) )
				fprintf(stderr, "Error %d Line: %d\n", bt->err, line), exit(0);
			  len = 0;
			}
			else if( len < 255 )
				key[len++] = ch;
		fprintf(stderr, "finished %s for %d keys\n", args->infile, line);
		break;

	case 'd':
		fprintf(stderr, "started deleting keys for %s\n", args->infile);
		if( in = fopen (args->infile, "rb") )
		  while( ch = getc(in), ch != EOF )
			if( ch == '\n' )
			{
			  line++;
			  if( args->num == 1 )
		  		sprintf((char *)key+len, "%.9d", 1000000000 - line), len += 9;

			  else if( args->num )
		  		sprintf((char *)key+len, "%.9d", line + args->idx * args->num), len += 9;

			  if( bt_deletekey (bt, key, len, 0) )
				fprintf(stderr, "Error %d Line: %d\n", bt->err, line), exit(0);
			  len = 0;
			}
			else if( len < 255 )
				key[len++] = ch;
		fprintf(stderr, "finished %s for keys, %d \n", args->infile, line);
		break;

	case 'f':
		fprintf(stderr, "started finding keys for %s\n", args->infile);
		if( in = fopen (args->infile, "rb") )
		  while( ch = getc(in), ch != EOF )
			if( ch == '\n' )
			{
			  line++;
			  if( args->num == 1 )
		  		sprintf((char *)key+len, "%.9d", 1000000000 - line), len += 9;

			  else if( args->num )
		  		sprintf((char *)key+len, "%.9d", line + args->idx * args->num), len += 9;

			  if( bt_findkey (bt, key, len) )
				found++;
			  else if( bt->err )
				fprintf(stderr, "Error %d Syserr %d Line: %d\n", bt->err, errno, line), exit(0);
			  len = 0;
			}
			else if( len < 255 )
				key[len++] = ch;
		fprintf(stderr, "finished %s for %d keys, found %d\n", args->infile, line, found);
		break;

	case 's':
		len = key[0] = 0;

		fprintf(stderr, "started reading\n");

		if( slot = bt_startkey (bt, key, len) )
		  slot--;
		else
		  fprintf(stderr, "Error %d in StartKey. Syserror: %d\n", bt->err, errno), exit(0);

		while( slot = bt_nextkey (bt, slot) ) {
			ptr = bt_key(bt, slot);
			fwrite (ptr->key, ptr->len, 1, stdout);
			fputc ('\n', stdout);
		}

		break;

	case 'c':
		fprintf(stderr, "started reading\n");

	  	do {
			if( bt->pool = bt_pinpool (bt, page_no) )
				page = bt_page (bt, bt->pool, page_no);
			else
				break;
			bt->set = bt_pinlatch (bt, page_no);
			bt_lockpage (BtLockRead, bt->set);
			cnt += page->act;
			next = bt_getid (page->right);
			bt_unlockpage (BtLockRead, bt->set);
			bt_unpinlatch (bt->set);
			bt_unpinpool (bt->pool);
	  	} while( page_no = next );

	  	cnt--;	// remove stopper key
		fprintf(stderr, " Total keys read %d\n", cnt);
		break;
	}

	bt_close (bt);
#ifdef unix
	return NULL;
#else
	return 0;
#endif
}

typedef struct timeval timer;

int main (int argc, char **argv)
{
int idx, cnt, len, slot, err;
int segsize, bits = 16;
#ifdef unix
pthread_t *threads;
timer start, stop;
#else
time_t start[1], stop[1];
HANDLE *threads;
#endif
double real_time;
ThreadArg *args;
uint poolsize = 0;
int num = 0;
char key[1];
BtMgr *mgr;
BtKey ptr;
BtDb *bt;

	if( argc < 3 ) {
		fprintf (stderr, "Usage: %s idx_file Read/Write/Scan/Delete/Find [page_bits mapped_segments seg_bits line_numbers src_file1 src_file2 ... ]\n", argv[0]);
		fprintf (stderr, "  where page_bits is the page size in bits\n");
		fprintf (stderr, "  mapped_segments is the number of mmap segments in buffer pool\n");
		fprintf (stderr, "  seg_bits is the size of individual segments in buffer pool in pages in bits\n");
		fprintf (stderr, "  line_numbers = 1 to append line numbers to keys\n");
		fprintf (stderr, "  src_file1 thru src_filen are files of keys separated by newline\n");
		exit(0);
	}

#ifdef unix
	gettimeofday(&start, NULL);
#else
	time(start);
#endif

	if( argc > 3 )
		bits = atoi(argv[3]);

	if( argc > 4 )
		poolsize = atoi(argv[4]);

	if( !poolsize )
		fprintf (stderr, "Warning: no mapped_pool\n");

	if( poolsize > 65535 )
		fprintf (stderr, "Warning: mapped_pool > 65535 segments\n");

	if( argc > 5 )
		segsize = atoi(argv[5]);
	else
		segsize = 4; 	// 16 pages per mmap segment

	if( argc > 6 )
		num = atoi(argv[6]);

	cnt = argc - 7;
#ifdef unix
	threads = malloc (cnt * sizeof(pthread_t));
#else
	threads = GlobalAlloc (GMEM_FIXED|GMEM_ZEROINIT, cnt * sizeof(HANDLE));
#endif
	args = malloc (cnt * sizeof(ThreadArg));

	mgr = bt_mgr ((argv[1]), BT_rw, bits, poolsize, segsize, poolsize / 8);

	if( !mgr ) {
		fprintf(stderr, "Index Open Error %s\n", argv[1]);
		exit (1);
	}

	//	fire off threads

	for( idx = 0; idx < cnt; idx++ ) {
		args[idx].infile = argv[idx + 7];
		args[idx].type = argv[2][0];
		args[idx].mgr = mgr;
		args[idx].num = num;
		args[idx].idx = idx;
#ifdef unix
		if( err = pthread_create (threads + idx, NULL, index_file, args + idx) )
			fprintf(stderr, "Error creating thread %d\n", err);
#else
		threads[idx] = (HANDLE)_beginthreadex(NULL, 65536, index_file, args + idx, 0, NULL);
#endif
	}

	// 	wait for termination

#ifdef unix
	for( idx = 0; idx < cnt; idx++ )
		pthread_join (threads[idx], NULL);
	gettimeofday(&stop, NULL);
	real_time = 1000.0 * ( stop.tv_sec - start.tv_sec ) + 0.001 * (stop.tv_usec - start.tv_usec );
#else
	WaitForMultipleObjects (cnt, threads, TRUE, INFINITE);

	for( idx = 0; idx < cnt; idx++ )
		CloseHandle(threads[idx]);

	time (stop);
	real_time = 1000 * (*stop - *start);
#endif
	fprintf(stderr, " Time to complete: %.2f seconds\n", real_time/1000);
	bt_mgrclose (mgr);
}

#endif	//STANDALONE