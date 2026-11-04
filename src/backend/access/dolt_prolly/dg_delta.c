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
#include "access/xloginsert.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

int			dolt_delta_memory = 256 * 1024;	/* kB */

#define DELTA_PARTITIONS 16

/* Trees with a delta at once. */
#define DELTA_MAX_TREES 16384

/* The smallest area, so that its own bookkeeping fits. */
#define DELTA_MIN_BYTES ((Size) 1024 * 1024)

/* The WAL payload of one append record, past which an append writes another. */
#define DELTA_RECORD_BYTES ((Size) 1024 * 1024)

/* Skiplist levels; each level holds about a quarter of the keys below it. */
#define DELTA_MAX_HEIGHT 12

/* A version's status, as its writer's xids decide it once known. */
#define VERSION_COMMITTED	0x1
#define VERSION_ABORTED		0x2

#define DELTA_DIR			"pg_dolt"
#define DELTA_STATE_FILE	DELTA_DIR "/delta.state"
#define DELTA_STATE_TEMP	DELTA_DIR "/delta.state.tmp"
#define DELTA_STATE_MAGIC	0x44474c32	/* "DGL2" */

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
	pg_prng_state heights;
} DeltaTree;

typedef struct DeltaControl
{
	Size		area_size;
	LWLockPadded locks[DELTA_PARTITIONS];
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

typedef struct DeltaStateHeader
{
	uint32		magic;
	uint32		pad;
	XLogRecPtr	redo;
	uint64		count;
} DeltaStateHeader;

static DeltaControl *control;
static HTAB *trees;
static dsa_area *area;

static Size
area_bytes(void)
{
	return Max((Size) dolt_delta_memory * 1024, DELTA_MIN_BYTES);
}

static Size
control_size(void)
{
	return MAXALIGN(sizeof(DeltaControl));
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

		control->area_size = area_bytes();
		for (int i = 0; i < DELTA_PARTITIONS; i++)
			LWLockInitialize(&control->locks[i].lock, LWTRANCHE_DOLT_DELTA_TREES);

		/* The postmaster cannot use dynamic segments: cap the area at its place. */
		created = dsa_create_in_place(area_place(), control->area_size, LWTRANCHE_DOLT_DELTA_DSA, NULL);
		dsa_pin(created);
		dsa_set_size_limit(created, control->area_size);
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
	LWLockAcquire(&tree->lock, LW_EXCLUSIVE);
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

	/* Allocate everything before the critical section, which must not fail. */
	pending = palloc(count * sizeof(Pending));
	for (int i = 0; i < count; i++)
	{
		if (!pending_make(tree, &pending[i], &edits[i], topxid, xid, cid))
		{
			for (int j = 0; j < i; j++)
			{
				dsa_free(area, pending[j].version);
				dsa_free(area, pending[j].key);
			}
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("prolly delta memory is full"),
					 errhint("Increase \"dolt_delta_memory\", or let pending changes fold into their trees.")));
		}
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
	errno = 0;
	if (write(fd, data, size) != size)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not write file \"%s\": %m", path)));
	}
}

/*
 * Save every logged version whose record precedes |redo|, so that recovery
 * from this checkpoint, which replays only records after it, has them all.
 * Versions of transactions that rolled back are saved too: their xids say so.
 */
void
CheckPointDoltDelta(XLogRecPtr redo)
{
	DeltaTree **all;
	int			ntrees;
	StringInfoData buffer;
	DeltaStateHeader header = {0};
	pg_crc32c	crc;
	int			fd;

	delta_attach();
	all = trees_all(&ntrees);
	initStringInfo(&buffer);
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
			}
		}
		LWLockRelease(&tree->lock);
	}
	pfree(all);

	if (MakePGDirectory(DELTA_DIR) < 0 && errno != EEXIST)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create directory \"%s\": %m", DELTA_DIR)));
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &header, sizeof(header));
	COMP_CRC32C(crc, buffer.data, buffer.len);
	FIN_CRC32C(crc);
	fd = OpenTransientFile(DELTA_STATE_TEMP, O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create file \"%s\": %m", DELTA_STATE_TEMP)));
	write_all(fd, &header, sizeof(header), DELTA_STATE_TEMP);
	write_all(fd, buffer.data, buffer.len, DELTA_STATE_TEMP);
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

	if (!pending_make(tree, &pending, edit, topxid, xid, cid))
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("prolly delta memory is full during recovery"),
				 errhint("Increase \"dolt_delta_memory\".")));
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

	memcpy(&header, contents, sizeof(header));
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

	p = contents + sizeof(header);
	end = contents + st.st_size - sizeof(crc);
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
