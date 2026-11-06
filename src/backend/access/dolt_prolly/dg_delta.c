/*-------------------------------------------------------------------------
 *
 * dg_delta.c
 *	  The prolly delta store: every tree's changes since its base, shared.
 *
 * Every prolly tree, one storage on one branch, is a base, which a
 * pg_dolt_root row names, plus a delta: the versions transactions wrote to it
 * since. A version is a signed occurrence count for one key with the row the
 * key names, stamped with its writer's top-level xid, (sub)transaction xid
 * and command. Every tree's values are counts followed by rows, and counts
 * add, so a delta applies in any order.
 *
 * A write appends its versions here and to the WAL in one step, where heap
 * would write its tuples; its commit is an ordinary commit record. A reader
 * filters versions by its snapshot, as heap filters tuples by xmin, and
 * leaves out those its base already holds: a base folded by a transaction
 * holds every version that transaction's snapshot saw (DoltDeltaHorizon).
 *
 * The versions live in shared memory, in a dynamic shared memory area created
 * in place in the main segment, so that the startup process can replay into
 * it and the checkpointer can save it. A partitioned hash table maps tree
 * keys to trees. Each tree is a skiplist of its keys, as Doltgrust's memtable
 * keeps a tree's keys in order, each key with its versions oldest first, under
 * the tree's own lock: a reader finds one key's versions, or the next keys in
 * either direction, without reading the rest.
 *
 * Durability follows heap's buffers: a version reaches the WAL before its
 * transaction commits, and a checkpoint saves every version whose record
 * precedes its redo point to pg_dolt/delta.state, so that recovery loads the
 * file and replays the rest. Appends hold DELAY_CHKPT_START between their WAL
 * insertion and linking the versions, so a checkpoint never finds a record
 * before its redo point whose versions are missing.
 *
 * src/backend/access/dolt_prolly/dg_delta.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/doltdelta.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/xloginsert.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/standby.h"
#include "utils/dsa.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

int			dolt_delta_memory = 256 * 1024;	/* kB */
int			dolt_delta_memory_limit = -1;	/* kB */
int			dolt_delta_throttle = 75;	/* percent of dolt_delta_memory */
double		dolt_delta_throttle_delay = 0.1;	/* ms per kB appended, at the budget */

#define DELTA_PARTITIONS 16

/* Trees with a delta at once. */
#define DELTA_MAX_TREES 16384

/* The smallest area, so that its own bookkeeping fits. */
#define DELTA_MIN_BYTES ((Size) 1024 * 1024)

/* The WAL payload of one append record, past which an append writes another. */
#define DELTA_RECORD_BYTES ((Size) 1024 * 1024)

/* How long a writer at the memory limit naps between looks, and how often it says so. */
#define DELTA_FULL_NAP_US 10000
#define DELTA_FULL_LOG_MS 10000

/* The longest one append's throttle delay. */
#define DELTA_THROTTLE_MAX_US 1000000

/* Keys a tree queues to settle, at most; past it, the next fold drops them. */
#define SETTLE_QUEUE_MAX 1048576

/* Skiplist levels; each level holds about a quarter of the keys below it. */
#define DELTA_MAX_HEIGHT 12

/* A version's status, as its writer's xids decide it once known. */
#define VERSION_COMMITTED	0x1
#define VERSION_ABORTED		0x2

#define DELTA_DIR			"pg_dolt"
#define DELTA_STATE_FILE	DELTA_DIR "/delta.state"
#define DELTA_STATE_TEMP	DELTA_DIR "/delta.state.tmp"
#define DELTA_STATE_MAGIC	0x44474c34	/* "DGL4" */

/* How much of the state file a checkpoint buffers before writing it. */
#define DELTA_STATE_CHUNK	(1024 * 1024)

typedef struct DeltaVersion
{
	dsa_pointer next;			/* the key's next newer version */
	uint64		seq;
	XLogRecPtr	lsn;			/* where its WAL record starts; invalid if unlogged */
	TransactionId topxid;
	TransactionId xid;			/* the writing (sub)transaction */
	CommandId	cid;
	pg_atomic_uint32 flags;
	int64		delta;
	uint32		rowlen;
	uint8		row[FLEXIBLE_ARRAY_MEMBER];
} DeltaVersion;

/* A key of a tree's skiplist, with its versions. The key's bytes follow next[]. */
typedef struct DeltaKey
{
	dsa_pointer first;			/* oldest version */
	dsa_pointer last;			/* newest version */
	uint32		keylen;
	uint32		height;
	dsa_pointer next[FLEXIBLE_ARRAY_MEMBER];
} DeltaKey;

#define KEY_BYTES(k) ((uint8 *) &(k)->next[(k)->height])

typedef struct DeltaTree
{
	DoltDeltaTree key;			/* hash key */
	LWLock		lock;
	dsa_pointer head;			/* a key of DELTA_MAX_HEIGHT with no bytes */
	uint64		count;			/* versions */
	uint64		keys;
	uint64		bytes;
	uint64		next_seq;
	int64		net;			/* every version's delta, summed */
	TimestampTz oldest;			/* when the oldest version still held arrived */
	dsa_pointer settle_queue;	/* keys that gained a version, to settle: key nodes linked by next[0] */
	uint32		settle_queued;
	pg_prng_state heights;

	/*
	 * The newest fold's transaction and the horizon its base holds: once the
	 * fold committed and every snapshot sees it, the versions it holds go.
	 */
	TransactionId fold;
	DoltDeltaHorizon folded;	/* xip in |folded_xip| */
	dsa_pointer folded_xip;
} DeltaTree;

typedef struct DeltaControl
{
	Size		budget;			/* dolt_delta_memory: the area's place in the main segment */
	pg_atomic_uint64 bytes;		/* held by every tree's keys and versions */
	pg_atomic_uint64 throttled_us;
	pg_atomic_uint64 limit_waits;
	LWLockPadded locks[DELTA_PARTITIONS];
	int			npins;
	pg_atomic_uint32 pins[FLEXIBLE_ARRAY_MEMBER];	/* per process: the oldest xmin it loaded under */
} DeltaControl;

/* One saved version in the state file. */
typedef struct DeltaSaved
{
	DoltDeltaTree tree;
	TransactionId topxid;
	TransactionId xid;
	CommandId	cid;
	XLogRecPtr	lsn;
	int64		delta;
	uint32		keylen;
	uint32		rowlen;
} DeltaSaved;

/*
 * A tree's fold note in the state file, then its running xids: without it, a
 * restart would keep versions the fold's base holds and could not prune them,
 * and readers of its base would know no better than the root row says.
 */
typedef struct DeltaSavedFold
{
	DoltDeltaTree tree;
	TransactionId fold;
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
} DeltaSavedFold;

/* The state file's trailer, after its versions and fold notes, before its CRC. */
typedef struct DeltaStateHeader
{
	uint32		magic;
	uint32		pad;
	XLogRecPtr	redo;
	uint64		count;
	uint64		folds;
} DeltaStateHeader;

static DeltaControl *control;
static HTAB *trees;
static dsa_area *area;

/* What this backend's top-level transaction has appended, which no fold frees before it ends. */
static TransactionId appended_xid = InvalidTransactionId;
static uint64 appended_bytes;

static DeltaTree **trees_all(int *count);
static void fold_note(const DoltDeltaTree *key, TransactionId fold, const DoltDeltaHorizon *horizon);

static Size
area_bytes(void)
{
	return Max((Size) dolt_delta_memory * 1024, DELTA_MIN_BYTES);
}

static int
pin_slots(void)
{
	return MaxBackends + NUM_AUXILIARY_PROCS;
}

static Size
control_size(void)
{
	return MAXALIGN(add_size(offsetof(DeltaControl, pins), mul_size(pin_slots(), sizeof(pg_atomic_uint32))));
}

static void *
area_place(void)
{
	return (char *) control + control_size();
}

Size
DoltDeltaShmemSize(void)
{
	Size		size = add_size(control_size(), area_bytes());

	return add_size(size, hash_estimate_size(DELTA_MAX_TREES, sizeof(DeltaTree)));
}

void
DoltDeltaShmemInit(void)
{
	HASHCTL		info = {0};
	bool		found;

	control = ShmemInitStruct("Dolt delta store", add_size(control_size(), area_bytes()), &found);
	if (!found)
	{
		dsa_area   *created;

		control->budget = area_bytes();
		pg_atomic_init_u64(&control->bytes, 0);
		pg_atomic_init_u64(&control->throttled_us, 0);
		pg_atomic_init_u64(&control->limit_waits, 0);
		for (int i = 0; i < DELTA_PARTITIONS; i++)
			LWLockInitialize(&control->locks[i].lock, LWTRANCHE_DOLT_DELTA_TREES);
		control->npins = pin_slots();
		for (int i = 0; i < control->npins; i++)
			pg_atomic_init_u32(&control->pins[i], InvalidTransactionId);

		/*
		 * The postmaster cannot use dynamic segments: cap the area at its
		 * place until a process that can attaches.
		 */
		created = dsa_create_in_place(area_place(), control->budget, LWTRANCHE_DOLT_DELTA_DSA, NULL);
		dsa_pin(created);
		dsa_set_size_limit(created, control->budget);
		dsa_detach(created);
	}
	info.keysize = sizeof(DoltDeltaTree);
	info.entrysize = sizeof(DeltaTree);
	info.num_partitions = DELTA_PARTITIONS;
	trees = ShmemInitHash("Dolt delta trees", DELTA_MAX_TREES, DELTA_MAX_TREES, &info,
						  HASH_ELEM | HASH_BLOBS | HASH_PARTITION | HASH_FIXED_SIZE);
}

static void
delta_attach(void)
{
	if (!area)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		area = dsa_attach_in_place(area_place(), NULL);
		dsa_pin_mapping(area);

		/*
		 * Past its budget, the store grows into dynamic segments rather than
		 * refuse writes, which a transaction larger than the budget, or folds
		 * that fall behind, would otherwise meet. Writers are slowed there,
		 * and wait at dolt_delta_memory_limit, before they allocate.
		 */
		if (IsUnderPostmaster)
			dsa_set_size_limit(area, (size_t) -1);
		MemoryContextSwitchTo(old);
	}
}

