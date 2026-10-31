/*-------------------------------------------------------------------------
 *
 * dg_chunk_heap.c
 *	  The pg_dolt_chunk heap catalog as a chunk store implementation.
 *
 * Reads probe the unique hash index with SnapshotAny: a chunk's bytes are fixed
 * by its hash, so any row carrying that hash has the right content.
 *
 * Existence probes also scan with SnapshotAny, then decide whether a row lives
 * from its inserting and deleting transactions' status, read without setting
 * hint bits. Setting hints on first probe would dirty each page and, with data
 * checksums, log a full-page image per page. A row whose inserter or deleter is
 * still running is waited on.
 *
 * Puts never wait and never probe: every put inserts its own copy, keyed by
 * its transaction ID, so concurrent puts of one chunk never conflict. Callers
 * put only chunks they built, which are almost always new; a chunk already
 * stored gains a duplicate copy, which readers ignore. Waiting for another
 * inserter instead could deadlock two committing transactions that each wait
 * for a chunk the other is inserting. Puts are transactional here; aborted
 * rows become dead tuples for VACUUM. Callers must not depend on that.
 *
 * A chunk longer than one heap tuple holds is stored as consecutive parts of
 * DG_CHUNK_PART bytes. Because a part's bytes follow from the chunk and its
 * number, every copy of a part is identical, so a reader may assemble a chunk
 * from parts of different copies.
 *
 * src/backend/access/dolt_prolly/dg_chunk_heap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_dolt_chunk.h"
#include "executor/tuptable.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* The largest part: a heap tuple less its header, part, size, copy and hash. */
#define DG_CHUNK_PART \
	(MaxHeapTupleSize - (MAXALIGN(SizeofHeapTupleHeader) + 2 * sizeof(int32) + sizeof(int64) + \
						 INTALIGN(1 + DOLT_CHUNK_HASH_SIZE) + VARHDRSZ))

typedef struct HeapChunkState
{
	Relation	heap;
	Relation	index;
	LOCKMODE	lockmode;
	IndexInfo  *index_info;		/* built on first write */
	TupleDesc	part_desc;		/* unpinned copy, for the slots of a put */
	TupleTableSlot *slot;
	IndexScanDesc scan;			/* SnapshotAny probes by hash */
	bytea	   *key;			/* scan key datum: the probed hash */
} HeapChunkState;

static Size
heap_max_size(void)
{
	/* Parts are addressed by an int32 and a chunk must fit one allocation. */
	return MaxAllocSize - VARHDRSZ;
}

static void *
heap_open_store(DoltChunkIntent intent)
{
	HeapChunkState *state = palloc0(sizeof(HeapChunkState));

	state->lockmode = intent == DOLT_CHUNK_WRITE ? RowExclusiveLock : AccessShareLock;
	state->heap = table_open(DoltChunkRelationId, state->lockmode);
	state->index = index_open(DoltChunkHashIndexId, state->lockmode);
	state->slot = table_slot_create(state->heap, NULL);
	state->key = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(state->key, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	return state;
}

static void
heap_close_store(void *arg)
{
	HeapChunkState *state = arg;

	if (state->scan)
		index_endscan(state->scan);
	ExecDropSingleTupleTableSlot(state->slot);
	index_close(state->index, state->lockmode);
	table_close(state->heap, state->lockmode);
	pfree(state->key);
	pfree(state);
}

/* Position the scan on the rows carrying |hash|, or only its part |part|. */
static void
probe(HeapChunkState *state, const uint8 *hash, int part)
{
	ScanKeyData keys[2];

	/* The scan lives as long as the store, not the caller's context. */
	if (!state->scan)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(state));

		state->scan = index_beginscan(state->heap, state->index, SnapshotAny, NULL, 2, 0);
		MemoryContextSwitchTo(old);
	}
	memcpy(VARDATA(state->key), hash, DOLT_CHUNK_HASH_SIZE);
	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(state->key));
	if (part < 0)
		ScanKeyInit(&keys[1], 2, BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
	else
		ScanKeyInit(&keys[1], 2, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(part));
	index_rescan(state->scan, keys, 2, NULL, 0);
}

static int32
slot_int(TupleTableSlot *slot, AttrNumber attribute)
{
	bool		isnull;

	return DatumGetInt32(slot_getattr(slot, attribute, &isnull));
}

