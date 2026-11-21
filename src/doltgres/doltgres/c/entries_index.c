/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The doltgres crate's entries of the fork's btree index method (entries.rs'
 * doltgres_index_routine, doltgres_index_insert and its helpers, and the
 * index entries from doltgres_index_gettreeheight to
 * doltgres_index_buildempty), other than its build's, its deletes', and
 * index_holds, which entries_tail.c holds.
 */
#ifndef FRONTEND
#include "postgres.h"
#include "access/genam.h"
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/index.h"
#include "doltgres/access/key.h"
#include "doltgres/access/slot.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/entries/searches.h"
#include "doltgres/doltgres/entries_tail.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/pg/lib.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/entry.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/state.h"
#include "pg_calls.h"
#include "storage/lmgr.h"

/* BTREE_METAPAGES is the pages nbtree keeps besides its entries', its metapage, which item 5's derived blocks of a btree index count, as Doltgrust's do. */
#define DOLTGRES_ENTRIES_BTREE_METAPAGES 1

/* txn::state::held at Option<Base>, which entries.c defines and gettreeheight takes. */
#define TXN_STATE_TS OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseBase *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* txn::state::held at (u64, u64), which entries_tail.c defines and vacuumcleanup takes. */
#define TXN_STATE_TS TupleU64U64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result_0, uint64 *result_1, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* txn::state::held at (), which layouts.c defines and buildempty takes. */
#define TXN_STATE_TS Unit
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* Inserting's Table, which index.c defines and insertcleanup ends. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_PREFIX doltgres_index_Inserting
#define HOST_OWNED_RS DoltgresIndexInserting
#define HOST_OWNED_R DoltgresIndexInserting
#include "doltgres/host/owned_template.h"

/*
 * doltgres_entries_doltgres_index_routine gives an index the fork stores a
 * copy of its routine in its own memory context, with the members the fork
 * implements replaced as their entries land, at the index's relcache build in
 * InitIndexAmRoutine (the btree text's section 4). The copy's wrapper asserts
 * at compile time that Postgres' routine is its 26 callbacks after
 * amkeytype, so an upstream change to them stops the build. Its ampredlocks
 * is false until the fork's leaf page locks land (item 7), so Postgres locks a
 * serializable scan's whole index and checks it at every insert.
 */
bool
doltgres_entries_doltgres_index_routine(const PgCatalogCaps *caps, const PgRel *relation, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	bool		ported;

	if (!doltgres_index_ported(raise, pg_CatalogCaps_catalog(caps), relation, &ported, error))
		return false;
	return !ported || pg_calls_index_routine_copy(raise, relation, error);
}

/* doltgres_entries_newest is the lent call of the index entries that read a storage's newest base record. */
static bool
doltgres_entries_newest(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held, bool *some,
						TxnBaseBase *result, HostError *error)
{
	return txn_state_newest(io, held, some, result, error);
}

/*
 * doltgres_entries_doltgres_index_gettreeheight_frame_t is gettreeheight's
 * state across its pad: what it opened, the index's table, and the base
 * records read, each while live.
 */