void
dolt_delta_tree_key(DoltDeltaTree *tree, Oid db, Oid space, RelFileNumber file, const char *branch)
{
	/* The key is hashed and compared as bytes: no garbage in the name's tail. */
	memset(tree, 0, sizeof(*tree));
	if (strlen(branch) >= sizeof(tree->branch))
		ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("branch name is too long for prolly storage")));
	tree->db = db;
	tree->space = space;
	tree->file = file;
	strlcpy(tree->branch, branch, sizeof(tree->branch));
}

static DeltaKey *
key_at(dsa_pointer dp)
{
	return (DeltaKey *) dsa_get_address(area, dp);
}

static DeltaVersion *
version_at(dsa_pointer dp)
{
	return (DeltaVersion *) dsa_get_address(area, dp);
}

/*
 * A key node of |height| for |key|, unlinked, or invalid when the area is
 * full. A tree's head is one with no bytes and the most levels.
 */
static dsa_pointer
key_make(const uint8 *key, uint32 keylen, uint32 height)
{
	Size		size = offsetof(DeltaKey, next) + height * sizeof(dsa_pointer) + keylen;
	dsa_pointer dp = dsa_allocate_extended(area, size, DSA_ALLOC_NO_OOM);
	DeltaKey   *k;

	if (!DsaPointerIsValid(dp))
		return dp;
	k = key_at(dp);
	k->first = k->last = InvalidDsaPointer;
	k->keylen = keylen;
	k->height = height;
	for (uint32 i = 0; i < height; i++)
		k->next[i] = InvalidDsaPointer;
	if (keylen)
		memcpy(KEY_BYTES(k), key, keylen);
	return dp;
}

/*
 * The tree for |key|, entered if |create|. Trees are never removed while the
 * server runs, so the pointer stays good after the partition lock is let go.
 */
static DeltaTree *
tree_find(const DoltDeltaTree *key, bool create)
{
	uint32		hash = get_hash_value(trees, key);
	LWLock	   *partition = &control->locks[hash % DELTA_PARTITIONS].lock;
	DeltaTree  *tree;
	bool		found;

	LWLockAcquire(partition, LW_SHARED);
	tree = hash_search_with_hash_value(trees, key, hash, HASH_FIND, NULL);
	LWLockRelease(partition);
	if (tree || !create)
		return tree;
	LWLockAcquire(partition, LW_EXCLUSIVE);
	tree = hash_search_with_hash_value(trees, key, hash, HASH_ENTER_NULL, &found);
	if (tree && !found)
	{
		LWLockInitialize(&tree->lock, LWTRANCHE_DOLT_DELTA_TREE);
		tree->head = key_make(NULL, 0, DELTA_MAX_HEIGHT);
		if (!DsaPointerIsValid(tree->head))
		{
			hash_search_with_hash_value(trees, key, hash, HASH_REMOVE, NULL);
			tree = NULL;
		}
		else
		{
			tree->count = tree->keys = tree->bytes = 0;
			tree->net = 0;
			tree->next_seq = 1;
			tree->oldest = 0;
			tree->fold = InvalidTransactionId;
			memset(&tree->folded, 0, sizeof(tree->folded));
			tree->folded_xip = InvalidDsaPointer;
			tree->settle_queue = InvalidDsaPointer;
			tree->settle_queued = 0;
			pg_prng_seed(&tree->heights, hash);
		}
	}
	LWLockRelease(partition);
	if (!tree)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("too many prolly trees have pending changes")));
	return tree;
}

/* Compare as pl_compare does: bytes, then length. */
static int
key_compare(const uint8 *a, uint32 alen, const uint8 *b, uint32 blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	if (c != 0)
		return c;
	return alen < blen ? -1 : alen > blen ? 1 : 0;
}

/*
 * The first key of |tree| at or past |key|, or invalid; |preds| receives, per
 * level, the last key before it (the head if none).
 */
static dsa_pointer
key_search(DeltaTree *tree, const uint8 *key, uint32 keylen, dsa_pointer *preds)
{
	dsa_pointer at = tree->head;
	dsa_pointer next = InvalidDsaPointer;

	for (int level = DELTA_MAX_HEIGHT - 1; level >= 0; level--)
	{
		for (;;)
		{
			DeltaKey   *k;

			next = key_at(at)->next[level];
			if (!DsaPointerIsValid(next))
				break;
			k = key_at(next);
			if (key_compare(KEY_BYTES(k), k->keylen, key, keylen) >= 0)
				break;
			at = next;
		}
		if (preds)
			preds[level] = at;
	}
	return next;
}

/* The last key of |tree| before |key|, or its last key if |key| is NULL; invalid if none. */
static dsa_pointer
key_before(DeltaTree *tree, const uint8 *key, uint32 keylen)
{
	dsa_pointer at = tree->head;

	for (int level = DELTA_MAX_HEIGHT - 1; level >= 0; level--)
	{
		for (;;)
		{
			dsa_pointer next = key_at(at)->next[level];
			DeltaKey   *k;

			if (!DsaPointerIsValid(next))
				break;
			k = key_at(next);
			if (key && key_compare(KEY_BYTES(k), k->keylen, key, keylen) >= 0)
				break;
			at = next;
		}
	}
	return at == tree->head ? InvalidDsaPointer : at;
}

static uint32
random_height(DeltaTree *tree)
{
	uint32		height = 1;

	while (height < DELTA_MAX_HEIGHT && pg_prng_uint32(&tree->heights) % 4 == 0)
		height++;
	return height;
}

/* A version's bytes, filled but not linked; invalid when the area is full. */
static dsa_pointer
version_make(const DoltDeltaEdit *edit, TransactionId topxid, TransactionId xid, CommandId cid)
{
	Size		size = offsetof(DeltaVersion, row) + edit->rowlen;
	dsa_pointer dp = dsa_allocate_extended(area, size, DSA_ALLOC_NO_OOM);
	DeltaVersion *v;

	if (!DsaPointerIsValid(dp))
		return dp;
	v = version_at(dp);
	v->next = InvalidDsaPointer;
	v->seq = 0;
	v->lsn = InvalidXLogRecPtr;
	v->topxid = topxid;
	v->xid = xid;
	v->cid = cid;
	pg_atomic_init_u32(&v->flags, 0);
	v->delta = edit->delta;
	v->rowlen = edit->rowlen;
	if (edit->rowlen)
		memcpy(v->row, edit->row, edit->rowlen);
	return dp;
}

/*
 * What one append of a version needs, allocated before the critical section
 * that links it: the version, and a key node in case its key is new.
 */
typedef struct Pending
{
	dsa_pointer version;
	dsa_pointer key;			/* unused, and freed, if the key exists */
} Pending;

/* Allocate |pending| for |edit|; false, with nothing allocated, if the area is full. */
static bool
pending_make(DeltaTree *tree, Pending *pending, const DoltDeltaEdit *edit, TransactionId topxid,
			 TransactionId xid, CommandId cid)
{
	pending->version = version_make(edit, topxid, xid, cid);
	if (!DsaPointerIsValid(pending->version))
		return false;
	pending->key = key_make(edit->key, edit->keylen, random_height(tree));
	if (!DsaPointerIsValid(pending->key))
	{
		dsa_free(area, pending->version);
		return false;
	}
	return true;
}

/*
 * Link pending versions of |edits| into |tree|, as written at |lsn|. Cannot
 * fail. A pending key node a version did not need is left for
 * pending_release, to be freed outside any critical section.
 */
static void
versions_link(DeltaTree *tree, const DoltDeltaEdit *edits, Pending *pending, int count, XLogRecPtr lsn)
{
	uint64		bytes;

	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	bytes = tree->bytes;
	if (count && !tree->count)
		tree->oldest = GetCurrentTimestamp();
	for (int i = 0; i < count; i++)
	{
		dsa_pointer preds[DELTA_MAX_HEIGHT];
		dsa_pointer found = key_search(tree, edits[i].key, edits[i].keylen, preds);
		DeltaVersion *v = version_at(pending[i].version);
		DeltaKey   *k;

		v->seq = tree->next_seq++;
		v->lsn = lsn;
		if (DsaPointerIsValid(found) &&
			key_compare(KEY_BYTES(key_at(found)), key_at(found)->keylen, edits[i].key, edits[i].keylen) == 0)
		{
			k = key_at(found);
			version_at(k->last)->next = pending[i].version;
			k->last = pending[i].version;

			/*
			 * The key may settle once every snapshot sees its versions: queue
			 * it, on the node the append made in case the key was new. Replay
			 * queues nothing; only a primary settles.
			 */
			if (DsaPointerIsValid(pending[i].key) && tree->settle_queued < SETTLE_QUEUE_MAX && !RecoveryInProgress())
			{
				DeltaKey   *q = key_at(pending[i].key);

				q->next[0] = tree->settle_queue;
				tree->settle_queue = pending[i].key;
				tree->settle_queued++;
				tree->bytes += offsetof(DeltaKey, next) + q->height * sizeof(dsa_pointer) + q->keylen;
				pending[i].key = InvalidDsaPointer;
			}
		}
		else
		{
			k = key_at(pending[i].key);
			for (uint32 level = 0; level < k->height; level++)
			{
				DeltaKey   *pred = key_at(preds[level]);

				k->next[level] = pred->next[level];
				pred->next[level] = pending[i].key;
			}
			k->first = k->last = pending[i].version;
			tree->keys++;
			tree->bytes += offsetof(DeltaKey, next) + k->height * sizeof(dsa_pointer) + k->keylen;
			pending[i].key = InvalidDsaPointer;
		}
		tree->count++;
		tree->net += v->delta;
		tree->bytes += offsetof(DeltaVersion, row) + v->rowlen;
	}
	pg_atomic_fetch_add_u64(&control->bytes, tree->bytes - bytes);
	LWLockRelease(&tree->lock);
}

/* Free the key nodes linking found unneeded. */
static void
pending_release(Pending *pending, int count)
{
	for (int i = 0; i < count; i++)
		if (DsaPointerIsValid(pending[i].key))
			dsa_free(area, pending[i].key);
}

