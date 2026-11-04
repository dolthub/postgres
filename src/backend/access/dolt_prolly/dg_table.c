#include "dg.h"
#include <math.h>
#include "nodes/tidbitmap.h"
#include "optimizer/plancat.h"
#include "access/doltbranch.h"
#include "miscadmin.h"
#include "catalog/catalogview.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/sampling.h"
#include "pgstat.h"
#include "access/doltcatalog.h"
#include "catalog/pg_am_d.h"
#include "executor/tuptable.h"
#include "access/sysattr.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

/* Planner page accounting: rows per 8 kB "page" of a prolly table. */
#define DG_ESTIMATE_BYTES_PER_ROW 32

static TableAmRoutine methods;
static bool initialized;

typedef struct MaterializedRow
{
	uint64		handle;
	PLSlice		row;
} MaterializedRow;

typedef struct DGScan
{
	TableScanDescData base;
	MemoryContext context;
	DGState    *state;

	/*
	 * A forward scan walks the content tree, each entry yielding its row's
	 * occurrences: the derived handle, plus the allocated ones the delta
	 * holds for the row. Backward scans materialize the rows instead.
	 */
	bool		streaming;
	DGCursor	content;
	PLSlice		row;			/* the current entry's row */
	uint64	   *handles;		/* its handles */
	int			handle_count,
				handle_capacity,
				handle_position;
	MaterializedRow *rows;
	int64		row_count,
				row_capacity;
	bool		materialized;
	int64		position;
	MaterializedRow *page_rows;	/* a lossy bitmap page's rows, from one range scan */
	int			page_count,
				page_capacity;
	bool		page_scanned;
	TBMIterateResult bitmap_page;
	OffsetNumber bitmap_offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			bitmap_count;
	int			bitmap_position;
}			DGScan;

PLHash
dg_rowhash(PLSlice row)
{
	PLHash		hash;

	if (!pl_hash_bytes(row, &hash))
		elog(ERROR, "could not hash canonical row");
	return hash;
}
static PLSlice
row_for_handle(DGState *state, PLSlice hash)
{
	PLValue		value;
	PLSlice		row;

	if (hash.len != PL_HASH_SIZE)
		elog(ERROR, "invalid prolly row reference");
	if (!dg_get(state, hash, &value) || value.bytes.len < 8)
		elog(ERROR, "missing canonical row contents");
	row.data = value.bytes.data + 8;
	row.len = value.bytes.len - 8;
	return row;
}
static const TupleTableSlotOps *
slots(Relation rel)
{
	return &TTSOpsProlly;
}
static void materialize(DGScan *scan);

