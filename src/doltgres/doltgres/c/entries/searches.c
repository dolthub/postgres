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
 * The fork btree's scans (doltgres' entries/searches.rs): an owned record
 * each scan's state names, the search of its keys over Postgres' own
 * preprocessed keys, read through the scan reader's merge of the index's
 * base and delta under the scan's snapshot, with the keys' functions called
 * through Postgres.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "access/sdir.h"
#include "doltgres/access/index.h"
#include "doltgres/access/key.h"
#include "doltgres/access/nbtsearch.h"
#include "doltgres/access/nbtutils.h"
#include "doltgres/access/slot.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/entries/searches.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec_deque.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/state.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "entries" STD_SEP "searches.rs"

/* DoltgresEntriesSearchesSkipTag is Skip's variants. */
typedef enum DoltgresEntriesSearchesSkipTag
{
	DOLTGRES_ENTRIES_SEARCHES_SKIP_KEY,
	DOLTGRES_ENTRIES_SEARCHES_SKIP_THROUGH,
} DoltgresEntriesSearchesSkipTag;

/*
 * DoltgresEntriesSearchesSkip is Skip: what a search passes over before its
 * first row, every row of the key its near bound excludes (a Vec<u8>), or
 * every row up to and including the one at the position a resumed search
 * goes on after.
 */
typedef struct DoltgresEntriesSearchesSkip
{
	DoltgresEntriesSearchesSkipTag tag;
	union
	{
		StdVec		key;
		AccessNbtsearchPosition through;
	}			u;
} DoltgresEntriesSearchesSkip;

/*
 * DoltgresEntriesSearchesRange is Range: what the reader keeps of the search
 * it reads from one call to the next, the way it goes, what it passes over
 * before its first row, its far bound, and the rows of a run of cut keys it
 * sorted and hasn't given yet with what it read past the run.
 */
typedef struct DoltgresEntriesSearchesRange
{
	DoltgresScanDirection way;
	bool		has_skip;
	DoltgresEntriesSearchesSkip skip;
	/* far_end is its far bound, far being a Windows macro. */
	access_BoundVecU8_t far_end;
	/* run is a VecDeque<Row>. */
	StdVecDeque run;
	/* ahead is an Option<Option<Row>>: whether it has one, then a row. */
	bool		has_ahead;
	bool		ahead_some;
	DoltgresScanRow ahead;
} DoltgresEntriesSearchesRange;

/*
 * DoltgresEntriesSearchesSearch is Search: an index scan's record, the
 * index's description (an Arc<Described>), its table's keyed layout, the
 * reader's scan record, the search of its keys, what the reader keeps of the
 * search it reads, whether a parallel scan's participant has a unit it
 * reads, its mark, and the scan's own long values (an Arc<Probes>).
 */
typedef struct DoltgresEntriesSearchesSearch
{
	StdArcInner *described;
	StdVec		keyed;
	bool		has_reader;
	HostOwnedHandle reader;
	bool		has_scan;
	AccessNbtsearchBtScan scan;
	DoltgresEntriesSearchesRange range;
	bool		taken;
	bool		has_mark;
	AccessNbtsearchBtMark mark;
	StdArcInner *probes;
} DoltgresEntriesSearchesSearch;

/* doltgres_entries_searches_Range_Default_default is Range's Default: forward, with nothing to pass over, and no far bound. */
static void
doltgres_entries_searches_Range_Default_default(DoltgresEntriesSearchesRange *result)
{
	DoltgresEntriesSearchesRange range = {.way = DOLTGRES_SCAN_DIRECTION_FORWARD, .far_end = {.tag = STD_BOUND_UNBOUNDED}};

	std_vec_deque_new(&range.run, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	*result = range;
}

/* doltgres_entries_searches_Row_drop is Row's drop glue, as a VecDeque's element takes it. */
static void
doltgres_entries_searches_Row_drop(void *row)
{
	doltgres_scan_Row_drop(row);
}

/* doltgres_entries_searches_Range_drop is Range's drop glue: its skip, its far bound, its run, then what it read ahead. */
static void
doltgres_entries_searches_Range_drop(DoltgresEntriesSearchesRange *self)
{
	if (self->has_skip && self->skip.tag == DOLTGRES_ENTRIES_SEARCHES_SKIP_KEY)
		std_vec_free_Checked(&self->skip.u.key, 1, 1);
	else if (self->has_skip)
		access_nbtsearch_Position_drop(&self->skip.u.through);
	access_nbtsearch_Bound_drop(&self->far_end);
	std_vec_deque_drop(&self->run, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
					   doltgres_entries_searches_Row_drop);
	if (self->has_ahead && self->ahead_some)
		doltgres_scan_Row_drop(&self->ahead);
}

/* doltgres_entries_searches_Described_drop is Described's drop glue, an Arc<Described>'s value's. */
static void
doltgres_entries_searches_Described_drop(void *described)
{
	doltgres_index_Described_drop(described);
}

/* doltgres_entries_searches_Search_drop is Search's drop glue: each of its fields' in order. */
static void
doltgres_entries_searches_Search_drop(void *record)
{
	DoltgresEntriesSearchesSearch *self = record;

	std_arc_drop(self->described, sizeof(DoltgresIndexDescribed), _Alignof(DoltgresIndexDescribed),
				 doltgres_entries_searches_Described_drop);
	std_vec_free_Checked(&self->keyed, 1, 1);
	if (self->has_scan)
		access_nbtsearch_BtScan_drop(&self->scan);
	doltgres_entries_searches_Range_drop(&self->range);
	if (self->has_mark)
		access_nbtsearch_BtMark_drop(&self->mark);
	std_arc_drop(self->probes, sizeof(HostLockIoMutexVecProbe), _Alignof(HostLockIoMutexVecProbe),
				 doltgres_layout_Probes_drop);
}

/* DOLTGRES_ENTRIES_SEARCHES_SEARCHES is SEARCHES, the backend's index scans. */
HostOwnedTable DOLTGRES_ENTRIES_SEARCHES_SEARCHES = HOST_OWNED_TABLE_NEW;

/* doltgres_entries_searches_Search_Owned_table is Search's Owned::table, SEARCHES. */
static HostOwnedTable *
doltgres_entries_searches_Search_Owned_table(void)
{
	return &DOLTGRES_ENTRIES_SEARCHES_SEARCHES;
}

/* Search's Table, host's owned Table<Search>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#include "doltgres/host/owned_template.h"

/* doltgres_OptionHandle_t is an Option<Handle>: a reader's scan record, or none. */
typedef struct doltgres_OptionHandle_t
{
	bool		has;
	HostOwnedHandle handle;
} doltgres_OptionHandle_t;

/* Search's Table's with at an Option<Handle>. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#define HOST_OWNED_WITH_TS OptionHandle
#define HOST_OWNED_WITH_T doltgres_OptionHandle_t
#include "doltgres/host/owned_template.h"

/* doltgres_entries_searches_handle returns the handle of the record a scan's state names. */
static HostOwnedHandle
doltgres_entries_searches_handle(const PgIndexScan *scan)
{
	DgOwned		owned = pg_calls_index_scan_record(scan);
	HostOwnedHandle handle = {owned.slot, owned.generation};

	return handle;
}

/* doltgres_entries_searches_internal returns an internal error of the fork's. */
static PgPgError
doltgres_entries_searches_internal(const char *message)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &owned);
}

/* doltgres_entries_searches_IoMutexVecProbe_drop is Probes' drop glue, as its Default type takes it. */
static void
doltgres_entries_searches_IoMutexVecProbe_drop(void *probes)
{
	doltgres_layout_Probes_drop(probes);
}

/* DOLTGRES_ENTRIES_SEARCHES_PROBES_TYPE is Probes' size, Default, and drop, which Arc::default takes. */
static const StdDefaultType DOLTGRES_ENTRIES_SEARCHES_PROBES_TYPE = {sizeof(HostLockIoMutexVecProbe),
	_Alignof(HostLockIoMutexVecProbe), host_lock_IoMutexVecProbe_Default_default,
doltgres_entries_searches_IoMutexVecProbe_drop};

/*
 * doltgres_entries_searches_begin_frame_0_t is begin's state across its pad:
 * its arguments, the session's repository, its reader, the index's table,
 * its base record, its key columns, and the description, each while live.
 */
typedef struct doltgres_entries_searches_begin_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgRel *index;
	int32		nkeys;
	int32		norderbys;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	PgOwnedRel	heap;
	bool		heap_live;
	TxnBaseBase base;
	bool		base_live;
	/* table_key is the Vec<(usize, Order, KeyType)> of the table's key. */
	StdVec		table_key;
	bool		table_key_live;
	DoltgresIndexDescribed described;
	bool		described_live;
	PgIndexScanDesc *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_begin_frame_0_t;

/* doltgres_entries_searches_Repo_drop is Repo's drop glue, as an Arc's value takes it. */
static void
doltgres_entries_searches_Repo_drop(void *repo)
{
	repo_Repo_drop(repo);
}

/* doltgres_entries_searches_begin_body_0 makes the scan's state and its record, the index described under its table's recorded key. */
static void
doltgres_entries_searches_begin_body_0(void *frame_)
{
	doltgres_entries_searches_begin_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DoltgresEntriesSearchesSearch search = {0};
	HostOwnedHandle owned;
	HostError	failed;
	DgOwned		record;

	if (!pg_calls_index_scan_make(raise, frame->index, frame->nkeys, frame->norderbys, frame->result, frame->error))
		return;
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!pg_OwnedRel_index_table(raise, catalog, frame->index, &frame->heap, frame->error))
		return;
	frame->heap_live = true;
	if (!doltgres_entries_recorded(frame->caps, &frame->io, STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo)),
								   &frame->reading, pg_OwnedRel_rel(&frame->heap), &frame->base, frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->base.key, &frame->table_key, frame->error))
		return;
	frame->table_key_live = true;
	if (!doltgres_index_describe(raise, catalog, frame->index, frame->table_key.ptr, frame->table_key.len,
								 &frame->base.key, &frame->described, frame->error))
		return;
	frame->described_live = true;
	access_slot_keyed(frame->table_key.ptr, frame->table_key.len, &search.keyed);
	doltgres_entries_searches_Range_Default_default(&search.range);
	search.probes = std_arc_default(&DOLTGRES_ENTRIES_SEARCHES_PROBES_TYPE);
	frame->described_live = false;
	search.described = std_arc_new_dropping(&frame->described, sizeof(DoltgresIndexDescribed),
											_Alignof(DoltgresIndexDescribed),
											doltgres_entries_searches_Described_drop);
	if (!host_owned_TableDoltgresEntriesSearchesSearch_make(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &frame->io, search,
															&owned, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	record.slot = owned.slot;
	record.generation = owned.generation;
	pg_calls_index_scan_set_record(pg_IndexScanDesc_scan(frame->result), record);
	frame->ok = true;
}

/* doltgres_entries_searches_begin_pad_0 drops the description, the key columns, the base, the table, the reader, and the repository. */
static void
doltgres_entries_searches_begin_pad_0(void *frame_)
{
	doltgres_entries_searches_begin_frame_0_t *frame = frame_;

	if (frame->described_live)
		doltgres_index_Described_drop(&frame->described);
	if (frame->table_key_live)
		doltgres_layout_VecKeyed_drop(&frame->table_key);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_searches_Repo_drop);
	frame->described_live = frame->table_key_live = frame->base_live = frame->heap_live = false;
	frame->reading_live = frame->repo_live = false;
}

/*
 * doltgres_entries_searches_begin is the fork's btree ambeginscan: it makes
 * the scan's state as btbeginscan does, with the fork's in place of
 * nbtree's, and its record, the index described under its table's recorded
 * key, which it opens for the description.
 */