/* The WAL record of versions [first, last) of an append, as one buffer. */
static char *
record_build(const DoltDeltaTree *key, TransactionId topxid, CommandId cid,
			 const DoltDeltaEdit *edits, int first, int last, Size *size)
{
	xl_dolt_delta_append header = {0};
	uint16		branchlen = strlen(key->branch);
	Size		total = SizeOfDoltDeltaAppend + branchlen;
	char	   *buffer;
	char	   *p;

	for (int i = first; i < last; i++)
		total += SizeOfDoltDeltaVersion + edits[i].keylen + edits[i].rowlen;
	buffer = palloc(total);
	header.db = key->db;
	header.space = key->space;
	header.file = key->file;
	header.topxid = topxid;
	header.cid = cid;
	header.count = last - first;
	header.branchlen = branchlen;
	memcpy(buffer, &header, SizeOfDoltDeltaAppend);
	p = buffer + SizeOfDoltDeltaAppend;
	memcpy(p, key->branch, branchlen);
	p += branchlen;
	for (int i = first; i < last; i++)
	{
		xl_dolt_delta_version version = {0};

		version.delta = edits[i].delta;
		version.keylen = edits[i].keylen;
		version.rowlen = edits[i].rowlen;
		memcpy(p, &version, SizeOfDoltDeltaVersion);
		p += SizeOfDoltDeltaVersion;
		memcpy(p, edits[i].key, edits[i].keylen);
		p += edits[i].keylen;
		if (edits[i].rowlen)
			memcpy(p, edits[i].row, edits[i].rowlen);
		p += edits[i].rowlen;
	}
	Assert(p == buffer + total);
	*size = total;
	return buffer;
}

/* The memory a writer may hold the store to, or 0 for no limit. */
static uint64
delta_limit(void)
{
	if (dolt_delta_memory_limit < 0)
		return 0;
	return Max((uint64) dolt_delta_memory_limit * 1024, control->budget);
}

/*
 * One nap of a writer that cannot append yet: the store is at its limit, or
 * the system would give it no more memory. Folds free memory as every
 * snapshot reads past them; the writer waits for that rather than fail, and
 * says so every DELTA_FULL_LOG_MS.
 */
static void
delta_wait(TimestampTz *said)
{
	TimestampTz now = GetCurrentTimestamp();

	if (*said == 0)
		pg_atomic_fetch_add_u64(&control->limit_waits, 1);
	if (*said == 0 || TimestampDifferenceExceeds(*said, now, DELTA_FULL_LOG_MS))
	{
		ereport(LOG,
				(errmsg("waiting for prolly folds to free delta store memory"),
				 errdetail("The store holds %llu bytes; its budget is %zu bytes.",
						   (unsigned long long) pg_atomic_read_u64(&control->bytes), control->budget)));
		*said = now;
	}
	pgstat_report_wait_start(WAIT_EVENT_DOLT_DELTA_LIMIT);
	pg_usleep(DELTA_FULL_NAP_US);
	pgstat_report_wait_end();
	CHECK_FOR_INTERRUPTS();
}

/*
 * Slow a writer about to append |edits| while the store is past its throttle
 * point: by dolt_delta_throttle_delay per kilobyte at the budget, in
 * proportion to how far past the point it is, so that the further folds fall
 * behind, the more writers yield to them. At dolt_delta_memory_limit, wait
 * for memory first; that counts every version, a writer's own included.
 */
static void
delta_throttle(const DoltDeltaEdit *edits, int count)
{
	TransactionId topxid = GetTopTransactionId();
	uint64		budget = control->budget;
	uint64		start = budget * dolt_delta_throttle / 100;
	uint64		limit = delta_limit();
	uint64		bytes = pg_atomic_read_u64(&control->bytes);
	uint64		appending = 0;
	double		past;
	long		delay_us;

	if (limit)
	{
		for (TimestampTz said = 0; bytes >= limit; bytes = pg_atomic_read_u64(&control->bytes))
			delta_wait(&said);
	}
	for (int i = 0; i < count; i++)
		appending += offsetof(DeltaVersion, row) + edits[i].rowlen + offsetof(DeltaKey, next) + edits[i].keylen;

	/*
	 * A writer's own transaction does not count toward its throttling: no
	 * fold frees those versions before it ends, so slowing it for them would
	 * only slow a large transaction the more it wrote.
	 */
	if (appended_xid != topxid)
	{
		appended_xid = topxid;
		appended_bytes = 0;
	}
	bytes -= Min(bytes, appended_bytes);
	appended_bytes += appending;
	if (bytes <= start || dolt_delta_throttle_delay <= 0)
		return;
	past = (double) (bytes - start) / (double) Max(budget - start, 1);
	delay_us = (long) Min(past * dolt_delta_throttle_delay * 1000.0 * ((double) appending / 1024.0),
						  (double) DELTA_THROTTLE_MAX_US);
	if (delay_us <= 0)
		return;
	pg_atomic_fetch_add_u64(&control->throttled_us, delay_us);
	pgstat_report_wait_start(WAIT_EVENT_DOLT_DELTA_THROTTLE);
	pg_usleep(delay_us);
	pgstat_report_wait_end();
	CHECK_FOR_INTERRUPTS();
}

/*
 * Append |count| versions to |key|'s delta as the current (sub)transaction's,
 * in command |cid|, which made them. A |logged| append reaches the WAL first,
 * one record per DELTA_RECORD_BYTES of payload; an unlogged one, for
 * relations no other session or recovery can see, stays in memory alone.
 */
void
dolt_delta_append(const DoltDeltaTree *key, const DoltDeltaEdit *edits, int count, CommandId cid, bool logged)
{
	TransactionId topxid;
	TransactionId xid;
	DeltaTree  *tree;
	Pending    *pending;
	int			first = 0;

	if (count <= 0)
		return;
	delta_attach();
	topxid = GetTopTransactionId();
	xid = GetCurrentTransactionId();
	(void) GetCurrentCommandId(true);	/* the command wrote */
	tree = tree_find(key, true);

	/*
	 * Allocate everything before the critical section, which must not fail.
	 * Past the throttle point, wait first in proportion to what this appends,
	 * so that writers slow to the pace folds free memory, as Doltgrust
	 * throttles writers past its delta budget.
	 */
	delta_throttle(edits, count);
	pending = palloc(count * sizeof(Pending));
	for (TimestampTz said = 0;;)
	{
		int			made = 0;

		while (made < count && pending_make(tree, &pending[made], &edits[made], topxid, xid, cid))
			made++;
		if (made == count)
			break;
		for (int j = 0; j < made; j++)
		{
			dsa_free(area, pending[j].version);
			dsa_free(area, pending[j].key);
		}

		/* Only a process that cannot grow the area meets it full: no folds run there. */
		if (!IsUnderPostmaster)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("prolly delta memory is full"),
					 errhint("Increase \"dolt_delta_memory\", or run dolt_fold().")));
		delta_wait(&said);
	}

	if (!logged)
	{
		versions_link(tree, edits, pending, count, InvalidXLogRecPtr);
		pending_release(pending, count);
		pfree(pending);
		return;
	}

	while (first < count)
	{
		int			last = first;
		Size		payload = 0;
		Size		size;
		char	   *record;

		while (last < count && (last == first || payload < DELTA_RECORD_BYTES))
		{
			payload += SizeOfDoltDeltaVersion + edits[last].keylen + edits[last].rowlen;
			last++;
		}
		record = record_build(key, topxid, cid, edits, first, last, &size);

		XLogBeginInsert();
		XLogRegisterData(record, size);

		/*
		 * A checkpoint must not complete between the record's insertion and
		 * the versions' linking: it would save neither, and replay would
		 * start past the record.
		 */
		START_CRIT_SECTION();
		Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;
		(void) XLogInsert(RM_DOLT_DELTA_ID, XLOG_DOLT_DELTA_APPEND);
		versions_link(tree, edits + first, pending + first, last - first, ProcLastRecPtr);
		MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
		END_CRIT_SECTION();

		pfree(record);
		first = last;
	}
	pending_release(pending, count);
	pfree(pending);
}

/*
 * Whether |v|, whose writer has ended, committed: its (sub)transaction's
 * status, kept as a hint once known.
 */
static bool
version_ended_committed(DeltaVersion *v)
{
	uint32		flags = pg_atomic_read_u32(&v->flags);

	if (flags & VERSION_COMMITTED)
		return true;
	if (flags & VERSION_ABORTED)
		return false;
	if (TransactionIdDidCommit(v->xid))
	{
		pg_atomic_fetch_or_u32(&v->flags, VERSION_COMMITTED);
		return true;
	}

	/* Rolled back as a whole or in part, or lost in a crash. */
	pg_atomic_fetch_or_u32(&v->flags, VERSION_ABORTED);
	return false;
}

/*
 * Whether |snapshot| sees |v|, as HeapTupleSatisfiesMVCC sees a tuple's xmin:
 * this transaction's own versions by command, unless their subtransaction
 * rolled back; others' when committed and not running for the snapshot.
 * Non-MVCC snapshots see what is committed now, and all of their own.
 */
static bool
version_visible(DeltaVersion *v, Snapshot snapshot)
{
	uint32		flags = pg_atomic_read_u32(&v->flags);

	if (flags & VERSION_ABORTED)
		return false;
	if (flags & VERSION_COMMITTED)
		return !snapshot || !IsMVCCSnapshot(snapshot) || !XidInMVCCSnapshot(v->topxid, snapshot);
	if (TransactionIdIsCurrentTransactionId(v->xid))
		return !snapshot || !IsMVCCSnapshot(snapshot) || v->cid < snapshot->curcid;
	if (snapshot && IsMVCCSnapshot(snapshot))
	{
		/* Not running for the snapshot: ended before it was taken. */
		if (XidInMVCCSnapshot(v->topxid, snapshot))
			return false;
	}
	else if (TransactionIdIsInProgress(v->topxid))
		return false;
	return version_ended_committed(v);
}

/* Whether |base| holds |v|, a committed version: its fold's snapshot saw it. */
static bool
base_holds(const DoltDeltaHorizon *base, const DeltaVersion *v)
{
	if (!base || !TransactionIdIsValid(base->xmax))
		return false;
	if (TransactionIdIsCurrentTransactionId(v->xid))
		return false;
	if (TransactionIdFollowsOrEquals(v->topxid, base->xmax))
		return false;
	if (TransactionIdPrecedes(v->topxid, base->xmin))
		return true;
	for (uint32 i = 0; i < base->xcnt; i++)
		if (TransactionIdEquals(base->xip[i], v->topxid))
			return false;
	return true;
}