static struct varlena *
slot_data(TupleTableSlot *slot)
{
	bool		isnull;
	struct varlena *value = (struct varlena *)
		DatumGetPointer(slot_getattr(slot, Anum_pg_dolt_chunk_chunkdata, &isnull));

	if (isnull || (VARATT_IS_EXTENDED(value) && !VARATT_IS_SHORT(value)))
		elog(ERROR, "dolt chunk is not stored inline");
	return value;
}

static void
heap_get(void *arg, int n, const uint8 *hashes, DoltChunkFoundFn found, void *found_arg)
{
	HeapChunkState *state = arg;

	for (int i = 0; i < n; i++)
	{
		uint8	   *assembled = NULL;
		int32		size = -1,
					next = 0;

		probe(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, -1);

		/*
		 * Parts arrive in order. Copies of a part are identical, so the first
		 * copy of each part is used and later ones are skipped.
		 */
		while (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			int32		part = slot_int(state->slot, Anum_pg_dolt_chunk_chunkpart);
			struct varlena *value;
			Size		length;

			if (part < next)
				continue;
			if (part > next)
				elog(ERROR, "dolt chunk is missing part %d", next);
			value = slot_data(state->slot);
			length = VARSIZE_ANY_EXHDR(value);
			if (part == 0)
			{
				size = slot_int(state->slot, Anum_pg_dolt_chunk_chunksize);
				if (length == (Size) size)
				{
					found(found_arg, i, (const uint8 *) VARDATA_ANY(value), length);
					next = -1;
					break;
				}
				assembled = palloc(size);
			}
			if ((Size) part * DG_CHUNK_PART + length > (Size) size)
				elog(ERROR, "dolt chunk part %d is too long", part);
			memcpy(assembled + (Size) part * DG_CHUNK_PART, VARDATA_ANY(value), length);
			next++;
			if ((Size) part * DG_CHUNK_PART + length == (Size) size)
			{
				found(found_arg, i, assembled, size);
				next = -1;
				break;
			}
		}
		ExecClearTuple(state->slot);
		if (next > 0)
			elog(ERROR, "dolt chunk is missing part %d", next);
		if (assembled)
			pfree(assembled);
	}
}

/*
 * Whether the row in the slot is a usable copy: inserted by a committed
 * transaction or this one, and not deleted by a committed one. Never waits
 * and never sets hint bits. Another transaction's uncommitted copy is not
 * usable, since that transaction may abort; a caller that needs the chunk
 * inserts its own copy instead.
 */
static bool
row_live(HeapChunkState *state)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) state->slot;
	HeapTupleHeader tuple;
	bool		live = true;

	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	tuple = bslot->base.tuple->t_data;
	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		TransactionId xmin = HeapTupleHeaderGetRawXmin(tuple);

		if (HeapTupleHeaderXminInvalid(tuple))
			live = false;
		else if (TransactionIdIsCurrentTransactionId(xmin))
			live = true;
		else if (TransactionIdIsInProgress(xmin))
			live = false;
		else
			live = TransactionIdDidCommit(xmin);
	}
	if (live && !(tuple->t_infomask & HEAP_XMAX_INVALID) &&
		!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		TransactionId xmax;

		/* Chunk rows are only inserted and deleted, never updated or locked. */
		if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			elog(ERROR, "unexpected multixact on a dolt chunk row");
		xmax = HeapTupleHeaderGetRawXmax(tuple);
		if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
			live = false;
		else if (TransactionIdIsCurrentTransactionId(xmax))
			live = false;
		else if (!TransactionIdIsInProgress(xmax))
			live = !TransactionIdDidCommit(xmax);
	}
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);
	return live;
}