static TableScanDesc
begin(Relation rel, Snapshot snap, int nkeys, ScanKey key, ParallelTableScanDesc parallel, uint32 flags)
{
	DGScan	   *scan = palloc0(sizeof(*scan));
	MemoryContext old;

	if (parallel || nkeys)
		dg_unsupported("parallel scans or table scan keys");
	dg_validate_descriptor(RelationGetDescr(rel));
	/* ANALYZE opens its sample scan without a snapshot; read the live state. */
	if (!snap && (flags & SO_TYPE_ANALYZE))
		snap = GetActiveSnapshot();
	scan->base.rs_rd = rel;
	scan->base.rs_snapshot = snap;
	scan->base.rs_flags = flags;
	scan->context = AllocSetContextCreate(CurrentMemoryContext, "prolly scan", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(scan->context);
	scan->state = CatalogViewIsProvisional(rel->rd_catalogview) ? dg_catalog_mutation_load(rel, snap) : dg_load(rel, snap);
	scan->streaming = !(flags & SO_TYPE_BITMAPSCAN) && !CatalogViewIsProvisional(rel->rd_catalogview) && scan->state->has_root;
	if (scan->streaming)
		dg_cursor_init(&scan->content, scan->state, NULL);
	else if (!(flags & SO_TYPE_BITMAPSCAN))
		materialize(scan);
	scan->position = -1;
	MemoryContextSwitchTo(old);
	if (flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(rel);
	return &scan->base;
}

static PLStatus
collect_materialized(void *arg, PLSlice key, PLSlice value)
{
	DGScan	   *scan = arg;
	PLHash		digest;
	int			n;

	if (key.len != PL_HASH_SIZE || value.len < 8)
		elog(ERROR, "invalid prolly scan entry");
	memcpy(digest.bytes, key.data, PL_HASH_SIZE);
	n = dg_handles_of(scan->state, digest, (int64) pl_read64(value.data), &scan->handles, &scan->handle_capacity);
	for (int i = 0; i < n; i++)
	{
		if (scan->row_count == scan->row_capacity)
		{
			scan->row_capacity = scan->row_capacity ? scan->row_capacity * 2 : 64;
			scan->rows = scan->rows ? repalloc(scan->rows, scan->row_capacity * sizeof(MaterializedRow)) :
				palloc(scan->row_capacity * sizeof(MaterializedRow));
		}
		scan->rows[scan->row_count].handle = scan->handles[i];
		scan->rows[scan->row_count].row = (PLSlice) {value.data + 8, value.len - 8};
		scan->row_count++;
	}
	return PL_OK;
}

/* Every row occurrence of a scan, for the directions the walk does not serve. */
static void
materialize(DGScan *scan)
{
	MemoryContext old = MemoryContextSwitchTo(scan->context);

	if (!scan->materialized)
	{
		scan->row_count = 0;
		dg_scan(scan->state, NULL, NULL, collect_materialized, scan);
		scan->materialized = true;
	}
	scan->streaming = false;
	MemoryContextSwitchTo(old);
}
static void
end(TableScanDesc base)
{
	DGScan	   *scan = (DGScan *) base;

	if (base->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(base->rs_snapshot);
	MemoryContextDelete(scan->context);
	pfree(scan);
}
static void
rescan(TableScanDesc base, ScanKey key, bool set, bool strat, bool sync, bool pagemode)
{
	DGScan	   *scan = (DGScan *) base;

	scan->position = -1;
	scan->bitmap_count = 0;
	scan->bitmap_position = 0;
	scan->handle_count = scan->handle_position = 0;
	if (scan->streaming)
		dg_cursor_init(&scan->content, scan->state, NULL);
	if (base->rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(base->rs_rd);
}

/* The next row occurrence of the walk: the current entry's next handle, or the next entry's first. */
static bool
next_streamed(DGScan *scan, TupleTableSlot *slot)
{
	ItemPointerData tid;

	while (scan->handle_position >= scan->handle_count)
	{
		PLEntry		content;
		PLHash		digest;

		if (!scan->content.valid)
		{
			ExecClearTuple(slot);
			return false;
		}
		content = dg_cursor_entry(&scan->content);
		if (content.key.len != PL_HASH_SIZE || content.value.len < 8)
			elog(ERROR, "invalid prolly scan entry");
		memcpy(digest.bytes, content.key.data, PL_HASH_SIZE);
		scan->row.data = content.value.data + 8;
		scan->row.len = content.value.len - 8;
		scan->handle_count = dg_handles_of(scan->state, digest, (int64) pl_read64(content.value.data),
										   &scan->handles, &scan->handle_capacity);
		scan->handle_position = 0;
		dg_cursor_next(&scan->content);
	}
	dg_tid(scan->handles[scan->handle_position++], &tid);
	dg_decode(scan->base.rs_rd, scan->row, &tid, slot);
	pgstat_count_heap_getnext(scan->base.rs_rd);
	return true;
}

static bool
next(TableScanDesc base, ScanDirection dir, TupleTableSlot *slot)
{
	DGScan	   *scan = (DGScan *) base;
	MaterializedRow *entry;
	ItemPointerData tid;

	if (scan->streaming && ScanDirectionIsForward(dir))
		return next_streamed(scan, slot);
	if (scan->streaming)
		materialize(scan);
	if (ScanDirectionIsBackward(dir))
	{
		if (scan->position < 0)
			scan->position = scan->row_count;
		--scan->position;
	}
	else
		++scan->position;
	if (scan->position < 0 || scan->position >= scan->row_count)
	{
		ExecClearTuple(slot);
		return false;
	}
	entry = &scan->rows[scan->position];
	dg_tid(entry->handle, &tid);
	if (CatalogViewIsProvisional(base->rs_rd->rd_catalogview))
		dg_decode_mutation(base->rs_rd, entry->row, &tid, slot);
	else
		dg_decode(base->rs_rd, entry->row, &tid, slot);
	pgstat_count_heap_getnext(base->rs_rd);
	return true;
}
/* The rows of a lossy bitmap page's derived handle range, collected by one range scan. */
static PLStatus
collect_page_row(void *arg, PLSlice key, PLSlice value)
{
	DGScan	   *scan = arg;
	PLHash		digest;
	int			n;

	if (key.len != PL_HASH_SIZE || value.len < 8)
		elog(ERROR, "invalid prolly scan entry");
	memcpy(digest.bytes, key.data, PL_HASH_SIZE);
	n = dg_handles_of(scan->state, digest, (int64) pl_read64(value.data), &scan->handles, &scan->handle_capacity);
	for (int i = 0; i < n; i++)
	{
		if (scan->handles[i] / TBM_MAX_TUPLES_PER_PAGE != scan->bitmap_page.blockno)
			continue;
		if (scan->page_count == scan->page_capacity)
		{
			scan->page_capacity = scan->page_capacity ? scan->page_capacity * 2 : 16;
			scan->page_rows = scan->page_rows ? repalloc(scan->page_rows, scan->page_capacity * sizeof(MaterializedRow)) :
				palloc(scan->page_capacity * sizeof(MaterializedRow));
		}
		scan->page_rows[scan->page_count].handle = scan->handles[i];
		scan->page_rows[scan->page_count].row = (PLSlice) {value.data + 8, value.len - 8};
		scan->page_count++;
	}
	return PL_OK;
}

/*
 * Bitmap blocks are logical handle ranges, not physical relation pages. A
 * lossy page of derived handles is one digest range of the content tree,
 * scanned once; one of allocated handles is probed handle by handle.
 */
static bool
bitmap_next(TableScanDesc base, TupleTableSlot *slot, bool *recheck,
			uint64 *lossy_pages, uint64 *exact_pages)
{
	DGScan	   *scan = (DGScan *) base;

	for (;;)
	{
		ItemPointerData tid;
		PLHash		digest;
		PLSlice		row;

		CHECK_FOR_INTERRUPTS();
		if (scan->bitmap_position >= scan->bitmap_count)
		{
			if (!tbm_iterate(&base->st.rs_tbmiterator, &scan->bitmap_page))
			{
				ExecClearTuple(slot);
				return false;
			}
			scan->bitmap_position = 0;
			scan->page_scanned = false;
			if (scan->bitmap_page.lossy)
			{
				uint64		first = (uint64) scan->bitmap_page.blockno * TBM_MAX_TUPLES_PER_PAGE;

				++*lossy_pages;
				if (first + TBM_MAX_TUPLES_PER_PAGE <= DG_DERIVED_HANDLES && !CatalogViewIsProvisional(base->rs_rd->rd_catalogview))
				{
					PLHash		lower,
								upper;
					bool		bounded = dg_handle_range(first, TBM_MAX_TUPLES_PER_PAGE, &lower, &upper);
					PLSlice		lo = {lower.bytes, PL_HASH_SIZE},
								hi = {upper.bytes, PL_HASH_SIZE};
					MemoryContext old = MemoryContextSwitchTo(scan->context);

					scan->page_count = 0;
					dg_scan(scan->state, &lo, bounded ? &hi : NULL, collect_page_row, scan);
					MemoryContextSwitchTo(old);
					scan->page_scanned = true;
					scan->bitmap_count = scan->page_count;
				}
				else
					scan->bitmap_count = TBM_MAX_TUPLES_PER_PAGE;	/* visit every possible handle */
			}
			else
			{
				scan->bitmap_count = tbm_extract_page_tuple(&scan->bitmap_page,
					scan->bitmap_offsets, TBM_MAX_TUPLES_PER_PAGE);
				++*exact_pages;
			}
			continue;
		}
		if (scan->page_scanned)
		{
			MaterializedRow *entry = &scan->page_rows[scan->bitmap_position++];

			*recheck = true;
			dg_tid(entry->handle, &tid);
			dg_decode(base->rs_rd, entry->row, &tid, slot);
			return true;
		}
		ItemPointerSet(&tid, scan->bitmap_page.blockno,
			scan->bitmap_page.lossy ? scan->bitmap_position + 1 :
			scan->bitmap_offsets[scan->bitmap_position]);
		++scan->bitmap_position;
		*recheck = scan->bitmap_page.lossy || scan->bitmap_page.recheck;
		if (!CatalogViewIsProvisional(base->rs_rd->rd_catalogview) &&
			dg_stash_get(RelationGetRelid(base->rs_rd), scan->state, dg_id(&tid), &row))
		{
			dg_decode(base->rs_rd, row, &tid, slot);
			return true;
		}
		if (!dg_handle_digest(scan->state, dg_id(&tid), &digest))
			continue;
		row = row_for_handle(scan->state, (PLSlice) {digest.bytes, PL_HASH_SIZE});
		if (CatalogViewIsProvisional(base->rs_rd->rd_catalogview))
			dg_decode_mutation(base->rs_rd, row, &tid, slot);
		else
			dg_decode(base->rs_rd, row, &tid, slot);
		return true;
	}
}
/* Release a state loaded for |rel|; a provisional rewrite keeps its own. */
static void
release_state(Relation rel, DGState *s)
{
	if (CatalogViewIsProvisional(rel->rd_catalogview))
		dg_catalog_mutation_release(s);
	else
		dg_release(s);
}
static bool
fetch(Relation rel, ItemPointer tid, Snapshot snap, TupleTableSlot *slot)
{
	DGState    *s;
	PLHash		digest;

	/* Any version at all, as heap serves dead tuples under SnapshotAny. */
	if (snap == SnapshotAny && !CatalogViewIsProvisional(rel->rd_catalogview))
	{
		PLSlice		row;

		if (!dg_handle_version(rel, dg_id(tid), &digest, &row))
		{
			ExecClearTuple(slot);
			return false;
		}
		dg_decode(rel, row, tid, slot);
		return true;
	}
	s = CatalogViewIsProvisional(rel->rd_catalogview) ? dg_catalog_mutation_load(rel, snap) : dg_load(rel, snap);
	if (!dg_handle_digest(s, dg_id(tid), &digest))
	{
		release_state(rel, s);
		ExecClearTuple(slot);
		return false;
	}
	dg_decode(rel, row_for_handle(s, (PLSlice) {digest.bytes, PL_HASH_SIZE}), tid, slot);
	release_state(rel, s);
	return true;
}
/* An index scan's fetches share one state, loaded under the scan's snapshot. */
typedef struct DGFetch
{
	IndexFetchTableData base;
	MemoryContext memory;		/* the scan's, where the state lives */
	Snapshot	snapshot;
	DGState    *state;
} DGFetch;

/* ---------------------------------------------------------------------------
 * The row stash
 * ---------------------------------------------------------------------------
 *
 * An index entry's value holds its row, and the index scan finds the row's
 * handles to return them as TIDs. The table fetch for each TID would then
 * descend the handle tree and the content tree again to reach the same
 * bytes. The scan stashes each row under its handle instead, tagged with
 * the content and handle roots it was read under; a fetch under the same
 * roots takes the row from the stash. The stash is bounded, and cleared as
 * a whole when it fills.
 */
typedef struct StashKey
{
	uint64		handle;
	Oid			relid;
	uint32		pad;			/* zeroed: the hash covers every byte */
} StashKey;

typedef struct StashEntry
{
	StashKey	key;
	PLHash		content,
				handles;
	PLSlice		row;
} StashEntry;

#define DG_STASH_BYTES ((Size) 16 * 1024 * 1024)

static HTAB *stash;
static MemoryContext stash_memory;
static Size stash_bytes;

static void
stash_reset(void)
{
	HASHCTL		ctl = {0};

	if (stash_memory)
		MemoryContextReset(stash_memory);
	else
		stash_memory = AllocSetContextCreate(TopMemoryContext, "prolly row stash", ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(StashKey);
	ctl.entrysize = sizeof(StashEntry);
	ctl.hcxt = stash_memory;
	stash = hash_create("prolly row stash", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	stash_bytes = 0;
}

void
dg_stash_put(Oid relid, const DGState *state, uint64 handle, PLSlice row)
{
	StashKey	key = {handle, relid, 0};
	StashEntry *entry;
	uint8	   *copy;
	bool		found;

	if (!stash || stash_bytes + row.len > DG_STASH_BYTES)
		stash_reset();
	entry = hash_search(stash, &key, HASH_ENTER, &found);
	if (found)
	{
		if (pl_hash_equal(entry->content, dg_state_view(state)))
			return;
		stash_bytes -= entry->row.len;
		pfree(unconstify(uint8 *, entry->row.data));
	}
	copy = MemoryContextAlloc(stash_memory, Max(row.len, 1));
	memcpy(copy, row.data, row.len);
	entry->content = dg_state_view(state);
	entry->row.data = copy;
	entry->row.len = row.len;
	stash_bytes += row.len;
}

bool
dg_stash_get(Oid relid, const DGState *state, uint64 handle, PLSlice *row)
{
	StashKey	key = {handle, relid, 0};
	StashEntry *entry;

	if (!stash)
		return false;
	entry = hash_search(stash, &key, HASH_FIND, NULL);
	if (!entry || !pl_hash_equal(entry->content, dg_state_view(state)))
		return false;
	*row = entry->row;
	return true;
}

static IndexFetchTableData *
fetch_begin(Relation rel)
{
	DGFetch    *f = palloc0(sizeof(*f));

	f->base.rel = rel;
	f->memory = CurrentMemoryContext;
	return &f->base;
}
static void
fetch_reset(IndexFetchTableData *f)
{
}
static void
fetch_end(IndexFetchTableData *base)
{
	DGFetch    *f = (DGFetch *) base;

	if (f->state)
		dg_release(f->state);
	pfree(f);
}
static bool
fetch_index(IndexFetchTableData *base, ItemPointer tid, Snapshot snap, TupleTableSlot *slot, bool *again, bool *dead)
{
	DGFetch    *f = (DGFetch *) base;
	Relation	rel = base->rel;
	PLSlice		row;

	*again = false;
	if (dead)
		*dead = false;
	if (snap->snapshot_type == SNAPSHOT_DIRTY)
	{
		snap->xmin = snap->xmax = InvalidTransactionId;
		snap->speculativeToken = 0;
	}
	if (CatalogViewIsProvisional(rel->rd_catalogview) || snap == SnapshotAny)
		return fetch(rel, tid, snap, slot);
	if (f->state && f->snapshot != snap)
	{
		dg_release(f->state);
		f->state = NULL;
	}
	if (!f->state)
	{
		MemoryContext old = MemoryContextSwitchTo(f->memory);

		f->state = dg_load(rel, snap);
		f->snapshot = snap;
		MemoryContextSwitchTo(old);
	}
	if (!dg_stash_get(RelationGetRelid(rel), f->state, dg_id(tid), &row))
	{
		PLHash		digest;

		if (!dg_handle_digest(f->state, dg_id(tid), &digest))
		{
			ExecClearTuple(slot);
			return false;
		}
		row = row_for_handle(f->state, (PLSlice) {digest.bytes, PL_HASH_SIZE});
	}
	dg_decode(rel, row, tid, slot);

	/* A dirty snapshot reports the running transaction changing the row. */
	if (snap->snapshot_type == SNAPSHOT_DIRTY)
		snap->xmax = dg_record_xwait(rel, dg_relation_branch(rel), (int64) dg_id(tid), false);
	return true;
}
static bool
tid_valid(TableScanDesc s, ItemPointer tid)
{
	return ItemPointerIsValid(tid);
}
static void
latest(TableScanDesc s, ItemPointer tid)
{
	/* Successors are found by locking; an unlocked lookup keeps the TID. */
}
/* Whether |snap| sees row |tid|. */
static bool
visible(Relation rel, ItemPointer tid, Snapshot snap)
{
	DGState    *s = CatalogViewIsProvisional(rel->rd_catalogview) ?
		dg_catalog_mutation_load(rel, snap) : dg_load(rel, snap);
	PLHash		digest;
	bool		present = dg_handle_digest(s, dg_id(tid), &digest);

	release_state(rel, s);
	return present;
}
static bool
satisfies(Relation rel, TupleTableSlot *slot, Snapshot snap)
{
	return visible(rel, &slot->tts_tid, snap);
}

/*
 * Whether this transaction inserted row |tid| of |rel|, as heap tests a
 * tuple's xmin: the row is in its changes and not in the committed state.
 */
bool
dg_tuple_inserted_by_self(Relation rel, ItemPointer tid)
{
	return !dg_row_committed(rel, dg_id(tid)) && visible(rel, tid, SnapshotSelf);
}
static TransactionId
index_delete(Relation rel, TM_IndexDeleteOp *op)
{
	dg_unsupported("physical index tuple deletion");
}
/*
 * Writers take no table-level write lock: they lock the rows they change, and
 * publication merges disjoint changes. Canonical storage bindings still check
 * their write authority.
 */
/* A revision is history: its rows can be neither changed nor locked. */
static void
read_only_revision(Relation rel)
{
	if (CatalogViewIsLeased(RelationGetRelid(rel)) && !dg_storage_bound(rel) &&
		!CatalogViewIsProvisional(rel->rd_catalogview))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("relation \"%s\" is a revision, which is read-only", RelationGetRelationName(rel))));
}

static void
writer(Relation rel)
{
	read_only_revision(rel);
	dg_serializable_write(rel);
	if (dg_storage_bound(rel) || CatalogViewIsLeased(RelationGetRelid(rel)))
		dg_storage_require_write(rel);
}
static void
multiplicity(DGState *s, PLSlice row, int delta)
{
	PLHash		hash = dg_rowhash(row);
	PLSlice		key = {hash.bytes, PL_HASH_SIZE};
	PLValue		old;
	uint64		count;
	uint8	   *bytes;
	PLEdit		edit;

	dg_check(pl_get(dg_content(s), key, &old));
	count = old.present ? pl_read64(old.bytes.data) : 0;
	if (old.present && (old.bytes.len != row.len + 8 || memcmp(old.bytes.data + 8, row.data, row.len)))
		elog(ERROR, "canonical row hash collision");
	if (delta < 0 && !count)
		elog(ERROR, "missing canonical row");
	count = delta > 0 ? count + 1 : count - 1;
	bytes = palloc(8 + row.len);
	pl_write64(bytes, count);
	memcpy(bytes + 8, row.data, row.len);
	edit.key = key;
	edit.value.present = count > 0;
	edit.value.bytes.data = bytes;
	edit.value.bytes.len = 8 + row.len;
	dg_check(pl_apply(dg_content(s), &edit, 1, dg_content(s)));
	pfree(bytes);
}
/*
 * An ordinary insert records its edits in the command's batch, applied to
 * the trees in one pass when something else reads them; a rewrite's state
 * is private and takes each edit directly.
 */
static void
insert_row(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate)
{
	PLSlice		row;
	uint64		handle;

	if (CatalogViewIsProvisional(rel->rd_catalogview))
	{
		DGState    *s = dg_catalog_rewrite_state(rel);
		PLHash		digest;

		row = dg_encode(rel, slot);
		digest = dg_rowhash(row);
		handle = dg_handle_choose(rel, s, NULL, digest);
		multiplicity(s, row, 1);
		dg_handle_note(s, handle, digest, true);
	}
	else
	{
		DGBatch    *batch;

		writer(rel);
		batch = dg_batch(rel, cid);
		row = dg_encode(rel, slot);
		handle = dg_handle_choose(rel, dg_batch_state(batch), batch, dg_rowhash(row));
		dg_batch_insert(batch, row, handle);
		dg_index_apply(rel, row, 1, handle, cid);
		pgstat_count_heap_insert(rel, 1);
		dg_collect();
	}
	dg_tid(handle, &slot->tts_tid);
	pfree((void *) row.data);
	slot->tts_tableOid = RelationGetRelid(rel);
}
/* The end of a COPY or multi-row INSERT: apply what the command recorded. */
static void
finish_bulk_insert(Relation rel, int options)
{
	if (!CatalogViewIsProvisional(rel->rd_catalogview))
		dg_batch_finish(rel);
}
static void
multi_insert(Relation rel, TupleTableSlot **slots_, int n, CommandId cid, int opts, BulkInsertState state)
{
	int			i;

	for (i = 0; i < n; i++)
		insert_row(rel, slots_[i], cid, opts, state);
}
/*
 * A speculative insertion is an ordinary one: the row stays private until
 * commit, and competing inserters of its key wait on the unique-key
 * reservation the arbiter index check takes, not on a speculative token.
 */
static void
speculative(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 token)
{
	insert_row(rel, slot, cid, options, bistate);
}
static void remove_row(Relation rel, uint64 handle, CommandId cid);

/* An insertion that met a conflicting row is undone without a trace. */
static void
complete(Relation rel, TupleTableSlot *slot, uint32 token, bool succeeded)
{
	if (!succeeded)
		remove_row(rel, dg_id(&slot->tts_tid), GetCurrentCommandId(true));
}
static PLSlice
copy_slice(PLSlice bytes)
{
	uint8	   *copy = palloc(Max(bytes.len, 1));

	memcpy(copy, bytes.data, bytes.len);
	return (PLSlice) {copy, bytes.len};
}

/* The digest row |handle| maps to in |s|; false if the handle is absent. */
static bool
handle_digest(DGState *s, uint64 handle, PLHash *digest)
{
	return dg_handle_digest(s, handle, digest);
}

/*
 * Whether this transaction already updated or deleted row version |handle|,
 * and if so the result heap gives: TM_SelfModified for the current or a later
 * command, TM_Invisible for an earlier one.
 */
static bool
self_modified(Relation rel, uint64 handle, Snapshot snapshot, TM_FailureData *tmfd, TM_Result *result)
{
	CommandId	cid;

	if (!dg_row_modified(rel, dg_relation_branch(rel), handle, &cid))
		return false;
	dg_tid(handle, &tmfd->ctid);
	tmfd->xmax = GetTopTransactionIdIfAny();
	tmfd->cmax = cid;
	tmfd->traversed = false;
	*result = snapshot && IsMVCCSnapshot(snapshot) && cid < snapshot->curcid ? TM_Invisible : TM_SelfModified;
	return true;
}

/*
 * After its lock record allowed a change, whether row version |handle| still
 * exists. A history install (merge, reset) replaces rows without touching lock
 * records, so a version can disappear without its record knowing.
 */
static TM_Result
still_current(Relation rel, uint64 handle, TM_Result result, TM_FailureData *tmfd)
{
	DGBatch    *b;
	DGState    *s;
	PLHash		digest;
	bool		present;

	if (result != TM_Ok)
		return result;
	if ((b = dg_batch_fresh(rel)) != NULL)
		present = dg_batch_handle_digest(b, handle, &digest);
	else
	{
		s = dg_load(rel, SnapshotSelf);
		present = handle_digest(s, handle, &digest);
		dg_release(s);
	}
	if (present)
		return TM_Ok;
	dg_tid(handle, &tmfd->ctid);
	return TM_Deleted;
}

/*
 * Referential actions under Repeatable Read pass the transaction snapshot as a
 * crosscheck: a row it cannot see was added after the snapshot, so changing
 * it is a serialization failure, as in heap_delete.
 */
static bool
crosscheck_visible(Relation rel, Snapshot crosscheck, uint64 handle)
{
	DGState    *s;
	PLHash		digest;
	bool		visible;

	if (crosscheck == InvalidSnapshot)
		return true;
	s = dg_load(rel, crosscheck);
	visible = handle_digest(s, handle, &digest);
	dg_release(s);
	return visible;
}

static TM_Result
delete_row(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, bool changing)
{
	uint64		handle = dg_id(tid);
	TM_Result	result;
	int64		next;

	writer(rel);
	if (self_modified(rel, handle, snapshot, failure, &result))
		return result;
	result = dg_record_delete(rel, dg_relation_branch(rel), (int64) handle, cid, wait, changing, failure, &next);
	if (result == TM_Ok && !crosscheck_visible(rel, crosscheck, handle))
		result = TM_Updated;
	result = still_current(rel, handle, result, failure);
	if (result != TM_Ok)
		return result;
	remove_row(rel, handle, cid);
	dg_note_modified(rel, dg_relation_branch(rel), handle, cid);
	pgstat_count_heap_delete(rel);
	return TM_Ok;
}

/* Remove row version |handle| and its index entries in command |cid|. */
static void
remove_row(Relation rel, uint64 handle, CommandId cid)
{
	DGBatch    *b = dg_batch_latest(rel, cid);
	PLHash		digest;
	PLSlice		row;

	if (!dg_batch_handle_digest(b, handle, &digest))
		elog(ERROR, "locked prolly row is missing");
	if (!dg_batch_row(b, digest, &row))
		elog(ERROR, "missing canonical row");
	row = copy_slice(row);
	dg_batch_remove(b, row, handle);
	dg_index_apply(rel, row, -1, handle, cid);
	dg_collect();
}

/* Whether the old and new rows differ in any column of |attributes|. */
static bool
columns_differ(PLSlice old, PLSlice new, Bitmapset *attributes)
{
	int			member = -1;

	while ((member = bms_next_member(attributes, member)) >= 0)
	{
		int			attribute = member + FirstLowInvalidHeapAttributeNumber - 1;
		PLValue		a,
					b;

		if (attribute < 0)
			continue;
		dg_check(pl_tuple_get(old, attribute, &a));
		dg_check(pl_tuple_get(new, attribute, &b));
		if (a.present != b.present ||
			(a.present && (a.bytes.len != b.bytes.len || memcmp(a.bytes.data, b.bytes.data, a.bytes.len))))
			return true;
	}
	return false;
}

/*
 * Replace row version |tid| with the slot's row. The new version gets a new
 * handle, as a heap update makes a new tuple, so AFTER triggers and RETURNING
 * can still fetch the old version by its TID. The old version's lock record
 * names the new one for transactions that waited on it.
 */
/*
 * The row version |handle| as this transaction sees it, with its digest; a
 * version a concurrent update already replaced in the latest state may still
 * be in the caller's |snapshot|. Read through the command's batch while its
 * trees are the latest, so a statement's earlier rows stay pending.
 */
static bool
current_row(Relation rel, uint64 handle, Snapshot snapshot, PLHash *digest, PLSlice *row)
{
	DGBatch    *b = dg_batch_fresh(rel);
	DGState    *s;
	bool		present;

	if (b && dg_batch_handle_digest(b, handle, digest))
	{
		if (!dg_batch_row(b, *digest, row))
			elog(ERROR, "missing canonical row");
		*row = copy_slice(*row);
		return true;
	}
	s = dg_load(rel, SnapshotSelf);
	if (!handle_digest(s, handle, digest) && snapshot && IsMVCCSnapshot(snapshot))
	{
		dg_release(s);
		s = dg_load(rel, snapshot);
	}
	present = handle_digest(s, handle, digest);
	if (present)
		*row = copy_slice(row_for_handle(s, (PLSlice) {digest->bytes, PL_HASH_SIZE}));
	dg_release(s);
	return present;
}

/* The handle of a row version about to replace another: derived from its digest when it can be. */
static uint64
successor_handle(Relation rel, CommandId cid, PLHash digest)
{
	DGBatch    *b = dg_batch_latest(rel, cid);

	return dg_handle_choose(rel, dg_batch_state(b), b, digest);
}

static TM_Result
update_row(Relation rel, ItemPointer tid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *failure, LockTupleMode *mode, TU_UpdateIndexes *indexes)
{
	DGBatch    *b;
	uint64		handle = dg_id(tid),
				successor;
	PLHash		old_digest,
				new_digest;
	PLSlice		old_row = {0},
				new_row;
	bool		key_change = true,
				indexed_change = true;
	TM_Result	result;
	int64		next;

	writer(rel);
	new_row = dg_encode(rel, slot);
	new_digest = dg_rowhash(new_row);
	/*
	 * Judge the key change from the version being updated. A concurrent
	 * update may already have replaced it in the latest state; the caller's
	 * snapshot still has it.
	 */
	if (current_row(rel, handle, snapshot, &old_digest, &old_row))
	{
		Bitmapset  *keys = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);
		Bitmapset  *indexed = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_HOT_BLOCKING);

		key_change = columns_differ(old_row, new_row, keys);
		indexed_change = columns_differ(old_row, new_row, indexed);
	}
	*mode = key_change ? LockTupleExclusive : LockTupleNoKeyExclusive;
	if (self_modified(rel, handle, snapshot, failure, &result))
		return result;
	successor = successor_handle(rel, cid, new_digest);
	result = dg_record_update(rel, dg_relation_branch(rel), (int64) handle, (int64) successor, cid, key_change,
							  wait, failure, &next);
	if (result == TM_Ok && !crosscheck_visible(rel, crosscheck, handle))
		result = TM_Updated;
	result = still_current(rel, handle, result, failure);
	if (result != TM_Ok)
		return result;

	/* The row may have changed between the first read and the lock. */
	b = dg_batch_latest(rel, cid);
	if (!dg_batch_handle_digest(b, handle, &old_digest))
		elog(ERROR, "locked prolly row is missing");
	if (!dg_batch_row(b, old_digest, &old_row))
		elog(ERROR, "missing canonical row");
	old_row = copy_slice(old_row);
	dg_batch_remove(b, old_row, handle);
	dg_batch_insert(b, new_row, successor);
	if (!pl_hash_equal(old_digest, new_digest))
	{
		dg_index_apply(rel, old_row, -1, handle, cid);
		dg_index_apply(rel, new_row, 1, successor, cid);
	}
	dg_note_modified(rel, dg_relation_branch(rel), handle, cid);
	dg_tid(successor, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	*indexes = indexed_change ? TU_All : TU_None;
	pgstat_count_heap_update(rel, false, false);
	dg_collect();
	return TM_Ok;
}

static TM_Result
lock_row(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy policy, uint8 flags, TM_FailureData *tmfd)
{
	TM_Result	result;
	bool		traversed = false;
	int64		next;
	CommandId	made = InvalidCommandId;	/* the command that made *tid, if this transaction did */

	read_only_revision(rel);

	/*
	 * A row this transaction inserted in the current command, as a later row
	 * of one INSERT ... ON CONFLICT finds, is invisible to heap_lock_tuple,
	 * which still returns it.
	 */
	if (snapshot && IsMVCCSnapshot(snapshot) && dg_tuple_inserted_by_self(rel, tid) &&
		!visible(rel, tid, snapshot))
	{
		fetch(rel, tid, SnapshotAny, slot);
		return TM_Invisible;
	}
	for (;;)
	{
		CommandId	modified;

		/*
		 * Following a concurrent update, heapam_tuple_lock reads the chain
		 * with a dirty snapshot, to which versions this transaction replaced
		 * or deleted are dead: it passes them by. The live version, if this
		 * transaction made it in this command or a later one, is
		 * TM_SelfModified, reporting the command that made it.
		 */
		if (traversed && dg_row_modified(rel, dg_relation_branch(rel), dg_id(tid), &modified))
		{
			int64		replacement = dg_record_successor(rel, dg_relation_branch(rel), (int64) dg_id(tid));

			if (!replacement)
			{
				dg_tid(dg_id(tid), &tmfd->ctid);
				result = TM_Deleted;
				break;
			}
			made = modified;
			dg_tid((uint64) replacement, tid);
			continue;
		}
		if (traversed && made != InvalidCommandId && made >= cid)
		{
			dg_tid(dg_id(tid), &tmfd->ctid);
			tmfd->xmax = GetTopTransactionIdIfAny();
			tmfd->cmax = made;
			result = TM_SelfModified;
			break;
		}
		made = InvalidCommandId;
		result = dg_record_lock(rel, dg_relation_branch(rel), (int64) dg_id(tid), cid, mode, policy,
								(flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0, tmfd, &next);
		if (result == TM_Updated && (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
		if (result != TM_Updated || !(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
			break;

		/* Read Committed: follow the update chain to the latest version. */
		*tid = tmfd->ctid;
		traversed = true;
	}
	tmfd->traversed = traversed;
	if (result != TM_Ok)
		return result;

	/*
	 * A caller that found the row by its TID, as a foreign key check does
	 * through an index, already holds the version it locked: a TID names one
	 * row version, so there is nothing to fetch again.
	 */
	if (!traversed && !TTS_EMPTY(slot) && slot->tts_tableOid == RelationGetRelid(rel) &&
		ItemPointerEquals(&slot->tts_tid, tid))
		return TM_Ok;

	/*
	 * A key-share lock may hold a version a committed non-key update replaced,
	 * as heap allows; fetch whichever version the TID names.
	 */
	if (!fetch(rel, tid, SnapshotAny, slot))
	{
		dg_tid(dg_id(tid), &tmfd->ctid);
		return TM_Deleted;
	}
	return TM_Ok;
}
static Size
parallel_estimate(Relation r)
{
	dg_unsupported("parallel scans");
}
static Size
parallel_init(Relation r, ParallelTableScanDesc p)
{
	dg_unsupported("parallel scans");
}
static void
parallel_reset(Relation r, ParallelTableScanDesc p)
{
	dg_unsupported("parallel scans");
}
static void
new_storage(Relation rel, const RelFileLocator *loc, char persistence, TransactionId *xid, MultiXactId *multi)
{
	if (persistence != RELPERSISTENCE_PERMANENT)
		dg_unsupported("temporary/unlogged tables");

	/*
	 * Prolly tables live on branches: CREATE TABLE publishes them to the
	 * branch's schema and allocates their storage there. Physical prolly
	 * relations, from CREATE TABLE AS, SET ACCESS METHOD and the like, are
	 * not supported yet.
	 */
	if (!rel->rd_catalogview)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("prolly tables can only be created with CREATE TABLE")));
	dg_validate_descriptor(RelationGetDescr(rel));
	GetHeapamTableAmRoutine()->relation_set_new_filelocator(rel, loc, persistence, xid, multi);
}
static void
cluster(Relation a, Relation b, Relation c, bool sort, TransactionId oldest, TransactionId *xid, MultiXactId *multi, double *rows, double *vacuumed, double *dead)
{
	dg_unsupported("CLUSTER/VACUUM FULL");
}
/*
 * Prolly rows carry no transaction IDs to freeze, and replaced versions live
 * in the chunk store, whose garbage collection is separate. VACUUM has
 * nothing to do here.
 */
static void
vacuum_rel(Relation rel, struct VacuumParams *p, BufferAccessStrategy s)
{
}

/*
 * The relation file holds no blocks, so ANALYZE samples nothing; planning
 * uses relation_estimate_size instead of sampled statistics.
 */
static bool
analyze_block(TableScanDesc s, ReadStream *stream)
{
	return false;
}
static bool
analyze_tuple(TableScanDesc s, TransactionId oldest, double *live, double *dead, TupleTableSlot *slot)
{
	return false;
}
/*
 * Whether ANALYZE collects statistics for |rel|: a branch's own table, named
 * by its durable OID, whose pg_statistic rows are keyed by that OID and so
 * belong to the table on that branch. Revisions and tables mid-DDL keep none.
 */
bool
dg_relation_analyzable(Relation rel)
{
	return rel->rd_rel->relam == DOLT_PROLLY_TABLE_AM_OID && DG_ROWS_KIND(rel->rd_rel->relkind) &&
		CatalogViewIsDurable(RelationGetRelid(rel)) && !CatalogViewIsProvisional(rel->rd_catalogview);
}

/*
 * ANALYZE's sample of a prolly table: every row of the snapshot is visited,
 * as the table has no blocks to sample, and a reservoir keeps |targrows| of
 * them chosen uniformly, by the selection ANALYZE applies within blocks.
 */
int
dg_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
					   double *totalrows, double *totaldeadrows)
{
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	TableScanDesc scan = table_beginscan(rel, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	ReservoirStateData rstate;
	double		seen = 0,
				skip = -1;
	int			numrows = 0;

	reservoir_init_selection_state(&rstate, targrows);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		if (numrows < targrows)
			rows[numrows++] = ExecCopySlotHeapTuple(slot);
		else
		{
			if (skip < 0)
				skip = reservoir_get_next_S(&rstate, seen, targrows);
			if (skip <= 0)
			{
				int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

				heap_freetuple(rows[k]);
				rows[k] = ExecCopySlotHeapTuple(slot);
			}
			skip -= 1;
		}
		seen += 1;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	UnregisterSnapshot(snapshot);
	*totalrows = seen;
	*totaldeadrows = 0;
	ereport(elevel,
			(errmsg("\"%s\": scanned %.0f rows; %d rows in sample",
					RelationGetRelationName(rel), seen, numrows)));
	return numrows;
}

static double
build_range(Relation t, Relation i, IndexInfo *info, bool sync, bool any, bool progress, BlockNumber start, BlockNumber n, IndexBuildCallback cb, void *arg, TableScanDesc scan)
{
	dg_unsupported("external index build callbacks");
}
static void
validate_index(Relation t, Relation i, IndexInfo *info, Snapshot s, struct ValidateIndexState *v)
{
	dg_unsupported("concurrent index builds");
}
static bool
toast(Relation rel)
{
	return false;
}
static void
estimate(Relation rel, int32 *widths, BlockNumber *pages, double *tuples, double *allvis)
{
	DGState *state = CatalogViewIsProvisional(rel->rd_catalogview) ?
		dg_catalog_mutation_load(rel, GetActiveSnapshot()) :
		dg_load(rel, GetActiveSnapshot());
	double bytes;

	/* Physical adapter pages do not describe a revision's row count. The
	 * handle root carries the exact count, including duplicate occurrences.
	 * Use logical row volume for costing until chunk statistics are exposed.
	 */
	*tuples = dg_handle_count(state);

	/*
	 * A row costs about the same to reach by handle as in sequence: there are
	 * no pages to seek, and the type-declared width the planner would size
	 * pages by overstates stored rows several times over. Report few pages,
	 * so that plans are chosen on per-row costs rather than on page fetches.
	 */
	bytes = *tuples * DG_ESTIMATE_BYTES_PER_ROW;
	*pages = (BlockNumber) Min((double) MaxBlockNumber,
		Max(1.0, ceil(bytes / BLCKSZ)));

	/* Index entries are versioned with the rows: index-only scans fetch nothing. */
	*allvis = 1;
	release_state(rel, state);
}
static bool
sample_block(TableScanDesc s, struct SampleScanState *state)
{
	dg_unsupported("TABLESAMPLE");
}
static bool
sample_tuple(TableScanDesc s, struct SampleScanState *state, TupleTableSlot *slot)
{
	dg_unsupported("TABLESAMPLE");
}
static const char *
index_am(Relation rel, const char *requested)
{
	if (strcmp(requested, "btree") && strcmp(requested, "dolt_prolly_index"))
		dg_unsupported("other index families");
	return "dolt_prolly_index";
}
static void
validate_alter(Relation rel, struct AlterTableCmd *cmd)
{
	if (CatalogViewIsProvisional(rel->rd_catalogview) &&
		(cmd->subtype == AT_AlterConstraint || cmd->subtype == AT_SetStatistics || cmd->subtype == AT_SetOptions ||
		 cmd->subtype == AT_ResetOptions || cmd->subtype == AT_SetStorage || cmd->subtype == AT_SetCompression ||
		 cmd->subtype == AT_SetNotNull || cmd->subtype == AT_DropNotNull || cmd->subtype == AT_AddColumn || cmd->subtype == AT_DropColumn || cmd->subtype == AT_ColumnDefault || cmd->subtype == AT_CookedColumnDefault || cmd->subtype == AT_AddConstraint || cmd->subtype == AT_DropConstraint || cmd->subtype == AT_ValidateConstraint || cmd->subtype == AT_AlterColumnType ||
		 cmd->subtype == AT_AddInherit || cmd->subtype == AT_DropInherit ||
		 cmd->subtype == AT_AddIdentity || cmd->subtype == AT_SetIdentity || cmd->subtype == AT_DropIdentity))
		return;

	/*
	 * These operations do not alter the stored value layout. pg_dump emits
	 * primary/unique constraints as ALTER TABLE after restoring the data.
	 */
	if (cmd->subtype == AT_AddIndex || cmd->subtype == AT_ChangeOwner)
		return;
	if (cmd->subtype == AT_AddConstraint && IsA(cmd->def, Constraint))
	{
		Constraint *constraint = (Constraint *) cmd->def;

		if (constraint->contype == CONSTR_FOREIGN)
			return;
		if ((constraint->contype == CONSTR_PRIMARY || constraint->contype == CONSTR_UNIQUE) &&
			!constraint->deferrable)
			return;
	}

	/*
	 * Restored primary-key columns are already NOT NULL. Permit only the
	 * idempotent SET NOT NULL generated while rebuilding their constraint.
	 */
	if (cmd->subtype == AT_SetNotNull)
	{
		AttrNumber	attnum = get_attnum(RelationGetRelid(rel), cmd->name);

		if (attnum > 0 && TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attnotnull)
			return;
	}
	dg_unsupported("ALTER TABLE (schema versioning is not implemented)");
}
const TableAmRoutine *
dg_table_routine(void)
{
	if (!initialized)
	{
		methods = *GetHeapamTableAmRoutine();
		methods.relation_index_am = index_am;
		methods.relation_validate_alter = validate_alter;
		methods.relation_begin_rewrite = dg_catalog_begin_rewrite;
		methods.relation_finish_rewrite = dg_catalog_finish_rewrite;
		methods.slot_callbacks = slots;
		methods.scan_begin = begin;
		methods.scan_end = end;
		methods.scan_rescan = rescan;
		methods.scan_getnextslot = next;
		methods.scan_set_tidrange = NULL;
		methods.scan_getnextslot_tidrange = NULL;
		methods.parallelscan_estimate = parallel_estimate;
		methods.parallelscan_initialize = parallel_init;
		methods.parallelscan_reinitialize = parallel_reset;
		methods.index_fetch_begin = fetch_begin;
		methods.index_fetch_reset = fetch_reset;
		methods.index_fetch_end = fetch_end;
		methods.index_fetch_tuple = fetch_index;
		methods.tuple_fetch_row_version = fetch;
		methods.tuple_tid_valid = tid_valid;
		methods.tuple_get_latest_tid = latest;
		methods.tuple_satisfies_snapshot = satisfies;
		methods.index_delete_tuples = index_delete;
		methods.tuple_insert = insert_row;
		methods.multi_insert = multi_insert;
		methods.finish_bulk_insert = finish_bulk_insert;
		methods.index_only_visible = true;
		methods.tuple_insert_speculative = speculative;
		methods.tuple_complete_speculative = complete;
		methods.tuple_delete = delete_row;
		methods.tuple_update = update_row;
		methods.tuple_lock = lock_row;
		methods.relation_set_new_filelocator = new_storage;
		methods.relation_copy_for_cluster = cluster;
		methods.relation_vacuum = vacuum_rel;
		methods.scan_analyze_next_block = analyze_block;
		methods.scan_analyze_next_tuple = analyze_tuple;
		methods.index_build_range_scan = build_range;
		methods.index_validate_scan = validate_index;
		methods.relation_needs_toast_table = toast;
		methods.relation_estimate_size = estimate;
		methods.scan_bitmap_next_tuple = bitmap_next;
		methods.scan_sample_next_block = sample_block;
		methods.scan_sample_next_tuple = sample_tuple;
		initialized = true;
	}
	return &methods;
}
Datum
dolt_prolly_table_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(dg_table_routine());
}

void
dg_history_install(Relation rel, DGState *state, PLTree contents)
{
	/* Bound resources retain their original snapshot during root publication. */
	if (rel->rd_catalogview)
		PushCopiedSnapshot(GetActiveSnapshot());
	dg_handle_install(rel, state, contents);
	dg_save(rel, state, GetCurrentCommandId(true));
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	dg_rebuild_indexes(rel);
	CommandCounterIncrement();
	UpdateActiveSnapshotCommandId();
	if (rel->rd_catalogview)
		PopActiveSnapshot();
}

/* TRUNCATE of a branch's table: an empty root for the branch, and its indexes. */
void
dg_truncate(Relation rel)
{
	DGState    *state;
	PLTree		empty;

	writer(rel);
	state = dg_load(rel, SnapshotSelf);
	dg_check(pl_build(state->store, NULL, 0, &empty));
	dg_history_install(rel, state, empty);
	dg_release(state);
}