bool
doltgres_entries_searches_begin(const PgCatalogCaps *caps, const PgRel *index, int32 nkeys, int32 norderbys,
								PgIndexScanDesc *result, PgPgError *error)
{
	doltgres_entries_searches_begin_frame_0_t frame = {.caps = caps, .index = index, .nkeys = nkeys,
	.norderbys = norderbys, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_searches_begin_body_0, doltgres_entries_searches_begin_pad_0);
	doltgres_entries_searches_begin_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_rescan_closure_0 is rescan's lend: the search started over, and the reader's record. */
static bool
doltgres_entries_searches_rescan_closure_0(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										   doltgres_OptionHandle_t *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	if (search->has_scan)
		access_nbtsearch_BtScan_drop(&search->scan);
	search->has_scan = false;
	doltgres_entries_searches_Range_drop(&search->range);
	doltgres_entries_searches_Range_Default_default(&search->range);
	search->taken = false;
	if (search->has_mark)
		access_nbtsearch_BtMark_drop(&search->mark);
	search->has_mark = false;
	result->has = search->has_reader;
	result->handle = search->reader;
	return true;
}

/*
 * doltgres_entries_searches_rescan is the fork's btree amrescan: it sets the
 * scan's keys as btrescan does and starts its search over, its keys
 * preprocessed again at its next read.
 */
bool
doltgres_entries_searches_rescan(const PgCaps *caps, PgIndexScan *scan, const PgScanKeys *keys, PgPgError *error)
{
	HostIoIo	io;
	doltgres_OptionHandle_t reader;
	HostError	failed;

	pg_calls_index_scan_restart(scan, keys);
	io = pghost_io(pg_Caps_may_raise(caps));
	if (!host_owned_TableDoltgresEntriesSearchesSearch_withOptionHandle(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io,
																		doltgres_entries_searches_handle(scan),
																		doltgres_entries_searches_rescan_closure_0,
																		NULL, NULL, &reader, &failed) ||
		(reader.has && !doltgres_scan_scan_rescan(&io, reader.handle, &failed)))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_searches_end_closure_0 is end's lend: the reader's record taken out of the search. */
static bool
doltgres_entries_searches_end_closure_0(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										doltgres_OptionHandle_t *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	result->has = search->has_reader;
	result->handle = search->reader;
	search->has_reader = false;
	return true;
}

/*
 * doltgres_entries_searches_end is the fork's btree amendscan: it ends the
 * reader's record and the scan's, and frees the scan's state as btendscan
 * does.
 */
bool
doltgres_entries_searches_end(const PgCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostOwnedHandle owned = doltgres_entries_searches_handle(scan);
	doltgres_OptionHandle_t reader;
	HostError	failed;

	if (!host_owned_TableDoltgresEntriesSearchesSearch_withOptionHandle(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io, owned,
																		doltgres_entries_searches_end_closure_0, NULL,
																		NULL, &reader, &failed) ||
		(reader.has && !doltgres_scan_scan_end(&io, reader.handle, &failed)) ||
		!host_owned_TableDoltgresEntriesSearchesSearch_end(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io, owned, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	pg_calls_index_scan_free_state(scan);
	return true;
}

/* doltgres_entries_searches_way returns whether a direction Postgres asks a scan to read in is backward, refusing no direction. */
static bool
doltgres_entries_searches_way(int32 direction, bool *result, PgPgError *error)
{
	switch (direction)
	{
		case ForwardScanDirection:
			*result = false;
			return true;
		case BackwardScanDirection:
			*result = true;
			return true;
		default:
			*error = doltgres_entries_searches_internal("an index scan was asked to read with no direction");
			return false;
	}
}

/* doltgres_entries_searches_key returns a key Postgres' preprocessing read, with its function. */
static AccessNbtutilsBtKey
doltgres_entries_searches_key(DgIndexKey read, AccessNbtutilsFunction func)
{
	AccessNbtutilsBtKey key = {.flags = (uint32) read.flags, .attno = read.attno, .strategy = read.strategy,
	.subtype = read.subtype, .collation = read.collation, .func = func};

	key.has_argument = ((uint32) read.flags & (ACCESS_NBTUTILS_SK_ISNULL | ACCESS_NBTUTILS_SK_BT_SKIP)) == 0;
	key.argument = key.has_argument ? read.argument : 0;
	std_vec_new(&key.members, sizeof(AccessNbtutilsBtKey), _Alignof(AccessNbtutilsBtKey));
	return key;
}

/* doltgres_entries_searches_function returns a function of a kind, by its key or column and its member. */
static AccessNbtutilsFunction
doltgres_entries_searches_function(AccessNbtutilsFunctionTag tag, size_t at, size_t member)
{
	AccessNbtutilsFunction function = {tag, at, member};

	return function;
}

/*
 * doltgres_entries_searches_preprocessed_frame_0_t is preprocessed's state
 * across its pad: its arguments and the keys and arrays it reads, while
 * live.
 */
typedef struct doltgres_entries_searches_preprocessed_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgIndexScan *scan;
	AccessNbtutilsPreprocessed result;
	bool		result_live;
	AccessNbtutilsPreprocessed *out;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_preprocessed_frame_0_t;

/* doltgres_entries_searches_preprocessed_body_0 reads the scan's preprocessed keys and arrays into the search's. */
static void
doltgres_entries_searches_preprocessed_body_0(void *frame_)
{
	doltgres_entries_searches_preprocessed_frame_0_t *frame = frame_;
	const PgIndexScan *scan = frame->scan;
	AccessNbtutilsPreprocessed *pre = &frame->result;
	DgPreprocessed summary;

	if (!pg_calls_index_scan_preprocess(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
										scan, &summary, frame->error))
		return;
	pre->qual_ok = summary.qual_ok;
	pre->orders = summary.orders;
	pre->skip_scan = summary.skip_scan;
	std_vec_with_capacity_Checked(&pre->keys, (size_t) summary.keys, sizeof(AccessNbtutilsBtKey),
								  _Alignof(AccessNbtutilsBtKey));
	std_vec_with_capacity_Checked(&pre->arrays, (size_t) summary.arrays, sizeof(AccessNbtutilsArrayKey),
								  _Alignof(AccessNbtutilsArrayKey));
	frame->result_live = true;
	for (int32 at = 0; at < summary.keys; at++)
	{
		DgIndexKey	read = pg_calls_index_scan_key(scan, at, -1);
		AccessNbtutilsBtKey header = doltgres_entries_searches_key(read,
																   doltgres_entries_searches_function(ACCESS_NBTUTILS_FUNCTION_KEY,
																									  (size_t) at, 0));

		if (((uint32) read.flags & ACCESS_NBTUTILS_SK_ROW_HEADER) != 0)
		{
			std_vec_with_capacity_Checked(&header.members, (size_t) Max(read.members, 0), sizeof(AccessNbtutilsBtKey),
										  _Alignof(AccessNbtutilsBtKey));
			for (int32 member = 0; member < read.members; member++)
			{
				AccessNbtutilsBtKey key = doltgres_entries_searches_key(pg_calls_index_scan_key(scan, at, member),
																		doltgres_entries_searches_function(ACCESS_NBTUTILS_FUNCTION_MEMBER,
																										   (size_t) at,
																										   (size_t) member));

				std_vec_push_Checked(&header.members, &key, sizeof(key), _Alignof(AccessNbtutilsBtKey));
			}
		}
		std_vec_push_Checked(&pre->keys, &header, sizeof(header), _Alignof(AccessNbtutilsBtKey));
	}
	for (int32 at = 0; at < summary.arrays; at++)
	{
		DgIndexArray read = pg_calls_index_scan_array(scan, at);
		AccessNbtutilsArrayKey array = {.scan_key = (size_t) read.scan_key, .null_elem = read.null_elem};

		array.has_elements = read.num_elems >= 0;
		if (array.has_elements)
		{
			std_vec_with_capacity_Checked(&array.elements, (size_t) read.num_elems, sizeof(uint64), _Alignof(uint64));
			for (int32 e = 0; e < read.num_elems; e++)
			{
				uint64		element = pg_calls_index_scan_element(scan, at, e);

				std_vec_push_Checked(&array.elements, &element, sizeof(uint64), _Alignof(uint64));
			}
		}
		array.has_sksup = read.skip_support;
		array.sksup.low_elem = read.skip_support ? read.low_elem : 0;
		array.sksup.high_elem = read.skip_support ? read.high_elem : 0;
		array.has_low_compare = read.low;
		if (read.low)
			array.low_compare = doltgres_entries_searches_key(pg_calls_index_scan_bound(scan, at, false),
															  doltgres_entries_searches_function(ACCESS_NBTUTILS_FUNCTION_LOW,
																								 (size_t) at, 0));
		array.has_high_compare = read.high;
		if (read.high)
			array.high_compare = doltgres_entries_searches_key(pg_calls_index_scan_bound(scan, at, true),
															   doltgres_entries_searches_function(ACCESS_NBTUTILS_FUNCTION_HIGH,
																								  (size_t) at, 0));
		std_vec_push_Checked(&pre->arrays, &array, sizeof(array), _Alignof(AccessNbtutilsArrayKey));
	}
	frame->result_live = false;
	*frame->out = *pre;
	frame->ok = true;
}

/* doltgres_entries_searches_preprocessed_pad_0 drops the keys and arrays read so far. */
static void
doltgres_entries_searches_preprocessed_pad_0(void *frame_)
{
	doltgres_entries_searches_preprocessed_frame_0_t *frame = frame_;

	if (frame->result_live)
		access_nbtutils_Preprocessed_drop(&frame->result);
	frame->result_live = false;
}

/*
 * doltgres_entries_searches_preprocessed returns a scan's keys as Postgres'
 * _bt_preprocess_keys preprocesses them in the scan's state, read into the
 * search's: each key with its function by its place, a row comparison's
 * members, and each array with its elements, a skip array's skip support,
 * and its bounds.
 */
static bool
doltgres_entries_searches_preprocessed(const PgCatalogCaps *caps, const PgIndexScan *scan,
									   AccessNbtutilsPreprocessed *result, PgPgError *error)
{
	doltgres_entries_searches_preprocessed_frame_0_t frame = {.caps = caps, .scan = scan, .out = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_preprocessed_body_0, doltgres_entries_searches_preprocessed_pad_0);
	doltgres_entries_searches_preprocessed_pad_0(&frame);
	return frame.ok;
}

/*
 * DoltgresEntriesSearchesScanCalls is ScanCalls: a scan's calls into
 * Postgres through the scan's state, and the scan's own long values (its
 * Probes), which its comparator reads by their names.
 */
typedef struct DoltgresEntriesSearchesScanCalls
{
	const PgIndexScan *scan;
	HostLockIoMutexVecProbe *probes;
} DoltgresEntriesSearchesScanCalls;

/* doltgres_entries_searches_ScanCalls_Names_name is ScanCalls' Names: BlobNames' name of a stored value. */
static void
doltgres_entries_searches_ScanCalls_Names_name(const void *self, const uint8 *stored, size_t stored_len,
											   uint8 result[32])
{
	(void) self;
	DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS.name(NULL, stored, stored_len, result);
}

/* doltgres_entries_searches_ScanCalls_Calls_call calls one of the scan's functions through Postgres. */
static bool
doltgres_entries_searches_ScanCalls_Calls_call(const void *self_, const PgCatalogCaps *caps,
											   AccessNbtutilsFunction function, uint32 collation, uint64 a, uint64 b,
											   uint64 *result, PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;
	int32		kind = 0;
	size_t		key = function.f0;
	size_t		member = 0;

	switch (function.tag)
	{
		case ACCESS_NBTUTILS_FUNCTION_KEY:
			kind = 0;
			break;
		case ACCESS_NBTUTILS_FUNCTION_MEMBER:
			kind = 1;
			member = function.f1;
			break;
		case ACCESS_NBTUTILS_FUNCTION_LOW:
			kind = 2;
			break;
		case ACCESS_NBTUTILS_FUNCTION_HIGH:
			kind = 3;
			break;
		case ACCESS_NBTUTILS_FUNCTION_ORDER:
			kind = 4;
			break;
		case ACCESS_NBTUTILS_FUNCTION_START:
			kind = 5;
			break;
	}
	return pg_calls_index_scan_call(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), self->scan, kind,
									(int32) key, (int32) member, collation, a, b, result, error);
}

/* doltgres_entries_searches_ScanCalls_Calls_copy copies a datum of a column into the scan's array memory. */
static bool
doltgres_entries_searches_ScanCalls_Calls_copy(const void *self_, const PgCatalogCaps *caps, size_t column,
											   uint64 datum, uint64 *result, PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;

	return pg_calls_index_scan_copy(pg_CatalogCaps_may_raise(caps), self->scan, (int32) column, datum, result, error);
}

/* doltgres_entries_searches_ScanCalls_Calls_free frees a datum of a column the scan's array memory holds. */
static void
doltgres_entries_searches_ScanCalls_Calls_free(const void *self_, size_t column, uint64 datum)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;

	pg_calls_index_scan_free(self->scan, (int32) column, datum);
}

/* doltgres_entries_searches_ScanCalls_Calls_step steps a skip array's datum, none past its type's end. */
static bool
doltgres_entries_searches_ScanCalls_Calls_step(const void *self_, const PgCatalogCaps *caps, size_t array,
											   uint64 datum, bool increment, bool *has_result, uint64 *result,
											   PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;
	DgSkipStep	stepped;

	if (!pg_calls_index_scan_skip_step(pg_CatalogCaps_may_raise(caps), self->scan, (int32) array, datum, increment,
									   &stepped, error))
		return false;
	*has_result = !stepped.overflow;
	if (!stepped.overflow)
		*result = stepped.datum;
	return true;
}

/* doltgres_entries_searches_ScanCalls_SearchCalls_start_order starts the scan's ordering of a column by a subtype. */
static bool
doltgres_entries_searches_ScanCalls_SearchCalls_start_order(const void *self_, const PgCatalogCaps *caps,
															size_t column, uint32 subtype, PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;

	return pg_calls_index_scan_start_order(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), self->scan,
										   (int32) column, subtype, error);
}

/* doltgres_entries_searches_ScanCalls_SearchCalls_bytes returns a key's argument's bytes, of a length. */
static bool
doltgres_entries_searches_ScanCalls_SearchCalls_bytes(const void *self_, const PgCatalogCaps *caps, uint64 argument,
													  int16 len, StdVec *result, PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;
	const uint8 *bytes;
	size_t		bytes_len;

	if (!pg_calls_index_scan_bytes(pg_CatalogCaps_may_raise(caps), self->scan, argument, (int32) len, &bytes,
								   &bytes_len, error))
		return false;
	std_slice_to_vec_Checked(result, bytes, bytes_len, 1, 1);
	return true;
}

/*
 * doltgres_entries_searches_probes_insert is BTreeMap<[u8; 32],
 * Vec<u8>>::insert on Probes' sorted Vec<Probe>: the value of a name, an
 * old one's dropped; it takes the value.
 */
static void
doltgres_entries_searches_probes_insert(StdVec *probes, const uint8 name[32], StdVec *stored)
{
	DoltgresLayoutProbe *at = probes->ptr;
	size_t		low = 0;
	size_t		high = probes->len;
	DoltgresLayoutProbe probe;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		int			compared = memcmp(at[mid].name, name, 32);

		if (compared == 0)
		{
			std_vec_free_Checked(&at[mid].bytes, 1, 1);
			at[mid].bytes = *stored;
			return;
		}
		if (compared < 0)
			low = mid + 1;
		else
			high = mid;
	}
	memcpy(probe.name, name, 32);
	probe.bytes = *stored;
	std_vec_push_Checked(probes, &probe, sizeof(DoltgresLayoutProbe), _Alignof(DoltgresLayoutProbe));
	at = probes->ptr;
	memmove(&at[low + 1], &at[low], (probes->len - 1 - low) * sizeof(DoltgresLayoutProbe));
	at[low] = probe;
}

/* doltgres_entries_searches_ScanCalls_SearchCalls_probe keeps a long value of the scan's by its name; it takes the value. */
static bool
doltgres_entries_searches_ScanCalls_SearchCalls_probe(const void *self_, const PgCatalogCaps *caps,
													  const uint8 name[32], StdVec *stored, PgPgError *error)
{
	const DoltgresEntriesSearchesScanCalls *self = self_;
	HostIoIo	io = pghost_io_catalog(caps);
	HostLockLockedRwLockWriteGuard guard;

	(void) error;
	guard = host_lock_IoMutex_lock((HostLockIoMutex *) self->probes, &DOLTGRES_LAYOUT_PROBES_LOCK_TYPE, &io.held);
	doltgres_entries_searches_probes_insert(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&guard), name, stored);
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	return true;
}

/* DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS is ScanCalls' SearchCalls, with its Calls and Names. */
const AccessNbtsearchSearchCallsOps DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS = {
	.calls = {doltgres_entries_searches_ScanCalls_Calls_call, doltgres_entries_searches_ScanCalls_Calls_copy,
	doltgres_entries_searches_ScanCalls_Calls_free, doltgres_entries_searches_ScanCalls_Calls_step},
	.names = {doltgres_entries_searches_ScanCalls_Names_name},
	.start_order = doltgres_entries_searches_ScanCalls_SearchCalls_start_order,
	.bytes = doltgres_entries_searches_ScanCalls_SearchCalls_bytes,
	.probe = doltgres_entries_searches_ScanCalls_SearchCalls_probe,
};

/* doltgres_entries_searches_attnums returns the table's attribute number of each of an index's columns, from 1, its key columns first. */
static void
doltgres_entries_searches_attnums(const PgRel *index, StdVec *result)
{
	DgIndexColumn columns[INDEX_MAX_KEYS] = {0};
	size_t		all = (size_t) (pg_calls_index_counts(index) & 0xffff);

	pg_calls_index_columns(index, columns);
	std_range_to_check(all, lengthof(columns), RUST_FILE, 546, 5);
	std_vec_with_capacity_Checked(result, all, sizeof(int32), _Alignof(int32));
	for (size_t at = 0; at < all; at++)
	{
		int32		attnum = columns[at].attnum;

		std_vec_push_Checked(result, &attnum, sizeof(int32), _Alignof(int32));
	}
}

/* state.rs' held at Option<Row>. */
#define TXN_STATE_TS OptionRow
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, DoltgresScanRow *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; DoltgresScanRow *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at Option<Base>, which entries.c defines. */
#define TXN_STATE_TS OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseBase *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* trees' pin at Result<Vec<Vec<u8>>, Error>, for splits. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultVecVecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* doltgres_entries_searches_bytes_drop drops a Vec<Vec<u8>>: its vectors, then it. */
static void
doltgres_entries_searches_bytes_drop(StdVec *bytes)
{
	for (size_t at = 0; at < bytes->len; at++)
		std_vec_free_Checked(&((StdVec *) bytes->ptr)[at], 1, 1);
	std_vec_free_Checked(bytes, sizeof(StdVec), _Alignof(StdVec));
}

/*
 * DoltgresEntriesSearchesKeyed is Keyed: what a primary key's index's scan
 * reads its table's rows through, the table, a slot of its rows, and the
 * table's attribute number of each of the index's columns (a Vec<i32>).
 */