/* Whether a usable copy of part |part| of |hash| exists. */
static bool
find_live(HeapChunkState *state, const uint8 *hash, int part)
{
	bool		live = false;

	probe(state, hash, part);
	while (!live && index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		live = row_live(state);
	ExecClearTuple(state->slot);
	return live;
}

static void
heap_exists(void *arg, int n, const uint8 *hashes, bool *found)
{
	HeapChunkState *state = arg;

	for (int i = 0; i < n; i++)
	{
		found[i] = find_live(state, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, 0);
	}
}

/* Form part |part| of a chunk; the caller frees the tuple and its values. */
static HeapTuple
form_part(HeapChunkState *state, bytea *hashvalue, const uint8 *data, Size length, int32 part,
		  Datum *index_values)
{
	Datum		values[Natts_pg_dolt_chunk];
	bool		nulls[Natts_pg_dolt_chunk] = {false};
	Size		offset = (Size) part * DG_CHUNK_PART;
	Size		piece = Min(DG_CHUNK_PART, length - offset);
	bytea	   *datavalue = palloc(VARHDRSZ + piece);
	HeapTuple	tuple;

	SET_VARSIZE(datavalue, VARHDRSZ + piece);
	memcpy(VARDATA(datavalue), data + offset, piece);
	values[Anum_pg_dolt_chunk_chunkpart - 1] = Int32GetDatum(part);
	values[Anum_pg_dolt_chunk_chunksize - 1] = Int32GetDatum((int32) length);
	values[Anum_pg_dolt_chunk_chunkcopy - 1] =
		Int64GetDatum((int64) U64FromFullTransactionId(GetTopFullTransactionId()));
	values[Anum_pg_dolt_chunk_chunkhash - 1] = PointerGetDatum(hashvalue);
	values[Anum_pg_dolt_chunk_chunkdata - 1] = PointerGetDatum(datavalue);
	tuple = heap_form_tuple(RelationGetDescr(state->heap), values, nulls);
	pfree(datavalue);
	index_values[0] = PointerGetDatum(hashvalue);
	index_values[1] = Int32GetDatum(part);
	index_values[2] = values[Anum_pg_dolt_chunk_chunkcopy - 1];
	return tuple;
}

/* The part tuples of the chunks a put inserts, written to the heap together. */
typedef struct PutBatch
{
	TupleTableSlot **slots;
	Datum	   *index_values;	/* three per slot */
	int			count,
				capacity;
} PutBatch;

/*
 * Form the parts of one chunk; true, since every put adds a copy.
 * A copy is keyed by this transaction's ID, so it never conflicts with
 * another's.
 */
static bool
put_one(HeapChunkState *state, PutBatch *batch, const uint8 *hash, const uint8 *data, Size length)
{
	bytea	   *hashvalue;
	int32		parts = length ? (int32) ((length + DG_CHUNK_PART - 1) / DG_CHUNK_PART) : 1;

	hashvalue = palloc(VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	SET_VARSIZE(hashvalue, VARHDRSZ + DOLT_CHUNK_HASH_SIZE);
	memcpy(VARDATA(hashvalue), hash, DOLT_CHUNK_HASH_SIZE);
	for (int32 part = 0; part < parts; part++)
	{
		HeapTuple	tuple;

		if (batch->count == batch->capacity)
		{
			batch->capacity = batch->capacity ? batch->capacity * 2 : 16;
			batch->slots = batch->slots ? repalloc(batch->slots, batch->capacity * sizeof(TupleTableSlot *)) :
				palloc(batch->capacity * sizeof(TupleTableSlot *));
			batch->index_values = batch->index_values ? repalloc(batch->index_values, batch->capacity * 3 * sizeof(Datum)) :
				palloc(batch->capacity * 3 * sizeof(Datum));
		}
		tuple = form_part(state, hashvalue, data, length, part, batch->index_values + 3 * batch->count);
		batch->slots[batch->count] = MakeSingleTupleTableSlot(state->part_desc, &TTSOpsHeapTuple);
		ExecStoreHeapTuple(tuple, batch->slots[batch->count], true);
		batch->count++;
	}
	return true;
}

/* Insert the batch's parts in one heap operation, then index each. */
static void
put_flush(HeapChunkState *state, PutBatch *batch)
{
	bool		index_nulls[3] = {false};

	if (!batch->count)
		return;
	heap_multi_insert(state->heap, batch->slots, batch->count, GetCurrentCommandId(true), 0, NULL);
	for (int i = 0; i < batch->count; i++)
	{
		index_insert(state->index, batch->index_values + 3 * i, index_nulls, &batch->slots[i]->tts_tid,
					 state->heap, UNIQUE_CHECK_NO, false, state->index_info);
		ExecDropSingleTupleTableSlot(batch->slots[i]);
	}
	batch->count = 0;
}

static const uint8 *sort_hashes;

static int
compare_hashes(const void *a, const void *b)
{
	return memcmp(sort_hashes + (Size) *(const int *) a * DOLT_CHUNK_HASH_SIZE,
				  sort_hashes + (Size) *(const int *) b * DOLT_CHUNK_HASH_SIZE,
				  DOLT_CHUNK_HASH_SIZE);
}

static int
heap_put(void *arg, int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths)
{
	HeapChunkState *state = arg;
	int		   *order = palloc(Max(n, 1) * sizeof(int));
	int			stored = 0;
	PutBatch	batch = {0};

	if (!state->index_info)
		state->index_info = BuildIndexInfo(state->index);

	/*
	 * A slot pins a reference-counted descriptor in the resource owner, which
	 * costs more than the insert itself over thousands of parts; a private
	 * copy of the descriptor is not counted.
	 */
	if (!state->part_desc)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(state));

		state->part_desc = CreateTupleDescCopy(RelationGetDescr(state->heap));
		MemoryContextSwitchTo(old);
	}

	/* Insert in hash order, so consecutive index insertions share leaf pages. */
	for (int i = 0; i < n; i++)
		order[i] = i;
	sort_hashes = hashes;
	qsort(order, n, sizeof(int), compare_hashes);
	for (int i = 0; i < n; i++)
	{
		int			j = order[i];
		const uint8 *hash = hashes + (Size) j * DOLT_CHUNK_HASH_SIZE;

		/* A batch's repeats of one chunk are adjacent in hash order. */
		if (i > 0 && memcmp(hash, hashes + (Size) order[i - 1] * DOLT_CHUNK_HASH_SIZE, DOLT_CHUNK_HASH_SIZE) == 0)
			continue;
		if (put_one(state, &batch, hash, data[j], lengths[j]))
			stored++;
	}
	put_flush(state, &batch);
	pfree(order);
	return stored;
}