/* Whether a reader of |view| counts |v|. */
static bool
view_counts(const DoltDeltaView *view, DeltaVersion *v)
{
	if (!view->own && TransactionIdIsCurrentTransactionId(v->xid))
		return false;
	return version_visible(v, view->snapshot) && !base_holds(view->base, v);
}

/*
 * The net change |view| sees to key |k|: the sum of the counts of its
 * versions the view counts, and the row they name. False if it counts none.
 */
static bool
key_net(const DoltDeltaView *view, DeltaKey *k, int64 *net, const uint8 **row, uint32 *rowlen)
{
	bool		any = false;

	*net = 0;
	*row = NULL;
	*rowlen = 0;
	for (dsa_pointer dp = k->first; DsaPointerIsValid(dp);)
	{
		DeltaVersion *v = version_at(dp);

		dp = v->next;
		if (!view_counts(view, v))
			continue;
		any = true;
		*net += v->delta;
		if (!*row && v->rowlen)
		{
			*row = v->row;
			*rowlen = v->rowlen;
		}
	}
	return any;
}

static void
visit_key(DeltaKey *k, int64 net, const uint8 *row, uint32 rowlen, DoltDeltaVisit visit, void *arg)
{
	DoltDeltaEdit edit;

	edit.key = KEY_BYTES(k);
	edit.keylen = k->keylen;
	edit.row = row;
	edit.rowlen = rowlen;
	edit.delta = net;
	visit(arg, &edit);
}

/*
 * Visit the net change |view| sees to |key|, if any is nonzero, and return
 * whether there was one. The visitor runs under the tree's shared lock with
 * pointers into shared memory: it copies what it keeps.
 */
bool
dolt_delta_get(const DoltDeltaView *view, const uint8 *key, uint32 keylen, DoltDeltaVisit visit, void *arg)
{
	DeltaTree  *tree;
	dsa_pointer found;
	bool		changed = false;

	delta_attach();
	tree = tree_find(&view->tree, false);
	if (!tree)
		return false;
	LWLockAcquire(&tree->lock, LW_SHARED);
	found = key_search(tree, key, keylen, NULL);
	if (DsaPointerIsValid(found))
	{
		DeltaKey   *k = key_at(found);
		int64		net;
		const uint8 *row;
		uint32		rowlen;

		if (key_compare(KEY_BYTES(k), k->keylen, key, keylen) == 0 &&
			key_net(view, k, &net, &row, &rowlen) && net != 0)
		{
			visit_key(k, net, row, rowlen, visit, arg);
			changed = true;
		}
	}
	LWLockRelease(&tree->lock);
	return changed;
}

/*
 * Visit, in key order, up to |max| keys whose net change |view| sees is
 * nonzero: forward from |from| (inclusive or not), or from the start if
 * |from| is NULL, up to |to| (exclusive); or backward from before |from|, or
 * from the end if |from| is NULL, down to |to| (inclusive). Returns how many
 * it visited; fewer than |max| means it reached the bound.
 */
int
dolt_delta_range(const DoltDeltaView *view, const uint8 *from, uint32 fromlen, bool inclusive,
				 const uint8 *to, uint32 tolen, bool backward, int max, DoltDeltaVisit visit, void *arg)
{
	DeltaTree  *tree;
	dsa_pointer at;
	int			visited = 0;

	delta_attach();
	tree = tree_find(&view->tree, false);
	if (!tree)
		return 0;
	LWLockAcquire(&tree->lock, LW_SHARED);
	if (!backward)
	{
		at = from ? key_search(tree, from, fromlen, NULL) : key_at(tree->head)->next[0];
		if (from && !inclusive && DsaPointerIsValid(at) &&
			key_compare(KEY_BYTES(key_at(at)), key_at(at)->keylen, from, fromlen) == 0)
			at = key_at(at)->next[0];
	}
	else
		at = key_before(tree, from, fromlen);
	while (DsaPointerIsValid(at) && visited < max)
	{
		DeltaKey   *k = key_at(at);
		int64		net;
		const uint8 *row;
		uint32		rowlen;

		if (to)
		{
			int			order = key_compare(KEY_BYTES(k), k->keylen, to, tolen);

			if (backward ? order < 0 : order >= 0)
				break;
		}
		if (key_net(view, k, &net, &row, &rowlen) && net != 0)
		{
			visit_key(k, net, row, rowlen, visit, arg);
			visited++;
		}
		at = backward ? key_before(tree, KEY_BYTES(k), k->keylen) : k->next[0];
	}
	LWLockRelease(&tree->lock);
	return visited;
}

/* The next sequence |key|'s delta will give a version, which identifies what it holds now. */
uint64
dolt_delta_seq(const DoltDeltaTree *key)
{
	DeltaTree  *tree;
	uint64		seq;

	delta_attach();
	tree = tree_find(key, false);
	if (!tree)
		return 0;
	LWLockAcquire(&tree->lock, LW_SHARED);
	seq = tree->next_seq;
	LWLockRelease(&tree->lock);
	return seq;
}

/* The sum of every count in |key|'s delta, whoever wrote it: an estimate of rows it adds. */
int64
dolt_delta_net(const DoltDeltaTree *key)
{
	DeltaTree  *tree;
	int64		net;

	delta_attach();
	tree = tree_find(key, false);
	if (!tree)
		return 0;
	LWLockAcquire(&tree->lock, LW_SHARED);
	net = tree->net;
	LWLockRelease(&tree->lock);
	return net;
}

/* How many versions |key|'s delta holds, whoever wrote them. */
uint64
dolt_delta_count(const DoltDeltaTree *key)
{
	DeltaTree  *tree;
	uint64		count;

	delta_attach();
	tree = tree_find(key, false);
	if (!tree)
		return 0;
	LWLockAcquire(&tree->lock, LW_SHARED);
	count = tree->count;
	LWLockRelease(&tree->lock);
	return count;
}

/* ---------------------------------------------------------------------------
 * Folding and pruning
 * ---------------------------------------------------------------------------
 *
 * A fold (dg_store.c) writes a new base holding every version its snapshot
 * saw and records here which those are. Once the fold has committed and
 * every snapshot sees it, no reader can select an older base, so the
 * versions it holds go; so do versions whose writers rolled back.
 *
 * A reader selects its base under a snapshot it holds, and then reads the
 * delta through a copy of that snapshot for as long as its state lives. The
 * pins keep the oldest xmin each process loaded under, until its transaction
 * ends, so that pruning waits for those readers as well.
 */

/* Note that this process loaded a state under a snapshot with |xmin|. */
void
dolt_delta_pin(TransactionId xmin)
{
	pg_atomic_uint32 *pin;
	TransactionId held;

	if (MyProcNumber < 0 || MyProcNumber >= control->npins || !TransactionIdIsNormal(xmin))
		return;
	pin = &control->pins[MyProcNumber];
	held = pg_atomic_read_u32(pin);
	if (!TransactionIdIsValid(held) || TransactionIdPrecedes(xmin, held))
		pg_atomic_write_u32(pin, xmin);
}

/* This process's transaction ended: its states are gone. */
void
dolt_delta_unpin(void)
{
	if (control && MyProcNumber >= 0 && MyProcNumber < control->npins)
		pg_atomic_write_u32(&control->pins[MyProcNumber], InvalidTransactionId);
}

/* The oldest xid every snapshot, and every pinned state, still sees as running or later. */
static TransactionId
prune_horizon(void)
{
	TransactionId horizon;

	/*
	 * Crash recovery, and a standby before it takes snapshots, has no readers:
	 * every committed fold's versions are free, by a horizon past every xid.
	 */
	if (RecoveryInProgress() && standbyState != STANDBY_SNAPSHOT_READY)
		return XidFromFullTransactionId(ReadNextFullTransactionId());
	horizon = GetOldestNonRemovableTransactionId(NULL);

	for (int i = 0; i < control->npins; i++)
	{
		TransactionId pin = pg_atomic_read_u32(&control->pins[i]);

		if (TransactionIdIsValid(pin) && TransactionIdPrecedes(pin, horizon))
			horizon = pin;
	}
	return horizon;
}

/*
 * Record that fold |fold|, not yet committed, wrote |key|'s base holding the
 * versions |horizon| sees. A newer fold's record replaces an older one's:
 * its horizon holds everything the older one's did. The record reaches the
 * WAL too, for standbys to prune by.
 */
void
dolt_delta_set_fold(const DoltDeltaTree *key, TransactionId fold, const DoltDeltaHorizon *horizon,
					const uint8 *root, const uint8 *base, const DoltDeltaHorizon *base_horizon)
{
	/*
	 * Logged at every wal_level: crash recovery prunes by it, or replay of the
	 * versions since the checkpoint could fill the delta's memory.
	 */
	{
		xl_dolt_delta_fold record = {0};

		record.db = key->db;
		record.space = key->space;
		record.file = key->file;
		record.fold = fold;
		record.xmin = horizon->xmin;
		record.xmax = horizon->xmax;
		record.xcnt = horizon->xcnt;
		if (base_horizon && TransactionIdIsValid(base_horizon->xmax))
		{
			record.base_xmin = base_horizon->xmin;
			record.base_xmax = base_horizon->xmax;
			record.base_xcnt = base_horizon->xcnt;
		}
		memcpy(record.root, root, sizeof(record.root));
		memcpy(record.base, base, sizeof(record.base));
		record.branchlen = strlen(key->branch);
		XLogBeginInsert();
		XLogRegisterData(&record, SizeOfDoltDeltaFold);
		XLogRegisterData(key->branch, record.branchlen);
		if (horizon->xcnt)
			XLogRegisterData(horizon->xip, horizon->xcnt * sizeof(TransactionId));
		if (record.base_xcnt)
			XLogRegisterData(base_horizon->xip, record.base_xcnt * sizeof(TransactionId));
		(void) XLogInsert(RM_DOLT_DELTA_ID, XLOG_DOLT_DELTA_FOLD);
	}
	fold_note(key, fold, horizon);
}