typedef struct DoltgresEntriesSearchesKeyed
{
	const PgRel *heap;
	PgSlot	   *slot;
	StdVec		attnums;
} DoltgresEntriesSearchesKeyed;

/*
 * DoltgresEntriesSearchesEntries is Entries: what reads a scan's searches
 * through its reader's record under the scan's snapshot, the tree's order,
 * the table of a primary key's index, whether it decodes its entries'
 * INCLUDE columns too, what it keeps of the search it reads, and the row it
 * gave last with the values its decode gave (a Vec<Option<Datum>>).
 */
typedef struct DoltgresEntriesSearchesEntries
{
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgIndexScan *scan;
	const HostSnapshot *read;
	const DoltgresIndexDescribed *described;
	TreesKeyOrder order;
	bool		has_table;
	DoltgresEntriesSearchesKeyed table;
	bool		includes;
	HostOwnedHandle reader;
	DoltgresEntriesSearchesRange range;
	bool		has_row;
	DoltgresScanRow row;
	StdVec		decoded;
} DoltgresEntriesSearchesEntries;

/* doltgres_entries_searches_Entries_drop is Entries' drop glue: its order, its table's attnums, its range, its row, and its values. */
static void
doltgres_entries_searches_Entries_drop(DoltgresEntriesSearchesEntries *self)
{
	trees_KeyOrder_drop(&self->order);
	if (self->has_table)
		std_vec_free_Checked(&self->table.attnums, sizeof(int32), _Alignof(int32));
	doltgres_entries_searches_Range_drop(&self->range);
	if (self->has_row)
		doltgres_scan_Row_drop(&self->row);
	std_vec_free_Checked(&self->decoded, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
}

/* doltgres_entries_searches_found finds the state of the storage an entries' reads read, its table's or its index's. */
static bool
doltgres_entries_searches_found(const DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps, HostIoIo *io,
								TxnStateFound *result, PgPgError *error)
{
	if (self->has_table)
		return doltgres_entries_making(caps, io, self->table.heap, result, error);
	return doltgres_entries_index_making(io, pg_IndexScan_index(self->scan), result, error);
}

/* doltgres_entries_searches_Entries_raw_closure_0 is raw's lent call: the reader's next row under the scan's snapshot. */
static bool
doltgres_entries_searches_Entries_raw_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
												const HostSharedHeld *held, bool *some, DoltgresScanRow *result,
												HostError *error)
{
	const DoltgresEntriesSearchesEntries *self = captures;
	DoltgresScanUnder under = {.state = host_shared_Held_record(held), .snapshot = self->read};
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, repo};

	return doltgres_scan_next(io, &stores, &under, self->reader, self->range.way, some, result, error);
}