static void
heap_scan(void *arg, DoltChunkScanFn fn, void *fn_arg)
{
	HeapChunkState *state = arg;
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(state->heap, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(state->heap, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		bytea	   *hash;

		if (slot_int(slot, Anum_pg_dolt_chunk_chunkpart) != 0)
			continue;
		hash = DatumGetByteaPP(slot_getattr(slot, Anum_pg_dolt_chunk_chunkhash, &isnull));
		if (!fn(fn_arg, (const uint8 *) VARDATA_ANY(hash), slot_int(slot, Anum_pg_dolt_chunk_chunksize)))
			break;
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
}

static int
heap_remove(void *arg, int n, const uint8 *hashes)
{
	HeapChunkState *state = arg;
	int			removed = 0;

	for (int i = 0; i < n; i++)
	{
		const uint8 *hash = hashes + (Size) i * DOLT_CHUNK_HASH_SIZE;
		ItemPointerData *tids;
		int			ntids = 0,
					maxtids = 4;

		if (!find_live(state, hash, 0))
			continue;

		/* Delete every live part; dead copies are left to VACUUM. */
		tids = palloc(maxtids * sizeof(ItemPointerData));
		probe(state, hash, -1);
		while (index_getnext_slot(state->scan, ForwardScanDirection, state->slot))
		{
			if (!row_live(state))
				continue;
			if (ntids == maxtids)
			{
				maxtids *= 2;
				tids = repalloc(tids, maxtids * sizeof(ItemPointerData));
			}
			tids[ntids++] = state->slot->tts_tid;
		}
		ExecClearTuple(state->slot);
		for (int t = 0; t < ntids; t++)
			simple_heap_delete(state->heap, &tids[t]);
		pfree(tids);
		removed++;
	}
	return removed;
}

static void
heap_stats(DoltChunkStoreStats *stats)
{
	Relation	heap = table_open(DoltChunkRelationId, AccessShareLock);
	Relation	index = index_open(DoltChunkHashIndexId, AccessShareLock);

	stats->chunks = Max(heap->rd_rel->reltuples, 0);
	stats->bytes = (uint64) (RelationGetNumberOfBlocks(heap) + RelationGetNumberOfBlocks(index)) * BLCKSZ;
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
}

static const DoltChunkStoreRoutine heap_routine = {
	.name = "heap",
	.max_size = heap_max_size,
	.open = heap_open_store,
	.close = heap_close_store,
	.get = heap_get,
	.exists = heap_exists,
	.put = heap_put,
	.scan = heap_scan,
	.remove = heap_remove,
	.stats = heap_stats,
};

const DoltChunkStoreRoutine *
dolt_chunk_heap_routine(void)
{
	return &heap_routine;
}