/* Note fold |fold|'s horizon in |key|'s tree, for pruning. */
static void
fold_note(const DoltDeltaTree *key, TransactionId fold, const DoltDeltaHorizon *horizon)
{
	DeltaTree  *tree;
	dsa_pointer xip = InvalidDsaPointer;

	delta_attach();
	tree = tree_find(key, true);
	if (horizon->xcnt)
	{
		xip = dsa_allocate_extended(area, horizon->xcnt * sizeof(TransactionId), DSA_ALLOC_NO_OOM);
		if (!DsaPointerIsValid(xip))
			return;				/* the versions stay until a later fold */
		memcpy(dsa_get_address(area, xip), horizon->xip, horizon->xcnt * sizeof(TransactionId));
	}
	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	if (DsaPointerIsValid(tree->folded_xip))
		dsa_free(area, tree->folded_xip);
	tree->fold = fold;
	tree->folded = *horizon;
	tree->folded.xip = NULL;
	tree->folded_xip = xip;
	LWLockRelease(&tree->lock);
}

/*
 * Whether |v| can go: rolled back, or held by a base every snapshot reads
 * past. A writer that precedes |horizon| has ended: the horizon is no newer
 * than any running xid. Asking the proc array whether it is still running
 * would be wrong in the launcher, which is not in it.
 */
static bool
version_prunable(DeltaVersion *v, const DoltDeltaHorizon *held, TransactionId horizon)
{
	uint32		flags = pg_atomic_read_u32(&v->flags);

	if (flags & VERSION_ABORTED)
		return true;
	if (!(flags & VERSION_COMMITTED))
	{
		if (!TransactionIdPrecedes(v->topxid, horizon))
			return false;

		/*
		 * In recovery, a writer whose commit is still to be replayed, or a
		 * prepared one, has not ended: only a replayed abort ends it.
		 */
		if (RecoveryInProgress() && !TransactionIdDidCommit(v->xid))
			return TransactionIdDidAbort(v->xid);
		if (!version_ended_committed(v))
			return true;
	}
	return held && base_holds(held, v);
}

/* ---------------------------------------------------------------------------
 * Settling
 * ---------------------------------------------------------------------------
 *
 * Rows are keyed by their contents, so every update of a hot row leaves its
 * old key behind with an insert and a delete that cancel, and index entries
 * likewise. Until a fold, readers walk past them. Once every snapshot sees a
 * key's versions and no base holds any of them, every reader counts all of
 * them or none, so they can become one version with their net count, or none
 * at all if it is zero: the key is settled. Settling is logged, naming the
 * versions by their records' positions and xids, so that replay, the
 * checkpoint's state file and standbys agree on what replaced what.
 */

/* Keys a settling pass looks at under one lock acquisition. */
#define SETTLE_CHUNK 256

/* Whether |v| may settle: committed, seen by every snapshot, held by no base. */
static bool
version_settleable(DeltaVersion *v, const DoltDeltaHorizon *held, TransactionId horizon)
{
	uint32		flags = pg_atomic_read_u32(&v->flags);

	if ((flags & VERSION_ABORTED) || !XLogRecPtrIsValid(v->lsn) || !TransactionIdPrecedes(v->topxid, horizon))
		return false;
	if (!(flags & VERSION_COMMITTED) && !version_ended_committed(v))
		return false;
	return !(held && base_holds(held, v));
}

/*
 * Replace the versions of |key| in |tree| that |ids| name by the first of
 * them with count |delta|, written at |lsn|, or by none if |delta| is zero.
 * A kept version missing from the tree is made from |row|. Under the tree's
 * lock, exclusive.
 */
static void
settle_apply(DeltaTree *tree, const uint8 *key, uint32 keylen, const xl_dolt_delta_version_id *ids, int nids,
			 int64 delta, const uint8 *row, uint32 rowlen, XLogRecPtr lsn)
{
	dsa_pointer preds[DELTA_MAX_HEIGHT];
	dsa_pointer found = key_search(tree, key, keylen, preds);
	DeltaKey   *k;
	dsa_pointer keep = InvalidDsaPointer,
				last = InvalidDsaPointer;
	bool		kept = false;

	if (!DsaPointerIsValid(found) || key_compare(KEY_BYTES(key_at(found)), key_at(found)->keylen, key, keylen) != 0)
		return;
	k = key_at(found);
	for (dsa_pointer dp = k->first; DsaPointerIsValid(dp);)
	{
		DeltaVersion *v = version_at(dp);
		dsa_pointer vnext = v->next;
		int			named = -1;

		for (int i = 0; i < nids && named < 0; i++)
			if (v->lsn == ids[i].lsn && TransactionIdEquals(v->xid, ids[i].xid))
				named = i;
		if (named == 0 && delta != 0)
		{
			tree->net += delta - v->delta;
			v->delta = delta;
			v->lsn = lsn;
			pg_atomic_fetch_or_u32(&v->flags, VERSION_COMMITTED);
			kept = true;
		}
		else if (named >= 0)
		{
			tree->count--;
			tree->net -= v->delta;
			tree->bytes -= offsetof(DeltaVersion, row) + v->rowlen;
			dsa_free(area, dp);
			dp = vnext;
			continue;
		}
		v->next = InvalidDsaPointer;
		if (DsaPointerIsValid(last))
			version_at(last)->next = dp;
		else
			keep = dp;
		last = dp;
		dp = vnext;
	}
	if (delta != 0 && !kept)
	{
		DoltDeltaEdit edit = {key, keylen, row, rowlen, delta};
		dsa_pointer made = version_make(&edit, ids[0].topxid, ids[0].xid, FirstCommandId);
		DeltaVersion *v;

		if (!DsaPointerIsValid(made))
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("prolly delta memory is full")));
		v = version_at(made);
		v->seq = tree->next_seq++;
		v->lsn = lsn;
		pg_atomic_fetch_or_u32(&v->flags, VERSION_COMMITTED);
		tree->count++;
		tree->net += delta;
		tree->bytes += offsetof(DeltaVersion, row) + rowlen;
		if (DsaPointerIsValid(last))
			version_at(last)->next = made;
		else
			keep = made;
		last = made;
	}
	k->first = keep;
	k->last = last;
	if (!DsaPointerIsValid(keep))
	{
		for (uint32 level = 0; level < k->height; level++)
			key_at(preds[level])->next[level] = k->next[level];
		tree->keys--;
		tree->bytes -= offsetof(DeltaKey, next) + k->height * sizeof(dsa_pointer) + k->keylen;
		dsa_free(area, found);
	}
}

/* A key a settling pass chose, as it found it. */
typedef struct SettleCandidate
{
	uint8	   *key;
	uint32		keylen;
	int			nids;
	xl_dolt_delta_version_id *ids;	/* oldest first; the kept one is moved first */
	int64		delta;
	uint8	   *row;
	uint32		rowlen;
} SettleCandidate;

/* Whether |k|'s versions are still exactly those |c| names, in order. */
static bool
settle_unchanged(DeltaKey *k, const SettleCandidate *c, const xl_dolt_delta_version_id *found)
{
	int			i = 0;

	for (dsa_pointer dp = k->first; DsaPointerIsValid(dp); dp = version_at(dp)->next, i++)
		if (i >= c->nids || version_at(dp)->lsn != found[i].lsn || !TransactionIdEquals(version_at(dp)->xid, found[i].xid))
			return false;
	return i == c->nids;
}

/* A queued key's bytes, copied out of the queue. */
typedef struct SettleKey
{
	uint8	   *data;
	uint32		len;
} SettleKey;

/*
 * Choose, of |keys| (|n| of them), those whose versions may settle, under the
 * shared lock, into |cands| with their versions in |orders|. Keys that could
 * settle once snapshots move on go back into |waiting|.
 */
static int
settle_choose(DeltaTree *tree, TransactionId horizon, SettleKey *keys, int n,
			  SettleCandidate *cands, xl_dolt_delta_version_id **orders, List **waiting)
{
	DoltDeltaHorizon held;
	DoltDeltaHorizon *outstanding = NULL;
	int			ncands = 0;

	LWLockAcquire(&tree->lock, LW_SHARED);
	if (TransactionIdIsValid(tree->fold))
	{
		held = tree->folded;
		held.xip = DsaPointerIsValid(tree->folded_xip) ?
			(TransactionId *) dsa_get_address(area, tree->folded_xip) : NULL;
		outstanding = &held;
	}
	for (int i = 0; i < n; i++)
	{
		dsa_pointer kp = key_search(tree, keys[i].data, keys[i].len, NULL);
		DeltaKey   *k;
		int			count = 0,
					keep = -1;
		int64		net = 0;
		bool		ok = true;
		SettleCandidate *c;

		if (!DsaPointerIsValid(kp) || key_compare(KEY_BYTES(key_at(kp)), key_at(kp)->keylen, keys[i].data, keys[i].len) != 0)
			continue;
		k = key_at(kp);
		for (dsa_pointer dp = k->first; DsaPointerIsValid(dp); dp = version_at(dp)->next)
		{
			DeltaVersion *v = version_at(dp);

			ok = ok && version_settleable(v, outstanding, horizon);
			net += v->delta;
			count++;
		}
		if (count == 0 || (count == 1 && net != 0))
			continue;
		if (!ok)
		{
			SettleKey	   *again = palloc(sizeof(SettleKey));

			*again = keys[i];
			*waiting = lappend(*waiting, again);
			continue;
		}
		c = &cands[ncands];
		c->keylen = keys[i].len;
		c->key = keys[i].data;
		c->nids = count;
		c->ids = palloc(count * sizeof(xl_dolt_delta_version_id));
		orders[ncands] = palloc(count * sizeof(xl_dolt_delta_version_id));
		c->delta = net;
		c->row = NULL;
		c->rowlen = 0;
		count = 0;
		for (dsa_pointer dp = k->first; DsaPointerIsValid(dp); dp = version_at(dp)->next, count++)
		{
			DeltaVersion *v = version_at(dp);

			orders[ncands][count].lsn = v->lsn;
			orders[ncands][count].topxid = v->topxid;
			orders[ncands][count].xid = v->xid;

			/* Keep the version with the longest row: an insert's, not a delete's. */
			if (net != 0 && (keep < 0 || v->rowlen > c->rowlen))
			{
				keep = count;
				c->rowlen = v->rowlen;
				c->row = (uint8 *) v->row;
			}
		}
		if (c->row)
			c->row = memcpy(palloc(Max(c->rowlen, 1)), c->row, c->rowlen);
		memcpy(c->ids, orders[ncands], c->nids * sizeof(xl_dolt_delta_version_id));
		if (keep > 0)
		{
			xl_dolt_delta_version_id first = c->ids[0];

			c->ids[0] = c->ids[keep];
			c->ids[keep] = first;
		}
		ncands++;
	}
	LWLockRelease(&tree->lock);
	return ncands;
}