typedef struct doltgres_entries_doltgres_index_gettreeheight_frame_t
{
	doltgres_entries_opened_t opened;
	PgOwnedRel	heap;
	bool		heap_live;
	TxnBaseBase recorded;
	bool		recorded_live;
	TxnBaseBase own;
	bool		own_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *index;
	int32	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_gettreeheight_frame_t;

/* doltgres_entries_doltgres_index_gettreeheight_pad frees what gettreeheight holds. */
static void
doltgres_entries_doltgres_index_gettreeheight_pad(void *frame_)
{
	doltgres_entries_doltgres_index_gettreeheight_frame_t *frame = frame_;

	if (frame->own_live)
		txn_base_Base_drop(&frame->own);
	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	doltgres_entries_opened_drop(&frame->opened);
	frame->own_live = frame->recorded_live = frame->heap_live = false;
}

/* doltgres_entries_doltgres_index_gettreeheight_newest reads the newest base record of a found storage into the frame's own, answering whether it has one. */
static bool
doltgres_entries_doltgres_index_gettreeheight_newest(doltgres_entries_doltgres_index_gettreeheight_frame_t *frame,
													 TxnStateFound found, bool *some)
{
	HostError	failed;

	if (!txn_state_held_OptionBase(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading,
								   found, doltgres_entries_newest, NULL, NULL, some, &frame->own, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->own_live = *some;
	return true;
}

/* doltgres_entries_doltgres_index_gettreeheight_body reads the height of the index's tree, or of its table's for a primary key's index. */
static void
doltgres_entries_doltgres_index_gettreeheight_body(void *frame_)
{
	doltgres_entries_doltgres_index_gettreeheight_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	TreesStoreReader store;
	TxnStateFound found;
	ChunksErrorError failed;
	uint8		height;
	bool		some;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_index_making(&frame->io, frame->index, &found, frame->error) ||
		!doltgres_entries_doltgres_index_gettreeheight_newest(frame, found, &some))
		return;
	if (!some || !frame->own.has_root)
	{
		bool		keyed;

		if (frame->own_live)
		{
			txn_base_Base_drop(&frame->own);
			frame->own_live = false;
		}
		if (!pg_OwnedRel_index_table(raise, pg_CatalogCaps_catalog(frame->caps), frame->index, &frame->heap,
									 frame->error))
			return;
		frame->heap_live = true;
		if (!doltgres_entries_recorded(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
									   &frame->opened.reading, pg_OwnedRel_rel(&frame->heap), &frame->recorded,
									   frame->error))
			return;
		frame->recorded_live = true;
		if (!doltgres_index_keyed(raise, pg_CatalogCaps_catalog(frame->caps), frame->index, &frame->recorded.key,
								  &keyed, frame->error))
			return;
		some = false;
		if (keyed &&
			(!doltgres_entries_making(frame->caps, &frame->io, pg_OwnedRel_rel(&frame->heap), &found, frame->error) ||
			 !doltgres_entries_doltgres_index_gettreeheight_newest(frame, found, &some)))
			return;
		if (!some || !frame->own.has_root)
		{
			*frame->result = 0;
			frame->ok = true;
			return;
		}
	}
	store = repo_Repo_reader(doltgres_entries_opened_repo(&frame->opened), HOST_CACHE_PLACEMENT_SERVER,
							 &frame->opened.reading);
	if (!trees_height(&frame->io, TREES_STORE_READER_NODE_STORE(&store), TREES_STORE_TREE_NAMED(&frame->own.root),
					  &height, &failed))
	{
		*frame->error = doltgres_entries_storage_error(&failed);
		return;
	}
	*frame->result = (int32) height;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_gettreeheight is the fork's btree
 * amgettreeheight (the btree's part 2 text, §3): the levels of the index's
 * base tree above its leaves, as nbtree answers its root's level from its
 * metapage; a primary key's index, which has no tree of its own
 * (index::keyed), answers its table's base tree's; and a base with no tree
 * answers 0.
 */
bool
doltgres_entries_doltgres_index_gettreeheight(const PgCatalogCaps *caps, const PgRel *index, int32 *result,
											  PgPgError *error)
{
	doltgres_entries_doltgres_index_gettreeheight_frame_t frame = {.caps = caps, .index = index, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_gettreeheight_body,
				doltgres_entries_doltgres_index_gettreeheight_pad);
	doltgres_entries_doltgres_index_gettreeheight_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_index_beginscan is the fork's btree ambeginscan (the btree's part 2, §3), as searches::begin begins it. */
bool
doltgres_entries_doltgres_index_beginscan(const PgCatalogCaps *caps, const PgRel *index, int32 nkeys, int32 norderbys,
										  PgIndexScanDesc *result, PgPgError *error)
{
	return doltgres_entries_searches_begin(caps, index, nkeys, norderbys, result, error);
}

/* doltgres_entries_doltgres_index_rescan is the fork's btree amrescan, as searches::rescan starts the scan over; a btree takes no ordering keys. */
bool
doltgres_entries_doltgres_index_rescan(const PgCaps *caps, PgIndexScan *scan, const PgScanKeys *keys, int32 nkeys,
									   const PgScanKeys *orderbys, int32 norderbys, PgPgError *error)
{
	return doltgres_entries_searches_rescan(caps, scan, keys, error);
}

/* doltgres_entries_doltgres_index_gettuple is the fork's btree amgettuple, as searches::next reads the scan's next entry. */
bool
doltgres_entries_doltgres_index_gettuple(const PgCatalogCaps *caps, PgIndexScan *scan, int32 direction, bool *result,
										 PgPgError *error)
{
	return doltgres_entries_searches_next(caps, scan, direction, result, error);
}

/* doltgres_entries_doltgres_index_getbitmap is the fork's btree amgetbitmap, as searches::bitmap adds the scan's rows. */
bool
doltgres_entries_doltgres_index_getbitmap(const PgCatalogCaps *caps, PgIndexScan *scan, PgBitmap *tbm, int64 *result,
										  PgPgError *error)
{
	return doltgres_entries_searches_bitmap(caps, scan, tbm, result, error);
}

/* doltgres_entries_doltgres_index_markpos is the fork's btree ammarkpos, as searches::mark keeps the scan's place. */
bool
doltgres_entries_doltgres_index_markpos(const PgCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	return doltgres_entries_searches_mark(caps, scan, error);
}

/* doltgres_entries_doltgres_index_restrpos is the fork's btree amrestrpos, as searches::restore puts the scan back at its mark. */
bool
doltgres_entries_doltgres_index_restrpos(const PgCatalogCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	return doltgres_entries_searches_restore(caps, scan, error);
}

/*
 * doltgres_entries_doltgres_index_estimateparallelscan is the fork's btree
 * amestimateparallelscan (the btree's part 2, §4): the size of the fork's
 * part of a parallel scan's shared memory, its units' split keys.
 */
bool
doltgres_entries_doltgres_index_estimateparallelscan(const PgCaps *caps, const PgRel *index, int32 nkeys,
													 int32 norderbys, size_t *result, PgPgError *error)
{
	*result = pg_calls_index_parallel_size();
	return true;
}

/*
 * doltgres_entries_doltgres_index_initparallelscan is the fork's btree
 * aminitparallelscan: it initializes the fork's part of a parallel scan's
 * shared memory with no units taken and no split keys stored.
 */
bool
doltgres_entries_doltgres_index_initparallelscan(const PgCaps *caps, PgParallelTarget *target, PgPgError *error)
{
	pg_calls_index_parallel_init(target);
	return true;
}

/*
 * doltgres_entries_doltgres_index_parallelrescan is the fork's btree
 * amparallelrescan: it starts the shared units over, their split keys stored
 * again by the first participant to need them.
 */
bool
doltgres_entries_doltgres_index_parallelrescan(const PgCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	pg_calls_index_parallel_restart(scan);
	return true;
}

/*
 * doltgres_entries_doltgres_index_only_tid is execCurrent's call site for an
 * index-only scan (the hook ledger's row): it overwrites the tid with that of
 * the version of the scan's current row its snapshot sees for an index the
 * fork scans (searches::current), and leaves the tid the scan's last entry
 * named for any other.
 */
bool
doltgres_entries_doltgres_index_only_tid(const PgCatalogCaps *caps, PgIndexScan *scan, PgTid *tid, PgPgError *error)
{
	return !pg_calls_index_scan_forks(scan) || doltgres_entries_searches_current(caps, scan, tid, error);
}

/* doltgres_entries_doltgres_index_endscan is the fork's btree amendscan, as searches::end ends the scan. */
bool
doltgres_entries_doltgres_index_endscan(const PgCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	return doltgres_entries_searches_end(caps, scan, error);
}

/*
 * doltgres_entries_doltgres_index_costestimate_frame_t is costestimate's state
 * across its pad: the index and its table, what it opened, and the table's
 * base record, each while live.
 */
typedef struct doltgres_entries_doltgres_index_costestimate_frame_t
{
	PgOwnedRel	index;
	bool		index_live;
	PgOwnedRel	heap;
	bool		heap_live;
	doltgres_entries_opened_t opened;
	TxnBaseBase recorded;
	bool		recorded_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	PgPlanner  *root;
	PgIndexPath *path;
	float8		loop_count;
	float8	   *startup;
	float8	   *total;
	float8	   *selectivity;
	float8	   *correlation;
	float8	   *pages;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_costestimate_frame_t;

/* doltgres_entries_doltgres_index_costestimate_pad frees what costestimate holds, in the reverse of its making. */
static void
doltgres_entries_doltgres_index_costestimate_pad(void *frame_)
{
	doltgres_entries_doltgres_index_costestimate_frame_t *frame = frame_;

	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	doltgres_entries_opened_drop(&frame->opened);
	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	if (frame->index_live)
		pg_OwnedRel_drop(&frame->index);
	frame->recorded_live = frame->heap_live = frame->index_live = false;
}

/* doltgres_entries_doltgres_index_costestimate_body estimates the scan's costs as nbtree does, a primary key's index's correlation 1. */
static void
doltgres_entries_doltgres_index_costestimate_body(void *frame_)
{
	doltgres_entries_doltgres_index_costestimate_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	bool		keyed;

	if (!pg_calls_index_costs(raise, catalog, frame->root, frame->path, frame->loop_count, frame->startup,
							  frame->total, frame->selectivity, frame->correlation, frame->pages, frame->error) ||
		!pg_OwnedRel_path_index(raise, catalog, frame->path, &frame->index, frame->error))
		return;
	frame->index_live = true;
	if (!pg_OwnedRel_index_table(raise, catalog, pg_OwnedRel_rel(&frame->index), &frame->heap, frame->error))
		return;
	frame->heap_live = true;
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_recorded(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
								   &frame->opened.reading, pg_OwnedRel_rel(&frame->heap), &frame->recorded,
								   frame->error))
		return;
	frame->recorded_live = true;
	if (!doltgres_index_keyed(raise, catalog, pg_OwnedRel_rel(&frame->index), &frame->recorded.key, &keyed,
							  frame->error))
		return;
	if (keyed)
		*frame->correlation = 1.0;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_costestimate is the fork's btree
 * amcostestimate (the btree's part 2, §3, PLAN item 16): nbtree's estimate,
 * whose descent costs a comparison a level, since the node cache holds the
 * tree's internal bytes, over the index's pages from item 5's derived sizes
 * and its height from the fork's amgettreeheight, both of which the planner
 * reads through the fork's call sites, with a correlation of 1 for a primary
 * key's index, whose entries are its table's rows in their tree's order; its
 * constants are the performance stage's to measure.
 */
bool
doltgres_entries_doltgres_index_costestimate(const PgCatalogCaps *caps, PgPlanner *root, PgIndexPath *path,
											 float8 loop_count, float8 *startup, float8 *total, float8 *selectivity,
											 float8 *correlation, float8 *pages, PgPgError *error)
{
	doltgres_entries_doltgres_index_costestimate_frame_t frame = {.caps = caps, .root = root, .path = path,
		.loop_count = loop_count, .startup = startup, .total = total, .selectivity = selectivity,
	.correlation = correlation, .pages = pages, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_costestimate_body,
				doltgres_entries_doltgres_index_costestimate_pad);
	doltgres_entries_doltgres_index_costestimate_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_index_bulkdelete is the fork's btree
 * ambulkdelete (the btree text's section 6): no entry of the fork's names a
 * dead row, since every way a row dies removes its entries (S1), so it
 * removes nothing and asks VACUUM's callback about no tid, answering the
 * statistics it was handed, made when it was handed none. It passes the pause
 * doltgres-index-bulkdelete first, where a case shows that VACUUM never calls
 * it (the unknown methods' text, section 4).
 */
bool
doltgres_entries_doltgres_index_bulkdelete(const PgCaps *caps, PgVacuumInfo *info, PgIndexStatsOption *stats,
										   PgBulkCallback callback, void *callback_state, PgIndexStatsOption *result,
										   PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io = pghost_io(raise);
	HostError	failed;

	if (!host_pause(HOST_SHARED_IO_PAUSES(&io), "doltgres-index-bulkdelete", &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	return pg_calls_stats_made(raise, stats, result, error);
}

/* doltgres_entries_doltgres_index_vacuumcleanup_closure_0 is vacuumcleanup's lent call: the base's count of rows and its tree's nodes, none for no base. */
static bool
doltgres_entries_doltgres_index_vacuumcleanup_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														const HostSharedHeld *held, uint64 *result_0,
														uint64 *result_1, HostError *error)
{
	const HostSharedReader *reading = captures;
	TxnBaseBase base;
	bool		some;

	if (!txn_state_newest(io, held, &some, &base, error))
		return false;
	*result_0 = *result_1 = 0;
	if (!some)
		return true;
	if (base.has_root)
	{
		TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, reading);
		std_TupleU64U64_t counts;
		ChunksErrorError failed;

		if (!trees_node_counts(io, TREES_STORE_READER_NODE_STORE(&store), TREES_STORE_TREE_NAMED(&base.root), &counts,
							   &failed))
		{
			*error = txn_base_storage(&failed);
			txn_base_Base_drop(&base);
			return false;
		}
		*result_1 = counts.f0 + counts.f1;
	}
	*result_0 = base.sizes.rows;
	txn_base_Base_drop(&base);
	return true;
}

/*
 * doltgres_entries_doltgres_index_vacuumcleanup_frame_t is vacuumcleanup's
 * state across its pad: what it opened and the table's base record, each
 * while live.
 */
typedef struct doltgres_entries_doltgres_index_vacuumcleanup_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseBase recorded;
	bool		recorded_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	PgVacuumInfo *info;
	PgIndexStatsOption *stats;
	PgIndexStatsOption *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_vacuumcleanup_frame_t;

/* doltgres_entries_doltgres_index_vacuumcleanup_pad frees what vacuumcleanup holds. */
static void
doltgres_entries_doltgres_index_vacuumcleanup_pad(void *frame_)
{
	doltgres_entries_doltgres_index_vacuumcleanup_frame_t *frame = frame_;

	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	doltgres_entries_opened_drop(&frame->opened);
	frame->recorded_live = false;
}

/* doltgres_entries_doltgres_index_vacuumcleanup_body counts the index's rows and pages from its tree, or its table's for a primary key's index. */
static void
doltgres_entries_doltgres_index_vacuumcleanup_body(void *frame_)
{
	doltgres_entries_doltgres_index_vacuumcleanup_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgRel *index = pg_calls_vacuum_index(frame->info);
	const PgRel *heap = pg_calls_vacuum_heap(frame->info);
	PgIndexStatsOption made;
	TxnStateFound found;
	HostError	failed;
	uint64		rows;
	uint64		nodes;
	bool		keyed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_recorded(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
								   &frame->opened.reading, heap, &frame->recorded, frame->error))
		return;
	frame->recorded_live = true;
	if (!doltgres_index_keyed(raise, pg_CatalogCaps_catalog(frame->caps), index, &frame->recorded.key, &keyed,
							  frame->error))
		return;
	txn_base_Base_drop(&frame->recorded);
	frame->recorded_live = false;
	if (keyed ? !doltgres_entries_making(frame->caps, &frame->io, heap, &found, frame->error) :
		!doltgres_entries_index_making(&frame->io, index, &found, frame->error))
		return;
	if (!txn_state_held_TupleU64U64(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading,
									found, doltgres_entries_doltgres_index_vacuumcleanup_closure_0,
									&frame->opened.reading, NULL, &rows, &nodes, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!pg_calls_stats_made(raise, frame->stats, &made, frame->error))
		return;
	*frame->result = pg_calls_stats_counted(&made, (float8) rows,
											(uint32) Min(DOLTGRES_ENTRIES_BTREE_METAPAGES + nodes, PG_UINT32_MAX));
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_vacuumcleanup is the fork's btree
 * amvacuumcleanup (the btree text's section 6, PLAN item 4): the index's
 * counts from its tree, its rows as its base's sizes count them, and its
 * pages its tree's nodes and nbtree's metapage, as item 5 derives an index's
 * blocks, the tree of an index with none of its own (index::keyed) its
 * table's; an ANALYZE's cleanup answers what it was handed, as
 * btvacuumcleanup does.
 */
bool
doltgres_entries_doltgres_index_vacuumcleanup(const PgCatalogCaps *caps, PgVacuumInfo *info,
											  PgIndexStatsOption *stats, PgIndexStatsOption *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_vacuumcleanup_frame_t frame = {.caps = caps, .info = info, .stats = stats,
	.result = result, .error = error};

	if (pg_calls_vacuum_analyze_only(info))
	{
		*result = *stats;
		return true;
	}
	std_landing(&frame, doltgres_entries_doltgres_index_vacuumcleanup_body,
				doltgres_entries_doltgres_index_vacuumcleanup_pad);
	doltgres_entries_doltgres_index_vacuumcleanup_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_index_insertcleanup is the fork's btree
 * aminsertcleanup (the btree text's section 4): it ends the index's
 * description the insert's state names, which a statement's end reaches
 * here, and which an error's leaves to the resource owner the description
 * was made under.
 */
bool
doltgres_entries_doltgres_index_insertcleanup(const PgCatalogCaps *caps, const PgRel *index, PgIndexInfo *info,
											  PgPgError *error)
{
	DgOwned		owned = pg_calls_insert_record(info);
	HostOwnedHandle handle = {.slot = owned.slot, .generation = owned.generation};
	HostIoIo	io;
	HostError	failed;

	if (owned.generation == 0)
		return true;
	io = pghost_io_catalog(caps);
	if (!host_owned_TableDoltgresIndexInserting_end(&DOLTGRES_INDEX_INSERTING, &io, handle, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_doltgres_index_buildempty_closure_0 is buildempty's lent call, which writes nothing: the state's make is the work. */
static bool
doltgres_entries_doltgres_index_buildempty_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
													 const HostSharedHeld *held, HostError *error)
{
	return true;
}

/* doltgres_entries_doltgres_index_buildempty_frame_t is buildempty's state across its pad: what it opened. */
typedef struct doltgres_entries_doltgres_index_buildempty_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *index;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_buildempty_frame_t;

/* doltgres_entries_doltgres_index_buildempty_pad frees what buildempty holds. */
static void
doltgres_entries_doltgres_index_buildempty_pad(void *frame_)
{
	doltgres_entries_doltgres_index_buildempty_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_buildempty_body makes the index's storage's state under the backend's top-level transaction. */
static void
doltgres_entries_doltgres_index_buildempty_body(void *frame_)
{
	doltgres_entries_doltgres_index_buildempty_frame_t *frame = frame_;
	TxnStateFound found;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_index_making(&frame->io, frame->index, &found, frame->error))
		return;
	if (!txn_state_held_Unit(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading, found,
							 doltgres_entries_doltgres_index_buildempty_closure_0, NULL, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_buildempty is the fork's btree
 * ambuildempty (the btree text's section 5): an unlogged index's storage is
 * made with no entries, its state's first base record naming its table and
 * itself, as index_build calls it once Postgres made the index's init fork,
 * whose name the unlogged reset finds the storage by.
 */
bool
doltgres_entries_doltgres_index_buildempty(const PgCatalogCaps *caps, const PgRel *index, PgPgError *error)
{
	doltgres_entries_doltgres_index_buildempty_frame_t frame = {.caps = caps, .index = index, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_buildempty_body, doltgres_entries_doltgres_index_buildempty_pad);
	doltgres_entries_doltgres_index_buildempty_pad(&frame);
	return frame.ok;
}

/* txn::state::held at Option<(Vec<u8>, bool)>, which entries_tail.c defines and index_insert takes for its row's key. */
#define TXN_STATE_TS OptionTupleVecU8Bool
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, StdVec *result_0, bool *result_1, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* txn::state::held at entry's Changed, which entry_unique takes. */
#define TXN_STATE_TS Changed
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , TxnEntryChanged *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS TxnEntryChanged *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* txn::state::held at Option<Row>, which entries.c defines and others takes. */
#define TXN_STATE_TS OptionRow
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, DoltgresScanRow *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* doltgres_entries_speculated_entry_captures_t is speculated_entry's captures: the insert's table and row, and the index's entry. */
typedef struct doltgres_entries_speculated_entry_captures_t
{
	uint32		relation;
	uint64		tuple;
	uint32		index;
	const uint8 *key;
	size_t		key_len;
} doltgres_entries_speculated_entry_captures_t;

/* doltgres_entries_speculated_entry_op records the entry in the speculative insert of the row, when the backend has one. */
static void
doltgres_entries_speculated_entry_op(void *value, void *captures)
{
	StdVec	   *all = value;
	const doltgres_entries_speculated_entry_captures_t *c = captures;

	for (size_t k = 0; k < all->len; k++)
	{
		DoltgresEntriesSpeculated *speculated = &STD_VEC_AT(all, DoltgresEntriesSpeculated, k);
		doltgres_TupleU32VecU8_t entry = {.f0 = c->index};

		if (speculated->relation != c->relation || speculated->tuple != c->tuple)
			continue;
		std_slice_to_vec_Checked(&entry.f1, c->key, c->key_len, 1, 1);
		std_vec_push_Checked(&speculated->entries, &entry, sizeof(entry), _Alignof(doltgres_TupleU32VecU8_t));
		return;
	}
}

/*
 * doltgres_entries_speculated_entry records an entry an index's aminsert
 * writes for a row the backend's speculative insert made, by the index and
 * the entry's key, which a kill of the insert removes; it copies the key,
 * since the entry's push moves it.
 */
static void
doltgres_entries_speculated_entry(HostIoIo *io, const PgRel *index, uint64 tuple, const uint8 *key, size_t key_len)
{
	doltgres_entries_speculated_entry_captures_t c = {.relation = pg_calls_index_table(index), .tuple = tuple,
	.index = pg_calls_rel_id(index), .key = key, .key_len = key_len};

	doltgres_entries_locked(io, &DOLTGRES_ENTRIES_SPECULATED.local, &DOLTGRES_ENTRIES_SPECULATED_TYPE,
							&DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE, doltgres_entries_speculated_entry_op, &c);
}

/* doltgres_entries_formed_t is an index entry as entry_changed and entry_unique form it: its key, its value and blobs' addresses, and its row's record. */
typedef struct doltgres_entries_formed_t
{
	/*
	 * read is the executor's values as the description reads them, a
	 * Vec<Option<Vec<u8>>>.
	 */
	StdVec		read;
	bool		read_live;
	DoltgresIndexFormed formed;
	bool		key_live;
	bool		placing_live;
	AccessIndexEntry entry;
	bool		entry_live;
} doltgres_entries_formed_t;

/* doltgres_entries_formed_drop frees what a forming holds. */
static void
doltgres_entries_formed_drop(doltgres_entries_formed_t *self)
{
	if (self->entry_live)
		access_index_Entry_drop(&self->entry);
	if (self->placing_live)
		access_heaptuple_Placing_drop(&self->formed.placing);
	if (self->key_live)
		access_index_EntryKey_drop(&self->formed.key);
	if (self->read_live)
		doltgres_index_values_drop(&self->read);
	self->entry_live = self->placing_live = self->key_live = self->read_live = false;
}

/*
 * doltgres_entries_forming forms a row's entry of an index by its
 * description from the executor's values under the row's key and writes its
 * value's blobs (placed), as entry_changed and entry_unique begin, leaving
 * the entry's value and addresses with its key in the forming.
 */
static bool
doltgres_entries_forming(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						 const PgRel *index, const DoltgresIndexDescribed *described, const PgDatums *values,
						 const PgIsNull *isnull, const uint8 *row_key, size_t row_key_len,
						 doltgres_entries_formed_t *self, PgPgError *error)
{
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, (void *) caps};
	AccessHeaptupleFormed formed;
	StdVec		placed;

	if (!doltgres_index_Described_values(described, caps, values, isnull, &self->read, error))
		return false;
	self->read_live = true;
	if (!doltgres_index_Described_formed(described, sorter, self->read.ptr, self->read.len, row_key, row_key_len,
										 &self->formed, error))
		return false;
	self->key_live = self->placing_live = true;
	if (!doltgres_entries_placed(io, repo, reading, index, &self->formed.placing, &placed, error))
		return false;
	std_vec_free_Checked(&placed, sizeof(doltgres_TupleUsizeAddressU64_t), _Alignof(doltgres_TupleUsizeAddressU64_t));
	self->placing_live = false;
	if (!access_heaptuple_Placing_formed(&self->formed.placing, &formed, error))
		return false;
	self->entry = access_index_IndexKey_entry(&formed);
	std_vec_free_Checked(&self->entry.key, 1, 1);
	std_vec_new(&self->entry.key, 1, 1);
	self->entry_live = true;
	return true;
}

/* doltgres_entries_entry_op returns an index entry's op: a put naming no tuple id, or for a keyless table's row an added copy naming the row's, of a value and its blobs' addresses, which it takes. */
static TxnMemtableOp
doltgres_entries_entry_op(const DoltgresIndexDescribed *described, uint64 tuple, StdVec value, StdVec addresses)
{
	bool		keyless = doltgres_index_Described_keyless(described);
	TxnMemtableOp op = {.tag = keyless ? TXN_MEMTABLE_OP_ADD : TXN_MEMTABLE_OP_PUT};
	TxnMemtablePut *put = keyless ? &op.u.add : &op.u.put;

	put->value = value;
	put->addresses = addresses;
	std_vec_new(&put->values, sizeof(uint32), _Alignof(uint32));
	put->tuple.f0 = keyless ? tuple : 0;
	return op;
}

/* doltgres_entries_entry_record_frame_t is entry_record's state across its pad: the record's identity, while live. */
typedef struct doltgres_entries_entry_record_frame_t
{
	StdVec		identity;
	bool		identity_live;
	const DoltgresIndexDescribed *described;
	StdBytes	row_key;
	uint64		tuple;
	const StdVec *key;
	txn_TupleVecU8Op_t *result;
} doltgres_entries_entry_record_frame_t;

/* doltgres_entries_entry_record_pad frees what entry_record holds. */
static void
doltgres_entries_entry_record_pad(void *frame_)
{
	doltgres_entries_entry_record_frame_t *frame = frame_;

	if (frame->identity_live)
		std_vec_free_Checked(&frame->identity, 1, 1);
	frame->identity_live = false;
}

/* doltgres_entries_entry_record_body makes the record's identity, then its change of a copy of the key. */
static void
doltgres_entries_entry_record_body(void *frame_)
{
	doltgres_entries_entry_record_frame_t *frame = frame_;
	StdVec		copy;

	doltgres_entries_record_identity(frame->described, frame->row_key.ptr, frame->row_key.len, frame->tuple,
									 &frame->identity);
	frame->identity_live = true;
	std_vec_clone(&copy, frame->key, 1, 1);
	doltgres_entries_record_change(frame->described, frame->identity.ptr, frame->identity.len, frame->tuple, &copy,
								   frame->result);
}

/* doltgres_entries_entry_record returns the row's record of an index entry's key for an index that keeps records, as entry_changed and entry_unique push it, copying the key. */
static void
doltgres_entries_entry_record(const DoltgresIndexDescribed *described, const uint8 *row_key, size_t row_key_len,
							  uint64 tuple, const StdVec *key, txn_TupleVecU8Op_t *result)
{
	doltgres_entries_entry_record_frame_t frame = {.described = described, .row_key = {row_key, row_key_len},
	.tuple = tuple, .key = key, .result = result};

	std_landing(&frame, doltgres_entries_entry_record_body, doltgres_entries_entry_record_pad);
	doltgres_entries_entry_record_pad(&frame);
}

/*
 * doltgres_entries_entry_changed_frame_t is entry_changed's state across its
 * pad: the entry formed, its op, and its record, each while live.
 */
typedef struct doltgres_entries_entry_changed_frame_t
{
	doltgres_entries_formed_t forming;
	TxnMemtableOp op;
	bool		op_live;
	txn_TupleVecU8Op_t record;
	bool		record_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	const DoltgresIndexDescribed *described;
	const PgDatums *values;
	PgIsNull   *isnull;
	StdBytes	row_key;
	uint64		tuple;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entry_changed_frame_t;

/* doltgres_entries_entry_changed_pad frees what entry_changed holds. */
static void
doltgres_entries_entry_changed_pad(void *frame_)
{
	doltgres_entries_entry_changed_frame_t *frame = frame_;

	if (frame->record_live)
		doltgres_entries_Record_drop(&frame->record);
	if (frame->op_live)
		txn_memtable_Op_drop(&frame->op);
	doltgres_entries_formed_drop(&frame->forming);
	frame->record_live = frame->op_live = false;
}

/* doltgres_entries_entry_changed_body forms the entry and pushes its change, with its record's. */
static void
doltgres_entries_entry_changed_body(void *frame_)
{
	doltgres_entries_entry_changed_frame_t *frame = frame_;
	doltgres_entries_formed_t *forming = &frame->forming;
	StdVec		key;
	bool		has_record;

	if (!doltgres_entries_forming(frame->caps, frame->io, frame->repo, frame->reading, frame->index, frame->described,
								  frame->values, frame->isnull, frame->row_key.ptr, frame->row_key.len, forming,
								  frame->error))
		return;
	frame->op = doltgres_entries_entry_op(frame->described, frame->tuple, forming->entry.value,
										  forming->entry.addresses);
	forming->entry_live = false;
	std_vec_free_Checked(&forming->entry.key, 1, 1);
	frame->op_live = true;
	if (doltgres_index_Described_records(frame->described))
	{
		doltgres_entries_entry_record(frame->described, frame->row_key.ptr, frame->row_key.len, frame->tuple,
									  &forming->formed.key.key, &frame->record);
		frame->record_live = true;
	}
	doltgres_entries_speculated_entry(frame->io, frame->index, frame->tuple, forming->formed.key.key.ptr,
									  forming->formed.key.key.len);
	key = forming->formed.key.key;
	std_vec_new(&forming->formed.key.key, 1, 1);
	has_record = frame->record_live;
	frame->op_live = frame->record_live = false;
	frame->ok = doltgres_entries_entry_pushed(frame->io, frame->repo, frame->reading, frame->index, &key, &frame->op,
											  has_record ? &frame->record : NULL, frame->writer, frame->roomed,
											  frame->error);
}

/*
 * doltgres_entries_entry_changed forms a row's entry of an index by its
 * description from the executor's values under the row's key, writes its
 * value's blobs (placed), and changes it in the index's storage under the
 * state the backend's top-level transaction holds: a put naming no tuple id,
 * or for a keyless table's row an added copy naming the row's, with the row's
 * record of it for an index that keeps records, whose value copies the
 * entry's key, since the entry's change moves the key.
 */
static bool
doltgres_entries_entry_changed(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							   const HostSharedReader *reading, const PgRel *index,
							   const DoltgresIndexDescribed *described, const PgDatums *values, PgIsNull *isnull,
							   const uint8 *row_key, size_t row_key_len, uint64 tuple, TxnMemtableVersionWriter writer,
							   TxnStatusRoomed roomed, PgPgError *error)
{
	doltgres_entries_entry_changed_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading,
		.index = index, .described = described, .values = values, .isnull = isnull, .row_key = {row_key, row_key_len},
	.tuple = tuple, .writer = writer, .roomed = roomed, .error = error};

	std_landing(&frame, doltgres_entries_entry_changed_body, doltgres_entries_entry_changed_pad);
	doltgres_entries_entry_changed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_entry_unique_captures_t is entry_unique's lent call's
 * captures: the reader, the entry's op and key and its record, each while
 * live, until the resolve takes them, the uniqueness, and the inserted row's
 * tuple id and whether its check is partial.
 */
typedef struct doltgres_entries_entry_unique_captures_t
{
	const HostSharedReader *reading;
	TxnEntryEntry entry;
	bool		op_live;
	bool		key_live;
	bool		has_record;
	txn_TupleVecU8Op_t record;
	bool		record_live;
	const TxnInsertUniqueness *uniqueness;
	uint64		tuple;
	bool		partial;
} doltgres_entries_entry_unique_captures_t;

/* doltgres_entries_entry_unique_closure_drop drops what of the lent call's entry and record its resolve didn't take. */
static void
doltgres_entries_entry_unique_closure_drop(void *captures)
{
	doltgres_entries_entry_unique_captures_t *c = captures;

	if (c->record_live)
		doltgres_entries_Record_drop(&c->record);
	if (c->key_live)
		std_vec_free_Checked(&c->entry.key, 1, 1);
	if (c->op_live)
		txn_memtable_Op_drop(&c->entry.op);
	c->record_live = c->key_live = c->op_live = false;
}

/* doltgres_entries_entry_unique_closure_1 is entry_unique's lent call: the entry put when its key is free, under the index storage's stripe. */
static bool
doltgres_entries_entry_unique_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
										TxnEntryChanged *result, HostError *error)
{
	doltgres_entries_entry_unique_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	HostIoWaits waits = pghost_waits();
	HostRow		row = {c->tuple};

	c->record_live = c->key_live = c->op_live = false;
	return txn_entry_unique(io, TREES_STORE_READER_NODE_STORE(&store), held, &c->entry, c->has_record, &c->record,
							c->uniqueness, &waits, row, c->partial, result, error);
}

/*
 * doltgres_entries_entry_unique_frame_t is entry_unique's state across its
 * pad: the entry formed, its key's order, the existing entry found, and the
 * rows its value names, each while live.
 */
typedef struct doltgres_entries_entry_unique_frame_t
{
	doltgres_entries_formed_t forming;
	TreesKeyOrder order;
	bool		order_live;
	StdVec		existing;
	bool		existing_live;
	StdVec		rows;
	bool		rows_live;
	/* value is a resolve's copy of the entry's value, until its op takes it. */
	StdVec		value;
	bool		value_live;
	/* resolving is a resolve's captures, until held takes them. */
	doltgres_entries_entry_unique_captures_t resolving;
	bool		resolving_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	const DoltgresIndexDescribed *described;
	const PgDatums *values;
	PgIsNull   *isnull;
	const PgRel *heap;
	StdBytes	row_key;
	uint64		tuple;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	bool		partial;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entry_unique_frame_t;

/* doltgres_entries_entry_unique_pad frees what entry_unique holds. */
static void
doltgres_entries_entry_unique_pad(void *frame_)
{
	doltgres_entries_entry_unique_frame_t *frame = frame_;

	if (frame->resolving_live)
		doltgres_entries_entry_unique_closure_drop(&frame->resolving);
	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	frame->resolving_live = frame->value_live = false;
	if (frame->rows_live)
		std_vec_drop_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	if (frame->existing_live)
		std_vec_free_Checked(&frame->existing, 1, 1);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	doltgres_entries_formed_drop(&frame->forming);
	frame->rows_live = frame->existing_live = frame->order_live = false;
}

/*
 * doltgres_entries_entry_unique_resolved resolves the entry's key once,
 * putting the entry when the key is free, and answers what the resolve did,
 * any existing entry kept in the frame. The resolve's captures are the
 * frame's until held takes them.
 */
static bool
doltgres_entries_entry_unique_resolved(doltgres_entries_entry_unique_frame_t *frame,
									   const TxnInsertUniqueness *uniqueness, TxnEntryChanged *changed)
{
	doltgres_entries_formed_t *forming = &frame->forming;
	doltgres_entries_entry_unique_captures_t *c = &frame->resolving;
	StdVec		addresses;
	TxnStateFound made;
	HostError	failed;

	*c = (doltgres_entries_entry_unique_captures_t)
	{
		.reading = frame->reading, .uniqueness = uniqueness, .tuple = frame->tuple, .partial = frame->partial
	};
	frame->resolving_live = true;
	std_vec_clone(&frame->value, &forming->entry.value, 1, 1);
	frame->value_live = true;
	std_vec_clone(&addresses, &forming->entry.addresses, sizeof(chunks_TupleU32Locator_t),
				  _Alignof(chunks_TupleU32Locator_t));
	c->entry = (TxnEntryEntry)
	{
		.writer = frame->writer, .roomed = frame->roomed,
			.storage = doltgres_entries_storage(frame->index), .table = pg_calls_index_table(frame->index),
			.index = pg_calls_rel_id(frame->index),
			.op = doltgres_entries_entry_op(frame->described, frame->tuple, frame->value, addresses),
			.logged = pg_calls_rel_permanent(frame->index)
	};
	c->op_live = true;
	frame->value_live = false;
	std_vec_clone(&c->entry.key, &forming->formed.key.key, 1, 1);
	c->key_live = true;
	c->has_record = doltgres_index_Described_records(frame->described);
	if (c->has_record)
	{
		doltgres_entries_entry_record(frame->described, frame->row_key.ptr, frame->row_key.len, frame->tuple,
									  &forming->formed.key.key, &c->record);
		c->record_live = true;
	}
	if (!doltgres_entries_index_making(frame->io, frame->index, &made, frame->error))
		return false;
	frame->resolving_live = false;
	if (!txn_state_held_Changed(frame->io, frame->repo, frame->reading, made, doltgres_entries_entry_unique_closure_1,
								c, doltgres_entries_entry_unique_closure_drop, changed, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/*
 * doltgres_entries_entry_unique_judged judges the versions of the row an
 * existing entry names, read by its row key under SnapshotAny, by their
 * writers' and enders' outcomes: whether one is live, and whether another
 * transaction runs as a writer or ender of one, with its xid.
 */
static bool
doltgres_entries_entry_unique_judged(doltgres_entries_entry_unique_frame_t *frame, StdBytes other, bool *live,
									 bool *has_running, uint64 *running)
{
	HostSnapshot any = pghost_any();
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY, .key = other.ptr, .key_len = other.len};
	HostError	failed;

	*live = false;
	*has_running = false;
	if (!doltgres_entries_rows(frame->caps, frame->heap, NULL, &any, by, &frame->rows, frame->error))
		return false;
	frame->rows_live = true;
	for (size_t k = 0; k < frame->rows.len; k++)
	{
		const DoltgresScanRow *row = &STD_VEC_AT(&frame->rows, DoltgresScanRow, k);
		HostXid		writer = {row->stamp.txn};
		HostStatus	status;

		if (!txn_outcome_stamped(frame->io, row->stamp.txn, &status, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		if (status == HOST_STATUS_ABORTED)
			continue;
		if (status == HOST_STATUS_IN_PROGRESS && !host_is_current(HOST_IO_IO_IO_DOORS(frame->io), writer))
		{
			*has_running = true;
			*running = row->stamp.txn;
			continue;
		}
		if (!row->has_changer || row->changer.locks)
		{
			*live = true;
			continue;
		}
		if (!txn_outcome_stamped(frame->io, row->changer.xid, &status, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		if (status == HOST_STATUS_ABORTED)
			*live = true;
		else if (status == HOST_STATUS_IN_PROGRESS)
		{
			HostXid		ender = {row->changer.xid};

			if (!host_is_current(HOST_IO_IO_IO_DOORS(frame->io), ender))
			{
				*has_running = true;
				*running = row->changer.xid;
			}
		}
	}
	std_vec_drop_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	frame->rows_live = false;
	return true;
}

/* doltgres_entries_entry_unique_body resolves the entry's key until it stands, waits, conflicts, or refuses. */
static void
doltgres_entries_entry_unique_body(void *frame_)
{
	doltgres_entries_entry_unique_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	doltgres_entries_formed_t *forming = &frame->forming;
	bool		keyless = doltgres_index_Described_keyless(frame->described);
	TxnInsertUniqueness uniqueness = {.index = pg_calls_rel_id(frame->index)};
	HostIoWaits waits = pghost_waits();

	if (!doltgres_entries_forming(frame->caps, frame->io, frame->repo, frame->reading, frame->index, frame->described,
								  frame->values, frame->isnull, frame->row_key.ptr, frame->row_key.len, forming,
								  frame->error))
		return;
	doltgres_index_Described_order(frame->described, NULL, &frame->order);
	frame->order_live = true;
	uniqueness.order = &frame->order;
	uniqueness.has_equal = doltgres_layout_equal(&frame->order, &uniqueness.equal);
	for (;;)
	{
		TxnEntryChanged changed;
		StdBytes	other;
		HostError	failed;
		bool		live;
		bool		has_running;
		uint64		running;

		if (!doltgres_entries_entry_unique_resolved(frame, &uniqueness, &changed))
			return;
		if (changed.tag == TXN_ENTRY_CHANGED_DONE)
		{
			doltgres_entries_speculated_entry(frame->io, frame->index, frame->tuple, forming->formed.key.key.ptr,
											  forming->formed.key.key.len);
			*frame->result = true;
			frame->ok = true;
			return;
		}
		if (changed.tag == TXN_ENTRY_CHANGED_RUNNING)
		{
			*frame->result = false;
			frame->ok = true;
			return;
		}
		frame->existing = changed.u.exists;
		frame->existing_live = true;
		if (!access_index_IndexKey_row_key(frame->existing.ptr, frame->existing.len, &other, frame->error))
			return;
		if (!keyless && other.len == frame->row_key.len && memcmp(other.ptr, frame->row_key.ptr, other.len) == 0)
		{
			*frame->result = true;
			frame->ok = true;
			return;
		}
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-unique-reading", &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (!doltgres_entries_entry_unique_judged(frame, other, &live, &has_running, &running))
			return;
		std_vec_free_Checked(&frame->existing, 1, 1);
		frame->existing_live = false;
		if (!has_running)
		{
			if (!live)
				continue;
			*frame->result = false;
			frame->ok = frame->partial ||
				pg_calls_refuse_index_duplicate(raise, pg_CatalogCaps_catalog(frame->caps), frame->index, frame->heap,
												frame->values, frame->isnull, frame->error);
			return;
		}
		if (frame->partial)
		{
			*frame->result = false;
			frame->ok = true;
			return;
		}
		{
			HostXid		xid = {running};
			HostOperation on = {.relation = pg_calls_rel_id(frame->index), .tid = frame->tuple,
			.oper = XLTW_InsertIndex};
			bool		waited;

			if (!host_xact_wait(&waits, HOST_IO_IO_IO_DOORS(frame->io), xid, true, on, true, &waited, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
		}
	}
}

/*
 * doltgres_entries_entry_unique changes an immediately checked unique index's
 * entry (the btree's part 2, slice 5, U1): the entry is formed and its blobs
 * written as entry_changed does, then its key is resolved under the index
 * storage's stripe and the entry put there when the key is free
 * (txn::entry::unique). When an entry exists at the key, the versions of the
 * row its value names (IndexKey::row_key) are read by its row key under
 * SnapshotAny through the table's storage and judged by their writers' and
 * enders' outcomes, as _bt_check_unique judges the heap tuple an entry names
 * under SnapshotDirty: the inserted row's own entry is left alone and nothing
 * is written; a version another running transaction writes or ends is waited
 * for (XLTW_InsertIndex, naming the index and the inserted row's tuple id as
 * _bt_check_unique does) and the key resolved again; a live version, the
 * transaction's own included, refuses the insert with nbtree's 23505; and a
 * row every version of which is ended has its removal pushed before its
 * ender's commit, which the next resolve meets: the read finds such a row
 * only when its ender committed between the resolve and the read, which the
 * pause doltgres-unique-reading holds open for a case (orev-29's S1), so the
 * resolve again ends the retry. A speculative insert's partial check
 * (partial) answers a live or running version, or another transaction's
 * running write of the key, as a possible conflict instead, writing nothing
 * and waiting for none, as _bt_check_unique answers UNIQUE_CHECK_PARTIAL, so
 * the executor kills the speculative row and retries through its arbiter's
 * check. It answers whether no other row holds the key. The entry's value is
 * cloned for each resolve, since a resolve that finds an existing entry hands
 * back none of it.
 */
static bool
doltgres_entries_entry_unique(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							  const HostSharedReader *reading, const PgRel *index,
							  const DoltgresIndexDescribed *described, const PgDatums *values, PgIsNull *isnull,
							  const PgRel *heap, const uint8 *row_key, size_t row_key_len, uint64 tuple,
							  TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, bool partial, bool *result,
							  PgPgError *error)
{
	doltgres_entries_entry_unique_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading,
		.index = index, .described = described, .values = values, .isnull = isnull, .heap = heap,
		.row_key = {row_key, row_key_len}, .tuple = tuple, .writer = writer, .roomed = roomed, .partial = partial,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_entry_unique_body, doltgres_entries_entry_unique_pad);
	doltgres_entries_entry_unique_pad(&frame);
	return frame.ok;
}

/* DoltgresEntriesOthersTag is Others' variants. */
typedef enum DoltgresEntriesOthersTag
{
	/*
	 * By the row key in the table of a primary key's index, the inserted
	 * row's copy left out.
	 */
	DOLTGRES_ENTRIES_OTHERS_KEY,
	/* Through another index's entries with the inserted row's key columns. */
	DOLTGRES_ENTRIES_OTHERS_ENTRIES,
} DoltgresEntriesOthersTag;

/*
 * DoltgresEntriesOthers is Others: how a deferrable unique check finds the
 * rows other than the inserted one whose key equals its, with the inserted
 * row's table, row key, and tuple id, and for Entries the backend's
 * repository and its reader, the index and its description, its key
 * columns' prefix of its entry's key, and the table's keyed layout.
 */
typedef struct DoltgresEntriesOthers
{
	DoltgresEntriesOthersTag tag;
	const PgRel *heap;
	StdBytes	row_key;
	uint64		tuple;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	const DoltgresIndexDescribed *described;
	StdBytes	prefix;
	StdBytes	keyed;
} DoltgresEntriesOthers;

/*
 * doltgres_entries_others_captures_t is others' lent calls' captures: the
 * snapshot, the base generation its base was read at, and the reader.
 */
typedef struct doltgres_entries_others_captures_t
{
	const HostSnapshot *seen;
	uint64		generation;
	HostOwnedHandle reader;
} doltgres_entries_others_captures_t;

/* doltgres_entries_others_closure_1 is others' read of the index's base the snapshot sees, with the generation before it. */
static bool
doltgres_entries_others_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  bool *some, TxnBaseBase *result, HostError *error)
{
	doltgres_entries_others_captures_t *c = captures;

	c->generation = txn_state_generation(host_shared_Held_record(held));
	return txn_state_readable(io, held, c->seen, some, result, error);
}

/* doltgres_entries_others_closure_2 is others' next: the reader's next entry under the snapshot. */
static bool
doltgres_entries_others_closure_2(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  bool *some, DoltgresScanRow *result, HostError *error)
{
	const doltgres_entries_others_captures_t *c = captures;
	DoltgresScanUnder under = {.state = host_shared_Held_record(held), .snapshot = c->seen};
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, repo};

	return doltgres_scan_next(io, &stores, &under, c->reader, DOLTGRES_SCAN_DIRECTION_FORWARD, some, result, error);
}

/*
 * doltgres_entries_others_frame_t is others' state across its pad: the rows
 * found, the index's base, the entry read, and a key's rows, each while
 * live.
 */
typedef struct doltgres_entries_others_frame_t
{
	StdVec		others;
	bool		others_live;
	TxnBaseBase base;
	bool		base_live;
	DoltgresScanRow row;
	bool		row_live;
	StdVec		rows;
	bool		rows_live;
	TreesKeyOrder order;
	bool		order_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const DoltgresEntriesOthers *which;
	const PgSnapshot *snapshot;
	const HostSnapshot *seen;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_others_frame_t;

/* doltgres_entries_others_pad frees what others holds. */
static void
doltgres_entries_others_pad(void *frame_)
{
	doltgres_entries_others_frame_t *frame = frame_;

	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
	if (frame->rows_live)
		std_vec_drop_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->others_live)
		std_vec_drop_Checked(&frame->others, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
							 doltgres_entries_Row_drop);
	frame->rows_live = frame->row_live = frame->base_live = frame->others_live = false;
}

/* doltgres_entries_others_kept moves into the found rows the frame's key's rows that are of the copy given, any for zero, dropping the rest. */
static void
doltgres_entries_others_kept(doltgres_entries_others_frame_t *frame, bool keyless, uint64 copy)
{
	for (size_t k = 0; k < frame->rows.len; k++)
	{
		DoltgresScanRow *found = &STD_VEC_AT(&frame->rows, DoltgresScanRow, k);

		if (!keyless || found->stamp.tuple == copy)
			std_vec_push_Checked(&frame->others, found, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
		else
			doltgres_scan_Row_drop(found);
	}
	std_vec_free_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	frame->rows_live = false;
}

/* doltgres_entries_others_entries reads the rows the index's entries at or past the prefix name, as others' Entries does. */
static bool
doltgres_entries_others_entries(doltgres_entries_others_frame_t *frame)
{
	const DoltgresEntriesOthers *which = frame->which;
	bool		keyless = doltgres_index_Described_keyless(which->described);
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, which->repo};
	doltgres_entries_others_captures_t c = {.seen = frame->seen};
	TreesStoreReader store;
	TxnStateFound found;
	HostError	failed;
	StdVec		keyed;
	bool		some;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_index_making(&frame->io, which->index, &found, frame->error))
		return false;
	if (!txn_state_held_OptionBase(&frame->io, which->repo, which->reading, found, doltgres_entries_others_closure_1,
								   &c, NULL, &some, &frame->base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->base_live = some;
	doltgres_index_Described_order(which->described, NULL, &frame->order);
	frame->order_live = true;
	store = repo_Repo_reader(which->repo, HOST_CACHE_PLACEMENT_SERVER, which->reading);
	std_slice_to_vec_Checked(&keyed, which->keyed.ptr, which->keyed.len, 1, 1);
	frame->order_live = false;
	if (!doltgres_scan_scan_begin(&frame->io, TREES_STORE_READER_NODE_STORE(&store), 0, some ? &frame->base : NULL,
								  c.generation, &keyed, pg_calls_index_table(which->index), &frame->order, &c.reader,
								  &failed) ||
		!doltgres_scan_seek(&frame->io, &stores, c.reader, which->prefix.ptr, which->prefix.len,
							DOLTGRES_SCAN_DIRECTION_FORWARD, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	for (;;)
	{
		DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY};
		StdBytes	other;
		uint64		copy;

		if (!doltgres_entries_index_making(&frame->io, which->index, &found, frame->error))
			return false;
		if (!txn_state_held_OptionRow(&frame->io, which->repo, which->reading, found, doltgres_entries_others_closure_2,
									  &c, NULL, &some, &frame->row, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		frame->row_live = some;
		if (!some || frame->row.key.len < which->prefix.len ||
			memcmp(frame->row.key.ptr, which->prefix.ptr, which->prefix.len) != 0)
			break;
		if (!access_index_IndexKey_row_key_bytes(doltgres_index_Described_entry_key(which->described),
												 frame->row.key.ptr, frame->row.key.len, frame->row.tuple.ptr,
												 frame->row.tuple.len, &other, frame->error))
			return false;
		copy = keyless ? frame->row.stamp.tuple : 0;
		if (other.len == which->row_key.len && memcmp(other.ptr, which->row_key.ptr, other.len) == 0 &&
			(!keyless || copy == which->tuple))
		{
			doltgres_scan_Row_drop(&frame->row);
			frame->row_live = false;
			continue;
		}
		by.key = other.ptr;
		by.key_len = other.len;
		if (!doltgres_entries_rows(frame->caps, which->heap, frame->snapshot, frame->seen, by, &frame->rows,
								   frame->error))
			return false;
		frame->rows_live = true;
		doltgres_entries_others_kept(frame, keyless, copy);
		doltgres_scan_Row_drop(&frame->row);
		frame->row_live = false;
	}
	if (frame->row_live)
	{
		doltgres_scan_Row_drop(&frame->row);
		frame->row_live = false;
	}
	if (!doltgres_scan_scan_end(&frame->io, c.reader, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_others_body reads the key's rows other than the inserted one, as the check's Others finds them. */
static void
doltgres_entries_others_body(void *frame_)
{
	doltgres_entries_others_frame_t *frame = frame_;
	const DoltgresEntriesOthers *which = frame->which;

	std_vec_new(&frame->others, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	frame->others_live = true;
	if (which->tag == DOLTGRES_ENTRIES_OTHERS_KEY)
	{
		DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY, .key = which->row_key.ptr,
		.key_len = which->row_key.len};

		if (!doltgres_entries_rows(frame->caps, which->heap, frame->snapshot, frame->seen, by, &frame->rows,
								   frame->error))
			return;
		frame->rows_live = true;
		for (size_t k = 0; k < frame->rows.len; k++)
		{
			DoltgresScanRow *row = &STD_VEC_AT(&frame->rows, DoltgresScanRow, k);

			if (row->stamp.tuple != which->tuple)
				std_vec_push_Checked(&frame->others, row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
			else
				doltgres_scan_Row_drop(row);
		}
		std_vec_free_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
		frame->rows_live = false;
	}
	else if (!doltgres_entries_others_entries(frame))
		return;
	*frame->result = frame->others;
	frame->others_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_others returns a deferrable unique key's rows other than
 * the inserted one under a Dirty snapshot, as Others finds them: for an index
 * with entries, those of its entries at or past the key columns' prefix that
 * share it, read from the merged base and delta under the snapshot, each
 * naming a row other than the inserted one, which is then read by its key
 * under it, a keyless table's copy by its tuple id.
 */
static bool
doltgres_entries_others(const PgCatalogCaps *caps, const DoltgresEntriesOthers *which, const PgSnapshot *snapshot,
						const HostSnapshot *seen, StdVec *result, PgPgError *error)
{
	doltgres_entries_others_frame_t frame = {.caps = caps, .which = which, .snapshot = snapshot, .seen = seen,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_others_body, doltgres_entries_others_pad);
	doltgres_entries_others_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_unique_judged_frame_t is unique_judged's state across its
 * pad: the Dirty snapshot and the other rows read, each while live.
 */
typedef struct doltgres_entries_unique_judged_frame_t
{
	PgOwnedSnapshot owned;
	bool		owned_live;
	StdVec		others;
	bool		others_live;
	const PgCatalogCaps *caps;
	const PgRel *index;
	const PgRel *heap;
	const PgDatums *values;
	PgIsNull   *isnull;
	int32		check;
	const DoltgresEntriesOthers *which;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_unique_judged_frame_t;

/* doltgres_entries_unique_judged_pad frees what unique_judged holds. */
static void
doltgres_entries_unique_judged_pad(void *frame_)
{
	doltgres_entries_unique_judged_frame_t *frame = frame_;

	if (frame->others_live)
		std_vec_drop_Checked(&frame->others, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
							 doltgres_entries_Row_drop);
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	frame->others_live = frame->owned_live = false;
}

/* doltgres_entries_unique_judged_body reads the other rows until none is running, then judges them. */
static void
doltgres_entries_unique_judged_body(void *frame_)
{
	doltgres_entries_unique_judged_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostIoWaits waits = pghost_waits();

	if (!pg_OwnedSnapshot_dirty(raise, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	for (;;)
	{
		HostSnapshot read;
		const DoltgresScanRow *other;
		HostOperation on;
		HostIoIo	io;
		HostError	failed;
		HostXid		xid;
		bool		waited;

		if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &read, frame->error) ||
			!doltgres_entries_others(frame->caps, frame->which, pg_OwnedSnapshot_snapshot(&frame->owned), &read,
									 &frame->others, frame->error))
			return;
		frame->others_live = true;
		if (frame->others.len == 0)
		{
			*frame->result = true;
			frame->ok = true;
			return;
		}
		if (frame->check == UNIQUE_CHECK_PARTIAL)
		{
			*frame->result = false;
			frame->ok = true;
			return;
		}
		other = &STD_VEC_AT(&frame->others, DoltgresScanRow, 0);
		if (!other->dirty.has_xmin && !other->dirty.has_xmax)
		{
			*frame->result = false;
			frame->ok = pg_calls_refuse_index_duplicate(raise, pg_CatalogCaps_catalog(frame->caps), frame->index,
														frame->heap, frame->values, frame->isnull, frame->error);
			return;
		}
		xid = other->dirty.has_xmin ? other->dirty.xmin : other->dirty.xmax;
		on = (HostOperation)
		{
			.relation = pg_calls_rel_id(frame->index), .tid = other->stamp.tuple,
				.oper = XLTW_InsertIndex
		};
		std_vec_drop_Checked(&frame->others, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
							 doltgres_entries_Row_drop);
		frame->others_live = false;
		io = pghost_io_catalog(frame->caps);
		if (!host_xact_wait(&waits, HOST_IO_IO_IO_DOORS(&io), xid, true, on, true, &waited, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
	}
}

/*
 * doltgres_entries_unique_judged judges a deferrable unique key's other rows
 * under a Dirty snapshot, as _bt_check_unique judges the heap tuples its equal
 * entries name: the rows others reads under it, the key's rows other than the
 * inserted one; UNIQUE_CHECK_PARTIAL answers whether there is none, live or
 * being written, so the executor queues unique_key_recheck when there is one,
 * and UNIQUE_CHECK_EXISTING waits for a running writer or ender of one and
 * reads again, then refuses a live one with nbtree's 23505.
 */
static bool
doltgres_entries_unique_judged(const PgCatalogCaps *caps, const PgRel *index, const PgRel *heap,
							   const PgDatums *values, PgIsNull *isnull, int32 check,
							   const DoltgresEntriesOthers *which, bool *result, PgPgError *error)
{
	doltgres_entries_unique_judged_frame_t frame = {.caps = caps, .index = index, .heap = heap, .values = values,
	.isnull = isnull, .check = check, .which = which, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_unique_judged_body, doltgres_entries_unique_judged_pad);
	doltgres_entries_unique_judged_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_key_checked is the aminsert of an index with no tree of
 * its own (index::keyed), a primary key's: it writes nothing, and for an
 * immediate key it checks nothing either, since the table's resolve refused a
 * live row's key before the row's push ((4b)'s §4d), answering false as
 * btinsert does. For a deferrable key it reads the key's rows other than the
 * inserted one under a Dirty snapshot: UNIQUE_CHECK_PARTIAL answers whether
 * none is live or being written, so the executor queues unique_key_recheck
 * when one is, and UNIQUE_CHECK_EXISTING waits for each running writer or
 * ender of another row and reads again, then refuses a live one with
 * nbtree's 23505, as _bt_check_unique does.
 */
static bool
doltgres_entries_key_checked(const PgCatalogCaps *caps, const PgRel *index, const PgRel *heap, const PgDatums *values,
							 PgIsNull *isnull, const uint8 *row_key, size_t row_key_len, uint64 tuple, int32 check,
							 bool *result, PgPgError *error)
{
	DoltgresEntriesOthers which = {.tag = DOLTGRES_ENTRIES_OTHERS_KEY, .heap = heap, .row_key = {row_key, row_key_len},
	.tuple = tuple};

	if (check != UNIQUE_CHECK_PARTIAL && check != UNIQUE_CHECK_EXISTING)
	{
		*result = false;
		return true;
	}
	return doltgres_entries_unique_judged(caps, index, heap, values, isnull, check, &which, result, error);
}

/*
 * doltgres_entries_entry_deferred_frame_t is entry_deferred's state across
 * its pad: the values read, the entry's key, the table's base record, and
 * its keyed layout, each while live.
 */
typedef struct doltgres_entries_entry_deferred_frame_t
{
	StdVec		read;
	bool		read_live;
	AccessIndexEntryKey key;
	bool		key_live;
	TxnBaseBase recorded;
	bool		recorded_live;
	/* columns is the table's key columns, a Vec<(usize, Order, KeyType)>. */
	StdVec		columns;
	bool		columns_live;
	StdVec		keyed;
	bool		keyed_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	const DoltgresIndexDescribed *described;
	const PgDatums *values;
	PgIsNull   *isnull;
	const PgRel *heap;
	StdBytes	row_key;
	uint64		tuple;
	int32		check;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entry_deferred_frame_t;

/* doltgres_entries_entry_deferred_pad frees what entry_deferred holds. */
static void
doltgres_entries_entry_deferred_pad(void *frame_)
{
	doltgres_entries_entry_deferred_frame_t *frame = frame_;

	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	frame->columns_live = false;
	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	if (frame->key_live)
		access_index_EntryKey_drop(&frame->key);
	if (frame->read_live)
		doltgres_index_values_drop(&frame->read);
	frame->keyed_live = frame->recorded_live = frame->key_live = frame->read_live = false;
}

/* doltgres_entries_entry_deferred_body judges the other rows the index's entries with the row's key columns name. */
static void
doltgres_entries_entry_deferred_body(void *frame_)
{
	doltgres_entries_entry_deferred_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, (void *) frame->caps};
	DoltgresEntriesOthers which = {.tag = DOLTGRES_ENTRIES_OTHERS_ENTRIES, .heap = frame->heap,
		.row_key = frame->row_key, .tuple = frame->tuple, .repo = frame->repo, .reading = frame->reading,
	.index = frame->index, .described = frame->described};

	if (!doltgres_index_Described_values(frame->described, frame->caps, frame->values, frame->isnull, &frame->read,
										 frame->error))
		return;
	frame->read_live = true;
	if (doltgres_index_Described_null_key(frame->described, frame->read.ptr, frame->read.len) &&
		!pg_calls_index_nulls_not_distinct(frame->index))
	{
		*frame->result = true;
		frame->ok = true;
		return;
	}
	if (!doltgres_index_Described_key(frame->described, sorter, frame->read.ptr, frame->read.len, frame->row_key.ptr,
									  frame->row_key.len, &frame->key, frame->error))
		return;
	frame->key_live = true;
	which.prefix = (StdBytes)
	{
		frame->key.key.ptr, frame->key.key.len - frame->row_key.len
	};
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, frame->repo, frame->reading, frame->heap, &frame->recorded,
								   frame->error))
		return;
	frame->recorded_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->recorded.key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->keyed);
	frame->keyed_live = true;
	doltgres_layout_VecKeyed_drop(&frame->columns);
	frame->columns_live = false;
	which.keyed = (StdBytes)
	{
		frame->keyed.ptr, frame->keyed.len
	};
	frame->ok = doltgres_entries_unique_judged(frame->caps, frame->index, frame->heap, frame->values, frame->isnull,
											   frame->check, &which, frame->result, frame->error);
}

/*
 * doltgres_entries_entry_deferred checks a deferrable unique index's key (the
 * btree's part 2, §3, CommPF 9's ruling (a)), whose entries end with their
 * rows' keys so equal keys can stand until the check, as unique_judged judges
 * them: the other rows are those the index's entries with the inserted row's
 * key columns name, read from the merged base and delta under the Dirty
 * snapshot, each row then read by its key under it, a keyless table's copy by
 * its tuple id. A key with a NULL in a column has no other row unless the
 * index's NULLs are not distinct, as _bt_check_unique skips it.
 */
static bool
doltgres_entries_entry_deferred(const PgCatalogCaps *caps, const RepoRepo *repo, const HostSharedReader *reading,
								const PgRel *index, const DoltgresIndexDescribed *described, const PgDatums *values,
								PgIsNull *isnull, const PgRel *heap, const uint8 *row_key, size_t row_key_len,
								uint64 tuple, int32 check, bool *result, PgPgError *error)
{
	doltgres_entries_entry_deferred_frame_t frame = {.caps = caps, .repo = repo, .reading = reading, .index = index,
		.described = described, .values = values, .isnull = isnull, .heap = heap, .row_key = {row_key, row_key_len},
	.tuple = tuple, .check = check, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_entry_deferred_body, doltgres_entries_entry_deferred_pad);
	doltgres_entries_entry_deferred_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_index_insert_killed_captures_t is index_insert's look for its row's killed speculative insert: the row, and what it found. */
typedef struct doltgres_entries_doltgres_index_insert_killed_captures_t
{
	uint32		relation;
	uint64		tuple;
	bool		killed;
} doltgres_entries_doltgres_index_insert_killed_captures_t;

/* doltgres_entries_doltgres_index_insert_killed_op finds whether the backend's speculative insert of the row was killed. */
static void
doltgres_entries_doltgres_index_insert_killed_op(void *value, void *captures)
{
	const StdVec *all = value;
	doltgres_entries_doltgres_index_insert_killed_captures_t *c = captures;

	for (size_t k = 0; !c->killed && k < all->len; k++)
	{
		const DoltgresEntriesSpeculated *s = &STD_VEC_AT(all, DoltgresEntriesSpeculated, k);

		c->killed = s->relation == c->relation && s->tuple == c->tuple && s->killed;
	}
}

/* doltgres_entries_doltgres_index_insert_captures_t is index_insert's key read's captures: the reader and the row's tuple id. */
typedef struct doltgres_entries_doltgres_index_insert_captures_t
{
	const HostSharedReader *reading;
	uint64		tuple;
} doltgres_entries_doltgres_index_insert_captures_t;

/* doltgres_entries_doltgres_index_insert_closure_1 is index_insert's key read: the row's key from its tuple id through the table's delta and base. */
static bool
doltgres_entries_doltgres_index_insert_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
												 const HostSharedHeld *held, bool *some, StdVec *result_0,
												 bool *result_1, HostError *error)
{
	const doltgres_entries_doltgres_index_insert_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	return doltgres_scan_keyed(io, TREES_STORE_READER_NODE_STORE(&store), held, c->tuple, some, result_0, result_1,
							   error);
}

/* doltgres_entries_Described_drop_glue is an Arc<Described>'s value's drop glue. */
static void
doltgres_entries_Described_drop_glue(void *described)
{
	doltgres_index_Described_drop(described);
}

/*
 * doltgres_entries_doltgres_index_insert_frame_t is index_insert's state
 * across its pad: what it opened, the table's base record, the description
 * lent, and the row's key, each while live.
 */
typedef struct doltgres_entries_doltgres_index_insert_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseBase base;
	bool		base_live;
	/* table_key is the table's key columns, a Vec<(usize, Order, KeyType)>. */
	StdVec		table_key;
	bool		table_key_live;
	DoltgresIndexDescribed made;
	bool		made_live;
	StdArcInner *described;
	bool		described_live;
	StdVec		row_key;
	bool		row_key_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *index;
	const PgDatums *values;
	PgIsNull   *isnull;
	PgTid	   *heap_tid;
	const PgRel *heap;
	int32		check_unique;
	PgIndexInfo *info;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_insert_frame_t;

/* doltgres_entries_doltgres_index_insert_pad frees what index_insert holds. */
static void
doltgres_entries_doltgres_index_insert_pad(void *frame_)
{
	doltgres_entries_doltgres_index_insert_frame_t *frame = frame_;

	if (frame->row_key_live)
		std_vec_free_Checked(&frame->row_key, 1, 1);
	if (frame->described_live)
		std_arc_drop(frame->described, sizeof(DoltgresIndexDescribed), _Alignof(DoltgresIndexDescribed),
					 doltgres_entries_Described_drop_glue);
	if (frame->made_live)
		doltgres_index_Described_drop(&frame->made);
	if (frame->table_key_live)
		doltgres_layout_VecKeyed_drop(&frame->table_key);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	doltgres_entries_opened_drop(&frame->opened);
	frame->row_key_live = frame->described_live = frame->made_live = frame->table_key_live = false;
	frame->base_live = false;
}

/* doltgres_entries_doltgres_index_insert_handle returns the insert's description's handle, made at the statement's first row and named in its state. */
static bool
doltgres_entries_doltgres_index_insert_handle(doltgres_entries_doltgres_index_insert_frame_t *frame,
											  HostOwnedHandle *result)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DgOwned		owned = pg_calls_insert_record(frame->info);
	DoltgresIndexInserting inserting;
	DgOwned		record;
	HostError	failed;

	if (owned.generation != 0)
	{
		*result = (HostOwnedHandle)
		{
			.slot = owned.slot, .generation = owned.generation
		};
		return true;
	}
	if (!doltgres_layout_key_columns(raise, catalog, &frame->base.key, &frame->table_key, frame->error))
		return false;
	frame->table_key_live = true;
	if (!doltgres_index_describe(raise, catalog, frame->index, frame->table_key.ptr, frame->table_key.len,
								 &frame->base.key, &frame->made, frame->error))
		return false;
	frame->made_live = true;
	doltgres_layout_VecKeyed_drop(&frame->table_key);
	frame->table_key_live = false;
	inserting.f0 = std_arc_new(&frame->made, sizeof(frame->made), _Alignof(DoltgresIndexDescribed));
	frame->made_live = false;
	if (!host_owned_TableDoltgresIndexInserting_make(&DOLTGRES_INDEX_INSERTING, &frame->io, inserting, result,
													 &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	record = (DgOwned)
	{
		.slot = result->slot, .generation = result->generation
	};
	return pg_calls_insert_set_record(raise, frame->info, record, frame->error);
}

/* doltgres_entries_doltgres_index_insert_body changes the row's entry, or checks its key, as the index's description says. */
static void
doltgres_entries_doltgres_index_insert_body(void *frame_)
{
	doltgres_entries_doltgres_index_insert_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint64		tuple = pg_calls_tid_value(frame->heap_tid);
	doltgres_entries_doltgres_index_insert_killed_captures_t killed = {.relation = pg_calls_rel_id(frame->heap),
	.tuple = tuple};
	doltgres_entries_doltgres_index_insert_captures_t c = {.reading = &frame->opened.reading, .tuple = tuple};
	const RepoRepo *repo;
	const DoltgresIndexDescribed *described;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	HostOwnedHandle handle;
	TxnStateFound made;
	HostError	failed;
	uint32		command;
	bool		deferred;
	bool		keyless;
	bool		some;

	frame->io = pghost_io_catalog(frame->caps);
	if (!host_pause(HOST_SHARED_IO_PAUSES(&frame->io), "doltgres-index-insert", &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->heap, &frame->base,
								   frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_entries_doltgres_index_insert_handle(frame, &handle) ||
		!pg_calls_current_command(raise, true, &command, frame->error) ||
		!doltgres_entries_current(&frame->io, command, &writer, &roomed, frame->error))
		return;
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_SPECULATED.local, &DOLTGRES_ENTRIES_SPECULATED_TYPE,
							&DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE, doltgres_entries_doltgres_index_insert_killed_op,
							&killed);
	if (killed.killed || doltgres_index_unchanged(&frame->io, pg_calls_rel_id(frame->index), tuple, &writer))
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->heap, &made, frame->error))
		return;
	if (!txn_state_held_OptionTupleVecU8Bool(&frame->io, repo, &frame->opened.reading, made,
											 doltgres_entries_doltgres_index_insert_closure_1, &c, NULL, &some,
											 &frame->row_key, &keyless, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!some)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("an index entry's row ("), STD_FMT_ARG(0), STD_FMT_LIT(","),
		STD_FMT_ARG(1), STD_FMT_LIT(") has no key")};
		uint64		block = tuple >> 16;
		uint64		offset = tuple & 0xffff;
		StdFmtArg	args[] = {{&block, std_u64_Display_fmt}, {&offset, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return;
	}
	frame->row_key_live = true;
	if (!doltgres_index_inserting(&frame->io, handle, &frame->described, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->described_live = true;
	described = std_arc_Deref_deref(frame->described, _Alignof(DoltgresIndexDescribed));
	if (doltgres_index_Described_keyed(described))
	{
		frame->ok = doltgres_entries_key_checked(frame->caps, frame->index, frame->heap, frame->values, frame->isnull,
												 frame->row_key.ptr, frame->row_key.len, tuple, frame->check_unique,
												 frame->result, frame->error);
		return;
	}
	deferred = pg_calls_index_unique(frame->index) && !pg_calls_index_immediate(frame->index);
	if (doltgres_index_Described_unique(described) && !deferred &&
		(frame->check_unique == UNIQUE_CHECK_YES || frame->check_unique == UNIQUE_CHECK_PARTIAL))
	{
		frame->ok = doltgres_entries_entry_unique(frame->caps, &frame->io, repo, &frame->opened.reading, frame->index,
												  described, frame->values, frame->isnull, frame->heap,
												  frame->row_key.ptr, frame->row_key.len, tuple, writer, roomed,
												  frame->check_unique == UNIQUE_CHECK_PARTIAL, frame->result,
												  frame->error);
		return;
	}
	if (deferred && frame->check_unique == UNIQUE_CHECK_EXISTING)
	{
		frame->ok = doltgres_entries_entry_deferred(frame->caps, repo, &frame->opened.reading, frame->index, described,
													frame->values, frame->isnull, frame->heap, frame->row_key.ptr,
													frame->row_key.len, tuple, frame->check_unique, frame->result,
													frame->error);
		return;
	}
	if (!doltgres_entries_entry_changed(frame->caps, &frame->io, repo, &frame->opened.reading, frame->index, described,
										frame->values, frame->isnull, frame->row_key.ptr, frame->row_key.len, tuple,
										writer, roomed, frame->error))
		return;
	if (deferred && frame->check_unique == UNIQUE_CHECK_PARTIAL)
	{
		frame->ok = doltgres_entries_entry_deferred(frame->caps, repo, &frame->opened.reading, frame->index, described,
													frame->values, frame->isnull, frame->heap, frame->row_key.ptr,
													frame->row_key.len, tuple, frame->check_unique, frame->result,
													frame->error);
		return;
	}
	*frame->result = false;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_insert is the fork's btree aminsert (the
 * btree text's section 5): it lends the index's description from the
 * insert's state, made at the statement's first row and named in
 * ii_AmCache, takes the row's key from its tuple id through the table's
 * delta, forms the entry from the executor's values, writes its value's
 * blobs, and changes the entry in the index's own storage
 * (txn::entry::change): a put, or for a keyless table's row one more copy
 * counted under the row's tuple id; an update's new row whose entry the
 * update kept, and a speculative insert's place spent with no row written,
 * write nothing. An immediately checked unique index's entry is changed
 * through its key's resolve (entry_unique), which answers whether no other
 * row holds the key, UNIQUE_CHECK_PARTIAL being a speculative insert's check
 * there, as btinsert answers _bt_check_unique's; a deferrable one's is
 * checked by entry_deferred; any other index answers false, as btinsert does
 * for an index that isn't unique. It passes the pause doltgres-index-insert
 * first, where a case shows an update that changes no entry calls it for no
 * index.
 */
bool
doltgres_entries_doltgres_index_insert(const PgCatalogCaps *caps, const PgRel *index, const PgDatums *values,
									   PgIsNull *isnull, PgTid *heap_tid, const PgRel *heap, int32 check_unique,
									   bool index_unchanged, PgIndexInfo *info, bool *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_insert_frame_t frame = {.caps = caps, .index = index, .values = values,
		.isnull = isnull, .heap_tid = heap_tid, .heap = heap, .check_unique = check_unique, .info = info,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_insert_body, doltgres_entries_doltgres_index_insert_pad);
	doltgres_entries_doltgres_index_insert_pad(&frame);
	return frame.ok;
}
#endif							/* FRONTEND */