/* doltgres_entries_searches_Entries_raw reads the reader's next row in the search's way under the scan's snapshot, in its tree's byte order. */
static bool
doltgres_entries_searches_Entries_raw(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps, bool *has_result,
									  DoltgresScanRow *result, PgPgError *error)
{
	HostIoIo	io = pghost_io_catalog(caps);
	TxnStateFound found;
	HostError	failed;

	if (!doltgres_entries_searches_found(self, caps, &io, &found, error))
		return false;
	if (txn_state_held_OptionRow(&io, self->repo, self->reading, found, doltgres_entries_searches_Entries_raw_closure_0, self,
								 NULL, has_result, result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_searches_run_record returns a run's row as the run's sort
 * holds it: the bytes it sorts by, each zero byte doubled with 255, two zero
 * bytes, then the row's key, tuple, and value ids, each with its length as a
 * varint, its stamp's writer and tuple id, and its writer's top-level xid.
 */
static void
doltgres_entries_searches_run_record(const StdVec *whole, const DoltgresScanRow *row, StdVec *result)
{
	const StdVec *parts[3] = {&row->key, &row->tuple, &row->values};
	size_t		capacity = STD_ADD(usize, STD_ADD(usize, STD_ADD(usize, STD_MUL(usize, whole->len, 2, 751, 38),
																 row->key.len, 751, 38), row->tuple.len, 751, 38),
								   32, 751, 38);
	uint8		words[24];

	std_vec_with_capacity_Checked(result, capacity, 1, 1);
	for (size_t at = 0; at < whole->len; at++)
	{
		uint8		byte = ((const uint8 *) whole->ptr)[at];

		std_vec_u8_push_Checked(result, byte);
		if (byte == 0)
			std_vec_u8_push_Checked(result, 255);
	}
	std_vec_u8_push_Checked(result, 0);
	std_vec_u8_push_Checked(result, 0);
	for (int part = 0; part < 3; part++)
	{
		chunks_varint_put_varint(result, (uint64) parts[part]->len);
		std_vec_extend_from_slice_Checked(result, parts[part]->ptr, parts[part]->len, 1, 1);
	}
	for (int at = 0; at < 8; at++)
	{
		words[at] = (uint8) (row->stamp.txn >> (56 - 8 * at));
		words[8 + at] = (uint8) (row->stamp.tuple >> (56 - 8 * at));
		words[16 + at] = (uint8) (row->top >> (56 - 8 * at));
	}
	std_vec_extend_from_slice_Checked(result, words, sizeof(words), 1, 1);
}

/* doltgres_entries_searches_broken returns the error of a run's sort that gave back bytes it wasn't given. */
static PgPgError
doltgres_entries_searches_broken(void)
{
	return doltgres_entries_searches_internal("a run's sort gave back bytes it wasn't given");
}

/* doltgres_entries_searches_run_part reads back one part run_record wrote: its length as a varint, then its bytes. */
static bool
doltgres_entries_searches_run_part(const uint8 *bytes, size_t bytes_len, size_t *at, StdVec *result)
{
	ChunksVarintMalformed malformed;
	uint64		len;

	size_t		end;

	if (!chunks_varint_decode(bytes, bytes_len, at, &len, &malformed) || len > SIZE_MAX)
		return false;
	end = STD_ADD(usize, *at, (size_t) len, 786, 34);
	if (end > bytes_len)
		return false;
	std_slice_to_vec_Checked(result, bytes + *at, (size_t) len, 1, 1);
	*at = STD_ADD(usize, *at, (size_t) len, 787, 9);
	return true;
}

/* doltgres_entries_searches_run_word reads back a big-endian word run_record wrote at an offset, or false past its bytes. */
static bool
doltgres_entries_searches_run_word(const uint8 *bytes, size_t bytes_len, size_t at, uint64 *result)
{
	if (STD_ADD(usize, at, 8, 791, 42) > bytes_len)
		return false;
	*result = 0;
	for (int i = 0; i < 8; i++)
		*result = (*result << 8) | bytes[at + i];
	return true;
}

/*
 * doltgres_entries_searches_run_row reads back a row run_record wrote, with
 * its table's keyed layout and object id, refusing as an internal error
 * bytes it didn't write.
 */
static bool
doltgres_entries_searches_run_row(const uint8 *bytes, size_t bytes_len, const StdVec *keyed, uint32 table,
								  DoltgresScanRow *result, PgPgError *error)
{
	DoltgresScanRow row = {.table = table};
	size_t		at = 0;
	bool		ok = true;

	for (;;)
	{
		bool		has_first = at < bytes_len;
		bool		has_second = STD_ADD(usize, at, 1, 775, 41) < bytes_len;

		if (has_first && has_second && bytes[at] == 0 && bytes[at + 1] == 0)
			break;
		if (has_first && has_second && bytes[at] == 0 && bytes[at + 1] == 255)
			at = STD_ADD(usize, at, 2, 777, 37);
		else if (has_first && bytes[at] != 0)
			at = STD_ADD(usize, at, 1, 778, 46);
		else
		{
			*error = doltgres_entries_searches_broken();
			return false;
		}
	}
	at = STD_ADD(usize, at, 2, 782, 5);
	if (!doltgres_entries_searches_run_part(bytes, bytes_len, &at, &row.key))
		ok = false;
	else if (!doltgres_entries_searches_run_part(bytes, bytes_len, &at, &row.tuple))
	{
		std_vec_free_Checked(&row.key, 1, 1);
		ok = false;
	}
	else if (!doltgres_entries_searches_run_part(bytes, bytes_len, &at, &row.values))
	{
		std_vec_free_Checked(&row.tuple, 1, 1);
		std_vec_free_Checked(&row.key, 1, 1);
		ok = false;
	}
	if (ok && (!doltgres_entries_searches_run_word(bytes, bytes_len, at, &row.stamp.txn) ||
			   !doltgres_entries_searches_run_word(bytes, bytes_len, STD_ADD(usize, at, 8, 792, 63), &row.stamp.tuple) ||
			   !doltgres_entries_searches_run_word(bytes, bytes_len, STD_ADD(usize, at, 16, 793, 20), &row.top)))
	{
		std_vec_free_Checked(&row.values, 1, 1);
		std_vec_free_Checked(&row.tuple, 1, 1);
		std_vec_free_Checked(&row.key, 1, 1);
		ok = false;
	}
	if (!ok)
	{
		*error = doltgres_entries_searches_broken();
		return false;
	}
	std_vec_clone(&row.keyed, keyed, 1, 1);
	std_vec_new(&row.transform, 1, 1);
	*result = row;
	return true;
}

/*
 * doltgres_entries_searches_Entries_ordered_frame_0_t is ordered's state across its
 * pad: the run's prefix, its sort, its keyed layout, the row at hand and the
 * next one, a row's sort bytes and record, and the bytes the sort gives
 * back, each while live.
 */
typedef struct doltgres_entries_searches_Entries_ordered_frame_0_t
{
	DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	StdVec		prefix;
	bool		prefix_live;
	PgOwnedSort sort;
	bool		sort_live;
	StdVec		keyed;
	bool		keyed_live;
	DoltgresScanRow member;
	bool		member_live;
	DoltgresScanRow next;
	bool		next_live;
	StdVec		whole;
	bool		whole_live;
	StdVec		record;
	bool		record_live;
	StdVec		bytes;
	bool		bytes_live;
	bool	   *has_result;
	DoltgresScanRow *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_ordered_frame_0_t;

static bool doltgres_entries_searches_Entries_whole(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps,
													const DoltgresScanRow *row, StdVec *result, PgPgError *error);

/* doltgres_entries_searches_Entries_ordered_done gives ordered's answer: the run's front row, if any. */
static void
doltgres_entries_searches_Entries_ordered_done(doltgres_entries_searches_Entries_ordered_frame_0_t *frame)
{
	*frame->has_result = std_vec_deque_pop_front_Checked(&frame->self->range.run, frame->result,
														 sizeof(DoltgresScanRow));
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_ordered_body_0 reads the search's next row in the index's true order, sorting a run of cut keys whole. */
static void
doltgres_entries_searches_Entries_ordered_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_ordered_frame_0_t *frame = frame_;
	DoltgresEntriesSearchesEntries *self = frame->self;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DoltgresScanRow row;
	bool		has;
	size_t		len;
	uint32		table;

	if (std_vec_deque_pop_front_Checked(&self->range.run, frame->result, sizeof(DoltgresScanRow)))
	{
		*frame->has_result = true;
		frame->ok = true;
		return;
	}
	if (self->range.has_ahead)
	{
		self->range.has_ahead = false;
		if (!self->range.ahead_some)
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
		row = self->range.ahead;
	}
	else
	{
		if (!doltgres_entries_searches_Entries_raw(self, frame->caps, &has, &row, frame->error))
			return;
		if (!has)
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
	}
	frame->member = row;
	frame->member_live = true;
	if (!access_index_IndexKey_run_prefix(doltgres_index_Described_entry_key(self->described), row.key.ptr, row.key.len,
										  doltgres_index_Described_scan_bound(self->described), &has, &len,
										  frame->error))
		return;
	if (!has)
	{
		frame->member_live = false;
		*frame->result = frame->member;
		*frame->has_result = true;
		frame->ok = true;
		return;
	}
	std_range_to_check(len, frame->member.key.len, RUST_FILE, 611, 22);
	std_slice_to_vec_Checked(&frame->prefix, frame->member.key.ptr, len, 1, 1);
	frame->prefix_live = true;
	if (!pg_calls_sort_begin(raise, &frame->sort, frame->error))
		return;
	frame->sort_live = true;
	std_vec_clone(&frame->keyed, &frame->member.keyed, 1, 1);
	frame->keyed_live = true;
	table = frame->member.table;
	while (frame->member_live)
	{
		if (!doltgres_entries_searches_Entries_whole(self, frame->caps, &frame->member, &frame->whole, frame->error))
			return;
		frame->whole_live = true;
		doltgres_entries_searches_run_record(&frame->whole, &frame->member, &frame->record);
		frame->record_live = true;
		if (!pg_calls_sort_put(raise, pg_calls_sort_state(pg_OwnedSort_sort(&frame->sort)), frame->record.ptr,
							   frame->record.len, frame->error))
			return;
		frame->record_live = frame->whole_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
		std_vec_free_Checked(&frame->whole, 1, 1);
		if (!doltgres_entries_searches_Entries_raw(self, frame->caps, &has, &frame->next, frame->error))
			return;
		frame->next_live = has;
		frame->member_live = false;
		doltgres_scan_Row_drop(&frame->member);
		if (has && frame->next.key.len >= frame->prefix.len &&
			memcmp(frame->next.key.ptr, frame->prefix.ptr, frame->prefix.len) == 0)
		{
			frame->member = frame->next;
			frame->member_live = true;
			frame->next_live = false;
		}
		else
		{
			if (self->range.has_ahead && self->range.ahead_some)
				doltgres_scan_Row_drop(&self->range.ahead);
			self->range.has_ahead = true;
			self->range.ahead_some = has;
			if (has)
				self->range.ahead = frame->next;
			frame->next_live = false;
		}
	}
	if (!pg_calls_sort_performed(raise, catalog, pg_OwnedSort_sort(&frame->sort), frame->error))
		return;
	for (;;)
	{
		int64		sorted;
		DoltgresScanRow got;

		if (!pg_calls_sort_next_len(raise, catalog, pg_OwnedSort_sort(&frame->sort), &sorted, frame->error))
			return;
		if (sorted < 0)
			break;
		std_vec_u8_from_elem(&frame->bytes, 0, (size_t) sorted);
		frame->bytes_live = true;
		pg_calls_sort_read(pg_OwnedSort_sort(&frame->sort), frame->bytes.ptr, frame->bytes.len);
		if (!doltgres_entries_searches_run_row(frame->bytes.ptr, frame->bytes.len, &frame->keyed, table, &got,
											   frame->error))
			return;
		frame->bytes_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		if (self->range.way == DOLTGRES_SCAN_DIRECTION_FORWARD)
			std_vec_deque_push_back_Checked(&self->range.run, &got, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
		else
			std_vec_deque_push_front_Checked(&self->range.run, &got, sizeof(DoltgresScanRow),
											 _Alignof(DoltgresScanRow));
	}
	doltgres_entries_searches_Entries_ordered_done(frame);
}

/* doltgres_entries_searches_Entries_ordered_pad_0 drops what ordered holds. */
static void
doltgres_entries_searches_Entries_ordered_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_ordered_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->record_live)
		std_vec_free_Checked(&frame->record, 1, 1);
	if (frame->whole_live)
		std_vec_free_Checked(&frame->whole, 1, 1);
	if (frame->next_live)
		doltgres_scan_Row_drop(&frame->next);
	if (frame->member_live)
		doltgres_scan_Row_drop(&frame->member);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->sort_live)
		pg_OwnedSort_drop(&frame->sort);
	if (frame->prefix_live)
		std_vec_free_Checked(&frame->prefix, 1, 1);
	frame->bytes_live = frame->record_live = frame->whole_live = frame->next_live = frame->member_live = false;
	frame->keyed_live = frame->sort_live = frame->prefix_live = false;
}

/*
 * doltgres_entries_searches_Entries_ordered reads the search's next row in the
 * index's true order: a row of no run of cut keys as the tree gives it, and
 * a run's rows, read whole when its first row is reached and sorted by their
 * columns' whole encodings through Postgres' tuplesort, then given one at a
 * time in the search's way.
 */
static bool
doltgres_entries_searches_Entries_ordered(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps, bool *has_result,
										  DoltgresScanRow *result, PgPgError *error)
{
	doltgres_entries_searches_Entries_ordered_frame_0_t frame = {.self = self, .caps = caps, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_ordered_body_0, doltgres_entries_searches_Entries_ordered_pad_0);
	doltgres_entries_searches_Entries_ordered_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_corrupt returns the error of an index entry's value that is corrupt. */
static PgPgError
doltgres_entries_searches_corrupt(void)
{
	static const char corrupt[] = "an index entry's value is corrupt";
	StdString	owned;

	std_string_from_Checked(&owned, corrupt, sizeof(corrupt) - 1);
	return pg_PgError_fork((const uint8 *) "XX001", &owned);
}

/* doltgres_entries_searches_field_closure_1 is field's blob: a value's blob read whole by its address. */
static bool
doltgres_entries_searches_field_closure_1(HostIoIo *io, TxnBlobsBlobs *blobs, StdBytes address, StdVec *result,
										  PgPgError *error)
{
	TreesStoreTree tree;
	ChunksErrorError failed;

	if (address.len != 32)
	{
		*error = doltgres_entries_searches_corrupt();
		return false;
	}
	tree = doltgres_entries_chunk_tree(address.ptr);
	if (trees_blob_read_blob(io, TXN_BLOBS_BLOBS_NODE_STORE(blobs), TREES_STORE_TREE_NAMED(&tree), result, &failed))
		return true;
	*error = doltgres_entries_storage_error(&failed);
	return false;
}

/*
 * doltgres_entries_searches_field returns a field of an entry's value in its
 * stored form, or None for NULL: inline, out of line in its blob, or in the
 * value's overflow chunk, which may keep it in a further one.
 */
static bool
doltgres_entries_searches_field(HostIoIo *io, TxnBlobsBlobs *blobs, const EncodingTupleTuple *tuple, size_t at,
								access_OptionVecU8_t *result, PgPgError *error)
{
	EncodingTupleField field = encoding_tuple_Tuple_field(tuple, at);
	StdBytes	address;
	size_t		overflow;
	StdVec		chunk;
	EncodingTupleTuple chunked;
	bool		ok;

	switch (field.tag)
	{
		case ENCODING_TUPLE_FIELD_NULL:
			result->has = false;
			return true;
		case ENCODING_TUPLE_FIELD_VALUE:
			result->has = true;
			std_slice_to_vec_Checked(&result->bytes, field.u.value.ptr, field.u.value.len, 1, 1);
			return true;
		case ENCODING_TUPLE_FIELD_EXTERNAL:
			result->has = true;
			return doltgres_entries_searches_field_closure_1(io, blobs, field.u.external.address, &result->bytes,
															 error);
		default:
			break;
	}
	if (!encoding_tuple_Tuple_overflow(tuple, &address, &overflow))
	{
		*error = doltgres_entries_searches_corrupt();
		return false;
	}
	if (!doltgres_entries_searches_field_closure_1(io, blobs, address, &chunk, error))
		return false;
	if (!encoding_tuple_Tuple_parse(chunk.ptr, chunk.len, &chunked))
	{
		std_vec_free_Checked(&chunk, 1, 1);
		*error = doltgres_entries_searches_corrupt();
		return false;
	}
	ok = doltgres_entries_searches_field(io, blobs, &chunked, field.u.overflow, result, error);
	std_vec_free_Checked(&chunk, 1, 1);
	return ok;
}

/* doltgres_entries_searches_fields_drop drops a Vec<Option<Vec<u8>>>: each field's bytes, then it. */
static void
doltgres_entries_searches_fields_drop(StdVec *fields)
{
	for (size_t at = 0; at < fields->len; at++)
		if (((access_OptionVecU8_t *) fields->ptr)[at].has)
			std_vec_free_Checked(&((access_OptionVecU8_t *) fields->ptr)[at].bytes, 1, 1);
	std_vec_free_Checked(fields, sizeof(access_OptionVecU8_t), _Alignof(access_OptionVecU8_t));
}

/*
 * doltgres_entries_searches_Entries_fields_frame_0_t is fields' state across its pad:
 * the blobs it reads through and the fields it read, each while live.
 */
typedef struct doltgres_entries_searches_Entries_fields_frame_0_t
{
	const DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	const DoltgresScanRow *row;
	TxnBlobsBlobs blobs;
	bool		blobs_live;
	StdVec	   *result;
	bool		result_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_fields_frame_0_t;

/* doltgres_entries_searches_Entries_fields_body_0 reads each field of the row's value through the session's repository. */
static void
doltgres_entries_searches_Entries_fields_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_fields_frame_0_t *frame = frame_;
	HostIoIo	io = pghost_io_catalog(frame->caps);
	EncodingTupleTuple tuple;
	TreesStoreReader packs;
	HostError	failed;
	uint32		database;
	uint32		tablespace;

	if (!encoding_tuple_Tuple_parse(frame->row->tuple.ptr, frame->row->tuple.len, &tuple))
	{
		*frame->error = doltgres_entries_searches_corrupt();
		return;
	}
	packs = repo_Repo_reader(frame->self->repo, HOST_CACHE_PLACEMENT_SERVER, frame->self->reading);
	if (!host_session_database(&database, &tablespace, &failed) ||
		!txn_blobs_Blobs_new(&io, database, TREES_STORE_READER_NODE_STORE(&packs), &frame->blobs, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->blobs_live = true;
	std_vec_with_capacity_Checked(frame->result, encoding_tuple_Tuple_len(&tuple), sizeof(access_OptionVecU8_t),
								  _Alignof(access_OptionVecU8_t));
	frame->result_live = true;
	for (size_t at = 0; at < encoding_tuple_Tuple_len(&tuple); at++)
	{
		access_OptionVecU8_t field;

		if (!doltgres_entries_searches_field(&io, &frame->blobs, &tuple, at, &field, frame->error))
			return;
		std_vec_push_Checked(frame->result, &field, sizeof(field), _Alignof(access_OptionVecU8_t));
	}
	frame->result_live = false;
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_fields_pad_0 drops the fields of a failed read, then the blobs. */
static void
doltgres_entries_searches_Entries_fields_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_fields_frame_0_t *frame = frame_;

	if (frame->result_live)
		doltgres_entries_searches_fields_drop(frame->result);
	if (frame->blobs_live)
		txn_blobs_Blobs_drop(&frame->blobs);
	frame->result_live = frame->blobs_live = false;
}

/*
 * doltgres_entries_searches_Entries_fields returns an entry's value's fields in
 * their stored form, None for NULL, those out of line read through the
 * session's repository.
 */
static bool
doltgres_entries_searches_Entries_fields(const DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps,
										 const DoltgresScanRow *row, StdVec *result, PgPgError *error)
{
	doltgres_entries_searches_Entries_fields_frame_0_t frame = {.self = self, .caps = caps, .row = row, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_fields_body_0, doltgres_entries_searches_Entries_fields_pad_0);
	doltgres_entries_searches_Entries_fields_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_decoded_drop drops a Vec<Option<Decoded>>: each value's bytes, then it. */
static void
doltgres_entries_searches_decoded_drop(StdVec *decoded)
{
	std_vec_drop_Checked(decoded, sizeof(access_OptionDecoded_t), _Alignof(access_OptionDecoded_t),
						 access_index_decoded_drop);
}

/*
 * doltgres_entries_searches_Entries_whole_frame_0_t is whole's state across its pad:
 * the scan's columns, the key columns' originals, the fields they are
 * decoded from, and their values, each while live.
 */
typedef struct doltgres_entries_searches_Entries_whole_frame_0_t
{
	DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	const DoltgresScanRow *row;
	StdVec		columns;
	bool		columns_live;
	StdVec		originals;
	bool		originals_live;
	StdVec		fields;
	bool		fields_live;
	StdVec		values;
	bool		values_live;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_whole_frame_0_t;

/* doltgres_entries_searches_Entries_whole_body_0 returns the bytes a row of a run of cut keys sorts by, from its key columns' values. */
static void
doltgres_entries_searches_Entries_whole_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_whole_frame_0_t *frame = frame_;
	DoltgresEntriesSearchesEntries *self = frame->self;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const AccessIndexIndexKey *entry_key = doltgres_index_Described_entry_key(self->described);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};

	if (self->has_table)
	{
		DoltgresEntriesSearchesKeyed *table = &self->table;

		if (!doltgres_entries_stored(raise, table->slot, table->heap, frame->row, frame->error) ||
			!pg_calls_slot_getallattrs(raise, table->slot, frame->error))
			return;
		doltgres_index_Described_scan_columns(self->described, &frame->columns);
		frame->columns_live = true;
		std_vec_with_capacity_Checked(&frame->originals, frame->columns.len, sizeof(access_OptionDecoded_t),
									  _Alignof(access_OptionDecoded_t));
		frame->originals_live = true;
		for (size_t at = 0; at < table->attnums.len && at < frame->columns.len; at++)
		{
			const AccessNbtsearchKeyColumn *column = &((const AccessNbtsearchKeyColumn *) frame->columns.ptr)[at];
			int32		attnum = STD_SUB(i32, ((const int32 *) table->attnums.ptr)[at], 1, 650, 30);
			access_OptionDecoded_t original = {0};

			if (!pg_calls_slot_value_null(table->slot, attnum) && column->by_value)
			{
				original.has = true;
				original.value.tag = ACCESS_KEY_DECODED_WORD;
				original.value.u.word = pg_calls_slot_value_word(table->slot, attnum);
			}
			else if (!pg_calls_slot_value_null(table->slot, attnum))
			{
				const uint8 *bytes;
				size_t		len;

				if (!pg_calls_slot_value_bytes(raise, pg_CatalogCaps_catalog(frame->caps), table->slot, attnum,
											   &bytes, &len, frame->error))
					return;
				original.has = true;
				original.value.tag = ACCESS_KEY_DECODED_BYTES;
				std_slice_to_vec_Checked(&original.value.u.bytes, bytes, len, 1, 1);
			}
			std_vec_push_Checked(&frame->originals, &original, sizeof(original), _Alignof(access_OptionDecoded_t));
		}
	}
	else
	{
		if (!doltgres_entries_searches_Entries_fields(self, frame->caps, frame->row, &frame->fields, frame->error))
			return;
		frame->fields_live = true;
		if (!access_index_IndexKey_decode(entry_key, frame->row->key.ptr, frame->row->key.len, false,
										  frame->fields.ptr, frame->fields.len, &frame->originals, frame->error))
			return;
		frame->originals_live = true;
	}
	std_vec_with_capacity_Checked(&frame->values, frame->originals.len, sizeof(access_OptionValue_t),
								  _Alignof(access_OptionValue_t));
	frame->values_live = true;
	for (size_t at = 0; at < frame->originals.len; at++)
	{
		const access_OptionDecoded_t *original = &((const access_OptionDecoded_t *) frame->originals.ptr)[at];
		access_OptionValue_t value = {.has = original->has};

		if (original->has && original->value.tag == ACCESS_KEY_DECODED_WORD)
		{
			value.value.tag = ACCESS_KEY_VALUE_WORD;
			value.value.u.word = original->value.u.word;
		}
		else if (original->has)
		{
			value.value.tag = ACCESS_KEY_VALUE_BYTES;
			value.value.u.bytes.ptr = original->value.u.bytes.ptr;
			value.value.u.bytes.len = original->value.u.bytes.len;
		}
		std_vec_push_Checked(&frame->values, &value, sizeof(value), _Alignof(access_OptionValue_t));
	}
	frame->ok = access_index_IndexKey_whole(entry_key, sorter, frame->values.ptr, frame->values.len,
											frame->row->key.ptr, frame->row->key.len,
											doltgres_index_Described_scan_bound(self->described), frame->result,
											frame->error);
}

/* doltgres_entries_searches_Entries_whole_pad_0 drops the values, the fields, the originals, and the columns. */
static void
doltgres_entries_searches_Entries_whole_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_whole_frame_0_t *frame = frame_;

	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(access_OptionValue_t), _Alignof(access_OptionValue_t));
	if (frame->originals_live)
		doltgres_entries_searches_decoded_drop(&frame->originals);
	if (frame->fields_live)
		doltgres_entries_searches_fields_drop(&frame->fields);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, sizeof(AccessNbtsearchKeyColumn), _Alignof(AccessNbtsearchKeyColumn));
	frame->values_live = frame->originals_live = frame->fields_live = frame->columns_live = false;
}

/*
 * doltgres_entries_searches_Entries_whole returns the bytes a row of a run of cut
 * keys sorts by, from its key columns' values: an index entry's as its
 * decode gives them, and a primary key's index's row's from the table's
 * slot.
 */
static bool
doltgres_entries_searches_Entries_whole(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps,
										const DoltgresScanRow *row, StdVec *result, PgPgError *error)
{
	doltgres_entries_searches_Entries_whole_frame_0_t frame = {.self = self, .caps = caps, .row = row, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_whole_body_0, doltgres_entries_searches_Entries_whole_pad_0);
	doltgres_entries_searches_Entries_whole_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_bound_of returns a bound of a tree's keys at a copy of a key, included or excluded. */
static access_BoundVecU8_t
doltgres_entries_searches_bound_of(StdBoundTag tag, const StdVec *key)
{
	access_BoundVecU8_t bound = {.tag = tag};

	std_vec_clone(&bound.key, key, 1, 1);
	return bound;
}

/*
 * doltgres_entries_searches_Entries_unit_frame_0_t is unit's state across its pad:
 * the split keys the scan's shared memory holds, while live.
 */
typedef struct doltgres_entries_searches_Entries_unit_frame_0_t
{
	DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	bool		backward;
	StdVec		splits;
	bool		splits_live;
	StdVec		keys;
	bool		keys_live;
	bool	   *has_result;
	AccessNbtsearchKeyRange *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_unit_frame_0_t;

static bool doltgres_entries_searches_Entries_splits(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps,
													 StdVec *result, PgPgError *error);

/* doltgres_entries_searches_Entries_unit_body_0 takes the parallel scan's next unit of the tree's keys in the scan's way. */
static void
doltgres_entries_searches_Entries_unit_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_unit_frame_0_t *frame = frame_;
	const PgIndexScan *scan = frame->self->scan;
	const uint8 *bytes;
	size_t		bytes_len;
	size_t		taken;
	size_t		units;
	size_t		unit;
	HostError	failed;
	const StdVec *keys;

	if (!pg_calls_index_parallel_based(scan))
	{
		if (!doltgres_entries_searches_Entries_splits(frame->self, frame->caps, &frame->splits, frame->error))
			return;
		frame->splits_live = true;
		pg_calls_index_parallel_share(scan, frame->splits.ptr, frame->splits.len);
		frame->splits_live = false;
		std_vec_free_Checked(&frame->splits, 1, 1);
	}
	bytes = pg_calls_index_parallel_splits(scan, &bytes_len);
	if (!doltgres_scan_split_keys(bytes, bytes_len, &frame->keys, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->keys_live = true;
	keys = frame->keys.ptr;
	taken = (size_t) pg_calls_index_parallel_next(scan);
	units = STD_ADD(usize, frame->keys.len, 1, 689, 21);
	if (taken >= units)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	unit = frame->backward ? STD_SUB(usize, STD_SUB(usize, units, 1, 693, 34), taken, 693, 34) : taken;
	frame->result->f0.tag = STD_BOUND_UNBOUNDED;
	if (unit != 0)
	{
		size_t		before = STD_SUB(usize, unit, 1, 696, 42);

		std_index_check(before, frame->keys.len, RUST_FILE, 696, 37);
		frame->result->f0 = doltgres_entries_searches_bound_of(STD_BOUND_INCLUDED, &keys[before]);
	}
	frame->result->f1.tag = STD_BOUND_UNBOUNDED;
	if (unit < frame->keys.len)
		frame->result->f1 = doltgres_entries_searches_bound_of(STD_BOUND_EXCLUDED, &keys[unit]);
	*frame->has_result = true;
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_unit_pad_0 drops the split keys. */
static void
doltgres_entries_searches_Entries_unit_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_unit_frame_0_t *frame = frame_;

	if (frame->keys_live)
		doltgres_entries_searches_bytes_drop(&frame->keys);
	if (frame->splits_live)
		std_vec_free_Checked(&frame->splits, 1, 1);
	frame->keys_live = frame->splits_live = false;
}

/*
 * doltgres_entries_searches_Entries_unit takes a parallel scan's next unit of the
 * tree's keys in the scan's way, or none once every unit is taken: the units
 * are the ranges between the split keys a participant stored in the scan's
 * shared memory, the first participant to need them storing its own.
 */
static bool
doltgres_entries_searches_Entries_unit(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps, bool backward,
									   bool *has_result, AccessNbtsearchKeyRange *result, PgPgError *error)
{
	doltgres_entries_searches_Entries_unit_frame_0_t frame = {.self = self, .caps = caps, .backward = backward,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_unit_body_0, doltgres_entries_searches_Entries_unit_pad_0);
	doltgres_entries_searches_Entries_unit_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_Entries_splits_closure_0 is a read's lent call: the base the scan's snapshot reads. */
static bool
doltgres_entries_searches_Entries_splits_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
												   const HostSharedHeld *held, bool *some, TxnBaseBase *result,
												   HostError *error)
{
	(void) repo;
	return txn_state_readable(io, held, captures, some, result, error);
}

/*
 * doltgres_entries_searches_Entries_splits_closure_2_frame_0_t is the state of splits' pin
 * scope across its pad: the tree, the counts, the cursor, and the keys, each
 * while live.
 */
typedef struct doltgres_entries_searches_Entries_splits_closure_2_frame_0_t
{
	TreesStoreTree tree;
	uint64		entries;
	uint64		units;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	StdVec	   *result;
	bool		result_live;
	ChunksErrorError *error;
	bool		ok;
} doltgres_entries_searches_Entries_splits_closure_2_frame_0_t;

/* doltgres_entries_searches_Entries_splits_closure_2_body_0 reads the tree's keys at evenly spaced ordinals. */
static void
doltgres_entries_searches_Entries_splits_closure_2_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_splits_closure_2_frame_0_t *frame = frame_;

	std_vec_new(frame->result, sizeof(StdVec), _Alignof(StdVec));
	frame->result_live = true;
	for (uint64 unit = 1; unit < frame->units; unit++)
	{
		StdU128		at = std_div_u128(std_mul_u128(std_u128_from_u64(unit), std_u128_from_u64(frame->entries),
												   RUST_FILE, 726, 26),
									  std_u128_from_u64(frame->units), RUST_FILE, 726, 26);
		StdBytes	key;
		StdVec		owned;

		if (!trees_cursor_Cursor_at_ordinal(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->tree), at.lo,
											&frame->cursor, frame->error))
			return;
		frame->cursor_live = true;
		key = trees_cursor_Cursor_key(&frame->cursor);
		std_slice_to_vec_Checked(&owned, key.ptr, key.len, 1, 1);
		std_vec_push_Checked(frame->result, &owned, sizeof(StdVec), _Alignof(StdVec));
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
	}
	frame->result_live = false;
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_splits_closure_2_pad_0 drops the cursor and the keys of a failed read. */
static void
doltgres_entries_searches_Entries_splits_closure_2_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_splits_closure_2_frame_0_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	if (frame->result_live)
		doltgres_entries_searches_bytes_drop(frame->result);
	frame->cursor_live = frame->result_live = false;
}

/* doltgres_entries_searches_Entries_splits_closure_2 is splits' pin scope: the tree's keys at evenly spaced ordinals. */
static bool
doltgres_entries_searches_Entries_splits_closure_2(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result,
												   ChunksErrorError *error)
{
	doltgres_entries_searches_Entries_splits_closure_2_frame_0_t *frame = captures;

	frame->io = io;
	frame->pin = pin;
	frame->result = result;
	frame->error = error;
	std_landing(frame, doltgres_entries_searches_Entries_splits_closure_2_body_0, doltgres_entries_searches_Entries_splits_closure_2_pad_0);
	doltgres_entries_searches_Entries_splits_closure_2_pad_0(frame);
	return frame->ok;
}

/*
 * doltgres_entries_searches_Entries_splits_frame_0_t is splits' state across
 * its pad: the keys read and taken from keys_at on, and the starts, each
 * while live.
 */
typedef struct doltgres_entries_searches_Entries_splits_frame_0_t
{
	DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	StdVec		keys;
	bool		keys_live;
	size_t		keys_at;
	StdVec		key;
	bool		key_live;
	StdVec		starts;
	bool		starts_live;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_splits_frame_0_t;

/* doltgres_entries_searches_Entries_splits_body_0 returns the split keys of the parallel scan's units. */
static void
doltgres_entries_searches_Entries_splits_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_splits_frame_0_t *frame = frame_;
	DoltgresEntriesSearchesEntries *self = frame->self;
	doltgres_entries_searches_Entries_splits_closure_2_frame_0_t scope = {0};
	HostIoIo	io = pghost_io_catalog(frame->caps);
	TxnStateFound found;
	TxnBaseBase base;
	TreesStoreReader store;
	ChunksErrorError storage;
	HostError	failed;
	bool		some;
	bool		rooted;
	size_t		bound;

	if (!doltgres_entries_searches_found(self, frame->caps, &io, &found, frame->error))
		return;
	if (!txn_state_held_OptionBase(&io, self->repo, self->reading, found, doltgres_entries_searches_Entries_splits_closure_0,
								   (void *) self->read, NULL, &some, &base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	rooted = some && base.has_root;
	if (rooted)
		scope.tree = base.root;
	if (some)
		txn_base_Base_drop(&base);
	if (!rooted)
	{
		std_vec_new(frame->result, 1, 1);
		frame->ok = true;
		return;
	}
	store = repo_Repo_reader(self->repo, HOST_CACHE_PLACEMENT_SERVER, self->reading);
	if (!trees_count(&io, TREES_STORE_READER_NODE_STORE(&store), TREES_STORE_TREE_NAMED(&scope.tree), &scope.entries,
					 &storage))
	{
		*frame->error = doltgres_entries_storage_error(&storage);
		return;
	}
	scope.units = Max(Min(scope.entries, (uint64) DOLTGRES_SCAN_SPLITS_MOST + 1), 1);
	if (!trees_pin_pin_ResultVecVecU8(&io, TREES_STORE_READER_NODE_STORE(&store),
									  doltgres_entries_searches_Entries_splits_closure_2, &scope, NULL, &frame->keys, &storage))
	{
		*frame->error = doltgres_entries_storage_error(&storage);
		return;
	}
	frame->keys_live = true;
	bound = doltgres_index_Described_scan_bound(self->described);
	std_vec_with_capacity_Checked(&frame->starts, frame->keys.len, sizeof(StdVec), _Alignof(StdVec));
	frame->starts_live = true;
	while (frame->keys_at < frame->keys.len)
	{
		const StdVec *last;
		bool		has;
		size_t		len;

		frame->key = ((StdVec *) frame->keys.ptr)[frame->keys_at++];
		frame->key_live = true;
		if (!access_index_IndexKey_run_prefix(doltgres_index_Described_entry_key(self->described), frame->key.ptr,
											  frame->key.len, bound, &has, &len, frame->error))
			return;
		if (has)
		{
			StdVec		start;

			std_range_to_check(len, frame->key.len, RUST_FILE, 736, 30);
			std_slice_to_vec_Checked(&start, frame->key.ptr, len, 1, 1);
			std_vec_free_Checked(&frame->key, 1, 1);
			frame->key = start;
		}
		last = frame->starts.len > 0 ? &((const StdVec *) frame->starts.ptr)[frame->starts.len - 1] : NULL;
		if (last == NULL ||
			std_bytes_Ord_cmp(last->ptr, last->len, frame->key.ptr, frame->key.len) == STD_ORDERING_LESS)
			std_vec_push_Checked(&frame->starts, &frame->key, sizeof(StdVec), _Alignof(StdVec));
		else
			std_vec_free_Checked(&frame->key, 1, 1);
		frame->key_live = false;
	}
	doltgres_scan_splits(frame->starts.ptr, frame->starts.len, frame->result);
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_splits_pad_0 drops the starts, the key at hand, and the keys not taken. */
static void
doltgres_entries_searches_Entries_splits_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_splits_frame_0_t *frame = frame_;

	if (frame->starts_live)
		doltgres_entries_searches_bytes_drop(&frame->starts);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->keys_live)
	{
		while (frame->keys_at < frame->keys.len)
			std_vec_free_Checked(&((StdVec *) frame->keys.ptr)[frame->keys_at++], 1, 1);
		std_vec_free_Checked(&frame->keys, sizeof(StdVec), _Alignof(StdVec));
	}
	frame->starts_live = frame->key_live = frame->keys_live = false;
}

/*
 * doltgres_entries_searches_Entries_splits returns the split keys of a parallel
 * scan's units, as scan's splits stores them: the keys of the tree the
 * scan's snapshot reads at evenly spaced ordinals, each a run of cut keys'
 * prefix when it falls inside one, none for an empty tree.
 */
static bool
doltgres_entries_searches_Entries_splits(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps, StdVec *result,
										 PgPgError *error)
{
	doltgres_entries_searches_Entries_splits_frame_0_t frame = {.self = self, .caps = caps, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_splits_body_0, doltgres_entries_searches_Entries_splits_pad_0);
	doltgres_entries_searches_Entries_splits_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_skip_drop drops a range's skip and leaves it none. */
static void
doltgres_entries_searches_skip_drop(DoltgresEntriesSearchesRange *range)
{
	if (range->has_skip && range->skip.tag == DOLTGRES_ENTRIES_SEARCHES_SKIP_KEY)
		std_vec_free_Checked(&range->skip.u.key, 1, 1);
	else if (range->has_skip)
		access_nbtsearch_Position_drop(&range->skip.u.through);
	range->has_skip = false;
}

/*
 * doltgres_entries_searches_Entries_Reader_search is Entries' Reader search:
 * it seeks the reader to the search's near bound, past its key for an
 * excluded one, or starts it at the index's end the search goes from,
 * keeping the far bound the search stops at; a search that resumes from a
 * position passes over every row up to that position's, from the start of
 * its run of cut keys when it is in one.
 */
static bool
doltgres_entries_searches_Entries_Reader_search(void *self_, const PgCatalogCaps *caps,
												const access_BoundVecU8_t *lower, const access_BoundVecU8_t *upper,
												bool backward, const AccessNbtsearchPosition *after,
												PgPgError *error)
{
	DoltgresEntriesSearchesEntries *self = self_;
	DoltgresEntriesSearchesRange *range = &self->range;
	HostIoIo	io = pghost_io_catalog(caps);
	DoltgresScanDirection way = backward ? DOLTGRES_SCAN_DIRECTION_BACKWARD : DOLTGRES_SCAN_DIRECTION_FORWARD;
	const access_BoundVecU8_t *near = backward ? upper : lower;
	const access_BoundVecU8_t *far_bound = backward ? lower : upper;
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, self->repo};
	bool		has_at = true;
	StdVec		at;
	HostError	failed;
	bool		ok;

	range->way = way;
	access_nbtsearch_Bound_drop(&range->far_end);
	access_nbtsearch_Bound_Clone_clone(far_bound, &range->far_end);
	std_vec_deque_drop(&range->run, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
					   doltgres_entries_searches_Row_drop);
	std_vec_deque_new(&range->run, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	if (range->has_ahead && range->ahead_some)
		doltgres_scan_Row_drop(&range->ahead);
	range->has_ahead = false;
	if (after != NULL)
	{
		bool		has;
		size_t		len;

		doltgres_entries_searches_skip_drop(range);
		range->skip.tag = DOLTGRES_ENTRIES_SEARCHES_SKIP_THROUGH;
		access_nbtsearch_Position_Clone_clone(after, &range->skip.u.through);
		range->has_skip = true;
		if (!access_index_IndexKey_run_prefix(doltgres_index_Described_entry_key(self->described), after->key.ptr,
											  after->key.len, doltgres_index_Described_scan_bound(self->described),
											  &has, &len, error))
			return false;
		if (has && way == DOLTGRES_SCAN_DIRECTION_FORWARD)
		{
			std_range_to_check(len, after->key.len, RUST_FILE, 832, 56);
			std_slice_to_vec_Checked(&at, after->key.ptr, len, 1, 1);
		}
		else if (has)
		{
			std_range_to_check(len, after->key.len, RUST_FILE, 834, 31);
			if (!access_nbtsearch_past(after->key.ptr, len, doltgres_index_Described_compared(self->described), &at))
				std_vec_clone(&at, &after->key, 1, 1);
		}
		else
			std_vec_clone(&at, &after->key, 1, 1);
	}
	else
	{
		doltgres_entries_searches_skip_drop(range);
		has_at = near->tag != STD_BOUND_UNBOUNDED;
		if (near->tag == STD_BOUND_EXCLUDED)
		{
			range->skip.tag = DOLTGRES_ENTRIES_SEARCHES_SKIP_KEY;
			std_vec_clone(&range->skip.u.key, &near->key, 1, 1);
			range->has_skip = true;
		}
		if (has_at)
			std_vec_clone(&at, &near->key, 1, 1);
	}
	if (has_at)
	{
		ok = doltgres_scan_seek(&io, &stores, self->reader, at.ptr, at.len, way, &failed);
		std_vec_free_Checked(&at, 1, 1);
	}
	else
		ok = doltgres_scan_scan_rescan(&io, self->reader, &failed);
	if (!ok)
		*error = pghost_raised(&failed);
	return ok;
}

/*
 * doltgres_entries_searches_Entries_Reader_next is Entries' Reader next: the
 * search's next entry the scan's snapshot sees in the index's order, past
 * what the search starts past, as its position, or none past the far bound
 * or the index's end.
 */
static bool
doltgres_entries_searches_Entries_Reader_next(void *self_, const PgCatalogCaps *caps, bool *has_result,
											  AccessNbtsearchPosition *result, PgPgError *error)
{
	DoltgresEntriesSearchesEntries *self = self_;
	DoltgresEntriesSearchesRange *range = &self->range;

	for (;;)
	{
		DoltgresScanRow row;
		bool		has;
		bool		beyond = false;

		if (!doltgres_entries_searches_Entries_ordered(self, caps, &has, &row, error))
			return false;
		if (!has)
		{
			*has_result = false;
			return true;
		}
		if (range->has_skip && range->skip.tag == DOLTGRES_ENTRIES_SEARCHES_SKIP_KEY &&
			range->skip.u.key.len == row.key.len && memcmp(range->skip.u.key.ptr, row.key.ptr, row.key.len) == 0)
		{
			doltgres_scan_Row_drop(&row);
			continue;
		}
		if (range->has_skip && range->skip.tag == DOLTGRES_ENTRIES_SEARCHES_SKIP_THROUGH)
		{
			const AccessNbtsearchPosition *through = &range->skip.u.through;

			if (through->key.len == row.key.len && memcmp(through->key.ptr, row.key.ptr, row.key.len) == 0 &&
				through->copy == row.stamp.tuple)
				doltgres_entries_searches_skip_drop(range);
			doltgres_scan_Row_drop(&row);
			continue;
		}
		doltgres_entries_searches_skip_drop(range);
		if (range->far_end.tag != STD_BOUND_UNBOUNDED)
		{
			HostIoIo	io = pghost_io_catalog(caps);
			StdOrdering ordering;
			ChunksErrorError storage;

			if (!trees_KeyOrder_compare(&self->order, &io, row.key.ptr, row.key.len, range->far_end.key.ptr,
										range->far_end.key.len, &ordering, &storage))
			{
				doltgres_scan_Row_drop(&row);
				*error = doltgres_entries_storage_error(&storage);
				return false;
			}
			if (range->way == DOLTGRES_SCAN_DIRECTION_FORWARD)
				beyond = range->far_end.tag == STD_BOUND_INCLUDED ? ordering == STD_ORDERING_GREATER :
					ordering != STD_ORDERING_LESS;
			else
				beyond = range->far_end.tag == STD_BOUND_INCLUDED ? ordering == STD_ORDERING_LESS :
					ordering != STD_ORDERING_GREATER;
		}
		if (beyond)
		{
			doltgres_scan_Row_drop(&row);
			*has_result = false;
			return true;
		}
		std_vec_clone(&result->key, &row.key, 1, 1);
		result->copy = row.stamp.tuple;
		if (self->has_row)
			doltgres_scan_Row_drop(&self->row);
		self->row = row;
		self->has_row = true;
		*has_result = true;
		return true;
	}
}

/* doltgres_entries_searches_datums_clone returns a copy of a Vec<Option<Datum>>. */
static void
doltgres_entries_searches_datums_clone(const StdVec *values, StdVec *result)
{
	std_vec_clone(result, values, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
}

/*
 * doltgres_entries_searches_Entries_decoded_frame_0_t is decoded's state across its
 * pad: the fields and decoded values it reads and the values it makes, each
 * while live.
 */
typedef struct doltgres_entries_searches_Entries_decoded_frame_0_t
{
	DoltgresEntriesSearchesEntries *self;
	const PgCatalogCaps *caps;
	const DoltgresScanRow *row;
	StdVec		fields;
	bool		fields_live;
	StdVec		decoded;
	bool		decoded_live;
	size_t		decoded_at;
	StdVec	   *result;
	bool		result_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_Entries_decoded_frame_0_t;

/* doltgres_entries_searches_Entries_decoded_body_0 decodes the row's values, each by reference made in the scan's entry memory. */
static void
doltgres_entries_searches_Entries_decoded_body_0(void *frame_)
{
	doltgres_entries_searches_Entries_decoded_frame_0_t *frame = frame_;
	DoltgresEntriesSearchesEntries *self = frame->self;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	size_t		forms_len;
	const doltgres_TupleBoolI16_t *forms = doltgres_index_Described_forms(self->described, &forms_len);

	if (self->has_table)
	{
		DoltgresEntriesSearchesKeyed *table = &self->table;

		if (!doltgres_entries_stored(raise, table->slot, table->heap, frame->row, frame->error) ||
			!pg_calls_slot_getallattrs(raise, table->slot, frame->error))
			return;
		pg_calls_index_scan_entry_reset(self->scan);
		std_vec_with_capacity_Checked(frame->result, table->attnums.len, sizeof(access_OptionU64_t),
									  _Alignof(access_OptionU64_t));
		frame->result_live = true;
		for (size_t at = 0; at < table->attnums.len && at < forms_len; at++)
		{
			int32		attnum = STD_SUB(i32, ((const int32 *) table->attnums.ptr)[at], 1, 942, 26);
			access_OptionU64_t value = {0};

			if (!pg_calls_slot_value_null(table->slot, attnum) && forms[at].f0)
			{
				value.has = true;
				value.value = pg_calls_slot_value_word(table->slot, attnum);
			}
			else if (!pg_calls_slot_value_null(table->slot, attnum))
			{
				const uint8 *bytes;
				size_t		len;

				if (!pg_calls_slot_value_bytes(raise, pg_CatalogCaps_catalog(frame->caps), table->slot, attnum,
											   &bytes, &len, frame->error) ||
					!pg_calls_index_scan_datum(raise, self->scan, bytes, len, forms[at].f1, &value.value,
											   frame->error))
					return;
				value.has = true;
			}
			std_vec_push_Checked(frame->result, &value, sizeof(value), _Alignof(access_OptionU64_t));
		}
	}
	else
	{
		if (!doltgres_entries_searches_Entries_fields(self, frame->caps, frame->row, &frame->fields, frame->error))
			return;
		frame->fields_live = true;
		if (!access_index_IndexKey_decode(doltgres_index_Described_entry_key(self->described), frame->row->key.ptr,
										  frame->row->key.len, self->includes, frame->fields.ptr, frame->fields.len,
										  &frame->decoded, frame->error))
			return;
		frame->decoded_live = true;
		pg_calls_index_scan_entry_reset(self->scan);
		std_vec_with_capacity_Checked(frame->result, frame->decoded.len, sizeof(access_OptionU64_t),
									  _Alignof(access_OptionU64_t));
		frame->result_live = true;
		while (frame->decoded_at < frame->decoded.len && frame->decoded_at < forms_len)
		{
			access_OptionDecoded_t *decoded = &((access_OptionDecoded_t *) frame->decoded.ptr)[frame->decoded_at];
			int16		len = forms[frame->decoded_at].f1;
			access_OptionU64_t value = {.has = decoded->has};

			if (decoded->has && decoded->value.tag == ACCESS_KEY_DECODED_WORD)
				value.value = decoded->value.u.word;
			else if (decoded->has &&
					 !pg_calls_index_scan_datum(raise, self->scan, decoded->value.u.bytes.ptr,
												decoded->value.u.bytes.len, len, &value.value, frame->error))
				return;
			frame->decoded_at++;
			access_index_decoded_drop(decoded);
			std_vec_push_Checked(frame->result, &value, sizeof(value), _Alignof(access_OptionU64_t));
		}
	}
	std_vec_free_Checked(&self->decoded, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
	doltgres_entries_searches_datums_clone(frame->result, &self->decoded);
	frame->result_live = false;
	frame->ok = true;
}

/* doltgres_entries_searches_Entries_decoded_pad_0 drops the values of a failed decode, the decoded values not taken, and the fields. */
static void
doltgres_entries_searches_Entries_decoded_pad_0(void *frame_)
{
	doltgres_entries_searches_Entries_decoded_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
	if (frame->decoded_live)
	{
		while (frame->decoded_at < frame->decoded.len)
			access_index_decoded_drop(&((access_OptionDecoded_t *) frame->decoded.ptr)[frame->decoded_at++]);
		std_vec_free_Checked(&frame->decoded, sizeof(access_OptionDecoded_t), _Alignof(access_OptionDecoded_t));
	}
	if (frame->fields_live)
		doltgres_entries_searches_fields_drop(&frame->fields);
	frame->result_live = frame->decoded_live = frame->fields_live = false;
}

/*
 * doltgres_entries_searches_Entries_decoded is Reader values for a row, which the
 * reader keeps: a primary key's index's row's values from the table's slot,
 * each passed by reference read whole and made in the scan's entry memory,
 * as an index entry's are; it keeps a copy of the values it answers.
 */
static bool
doltgres_entries_searches_Entries_decoded(DoltgresEntriesSearchesEntries *self, const PgCatalogCaps *caps,
										  const DoltgresScanRow *row, StdVec *result, PgPgError *error)
{
	doltgres_entries_searches_Entries_decoded_frame_0_t frame = {.self = self, .caps = caps, .row = row, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_Entries_decoded_body_0, doltgres_entries_searches_Entries_decoded_pad_0);
	doltgres_entries_searches_Entries_decoded_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_Entries_Reader_values is Entries' Reader values: the entry read last decoded into its key columns' values. */
static bool
doltgres_entries_searches_Entries_Reader_values(void *self_, const PgCatalogCaps *caps, StdVec *result,
												PgPgError *error)
{
	DoltgresEntriesSearchesEntries *self = self_;
	DoltgresScanRow row;
	bool		ok;

	if (!self->has_row)
	{
		*error = doltgres_entries_searches_internal("a scan decoded an entry before reading one");
		return false;
	}
	row = self->row;
	self->has_row = false;
	ok = doltgres_entries_searches_Entries_decoded(self, caps, &row, result, error);
	self->row = row;
	self->has_row = true;
	return ok;
}

/* DOLTGRES_ENTRIES_SEARCHES_ENTRIES_READER_OPS is Entries' Reader. */
static const AccessNbtsearchReaderOps DOLTGRES_ENTRIES_SEARCHES_ENTRIES_READER_OPS = {
	doltgres_entries_searches_Entries_Reader_search,
	doltgres_entries_searches_Entries_Reader_next,
	doltgres_entries_searches_Entries_Reader_values,
};

/* DoltgresEntriesSearchesSinkTag is Sink's variants. */
typedef enum DoltgresEntriesSearchesSinkTag
{
	DOLTGRES_ENTRIES_SEARCHES_SINK_ONE,
	DOLTGRES_ENTRIES_SEARCHES_SINK_BITMAP,
} DoltgresEntriesSearchesSinkTag;

/*
 * DoltgresEntriesSearchesSink is Sink: what a scan's read hands its entries
 * to, amgettuple's one entry, kept as its row key, its copy's tuple id, and
 * its values (a Vec<Option<Datum>>), or amgetbitmap's bitmap, with the table
 * each row's tuple id is read through under the scan's snapshot and how many
 * tuple ids it added.
 */
typedef struct DoltgresEntriesSearchesSink
{
	DoltgresEntriesSearchesSinkTag tag;
	bool		has_given;
	StdVec		given_key;
	uint64		given_copy;
	StdVec		given_values;
	PgBitmap   *tbm;
	const PgRel *heap;
	const PgSnapshot *snapshot;
	const HostSnapshot *seen;
	int64		added;
} DoltgresEntriesSearchesSink;

/* doltgres_entries_searches_Sink_drop is Sink's drop glue: the entry it was given, if any. */
static void
doltgres_entries_searches_Sink_drop(DoltgresEntriesSearchesSink *self)
{
	if (self->has_given)
	{
		std_vec_free_Checked(&self->given_key, 1, 1);
		std_vec_free_Checked(&self->given_values, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
	}
	self->has_given = false;
}

/* doltgres_entries_searches_Rows_drop drops a Vec<Row>: its rows, then it. */
static void
doltgres_entries_searches_Rows_drop(StdVec *rows)
{
	std_vec_drop_Checked(rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_searches_Row_drop);
}

/*
 * doltgres_entries_searches_taken hands a sink an entry a scan's read found
 * and reports whether the read goes on: amgettuple's keeps the entry and
 * stops it; the bitmap's adds the row's tuple id, a keyless table's entry's
 * copy's, a primary key's index's row's own, and for another table's entry
 * the tuple id of the version of its row key the scan's snapshot sees, a row
 * it doesn't see adding nothing, and goes on. The one entry's values move
 * into the sink, leaving values empty; the caller drops the row and values.
 */
static bool
doltgres_entries_searches_taken(const PgCatalogCaps *caps, DoltgresEntriesSearchesSink *sink,
								const DoltgresIndexDescribed *described, const DoltgresScanRow *row, StdVec *values,
								bool *result, PgPgError *error)
{
	const AccessIndexIndexKey *entry_key = doltgres_index_Described_entry_key(described);
	StdBytes	row_key;
	bool		has_tuple = false;
	uint64		tuple = 0;

	if (sink->tag == DOLTGRES_ENTRIES_SEARCHES_SINK_ONE)
	{
		StdVec		key;

		if (doltgres_index_Described_keyed(described))
			std_vec_clone(&key, &row->key, 1, 1);
		else if (access_index_IndexKey_row_key_bytes(entry_key, row->key.ptr, row->key.len, row->tuple.ptr,
													 row->tuple.len, &row_key, error))
			std_slice_to_vec_Checked(&key, row_key.ptr, row_key.len, 1, 1);
		else
			return false;
		doltgres_entries_searches_Sink_drop(sink);
		sink->given_key = key;
		sink->given_copy = doltgres_index_Described_keyless(described) ? row->stamp.tuple : 0;
		sink->given_values = *values;
		std_vec_new(values, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
		sink->has_given = true;
		*result = false;
		return true;
	}
	if (doltgres_index_Described_keyed(described) || doltgres_index_Described_keyless(described))
	{
		has_tuple = true;
		tuple = row->stamp.tuple;
	}
	else
	{
		DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY};
		StdVec		rows;

		if (!access_index_IndexKey_row_key_bytes(entry_key, row->key.ptr, row->key.len, row->tuple.ptr,
												 row->tuple.len, &row_key, error))
			return false;
		by.key = row_key.ptr;
		by.key_len = row_key.len;
		if (!doltgres_entries_rows(caps, sink->heap, sink->snapshot, sink->seen, by, &rows, error))
			return false;
		has_tuple = rows.len > 0;
		if (has_tuple)
			tuple = ((const DoltgresScanRow *) rows.ptr)[0].stamp.tuple;
		doltgres_entries_searches_Rows_drop(&rows);
	}
	if (has_tuple)
	{
		if (!pg_calls_bitmap_add(pg_CatalogCaps_may_raise(caps), sink->tbm, tuple, error))
			return false;
		sink->added = STD_ADD(i64, sink->added, 1, 217, 17);
	}
	*result = true;
	return true;
}

/*
 * doltgres_entries_searches_Lent_t is what read takes out of a search's
 * record: the description's and the probes' Arcs, the reader's record, the
 * search, the range, and whether a parallel participant has a unit.
 */
typedef struct doltgres_entries_searches_Lent_t
{
	StdArcInner *described;
	doltgres_OptionHandle_t reader;
	bool		has_scan;
	AccessNbtsearchBtScan scan;
	DoltgresEntriesSearchesRange range;
	bool		taken;
	StdArcInner *probes;
} doltgres_entries_searches_Lent_t;

/* Search's Table's with at what read takes out. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#define HOST_OWNED_WITH_TS Lent
#define HOST_OWNED_WITH_T doltgres_entries_searches_Lent_t
#include "doltgres/host/owned_template.h"

/* Search's Table's with at a Vec<u8>. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#define HOST_OWNED_WITH_TS VecU8
#define HOST_OWNED_WITH_T StdVec
#include "doltgres/host/owned_template.h"

/* Search's Table's with at (). */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#define HOST_OWNED_WITH_TS Unit
#define HOST_OWNED_WITH_UNIT
#include "doltgres/host/owned_template.h"

/*
 * doltgres_entries_searches_read_frame_0_t is read's state across its pad:
 * its arguments, what it took out of the record, the order, the search, the
 * session's repository and reader, the table and its slot, the scan's
 * snapshot, the reader's keyed layout, and the entries, each while live.
 */
typedef struct doltgres_entries_searches_read_frame_0_t
{
	const PgCatalogCaps *caps;
	PgIndexScan *scan;
	bool		backward;
	DoltgresEntriesSearchesSink *sink;
	HostIoIo	io;
	HostOwnedHandle owned;
	doltgres_entries_searches_Lent_t lent;
	bool		lent_live;
	TreesKeyOrder order;
	bool		order_live;
	AccessNbtsearchBtScan search;
	bool		search_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	PgOwnedRel	heap;
	bool		heap_live;
	PgOwnedSlot slot;
	bool		slot_live;
	HostSnapshot read;
	StdVec		keyed;
	bool		keyed_live;
	DoltgresEntriesSearchesEntries entries;
	bool		entries_live;
	/* columns is the Vec<KeyColumn> of the scan's columns. */
	StdVec		columns;
	bool		columns_live;
	DoltgresScanRow row;
	bool		row_live;
	StdVec		values;
	bool		values_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_read_frame_0_t;

/* doltgres_entries_searches_read_closure_0 is read's lend: the record's description, reader, search, range, and probes taken out. */
static bool
doltgres_entries_searches_read_closure_0(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										 doltgres_entries_searches_Lent_t *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	result->range = search->range;
	doltgres_entries_searches_Range_Default_default(&search->range);
	result->probes = std_arc_clone(search->probes);
	result->described = std_arc_clone(search->described);
	result->reader.has = search->has_reader;
	result->reader.handle = search->reader;
	result->has_scan = search->has_scan;
	result->scan = search->scan;
	search->has_scan = false;
	result->taken = search->taken;
	return true;
}

/* doltgres_entries_searches_read_closure_2 is read's lend of the reader's keyed layout: a copy. */
static bool
doltgres_entries_searches_read_closure_2(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										 StdVec *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	std_vec_clone(result, &search->keyed, 1, 1);
	return true;
}

/* doltgres_entries_searches_read_closure_4 is read's lend that keeps the reader's record in the search's. */
static bool
doltgres_entries_searches_read_closure_4(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										 HostError *error)
{
	(void) io;
	(void) error;
	search->has_reader = true;
	search->reader = *(const HostOwnedHandle *) captures;
	return true;
}

/* doltgres_entries_searches_read_closure_5 is read's lend that puts the search, its range, and its unit back. */
static bool
doltgres_entries_searches_read_closure_5(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *record,
										 HostError *error)
{
	doltgres_entries_searches_read_frame_0_t *frame = captures;

	(void) io;
	(void) error;
	if (record->has_scan)
		access_nbtsearch_BtScan_drop(&record->scan);
	frame->search_live = false;
	record->scan = frame->search;
	record->has_scan = true;
	doltgres_entries_searches_Range_drop(&record->range);
	record->range = frame->entries.range;
	doltgres_entries_searches_Range_Default_default(&frame->entries.range);
	record->taken = frame->lent.taken;
	return true;
}

/* doltgres_entries_searches_read_closure_3 is read's lent call: the base the scan's snapshot reads, as splits' is. */
static bool
doltgres_entries_searches_read_closure_3(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held, bool *some,
										 TxnBaseBase *result, HostError *error)
{
	return doltgres_entries_searches_Entries_splits_closure_0(captures, io, repo, held, some, result, error);
}

/* doltgres_entries_searches_unsnapshotted returns the error of an index scan asked to read with no snapshot. */
static PgPgError
doltgres_entries_searches_unsnapshotted(void)
{
	return doltgres_entries_searches_internal("an index scan was asked to read with no snapshot");
}

/*
 * doltgres_entries_searches_entries_read is read's loop over a scan's
 * search: a parallel participant's next unit when it has none, the search's
 * next matching entry, and the sink's answer whether to go on.
 */
static bool
doltgres_entries_searches_entries_read(doltgres_entries_searches_read_frame_0_t *frame, AccessNbtsearchSearchCalls calls,
									   bool parallel)
{
	const DoltgresIndexDescribed *described = STD_ARC_VALUE(frame->lent.described, _Alignof(DoltgresIndexDescribed));
	AccessNbtsearchReader reader = {&DOLTGRES_ENTRIES_SEARCHES_ENTRIES_READER_OPS, &frame->entries};

	doltgres_index_Described_scan_columns(described, &frame->columns);
	frame->columns_live = true;
	for (;;)
	{
		bool		found;
		bool		goes_on;

		if (parallel && !frame->lent.taken)
		{
			AccessNbtsearchKeyRange unit;
			bool		has;

			if (!doltgres_entries_searches_Entries_unit(&frame->entries, frame->caps, frame->backward, &has, &unit,
														frame->error))
				return false;
			if (!has)
				return true;
			access_nbtsearch_BtScan_restrict(&frame->search, &unit);
			frame->lent.taken = true;
		}
		if (!access_nbtsearch_BtScan_next_entry(&frame->search, frame->caps, frame->columns.ptr, frame->columns.len,
												reader, calls, &found, frame->error))
			return false;
		if (!found)
		{
			frame->lent.taken = false;
			if (parallel)
				continue;
			return true;
		}
		if (!frame->entries.has_row)
		{
			*frame->error = doltgres_entries_searches_internal("a scan's search found an entry its reader didn't keep");
			return false;
		}
		frame->row = frame->entries.row;
		frame->entries.has_row = false;
		frame->row_live = true;
		frame->values = frame->entries.decoded;
		std_vec_new(&frame->entries.decoded, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
		frame->values_live = true;
		if (!doltgres_entries_searches_taken(frame->caps, frame->sink, described, &frame->row, &frame->values,
											 &goes_on, frame->error))
			return false;
		frame->values_live = frame->row_live = false;
		std_vec_free_Checked(&frame->values, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
		doltgres_scan_Row_drop(&frame->row);
		if (!goes_on)
			return true;
	}
}

/* doltgres_entries_searches_read_body_0 reads the scan's entries its keys match into the sink. */
static void
doltgres_entries_searches_read_body_0(void *frame_)
{
	doltgres_entries_searches_read_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	const PgRel *index = pg_IndexScan_index(frame->scan);
	const DoltgresIndexDescribed *described;
	const PgSnapshot *snapshot;
	HostOwnedHandle reader;
	HostError	failed;
	DoltgresEntriesSearchesScanCalls calls;
	AccessNbtsearchSearchCalls search_calls = {&DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS, &calls};
	bool		changed;
	uint64		searched;
	uint64		searches;

	frame->owned = doltgres_entries_searches_handle(frame->scan);
	frame->io = pghost_io_catalog(frame->caps);
	if (!host_owned_TableDoltgresEntriesSearchesSearch_withLent(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &frame->io,
																frame->owned, doltgres_entries_searches_read_closure_0,
																NULL, NULL, &frame->lent, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->lent_live = true;
	described = STD_ARC_VALUE(frame->lent.described, _Alignof(DoltgresIndexDescribed));
	doltgres_index_Described_order(described, std_arc_clone(frame->lent.probes), &frame->order);
	frame->order_live = true;
	if (frame->lent.has_scan)
	{
		frame->search = frame->lent.scan;
		frame->lent.has_scan = false;
	}
	else
	{
		AccessNbtutilsPreprocessed pre;

		if (!doltgres_entries_searches_preprocessed(frame->caps, frame->scan, &pre, frame->error))
			return;
		doltgres_index_Described_scan_columns(described, &frame->columns);
		frame->columns_live = true;
		access_nbtsearch_BtScan_new(frame->columns.ptr, frame->columns.len,
									doltgres_index_Described_scan_bound(described), &pre, frame->backward,
									&frame->search);
		frame->columns_live = false;
		std_vec_free_Checked(&frame->columns, sizeof(AccessNbtsearchKeyColumn), _Alignof(AccessNbtsearchKeyColumn));
	}
	frame->search_live = true;
	(void) access_nbtsearch_BtScan_set_direction(&frame->search, frame->backward, &changed);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (doltgres_index_Described_keyed(described))
	{
		if (!pg_OwnedRel_index_table(raise, catalog, index, &frame->heap, frame->error))
			return;
		frame->heap_live = true;
		if (!pg_calls_make_slot(raise, pg_OwnedRel_rel(&frame->heap), &frame->slot, frame->error))
			return;
		frame->slot_live = true;
	}
	snapshot = pg_IndexScan_snapshot(frame->scan);
	if (snapshot == NULL)
	{
		*frame->error = doltgres_entries_searches_unsnapshotted();
		return;
	}
	if (!pghost_snapshot(raise, snapshot, &frame->read, frame->error))
		return;
	if (frame->lent.reader.has)
		reader = frame->lent.reader.handle;
	else
	{
		TxnStateFound found;
		TxnBaseBase base;
		bool		some;
		TreesStoreReader store;
		TreesKeyOrder order;
		bool		ok;

		if (!host_owned_TableDoltgresEntriesSearchesSearch_withVecU8(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &frame->io,
																	 frame->owned,
																	 doltgres_entries_searches_read_closure_2, NULL,
																	 NULL, &frame->keyed, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->keyed_live = true;
		if (!(frame->heap_live ? doltgres_entries_making(frame->caps, &frame->io, pg_OwnedRel_rel(&frame->heap), &found,
														 frame->error) :
			  doltgres_entries_index_making(&frame->io, index, &found, frame->error)))
			return;
		if (!txn_state_held_OptionBase(&frame->io, STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo)), &frame->reading,
									   found, doltgres_entries_searches_read_closure_3, &frame->read, NULL, &some,
									   &base, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		store = repo_Repo_reader(STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo)), HOST_CACHE_PLACEMENT_SERVER,
								 &frame->reading);
		trees_KeyOrder_Clone_clone(&frame->order, &order);
		frame->keyed_live = false;
		ok = doltgres_scan_scan_begin(&frame->io, TREES_STORE_READER_NODE_STORE(&store), 0, some ? &base : NULL,
									  &frame->keyed, pg_calls_index_table(index), &order, &reader, &failed);
		if (some)
			txn_base_Base_drop(&base);
		if (!ok ||
			!host_owned_TableDoltgresEntriesSearchesSearch_withUnit(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &frame->io,
																	frame->owned,
																	doltgres_entries_searches_read_closure_4, &reader,
																	NULL, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
	}
	calls.scan = frame->scan;
	calls.probes = STD_ARC_VALUE(frame->lent.probes, _Alignof(HostLockIoMutexVecProbe));
	frame->entries.repo = STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo));
	frame->entries.reading = &frame->reading;
	frame->entries.scan = frame->scan;
	frame->entries.read = &frame->read;
	frame->entries.described = described;
	frame->entries.order = frame->order;
	frame->order_live = false;
	frame->entries.has_table = frame->slot_live;
	if (frame->slot_live)
	{
		frame->entries.table.heap = pg_OwnedRel_rel(&frame->heap);
		frame->entries.table.slot = pg_OwnedSlot_slot(&frame->slot);
		doltgres_entries_searches_attnums(index, &frame->entries.table.attnums);
	}
	frame->entries.includes = pg_calls_index_scan_wants_tuple(frame->scan);
	frame->entries.reader = reader;
	frame->entries.range = frame->lent.range;
	doltgres_entries_searches_Range_Default_default(&frame->lent.range);
	std_vec_new(&frame->entries.decoded, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
	frame->entries_live = true;
	searched = access_nbtsearch_BtScan_nsearches(&frame->search);
	if (!doltgres_entries_searches_entries_read(frame, search_calls, pg_calls_index_parallel_is(frame->scan)))
		return;
	searches = STD_SUB(u64, access_nbtsearch_BtScan_nsearches(&frame->search), searched, 308, 20);
	if (!host_owned_TableDoltgresEntriesSearchesSearch_withUnit(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &frame->io,
																frame->owned, doltgres_entries_searches_read_closure_5,
																frame, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	for (uint64 at = 0; at < searches; at++)
		if (!pg_calls_index_scan_searched(raise, frame->scan, frame->error))
			return;
	frame->ok = true;
}

/* doltgres_entries_searches_read_pad_0 drops what read holds. */
static void
doltgres_entries_searches_read_pad_0(void *frame_)
{
	doltgres_entries_searches_read_frame_0_t *frame = frame_;

	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(access_OptionU64_t), _Alignof(access_OptionU64_t));
	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, sizeof(AccessNbtsearchKeyColumn), _Alignof(AccessNbtsearchKeyColumn));
	if (frame->entries_live)
		doltgres_entries_searches_Entries_drop(&frame->entries);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->slot_live)
		pg_OwnedSlot_drop(&frame->slot);
	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_searches_Repo_drop);
	if (frame->search_live)
		access_nbtsearch_BtScan_drop(&frame->search);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->lent_live)
	{
		std_arc_drop(frame->lent.probes, sizeof(HostLockIoMutexVecProbe), _Alignof(HostLockIoMutexVecProbe),
					 doltgres_layout_Probes_drop);
		doltgres_entries_searches_Range_drop(&frame->lent.range);
		if (frame->lent.has_scan)
			access_nbtsearch_BtScan_drop(&frame->lent.scan);
		std_arc_drop(frame->lent.described, sizeof(DoltgresIndexDescribed), _Alignof(DoltgresIndexDescribed),
					 doltgres_entries_searches_Described_drop);
	}
	frame->values_live = frame->row_live = frame->columns_live = frame->entries_live = frame->keyed_live = false;
	frame->slot_live = frame->heap_live = frame->reading_live = frame->repo_live = frame->search_live = false;
	frame->order_live = frame->lent_live = false;
}

/*
 * doltgres_entries_searches_read reads the scan's entries its keys match in
 * a direction under its snapshot, handing each to a sink until it stops or
 * the scan ends; a parallel scan's participant reads one unit at a time.
 * The search is taken out of the record while it reads and put back after,
 * and a read that fails leaves it out, as the transaction's abort ends the
 * scan.
 */
static bool
doltgres_entries_searches_read(const PgCatalogCaps *caps, PgIndexScan *scan, bool backward,
							   DoltgresEntriesSearchesSink *sink, PgPgError *error)
{
	doltgres_entries_searches_read_frame_0_t frame = {.caps = caps, .scan = scan, .backward = backward,
	.sink = sink, .error = error};

	std_landing(&frame, doltgres_entries_searches_read_body_0, doltgres_entries_searches_read_pad_0);
	doltgres_entries_searches_read_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_searches_next_frame_0_t is next's state across its pad:
 * its sink and the packed values, each while live.
 */
typedef struct doltgres_entries_searches_next_frame_0_t
{
	const PgCatalogCaps *caps;
	PgIndexScan *scan;
	int32		direction;
	DoltgresEntriesSearchesSink sink;
	StdVec		packed;
	bool		packed_live;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_next_frame_0_t;

/* doltgres_entries_searches_next_body_0 reads the scan's next entry and names its row to the scan's fetch. */
static void
doltgres_entries_searches_next_body_0(void *frame_)
{
	doltgres_entries_searches_next_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	PgIndexFetch *fetch;
	bool		backward;

	if (!doltgres_entries_searches_way(frame->direction, &backward, frame->error) ||
		!doltgres_entries_searches_read(frame->caps, frame->scan, backward, &frame->sink, frame->error))
		return;
	if (!frame->sink.has_given)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	pg_calls_index_scan_found(frame->scan);
	if (pg_calls_index_scan_wants_tuple(frame->scan))
	{
		const access_OptionU64_t *values = frame->sink.given_values.ptr;

		std_vec_with_capacity_Checked(&frame->packed, STD_MUL(usize, frame->sink.given_values.len, 9, 368, 45), 1, 1);
		frame->packed_live = true;
		for (size_t at = 0; at < frame->sink.given_values.len; at++)
		{
			uint64		word = values[at].has ? values[at].value : 0;

			std_vec_extend_from_slice_Checked(&frame->packed, &word, sizeof(word), 1, 1);
			std_vec_u8_push_Checked(&frame->packed, values[at].has ? 0 : 1);
		}
		if (!pg_calls_index_scan_set_tuple(raise, frame->scan, frame->packed.ptr, frame->packed.len, frame->error))
			return;
	}
	fetch = pg_IndexScan_heap_fetch(frame->scan);
	if (fetch == NULL)
	{
		*frame->error = doltgres_entries_searches_internal("an index scan returned a row with no fetch to name it to");
		return;
	}
	if (!pg_calls_fetch_set_key(raise, fetch, frame->sink.given_key.ptr, frame->sink.given_key.len,
								frame->sink.given_copy, frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/* doltgres_entries_searches_next_pad_0 drops the packed values and the sink's entry. */
static void
doltgres_entries_searches_next_pad_0(void *frame_)
{
	doltgres_entries_searches_next_frame_0_t *frame = frame_;

	if (frame->packed_live)
		std_vec_free_Checked(&frame->packed, 1, 1);
	doltgres_entries_searches_Sink_drop(&frame->sink);
	frame->packed_live = false;
}

/*
 * doltgres_entries_searches_next is the fork's btree amgettuple: it reads
 * the scan's next entry its keys match in a direction, and names the entry's
 * row to the scan's fetch by its row key, with the copy's tuple id for a
 * keyless table's entry, with no recheck; for a scan that wants the entry's
 * values, an index-only scan's, it forms them into the scan's tuple.
 */
bool
doltgres_entries_searches_next(const PgCatalogCaps *caps, PgIndexScan *scan, int32 direction, bool *result,
							   PgPgError *error)
{
	doltgres_entries_searches_next_frame_0_t frame = {.caps = caps, .scan = scan, .direction = direction,
		.sink = {.tag = DOLTGRES_ENTRIES_SEARCHES_SINK_ONE}, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_next_body_0, doltgres_entries_searches_next_pad_0);
	doltgres_entries_searches_next_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_searches_bitmap_frame_0_t is bitmap's state across its
 * pad: the index's table, while live, and the sink.
 */
typedef struct doltgres_entries_searches_bitmap_frame_0_t
{
	const PgCatalogCaps *caps;
	PgIndexScan *scan;
	PgOwnedRel	heap;
	bool		heap_live;
	HostSnapshot seen;
	DoltgresEntriesSearchesSink sink;
	int64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_bitmap_frame_0_t;

/* doltgres_entries_searches_bitmap_body_0 adds every matching entry's row's tuple id to the bitmap. */
static void
doltgres_entries_searches_bitmap_body_0(void *frame_)
{
	doltgres_entries_searches_bitmap_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgSnapshot *snapshot;

	if (!pg_OwnedRel_index_table(raise, pg_CatalogCaps_catalog(frame->caps), pg_IndexScan_index(frame->scan),
								 &frame->heap, frame->error))
		return;
	frame->heap_live = true;
	snapshot = pg_IndexScan_snapshot(frame->scan);
	if (snapshot == NULL)
	{
		*frame->error = doltgres_entries_searches_unsnapshotted();
		return;
	}
	if (!pghost_snapshot(raise, snapshot, &frame->seen, frame->error))
		return;
	frame->sink.heap = pg_OwnedRel_rel(&frame->heap);
	frame->sink.snapshot = snapshot;
	frame->sink.seen = &frame->seen;
	if (!doltgres_entries_searches_read(frame->caps, frame->scan, false, &frame->sink, frame->error))
		return;
	*frame->result = frame->sink.added;
	frame->ok = true;
}

/* doltgres_entries_searches_bitmap_pad_0 drops the index's table. */
static void
doltgres_entries_searches_bitmap_pad_0(void *frame_)
{
	doltgres_entries_searches_bitmap_frame_0_t *frame = frame_;

	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	frame->heap_live = false;
}

/*
 * doltgres_entries_searches_bitmap is the fork's btree amgetbitmap: it reads
 * every entry the scan's keys match, forward, and adds each one's row's
 * tuple id to the bitmap with no recheck, answering how many it added, as
 * btgetbitmap does.
 */
bool
doltgres_entries_searches_bitmap(const PgCatalogCaps *caps, PgIndexScan *scan, PgBitmap *tbm, int64 *result,
								 PgPgError *error)
{
	doltgres_entries_searches_bitmap_frame_0_t frame = {.caps = caps, .scan = scan,
		.sink = {.tag = DOLTGRES_ENTRIES_SEARCHES_SINK_BITMAP, .tbm = tbm}, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_searches_bitmap_body_0, doltgres_entries_searches_bitmap_pad_0);
	doltgres_entries_searches_bitmap_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_searches_mark_closure_0 is mark's lend: the position of the entry the scan returned last as its mark. */
static bool
doltgres_entries_searches_mark_closure_0(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
										 HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	if (search->has_mark)
		access_nbtsearch_BtMark_drop(&search->mark);
	search->has_mark = search->has_scan && access_nbtsearch_BtScan_markpos(&search->scan, &search->mark);
	return true;
}

/*
 * doltgres_entries_searches_mark is the fork's btree ammarkpos: it keeps the
 * position of the entry the scan returned last as its mark, none before its
 * first, as btmarkpos keeps markPos.
 */
bool
doltgres_entries_searches_mark(const PgCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (host_owned_TableDoltgresEntriesSearchesSearch_withUnit(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io,
															   doltgres_entries_searches_handle(scan),
															   doltgres_entries_searches_mark_closure_0, NULL, NULL,
															   &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_searches_Restored_t is what restore takes out of a search's record: the search, a copy of its mark, and its probes' Arc. */
typedef struct doltgres_entries_searches_Restored_t
{
	bool		has_scan;
	AccessNbtsearchBtScan scan;
	bool		has_mark;
	AccessNbtsearchBtMark mark;
	StdArcInner *probes;
} doltgres_entries_searches_Restored_t;

/* Search's Table's with at what restore takes out. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_searches_Search
#define HOST_OWNED_RS DoltgresEntriesSearchesSearch
#define HOST_OWNED_R DoltgresEntriesSearchesSearch
#define HOST_OWNED_R_DROP doltgres_entries_searches_Search_drop
#define HOST_OWNED_KIND 5
#define HOST_OWNED_TABLE_FN doltgres_entries_searches_Search_Owned_table
#define HOST_OWNED_WITH_TS Restored
#define HOST_OWNED_WITH_T doltgres_entries_searches_Restored_t
#include "doltgres/host/owned_template.h"

/* doltgres_entries_searches_restore_closure_0 is restore's lend: the search taken out, with a copy of its mark and its probes. */
static bool
doltgres_entries_searches_restore_closure_0(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *search,
											doltgres_entries_searches_Restored_t *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	result->has_scan = search->has_scan;
	result->scan = search->scan;
	search->has_scan = false;
	result->has_mark = search->has_mark;
	if (search->has_mark)
		access_nbtsearch_BtMark_Clone_clone(&search->mark, &result->mark);
	result->probes = std_arc_clone(search->probes);
	return true;
}

/* doltgres_entries_searches_restore_closure_1 is restore's lend that puts the search back. */
static bool
doltgres_entries_searches_restore_closure_1(void *captures, HostIoIo *io, DoltgresEntriesSearchesSearch *record,
											HostError *error)
{
	doltgres_entries_searches_Restored_t *restored = captures;

	(void) io;
	(void) error;
	if (record->has_scan)
		access_nbtsearch_BtScan_drop(&record->scan);
	record->scan = restored->scan;
	record->has_scan = true;
	restored->has_scan = false;
	return true;
}

/*
 * doltgres_entries_searches_restore_frame_0_t is restore's state across its
 * pad: what it took out of the record, while live.
 */
typedef struct doltgres_entries_searches_restore_frame_0_t
{
	const PgCatalogCaps *caps;
	PgIndexScan *scan;
	doltgres_entries_searches_Restored_t restored;
	bool		restored_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_restore_frame_0_t;

/* doltgres_entries_searches_restore_body_0 puts the scan back at its mark. */
static void
doltgres_entries_searches_restore_body_0(void *frame_)
{
	doltgres_entries_searches_restore_frame_0_t *frame = frame_;
	HostIoIo	io = pghost_io_catalog(frame->caps);
	HostOwnedHandle owned = doltgres_entries_searches_handle(frame->scan);
	DoltgresEntriesSearchesScanCalls calls;
	AccessNbtsearchSearchCalls search_calls = {&DOLTGRES_ENTRIES_SEARCHES_SCAN_CALLS_OPS, &calls};
	HostError	failed;

	if (!host_owned_TableDoltgresEntriesSearchesSearch_withRestored(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io, owned,
																	doltgres_entries_searches_restore_closure_0, NULL,
																	NULL, &frame->restored, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->restored_live = true;
	if (!frame->restored.has_scan)
	{
		frame->ok = true;
		return;
	}
	calls.scan = frame->scan;
	calls.probes = STD_ARC_VALUE(frame->restored.probes, _Alignof(HostLockIoMutexVecProbe));
	access_nbtsearch_BtScan_restrpos(&frame->restored.scan, search_calls,
									 frame->restored.has_mark ? &frame->restored.mark : NULL);
	if (!host_owned_TableDoltgresEntriesSearchesSearch_withUnit(&DOLTGRES_ENTRIES_SEARCHES_SEARCHES, &io, owned,
																doltgres_entries_searches_restore_closure_1,
																&frame->restored, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/* doltgres_entries_searches_restore_pad_0 drops the search left out, the mark's copy, and the probes. */
static void
doltgres_entries_searches_restore_pad_0(void *frame_)
{
	doltgres_entries_searches_restore_frame_0_t *frame = frame_;

	if (frame->restored_live)
	{
		std_arc_drop(frame->restored.probes, sizeof(HostLockIoMutexVecProbe), _Alignof(HostLockIoMutexVecProbe),
					 doltgres_layout_Probes_drop);
		if (frame->restored.has_mark)
			access_nbtsearch_BtMark_drop(&frame->restored.mark);
		if (frame->restored.has_scan)
			access_nbtsearch_BtScan_drop(&frame->restored.scan);
	}
	frame->restored_live = false;
}

/*
 * doltgres_entries_searches_restore is the fork's btree amrestrpos: it puts
 * the scan back at its mark, so its next read gives the entry after the
 * marked one, as btrestrpos does; the search is taken out of the record for
 * the call, since setting its arrays back frees their values through
 * Postgres.
 */
bool
doltgres_entries_searches_restore(const PgCatalogCaps *caps, PgIndexScan *scan, PgPgError *error)
{
	doltgres_entries_searches_restore_frame_0_t frame = {.caps = caps, .scan = scan, .error = error};

	std_landing(&frame, doltgres_entries_searches_restore_body_0, doltgres_entries_searches_restore_pad_0);
	doltgres_entries_searches_restore_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_searches_current_frame_0_t is current's state across its
 * pad: the fetch's row key, the index's table, and the rows it reads, each
 * while live.
 */
typedef struct doltgres_entries_searches_current_frame_0_t
{
	const PgCatalogCaps *caps;
	PgIndexScan *scan;
	PgTid	   *tid;
	StdVec		key;
	bool		key_live;
	PgOwnedRel	heap;
	bool		heap_live;
	StdVec		rows;
	bool		rows_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_searches_current_frame_0_t;

/* doltgres_entries_searches_current_body_0 finds the tid of the version of the scan's current row its snapshot sees. */
static void
doltgres_entries_searches_current_body_0(void *frame_)
{
	doltgres_entries_searches_current_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	PgIndexFetch *fetch = pg_IndexScan_heap_fetch(frame->scan);
	const PgSnapshot *snapshot;
	HostSnapshot read;
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY};
	const uint8 *key;
	size_t		key_len;
	uint64		copy;

	if (fetch == NULL)
	{
		*frame->error = doltgres_entries_searches_internal("an index-only scan's row has no fetch to name it");
		return;
	}
	key = pg_calls_fetch_key(fetch, &key_len);
	std_slice_to_vec_Checked(&frame->key, key, key_len, 1, 1);
	frame->key_live = true;
	copy = pg_calls_fetch_copy(fetch);
	if (!pg_OwnedRel_index_table(raise, pg_CatalogCaps_catalog(frame->caps), pg_IndexScan_index(frame->scan),
								 &frame->heap, frame->error))
		return;
	frame->heap_live = true;
	snapshot = pg_IndexScan_snapshot(frame->scan);
	if (snapshot == NULL)
	{
		*frame->error = doltgres_entries_searches_unsnapshotted();
		return;
	}
	if (!pghost_snapshot(raise, snapshot, &read, frame->error))
		return;
	by.key = frame->key.ptr;
	by.key_len = frame->key.len;
	if (!doltgres_entries_rows(frame->caps, pg_OwnedRel_rel(&frame->heap), snapshot, &read, by, &frame->rows,
							   frame->error))
		return;
	frame->rows_live = true;
	for (size_t at = 0; at < frame->rows.len; at++)
	{
		const DoltgresScanRow *row = &((const DoltgresScanRow *) frame->rows.ptr)[at];

		if (copy == 0 || row->stamp.tuple == copy)
		{
			pg_calls_tid_set(frame->tid, row->stamp.tuple);
			frame->ok = true;
			return;
		}
	}
	*frame->error = doltgres_entries_searches_internal("an index-only scan's current row has no version its snapshot sees");
}

/* doltgres_entries_searches_current_pad_0 drops the rows, the table, and the key. */
static void
doltgres_entries_searches_current_pad_0(void *frame_)
{
	doltgres_entries_searches_current_frame_0_t *frame = frame_;

	if (frame->rows_live)
		doltgres_entries_searches_Rows_drop(&frame->rows);
	if (frame->heap_live)
		pg_OwnedRel_drop(&frame->heap);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->rows_live = frame->heap_live = frame->key_live = false;
}

/*
 * doltgres_entries_searches_current answers execCurrent for a fork index's
 * index-only scan: the tid of the version of the scan's current row its
 * snapshot sees, read by the row key the scan named to its fetch, a keyless
 * table's copy by its tuple id.
 */
bool
doltgres_entries_searches_current(const PgCatalogCaps *caps, PgIndexScan *scan, PgTid *tid, PgPgError *error)
{
	doltgres_entries_searches_current_frame_0_t frame = {.caps = caps, .scan = scan, .tid = tid, .error = error};

	std_landing(&frame, doltgres_entries_searches_current_body_0, doltgres_entries_searches_current_pad_0);
	doltgres_entries_searches_current_pad_0(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