/*
 * Check, log and apply |cands| under the exclusive lock: a writer or a prune
 * may have changed a key since it was chosen. Returns how many versions went.
 */
static uint64
settle_commit(DeltaTree *tree, SettleCandidate *cands, xl_dolt_delta_version_id **orders, int ncands)
{
	StringInfoData record;
	xl_dolt_delta_settle header = {0};
	uint64		bytes,
				settled = 0;
	int			nlogged = 0;

	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	bytes = tree->bytes;
	initStringInfo(&record);
	header.db = tree->key.db;
	header.space = tree->key.space;
	header.file = tree->key.file;
	header.branchlen = strlen(tree->key.branch);
	appendBinaryStringInfo(&record, (const char *) &header, SizeOfDoltDeltaSettle);
	appendBinaryStringInfo(&record, tree->key.branch, header.branchlen);
	for (int i = 0; i < ncands; i++)
	{
		SettleCandidate *c = &cands[i];
		dsa_pointer found = key_search(tree, c->key, c->keylen, NULL);
		xl_dolt_delta_settled entry;

		if (!DsaPointerIsValid(found) || key_compare(KEY_BYTES(key_at(found)), key_at(found)->keylen, c->key, c->keylen) != 0 ||
			!settle_unchanged(key_at(found), c, orders[i]))
		{
			c->nids = 0;
			continue;
		}
		entry.delta = c->delta;
		entry.keylen = c->keylen;
		entry.nversions = c->nids;
		entry.rowlen = c->delta != 0 ? c->rowlen : 0;
		appendBinaryStringInfo(&record, (const char *) &entry, SizeOfDoltDeltaSettled);
		appendBinaryStringInfo(&record, (const char *) c->key, c->keylen);
		appendBinaryStringInfo(&record, (const char *) c->ids, c->nids * sizeof(xl_dolt_delta_version_id));
		if (entry.rowlen)
			appendBinaryStringInfo(&record, (const char *) c->row, entry.rowlen);
		for (int j = 0; j < c->nids; j++)
			if (!TransactionIdIsValid(header.conflict) || TransactionIdFollows(c->ids[j].topxid, header.conflict))
				header.conflict = c->ids[j].topxid;
		nlogged++;
	}
	if (nlogged)
	{
		header.count = nlogged;
		memcpy(record.data, &header, SizeOfDoltDeltaSettle);
		XLogBeginInsert();
		XLogRegisterData(record.data, record.len);
		START_CRIT_SECTION();
		(void) XLogInsert(RM_DOLT_DELTA_ID, XLOG_DOLT_DELTA_SETTLE);
		for (int i = 0; i < ncands; i++)
		{
			SettleCandidate *c = &cands[i];

			if (!c->nids)
				continue;
			settle_apply(tree, c->key, c->keylen, c->ids, c->nids, c->delta, c->row, c->rowlen, ProcLastRecPtr);
			settled += c->nids - (c->delta != 0);
		}
		END_CRIT_SECTION();
	}
	if (!tree->count)
		tree->oldest = 0;
	pg_atomic_fetch_sub_u64(&control->bytes, bytes - tree->bytes);
	LWLockRelease(&tree->lock);
	pfree(record.data);
	return settled;
}

static int
compare_plkeys(const void *a, const void *b)
{
	const SettleKey *x = a,
			   *y = b;

	return key_compare(x->data, x->len, y->data, y->len);
}

/*
 * Settle the keys |tree| queued as they gained versions, whose versions every
 * snapshot by |horizon| sees and no base holds, SETTLE_CHUNK keys at a time,
 * so that neither lock is held long. Keys that must wait for snapshots go
 * back on the queue. Returns how many versions went.
 */
static uint64
tree_settle(DeltaTree *tree, TransactionId horizon)
{
	MemoryContext memory = AllocSetContextCreate(CurrentMemoryContext, "prolly settle", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(memory);
	SettleCandidate *cands = palloc(SETTLE_CHUNK * sizeof(SettleCandidate));
	xl_dolt_delta_version_id **orders = palloc(SETTLE_CHUNK * sizeof(xl_dolt_delta_version_id *));
	List	   *waiting = NIL;
	SettleKey	   *keys;
	int			nkeys = 0,
				unique = 0;
	dsa_pointer queue;
	uint64		settled = 0,
				freed = 0;

	/* Take the queue, and copy its keys, sorted and once each. */
	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	queue = tree->settle_queue;
	nkeys = tree->settle_queued;
	tree->settle_queue = InvalidDsaPointer;
	tree->settle_queued = 0;
	LWLockRelease(&tree->lock);
	keys = palloc(Max(nkeys, 1) * sizeof(SettleKey));
	nkeys = 0;
	while (DsaPointerIsValid(queue))
	{
		DeltaKey   *q = key_at(queue);
		dsa_pointer next = q->next[0];
		uint8	   *copy = palloc(Max(q->keylen, 1));

		memcpy(copy, KEY_BYTES(q), q->keylen);
		keys[nkeys].data = copy;
		keys[nkeys++].len = q->keylen;
		freed += offsetof(DeltaKey, next) + q->height * sizeof(dsa_pointer) + q->keylen;
		dsa_free(area, queue);
		queue = next;
	}
	if (freed)
	{
		LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
		tree->bytes -= freed;
		LWLockRelease(&tree->lock);
		pg_atomic_fetch_sub_u64(&control->bytes, freed);
	}
	qsort(keys, nkeys, sizeof(SettleKey), compare_plkeys);
	for (int i = 0; i < nkeys; i++)
		if (unique == 0 || compare_plkeys(&keys[unique - 1], &keys[i]) != 0)
			keys[unique++] = keys[i];

	for (int first = 0; first < unique; first += SETTLE_CHUNK)
	{
		int			n = Min(SETTLE_CHUNK, unique - first);
		int			ncands = settle_choose(tree, horizon, keys + first, n, cands, orders, &waiting);

		if (ncands)
			settled += settle_commit(tree, cands, orders, ncands);
		CHECK_FOR_INTERRUPTS();
	}

	/* Keys that wait for snapshots go back on the queue, on fresh nodes. */
	foreach_ptr(SettleKey, key, waiting)
	{
		dsa_pointer node = key_make(key->data, key->len, 1);

		if (!DsaPointerIsValid(node))
			break;
		LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
		key_at(node)->next[0] = tree->settle_queue;
		tree->settle_queue = node;
		tree->settle_queued++;
		tree->bytes += offsetof(DeltaKey, next) + sizeof(dsa_pointer) + key->len;
		LWLockRelease(&tree->lock);
		pg_atomic_fetch_add_u64(&control->bytes, offsetof(DeltaKey, next) + sizeof(dsa_pointer) + key->len);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(memory);
	return settled;
}

/* Replay a SETTLE record. */
static void
settle_redo(XLogReaderState *record)
{
	char	   *p = XLogRecGetData(record);
	char	   *end = p + XLogRecGetDataLen(record);
	xl_dolt_delta_settle header;
	char		branch[DOLT_DELTA_BRANCH_SIZE];
	DoltDeltaTree key;
	DeltaTree  *tree;
	uint64		bytes;

	delta_attach();
	memcpy(&header, p, SizeOfDoltDeltaSettle);
	p += SizeOfDoltDeltaSettle;
	if (header.branchlen >= sizeof(branch))
		elog(PANIC, "dolt_delta_redo: branch name too long");
	memcpy(branch, p, header.branchlen);
	branch[header.branchlen] = '\0';
	p += header.branchlen;
	if (InHotStandby)
	{
		RelFileLocator locator = {header.space, header.db, header.file};

		ResolveRecoveryConflictWithSnapshot(header.conflict, false, locator);
	}
	dolt_delta_tree_key(&key, header.db, header.space, header.file, branch);
	tree = tree_find(&key, false);
	if (!tree)
		return;
	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	bytes = tree->bytes;
	for (uint32 i = 0; i < header.count; i++)
	{
		xl_dolt_delta_settled entry;
		const uint8 *keybytes;
		xl_dolt_delta_version_id *ids;

		if (p + SizeOfDoltDeltaSettled > end)
			elog(PANIC, "dolt_delta_redo: malformed record");
		memcpy(&entry, p, SizeOfDoltDeltaSettled);
		p += SizeOfDoltDeltaSettled;
		keybytes = (const uint8 *) p;
		p += entry.keylen;
		ids = palloc(Max(entry.nversions, 1) * sizeof(xl_dolt_delta_version_id));
		memcpy(ids, p, entry.nversions * sizeof(xl_dolt_delta_version_id));
		p += entry.nversions * sizeof(xl_dolt_delta_version_id);
		settle_apply(tree, keybytes, entry.keylen, ids, entry.nversions, entry.delta,
					 (const uint8 *) p, entry.rowlen, record->ReadRecPtr);
		p += entry.rowlen;
		pfree(ids);
	}
	if (!tree->count)
		tree->oldest = 0;
	pg_atomic_fetch_sub_u64(&control->bytes, bytes - tree->bytes);
	LWLockRelease(&tree->lock);
	if (p != end)
		elog(PANIC, "dolt_delta_redo: malformed record");
}

/*
 * Remove the versions of |tree| that no reader can count: those of rolled
 * back writers, and those the newest fold's base holds once every snapshot
 * reads that base. Returns how many went.
 */
static uint64
tree_prune(DeltaTree *tree, TransactionId horizon)
{
	DoltDeltaHorizon held;
	DoltDeltaHorizon *usable = NULL;
	uint64		pruned = 0;
	uint64		bytes;
	dsa_pointer preds[DELTA_MAX_HEIGHT];

	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
	bytes = tree->bytes;
	if (TransactionIdIsValid(tree->fold) && TransactionIdPrecedes(tree->fold, horizon) &&
		(TransactionIdDidCommit(tree->fold) || TransactionIdDidAbort(tree->fold) ||
		 !(RecoveryInProgress() || TransactionIdIsInProgress(tree->fold))))
	{
		/* In recovery, a fold whose commit is still to be replayed waits for it. */
		if (!TransactionIdDidCommit(tree->fold))
		{
			/* The fold rolled back: its base never was. */
			tree->fold = InvalidTransactionId;
			if (DsaPointerIsValid(tree->folded_xip))
				dsa_free(area, tree->folded_xip);
			tree->folded_xip = InvalidDsaPointer;
		}
		else
		{
			held = tree->folded;
			held.xip = DsaPointerIsValid(tree->folded_xip) ?
				(TransactionId *) dsa_get_address(area, tree->folded_xip) : NULL;
			usable = &held;
		}
	}
	for (int level = 0; level < DELTA_MAX_HEIGHT; level++)
		preds[level] = tree->head;
	for (dsa_pointer kp = key_at(tree->head)->next[0]; DsaPointerIsValid(kp);)
	{
		DeltaKey   *k = key_at(kp);
		dsa_pointer next = k->next[0];
		dsa_pointer keep = InvalidDsaPointer,
					last = InvalidDsaPointer;

		for (dsa_pointer dp = k->first; DsaPointerIsValid(dp);)
		{
			DeltaVersion *v = version_at(dp);
			dsa_pointer vnext = v->next;

			if (version_prunable(v, usable, horizon))
			{
				tree->count--;
				tree->net -= v->delta;
				tree->bytes -= offsetof(DeltaVersion, row) + v->rowlen;
				dsa_free(area, dp);
				pruned++;
			}
			else
			{
				v->next = InvalidDsaPointer;
				if (DsaPointerIsValid(last))
					version_at(last)->next = dp;
				else
					keep = dp;
				last = dp;
			}
			dp = vnext;
		}
		k->first = keep;
		k->last = last;
		if (!DsaPointerIsValid(keep))
		{
			/* No versions left: unlink the key from every level it is on. */
			for (uint32 level = 0; level < k->height; level++)
				key_at(preds[level])->next[level] = k->next[level];
			tree->keys--;
			tree->bytes -= offsetof(DeltaKey, next) + k->height * sizeof(dsa_pointer) + k->keylen;
			dsa_free(area, kp);
		}
		else
			for (uint32 level = 0; level < k->height; level++)
				preds[level] = kp;
		kp = next;
	}
	if (usable)
	{
		/* Every version the fold holds is gone. */
		tree->fold = InvalidTransactionId;
		if (DsaPointerIsValid(tree->folded_xip))
			dsa_free(area, tree->folded_xip);
		tree->folded_xip = InvalidDsaPointer;
	}
	if (!tree->count)
		tree->oldest = 0;
	pg_atomic_fetch_sub_u64(&control->bytes, bytes - tree->bytes);
	LWLockRelease(&tree->lock);
	return pruned;
}

/* Prune |key|'s delta; returns how many versions went. */
uint64
dolt_delta_prune(const DoltDeltaTree *key)
{
	DeltaTree  *tree;

	delta_attach();
	tree = tree_find(key, false);
	return tree ? tree_prune(tree, prune_horizon()) : 0;
}

/*
 * Prune every tree with a fold whose versions may go, or any versions at all
 * when |all|; returns how many versions went.
 */
uint64
dolt_delta_prune_all(bool all)
{
	DeltaTree **every;
	int			ntrees;
	uint64		pruned = 0;
	TransactionId horizon;

	delta_attach();
	horizon = prune_horizon();
	every = trees_all(&ntrees);
	for (int i = 0; i < ntrees; i++)
	{
		bool		hinted = DsaPointerIsValid(every[i]->settle_queue);

		if (every[i]->count && (all || TransactionIdIsValid(every[i]->fold)))
			pruned += tree_prune(every[i], horizon);
		if (every[i]->count && (all || hinted) && !RecoveryInProgress())
			pruned += tree_settle(every[i], horizon);
	}
	pfree(every);
	return pruned;
}

/*
 * The trees due a fold, largest first, up to |max|, into |due|: those of
 * database |db| (any, if invalid) holding at least |versions| versions, or
 * whose oldest version has waited |timeout_ms|, or any tree with versions
 * while the store holds more than half its budget. A tree a fold has written
 * but not yet pruned is not due, unless |versions| is zero, which asks for
 * every tree with versions.
 */
int
dolt_delta_due(Oid db, int versions, int timeout_ms, DoltDeltaTree *due, int max)
{
	DeltaTree **every;
	int			ntrees;
	int			found = 0;
	bool		pressed;
	TimestampTz now = GetCurrentTimestamp();
	uint64	   *sizes = palloc(Max(max, 1) * sizeof(uint64));

	delta_attach();
	pressed = dolt_delta_pressure() > 0.5;
	every = trees_all(&ntrees);
	for (int i = 0; i < ntrees; i++)
	{
		DeltaTree  *tree = every[i];
		bool		wanted;
		uint64		size;
		int			at;

		if (OidIsValid(db) && tree->key.db != db)
			continue;
		LWLockAcquire(&tree->lock, LW_SHARED);
		size = tree->count;
		wanted = tree->count &&
			(versions <= 0 ||
			 (!TransactionIdIsValid(tree->fold) &&
			  (pressed || tree->count >= (uint64) versions ||
			   TimestampDifferenceExceeds(tree->oldest, now, timeout_ms))));
		LWLockRelease(&tree->lock);
		if (!wanted)
			continue;

		/* Keep the |max| largest, in order. */
		for (at = found; at > 0 && sizes[at - 1] < size; at--)
			if (at < max)
			{
				sizes[at] = sizes[at - 1];
				due[at] = due[at - 1];
			}
		if (at < max)
		{
			sizes[at] = size;
			due[at] = tree->key;
			found = Min(found + 1, max);
		}
	}
	pfree(every);
	pfree(sizes);
	return found;
}

/* How full the store is: its bytes over its budget, past 1 once it has grown. */
double
dolt_delta_pressure(void)
{
	delta_attach();
	return (double) pg_atomic_read_u64(&control->bytes) / (double) control->budget;
}

/* The store's totals, for monitoring. */
void
dolt_delta_totals(DoltDeltaTotals *totals)
{
	DeltaTree **every;
	int			n;

	delta_attach();
	every = trees_all(&n);
	memset(totals, 0, sizeof(*totals));
	for (int i = 0; i < n; i++)
	{
		LWLockAcquire(&every[i]->lock, LW_SHARED);
		if (every[i]->count)
			totals->trees++;
		totals->keys += every[i]->keys;
		totals->versions += every[i]->count;
		LWLockRelease(&every[i]->lock);
	}
	totals->bytes = pg_atomic_read_u64(&control->bytes);
	totals->budget = control->budget;
	totals->allocated = dsa_get_total_size(area);
	totals->throttled_us = pg_atomic_read_u64(&control->throttled_us);
	totals->limit_waits = pg_atomic_read_u64(&control->limit_waits);
	pfree(every);
}

/* ---------------------------------------------------------------------------
 * Checkpoints and recovery
 * ---------------------------------------------------------------------------
 */

/* Every tree, read under all the partition locks. */
static DeltaTree **
trees_all(int *count)
{
	HASH_SEQ_STATUS status;
	DeltaTree  *tree;
	DeltaTree **all;
	int			n = 0;

	for (int i = 0; i < DELTA_PARTITIONS; i++)
		LWLockAcquire(&control->locks[i].lock, LW_SHARED);
	all = palloc(Max(hash_get_num_entries(trees), 1) * sizeof(DeltaTree *));
	hash_seq_init(&status, trees);
	while ((tree = hash_seq_search(&status)) != NULL)
		all[n++] = tree;
	for (int i = DELTA_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(&control->locks[i].lock);
	*count = n;
	return all;
}

static void
write_all(int fd, const void *data, Size size, const char *path)
{
	pgstat_report_wait_start(WAIT_EVENT_DOLT_DELTA_STATE_WRITE);
	errno = 0;
	if (write(fd, data, size) != size)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not write file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();
}

/* Write what |buffer| holds to |fd|, adding it to |crc|, and empty it. */
static void
state_flush(int fd, StringInfo buffer, pg_crc32c *crc)
{
	COMP_CRC32C(*crc, buffer->data, buffer->len);
	write_all(fd, buffer->data, buffer->len, DELTA_STATE_TEMP);
	resetStringInfo(buffer);
}

/*
 * Save every logged version whose record precedes |redo|, so that recovery
 * from this checkpoint, which replays only records after it, has them all.
 * Versions of transactions that rolled back are saved too: their xids say so.
 * The file is written as the versions are read, a chunk at a time, so a large
 * delta costs the checkpointer no more memory than a small one.
 */
void
CheckPointDoltDelta(XLogRecPtr redo)
{
	DeltaTree **all;
	int			ntrees;
	StringInfoData buffer,
				folds;
	DeltaStateHeader header = {0};
	pg_crc32c	crc;
	int			fd;

	delta_attach();

	/* A standby has no launcher: its restartpoints prune what folds hold. */
	if (RecoveryInProgress())
		(void) dolt_delta_prune_all(true);
	if (MakePGDirectory(DELTA_DIR) < 0 && errno != EEXIST)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create directory \"%s\": %m", DELTA_DIR)));
	fd = OpenTransientFile(DELTA_STATE_TEMP, O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create file \"%s\": %m", DELTA_STATE_TEMP)));
	INIT_CRC32C(crc);
	all = trees_all(&ntrees);
	initStringInfo(&buffer);
	initStringInfo(&folds);
	header.magic = DELTA_STATE_MAGIC;
	header.redo = redo;
	for (int i = 0; i < ntrees; i++)
	{
		DeltaTree  *tree = all[i];

		LWLockAcquire(&tree->lock, LW_SHARED);
		for (dsa_pointer kp = key_at(tree->head)->next[0]; DsaPointerIsValid(kp); kp = key_at(kp)->next[0])
		{
			DeltaKey   *k = key_at(kp);

			for (dsa_pointer dp = k->first; DsaPointerIsValid(dp);)
			{
				DeltaVersion *v = version_at(dp);
				DeltaSaved	saved = {0};

				dp = v->next;
				if (XLogRecPtrIsInvalid(v->lsn) || v->lsn >= redo)
					continue;
				saved.tree = tree->key;
				saved.topxid = v->topxid;
				saved.xid = v->xid;
				saved.cid = v->cid;
				saved.lsn = v->lsn;
				saved.delta = v->delta;
				saved.keylen = k->keylen;
				saved.rowlen = v->rowlen;
				appendBinaryStringInfo(&buffer, &saved, sizeof(saved));
				appendBinaryStringInfo(&buffer, KEY_BYTES(k), k->keylen);
				appendBinaryStringInfo(&buffer, v->row, v->rowlen);
				header.count++;
				if (buffer.len >= DELTA_STATE_CHUNK)
					state_flush(fd, &buffer, &crc);
			}
		}

		/* Taken under the same lock as the versions, so the two agree. */
		if (TransactionIdIsValid(tree->fold))
		{
			DeltaSavedFold saved = {0};

			saved.tree = tree->key;
			saved.fold = tree->fold;
			saved.xmin = tree->folded.xmin;
			saved.xmax = tree->folded.xmax;
			saved.xcnt = DsaPointerIsValid(tree->folded_xip) ? tree->folded.xcnt : 0;
			appendBinaryStringInfo(&folds, &saved, sizeof(saved));
			if (saved.xcnt)
				appendBinaryStringInfo(&folds, dsa_get_address(area, tree->folded_xip), saved.xcnt * sizeof(TransactionId));
			header.folds++;
		}
		LWLockRelease(&tree->lock);
	}
	pfree(all);

	appendBinaryStringInfo(&buffer, folds.data, folds.len);
	pfree(folds.data);
	appendBinaryStringInfo(&buffer, &header, sizeof(header));
	state_flush(fd, &buffer, &crc);
	FIN_CRC32C(crc);
	write_all(fd, &crc, sizeof(crc), DELTA_STATE_TEMP);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(), errmsg("could not fsync file \"%s\": %m", DELTA_STATE_TEMP)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", DELTA_STATE_TEMP)));
	durable_rename(DELTA_STATE_TEMP, DELTA_STATE_FILE, ERROR);
	pfree(buffer.data);
}

/* Link one version read back from the WAL or the state file. */
static void
restore_version(const DoltDeltaTree *key, const DoltDeltaEdit *edit, TransactionId topxid,
				TransactionId xid, CommandId cid, XLogRecPtr lsn)
{
	DeltaTree  *tree = tree_find(key, true);
	Pending		pending;

	/*
	 * Recovery grows the area as writers do, and is never throttled or held
	 * to the limit: the primary already was. Free what committed folds hold
	 * before growing, though, since recovery has no fold workers.
	 */
	if (!pending_make(tree, &pending, edit, topxid, xid, cid) &&
		(dolt_delta_prune_all(false) == 0 || !pending_make(tree, &pending, edit, topxid, xid, cid)))
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate prolly delta store memory during recovery")));
	versions_link(tree, edit, &pending, 1, lsn);
	pending_release(&pending, 1);
}

/*
 * Load the versions the last checkpoint saved, before replay from |redo|
 * begins. A file saved by a later checkpoint than the one recovery starts
 * from also holds versions whose records recovery will replay; those are
 * skipped here.
 */
void
StartupDoltDelta(XLogRecPtr redo)
{
	int			fd;
	struct stat st;
	char	   *contents;
	DeltaStateHeader header;
	pg_crc32c	crc,
				stored;
	char	   *p,
			   *end;

	delta_attach();
	fd = OpenTransientFile(DELTA_STATE_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(FATAL, (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", DELTA_STATE_FILE)));
	}
	if (fstat(fd, &st) != 0)
		ereport(FATAL, (errcode_for_file_access(), errmsg("could not stat file \"%s\": %m", DELTA_STATE_FILE)));
	if (st.st_size < (off_t) (sizeof(header) + sizeof(crc)))
		ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is truncated", DELTA_STATE_FILE)));
	contents = palloc(st.st_size);
	if (read(fd, contents, st.st_size) != st.st_size)
		ereport(FATAL, (errcode_for_file_access(), errmsg("could not read file \"%s\": %m", DELTA_STATE_FILE)));
	CloseTransientFile(fd);

	memcpy(&header, contents + st.st_size - sizeof(crc) - sizeof(header), sizeof(header));
	memcpy(&stored, contents + st.st_size - sizeof(crc), sizeof(crc));
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, contents, st.st_size - sizeof(crc));
	FIN_CRC32C(crc);
	if (header.magic != DELTA_STATE_MAGIC || !EQ_CRC32C(crc, stored))
		ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is corrupt", DELTA_STATE_FILE)));
	if (header.redo < redo)
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly delta state file \"%s\" is older than the checkpoint recovery starts from",
						DELTA_STATE_FILE)));

	p = contents;
	end = contents + st.st_size - sizeof(crc) - sizeof(header);
	for (uint64 i = 0; i < header.count; i++)
	{
		DeltaSaved	saved;
		DoltDeltaEdit edit;

		if (p + sizeof(saved) > end)
			ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is corrupt", DELTA_STATE_FILE)));
		memcpy(&saved, p, sizeof(saved));
		p += sizeof(saved);
		if (p + saved.keylen + saved.rowlen > end)
			ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is corrupt", DELTA_STATE_FILE)));
		edit.key = (const uint8 *) p;
		edit.keylen = saved.keylen;
		edit.row = (const uint8 *) p + saved.keylen;
		edit.rowlen = saved.rowlen;
		edit.delta = saved.delta;
		p += saved.keylen + saved.rowlen;
		if (saved.lsn >= redo)
			continue;
		restore_version(&saved.tree, &edit, saved.topxid, saved.xid, saved.cid, saved.lsn);
	}
	for (uint64 i = 0; i < header.folds; i++)
	{
		DeltaSavedFold saved;
		DoltDeltaHorizon horizon;

		if (p + sizeof(saved) > end)
			ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is corrupt", DELTA_STATE_FILE)));
		memcpy(&saved, p, sizeof(saved));
		p += sizeof(saved);
		if (p + saved.xcnt * sizeof(TransactionId) > end)
			ereport(FATAL, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("prolly delta state file \"%s\" is corrupt", DELTA_STATE_FILE)));
		horizon.xmin = saved.xmin;
		horizon.xmax = saved.xmax;
		horizon.xcnt = saved.xcnt;
		horizon.xip = palloc(Max(saved.xcnt, 1) * sizeof(TransactionId));
		memcpy(horizon.xip, p, saved.xcnt * sizeof(TransactionId));
		p += saved.xcnt * sizeof(TransactionId);
		fold_note(&saved.tree, saved.fold, &horizon);
		pfree(horizon.xip);
	}
	pfree(contents);
}

void
dolt_delta_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *p = XLogRecGetData(record);
	char	   *end = p + XLogRecGetDataLen(record);
	xl_dolt_delta_append header;
	DoltDeltaTree key;
	char		branch[DOLT_DELTA_BRANCH_SIZE];

	if (info == XLOG_DOLT_DELTA_SETTLE)
	{
		/* A standby prunes with what its primary settles, as with what it folds. */
		settle_redo(record);
		(void) dolt_delta_prune_all(false);
		return;
	}
	if (info == XLOG_DOLT_DELTA_FOLD)
	{
		xl_dolt_delta_fold fold;
		DoltDeltaHorizon horizon,
					base_horizon = {0};

		delta_attach();
		memcpy(&fold, p, SizeOfDoltDeltaFold);
		p += SizeOfDoltDeltaFold;
		if (fold.branchlen >= sizeof(branch))
			elog(PANIC, "dolt_delta_redo: branch name too long");
		memcpy(branch, p, fold.branchlen);
		branch[fold.branchlen] = '\0';
		p += fold.branchlen;
		horizon.xmin = fold.xmin;
		horizon.xmax = fold.xmax;
		horizon.xcnt = fold.xcnt;
		horizon.xip = palloc(Max(fold.xcnt, 1) * sizeof(TransactionId));
		memcpy(horizon.xip, p, fold.xcnt * sizeof(TransactionId));
		p += fold.xcnt * sizeof(TransactionId);
		base_horizon.xmin = fold.base_xmin;
		base_horizon.xmax = fold.base_xmax;
		base_horizon.xcnt = fold.base_xcnt;
		base_horizon.xip = palloc(Max(fold.base_xcnt, 1) * sizeof(TransactionId));
		memcpy(base_horizon.xip, p, fold.base_xcnt * sizeof(TransactionId));
		dolt_delta_tree_key(&key, fold.db, fold.space, fold.file, branch);

		/*
		 * A standby, or crash recovery of a fold whose chunks never became
		 * durable, rebuilds the tree from its base and the versions replayed
		 * so far, before the fold's commit can make it a reader's base, and
		 * before pruning can drop the versions it holds.
		 */
		dg_refold(&key, &horizon, fold.root, fold.base, &base_horizon);
		fold_note(&key, fold.fold, &horizon);
		pfree(horizon.xip);
		pfree(base_horizon.xip);

		/*
		 * Earlier folds may have committed since, and the standby's readers
		 * moved on; crash recovery, and a standby before it takes snapshots,
		 * has no readers (prune_horizon).
		 */
		(void) dolt_delta_prune_all(false);
		return;
	}
	if (info != XLOG_DOLT_DELTA_APPEND)
		elog(PANIC, "dolt_delta_redo: unknown op code %u", info);
	delta_attach();
	memcpy(&header, p, SizeOfDoltDeltaAppend);
	p += SizeOfDoltDeltaAppend;
	if (header.branchlen >= sizeof(branch))
		elog(PANIC, "dolt_delta_redo: branch name too long");
	memcpy(branch, p, header.branchlen);
	branch[header.branchlen] = '\0';
	p += header.branchlen;
	dolt_delta_tree_key(&key, header.db, header.space, header.file, branch);
	for (uint32 i = 0; i < header.count; i++)
	{
		xl_dolt_delta_version version;
		DoltDeltaEdit edit;

		memcpy(&version, p, SizeOfDoltDeltaVersion);
		p += SizeOfDoltDeltaVersion;
		edit.key = (const uint8 *) p;
		edit.keylen = version.keylen;
		edit.row = (const uint8 *) p + version.keylen;
		edit.rowlen = version.rowlen;
		edit.delta = version.delta;
		p += version.keylen + version.rowlen;
		restore_version(&key, &edit, header.topxid, XLogRecGetXid(record), header.cid, record->ReadRecPtr);
	}
	if (p != end)
		elog(PANIC, "dolt_delta_redo: malformed record");
}
