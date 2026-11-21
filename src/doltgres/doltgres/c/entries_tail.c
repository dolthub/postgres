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
 * The second part of the doltgres crate's entries (entries.rs from its
 * tuple_tid_valid on, entries.c holding the part before): the table access
 * method's fetches, sizes, vacuum, parallel scans, inserts, deletes, locks,
 * and updates, and the index builds and deletes.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "access/nbtree.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_class_d.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "common/relpath.h"
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/index.h"
#include "doltgres/access/key.h"
#include "doltgres/access/slot.h"
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/doltgres/changes.h"
#include "doltgres/doltgres/deletes.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/entries_tail.h"
#include "doltgres/doltgres/identity.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/rekey.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/doltgres/rewrite.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/doltgres/settle.h"
#include "doltgres/doltgres/sizes.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/pg/lib.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/published.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/entry.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/vacuum.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/visible.h"
#include "nodes/lockoptions.h"
#include "pg_calls.h"
#include "storage/lmgr.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "entries.rs"

/* doltgres_entries_TupleU32VecU8_drop is (u32, Vec<u8>)'s drop: its key's. */
static void
doltgres_entries_TupleU32VecU8_drop(void *entry)
{
	std_vec_free_Checked(&((doltgres_TupleU32VecU8_t *) entry)->f1, 1, 1);
}

/* doltgres_entries_Speculated_drop is Speculated's drop: its key's and its entries'. */
static void
doltgres_entries_Speculated_drop(void *speculated)
{
	DoltgresEntriesSpeculated *self = speculated;

	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_drop_Checked(&self->entries, sizeof(doltgres_TupleU32VecU8_t), _Alignof(doltgres_TupleU32VecU8_t),
						 doltgres_entries_TupleU32VecU8_drop);
}

/* doltgres_entries_VecSpeculated_drop is Vec<Speculated>'s drop. */
void
doltgres_entries_VecSpeculated_drop(void *speculated)
{
	std_vec_drop_Checked(speculated, sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated),
						 doltgres_entries_Speculated_drop);
}

/* SPECULATED's IoMutex, host's lock at the backend's speculative inserts. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecSpeculated
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_VecSpeculated_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecSpeculated
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated))
#include "doltgres/host/lock_template.h"

/* doltgres_entries_IoMutexVecSpeculated_drop is IoMutex<Vec<Speculated>>' drop glue: its value's. */
static void
doltgres_entries_IoMutexVecSpeculated_drop(void *value)
{
	doltgres_entries_VecSpeculated_drop(&((HostLockIoMutexVecSpeculated *) value)->value);
}

/*
 * DOLTGRES_ENTRIES_SPECULATED_TYPE is IoMutex<Vec<Speculated>>' size, Default,
 * and drop, which SPECULATED's get takes.
 */
const StdDefaultType DOLTGRES_ENTRIES_SPECULATED_TYPE = {sizeof(HostLockIoMutexVecSpeculated),
	_Alignof(HostLockIoMutexVecSpeculated), host_lock_IoMutexVecSpeculated_Default_default,
doltgres_entries_IoMutexVecSpeculated_drop};

/* DOLTGRES_ENTRIES_SPECULATED is SPECULATED, the backend's speculative inserts whose completion is still to come. */
struct DoltgresEntriesSpeculatedLocal DOLTGRES_ENTRIES_SPECULATED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* doltgres_entries_open opens the repository's handle and then a reader, as an entry's repository and read do. */
bool
doltgres_entries_open(HostIoIo *io, doltgres_entries_opened_t *opened, PgPgError *error)
{
	HostError	failed;

	if (!doltgres_entries_repository(io, &opened->repo, error))
		return false;
	opened->repo_live = true;
	if (!host_shared_read(io, &opened->reading, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	opened->reading_live = true;
	return true;
}

/* doltgres_entries_opened_drop frees what open opened, the reader first. */
void
doltgres_entries_opened_drop(doltgres_entries_opened_t *opened)
{
	if (opened->reading_live)
		host_shared_Reader_drop(&opened->reading);
	if (opened->repo_live)
		std_arc_drop(opened->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	opened->reading_live = opened->repo_live = false;
}

/* doltgres_entries_pg_fork returns a fork error of a literal message. */
static PgPgError
doltgres_entries_pg_fork(const char *sqlstate, const char *message)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, strlen(message));
	return pg_PgError_fork((const uint8 *) sqlstate, &owned);
}

/* doltgres_entries_host_fork returns a host's fork error of a literal message. */
static HostError
doltgres_entries_host_fork(const char *sqlstate, const char *message)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, strlen(message));
	return host_Error_fork((const uint8 *) sqlstate, &owned);
}

/* doltgres_entries_opened_repo returns the repository an open's handle holds. */
const RepoRepo *
doltgres_entries_opened_repo(const doltgres_entries_opened_t *opened)
{
	return std_arc_Deref_deref(opened->repo, _Alignof(RepoRepo));
}

/*
 * doltgres_entries_doltgres_tuple_tid_valid is the fork's tuple_tid_valid: it
 * reports whether a tid's block is below the storage's count of blocks when
 * the scan began and its offset is from 1 to a page's most tuples.
 */
bool
doltgres_entries_doltgres_tuple_tid_valid(const PgCaps *caps, PgScan *scan, PgTid *tid, bool *result,
										  PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (doltgres_scan_tid_valid(&io, doltgres_entries_handle(scan), pg_calls_tid_value(tid),
								pg_calls_max_heap_tuples_per_page(), result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* state.rs' held at Option<(u64, u64)>, which chained takes. */
#define TXN_STATE_TS OptionTupleU64U64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, std_TupleU64U64_t *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; std_TupleU64U64_t *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* doltgres_entries_chained_captures_t is chained's lent call's captures: the reader and the row's tuple id. */
typedef struct doltgres_entries_chained_captures_t
{
	const HostSharedReader *reading;
	uint64		tuple;
} doltgres_entries_chained_captures_t;

/*
 * doltgres_entries_chained_closure_0_frame_t is chained's lent call's state
 * across its pad: the row's key, the delta's hold, and the key's versions,
 * each while live.
 */
typedef struct doltgres_entries_chained_closure_0_frame_t
{
	StdVec		key;
	bool		key_live;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	doltgres_entries_chained_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	bool	   *some;
	std_TupleU64U64_t *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_chained_closure_0_frame_t;

/* doltgres_entries_chained_closure_0_pad frees what chained's lent call holds. */
static void
doltgres_entries_chained_closure_0_pad(void *frame_)
{
	doltgres_entries_chained_closure_0_frame_t *frame = frame_;

	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->versions_live = frame->hold_live = frame->key_live = false;
}

/* doltgres_entries_chained_closure_0_1 is chained's row: whether a version puts the row of a tuple id. */
static bool
doltgres_entries_chained_closure_0_1(uint64 tuple, TxnMemtableVersionVersion *version)
{
	TxnVisibleRowVersion row;

	return txn_visible_RowVersion_of(version, &row) && row.put.tuple.f0 == tuple;
}

/* doltgres_entries_chained_closure_0_2 is chained's ends: whether a version ends the row of a tuple id. */
static bool
doltgres_entries_chained_closure_0_2(uint64 tuple, const TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			return op.u.remove.f0.f0 == tuple;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			return op.u.update_row.f0.f0 == tuple;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			return op.u.delete_row.f0.f0 == tuple;
		default:
			return false;
	}
}

/*
 * doltgres_entries_chained_closure_0_body finds the row's key and the nearest
 * version above the row that ends it, with its successor and writer.
 */
static void
doltgres_entries_chained_closure_0_body(void *frame_)
{
	doltgres_entries_chained_closure_0_frame_t *frame = frame_;
	doltgres_entries_chained_captures_t *c = frame->c;
	TreesStoreReader store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	TxnBaseBase newest;
	bool		some;
	HostLsn		folded = 0;
	bool		has_key;
	bool		keyless;
	TxnMemtableVersionVersion *versions;
	size_t		at = 0;

	if (!txn_state_newest(frame->io, frame->held, &some, &newest, frame->error))
		return;
	if (some)
	{
		folded = newest.read;
		txn_base_Base_drop(&newest);
	}
	if (!doltgres_scan_keyed(frame->io, TREES_STORE_READER_NODE_STORE(&store), frame->held, c->tuple, &has_key,
							 &frame->key, &keyless, frame->error))
		return;
	*frame->some = false;
	if (!has_key)
	{
		frame->ok = true;
		return;
	}
	frame->key_live = true;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_visible_versions(frame->io, &frame->hold, host_shared_Held_record(frame->held), frame->key.ptr,
							  frame->key.len, folded, &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	versions = frame->versions.ptr;
	while (at < frame->versions.len && !doltgres_entries_chained_closure_0_1(c->tuple, &versions[at]))
		at++;
	for (size_t i = at; i > 0; i--)
	{
		TxnMemtableVersionRecord record;

		if (!doltgres_entries_chained_closure_0_2(c->tuple, &versions[i - 1]))
			continue;
		record = txn_memtable_version_Version_record(&versions[i - 1]);
		frame->result->f0 = doltgres_changes_successor(&versions[i - 1], c->tuple);
		frame->result->f1 = txn_memtable_version_Record_writer(&record).xid;
		*frame->some = true;
		break;
	}
	frame->ok = true;
}

/* doltgres_entries_chained_closure_0 is chained's lent call: the change that ended the row, under the held state. */
static bool
doltgres_entries_chained_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   bool *some, std_TupleU64U64_t *result, HostError *error)
{
	doltgres_entries_chained_closure_0_frame_t frame = {.c = captures, .io = io, .repo = repo, .held = held,
	.some = some, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_chained_closure_0_body, doltgres_entries_chained_closure_0_pad);
	doltgres_entries_chained_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_chained_frame_t is chained's state across its pad: what it
 * opened.
 */
typedef struct doltgres_entries_chained_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	uint64		tuple;
	bool	   *some;
	std_TupleU64U64_t *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_chained_frame_t;

/* doltgres_entries_chained_pad frees what chained holds. */
static void
doltgres_entries_chained_pad(void *frame_)
{
	doltgres_entries_chained_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_chained_body reads the row's ender under the storage's held state. */
static void
doltgres_entries_chained_body(void *frame_)
{
	doltgres_entries_chained_frame_t *frame = frame_;
	doltgres_entries_chained_captures_t captures = {.reading = &frame->opened.reading, .tuple = frame->tuple};
	TxnStateFound made;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &made, frame->error))
		return;
	if (!txn_state_held_OptionTupleU64U64(&frame->io, doltgres_entries_opened_repo(&frame->opened),
										  &frame->opened.reading, made, doltgres_entries_chained_closure_0, &captures,
										  NULL, frame->some, frame->result, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_chained returns the tuple id of the row the change that
 * ended the row a tid names moved it to, the row's own for one that moved it
 * nowhere, with the change's writer, or none for a row no change ended: the
 * nearest version above the row that ends it.
 */
static bool
doltgres_entries_chained(const PgCatalogCaps *caps, const PgRel *rel, uint64 tuple, bool *some,
						 std_TupleU64U64_t *result, PgPgError *error)
{
	doltgres_entries_chained_frame_t frame = {.caps = caps, .rel = rel, .tuple = tuple, .some = some,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_chained_body, doltgres_entries_chained_pad);
	doltgres_entries_chained_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tuple_get_latest_tid_frame_t is get_latest_tid's
 * state across its pad: the chain's row, while live.
 */
typedef struct doltgres_entries_doltgres_tuple_get_latest_tid_frame_t
{
	DoltgresScanRow row;
	bool		row_live;
	const PgCatalogCaps *caps;
	PgScan	   *scan;
	PgTid	   *tid;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_get_latest_tid_frame_t;

/* doltgres_entries_doltgres_tuple_get_latest_tid_pad frees what get_latest_tid holds. */
static void
doltgres_entries_doltgres_tuple_get_latest_tid_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_get_latest_tid_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->row_live = false;
}

/* doltgres_entries_doltgres_tuple_get_latest_tid_body follows the chain from the tid's row. */
static void
doltgres_entries_doltgres_tuple_get_latest_tid_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_get_latest_tid_frame_t *frame = frame_;
	const PgRel *rel = pg_calls_scan_relation(frame->scan);
	const PgSnapshot *snapshot = pg_calls_scan_snapshot(frame->scan);
	uint64		tuple = pg_calls_tid_value(frame->tid);
	bool		has_prior = false;
	uint64		prior = 0;

	for (;;)
	{
		DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tuple};
		HostSnapshot any = pghost_any();
		StdVec		rows;
		bool		has_row;
		bool		has_fetched;
		DoltgresScanRow fetched;
		bool		has_next;
		std_TupleU64U64_t next;

		if (!doltgres_entries_rows(frame->caps, rel, NULL, &any, by, &rows, frame->error))
			return;
		has_row = std_vec_pop(&rows, &frame->row, sizeof(DoltgresScanRow));
		std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
		if (!has_row)
			break;
		frame->row_live = true;
		if (has_prior && prior != frame->row.stamp.txn)
			break;
		if (!doltgres_entries_fetched(frame->caps, rel, tuple, snapshot, &has_fetched, &fetched, frame->error))
			return;
		if (has_fetched)
		{
			doltgres_scan_Row_drop(&fetched);
			pg_calls_tid_set(frame->tid, tuple);
		}
		if (!doltgres_entries_chained(frame->caps, rel, tuple, &has_next, &next, frame->error))
			return;
		if (!has_next || next.f0 == tuple || next.f0 == DOLTGRES_CHANGES_MOVED_PARTITIONS)
			break;
		tuple = next.f0;
		has_prior = true;
		prior = next.f1;
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_tuple_get_latest_tid is the fork's
 * tuple_get_latest_tid ((4b)'s section 4), as heap's heap_get_latest_tid:
 * from the tid's row it follows each update to the row it moved to, by the
 * successor its end names, across a key change too, while each row's writer
 * is the update that named it, and leaves the tid at the newest row of the
 * chain the scan's snapshot sees.
 */
bool
doltgres_entries_doltgres_tuple_get_latest_tid(const PgCatalogCaps *caps, PgScan *scan, PgTid *tid, PgPgError *error)
{
	doltgres_entries_doltgres_tuple_get_latest_tid_frame_t frame = {.caps = caps, .scan = scan, .tid = tid,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_get_latest_tid_body,
				doltgres_entries_doltgres_tuple_get_latest_tid_pad);
	doltgres_entries_doltgres_tuple_get_latest_tid_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tuple_satisfies_snapshot is the fork's
 * tuple_satisfies_snapshot: it reports whether a snapshot sees the row
 * version a slot holds, the one its ctid names, as fetched reads it.
 */
bool
doltgres_entries_doltgres_tuple_satisfies_snapshot(const PgCatalogCaps *caps, const PgRel *relation, PgSlot *slot,
												   const PgSnapshot *snapshot, bool *result, PgPgError *error)
{
	uint32		table;
	access_TupleU32U16_t at;
	DoltgresScanRow row;

	access_slot_identity(slot, &table, &at);
	if (!doltgres_entries_fetched(caps, relation, (uint64) at.f0 << 16 | at.f1, snapshot, result, &row, error))
		return false;
	if (*result)
		doltgres_scan_Row_drop(&row);
	return true;
}

/*
 * doltgres_entries_doltgres_index_fetch_begin is the fork's
 * index_fetch_begin: a fetch keeps nothing of its own between rows, so it is
 * Postgres' descriptor alone, holding its relation and flags.
 */
bool
doltgres_entries_doltgres_index_fetch_begin(const PgCaps *caps, const PgRel *rel, uint32 flags,
											PgIndexFetchDesc *result, PgPgError *error)
{
	return pg_calls_fetch_new(pg_Caps_may_raise(caps), rel, flags, result, error);
}

/*
 * doltgres_entries_doltgres_index_fetch_reset is the fork's
 * index_fetch_reset, which has nothing to reset, since a fetch keeps no place
 * between rows.
 */
bool
doltgres_entries_doltgres_index_fetch_reset(const PgCaps *caps, PgIndexFetch *fetch, PgPgError *error)
{
	return true;
}

/* doltgres_entries_doltgres_index_fetch_end is the fork's index_fetch_end: it frees the fetch. */
bool
doltgres_entries_doltgres_index_fetch_end(const PgCaps *caps, PgIndexFetch *fetch, PgPgError *error)
{
	pg_calls_fetch_free(fetch);
	return true;
}

/*
 * doltgres_entries_doltgres_index_fetch_tuple_frame_t is index_fetch_tuple's
 * state across its pad: the key's rows not yet taken and the row found, each
 * while live.
 */
typedef struct doltgres_entries_doltgres_index_fetch_tuple_frame_t
{
	StdVecIntoIter copies;
	bool		copies_live;
	DoltgresScanRow row;
	bool		row_live;
	const PgCatalogCaps *caps;
	PgIndexFetch *fetch;
	PgTid	   *tid;
	const PgSnapshot *snapshot;
	PgSlot	   *slot;
	PgIsNull   *call_again;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_fetch_tuple_frame_t;

/* doltgres_entries_doltgres_index_fetch_tuple_pad frees what index_fetch_tuple holds. */
static void
doltgres_entries_doltgres_index_fetch_tuple_pad(void *frame_)
{
	doltgres_entries_doltgres_index_fetch_tuple_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->copies_live)
		std_VecIntoIter_drop(&frame->copies);
	frame->row_live = frame->copies_live = false;
}

/*
 * doltgres_entries_doltgres_index_fetch_tuple_closure_0 is index_fetch_tuple's
 * filter: whether a row is of the copy the fetch names, any for none.
 */
static bool
doltgres_entries_doltgres_index_fetch_tuple_closure_0(uint64 copy, const DoltgresScanRow *row)
{
	return copy == 0 || row->stamp.tuple == copy;
}

/* doltgres_entries_doltgres_index_fetch_tuple_next takes the key's next row of the copy the fetch names. */
static bool
doltgres_entries_doltgres_index_fetch_tuple_next(doltgres_entries_doltgres_index_fetch_tuple_frame_t *frame,
												 uint64 copy, DoltgresScanRow *row)
{
	while (std_VecIntoIter_Iterator_next(&frame->copies, row))
	{
		if (doltgres_entries_doltgres_index_fetch_tuple_closure_0(copy, row))
			return true;
		doltgres_scan_Row_drop(row);
	}
	return false;
}

/* doltgres_entries_doltgres_index_fetch_tuple_body fetches the key's version the snapshot sees and stores it. */
static void
doltgres_entries_doltgres_index_fetch_tuple_body(void *frame_)
{
	doltgres_entries_doltgres_index_fetch_tuple_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgRel *relation = pg_calls_fetch_relation(frame->fetch);
	bool		again = pg_calls_flag(frame->call_again);
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_KEY};
	HostSnapshot read;
	uint64		copy;
	StdVec		rows;

	if (!again && !pg_calls_tid_keyed(frame->tid))
	{
		frame->ok = doltgres_entries_doltgres_tuple_fetch_row_version(frame->caps, relation, frame->tid,
																	  frame->snapshot, frame->slot, frame->result,
																	  frame->error);
		return;
	}
	if (frame->snapshot == NULL)
	{
		*frame->error = doltgres_entries_unsnapshotted();
		return;
	}
	if (!pghost_snapshot(raise, frame->snapshot, &read, frame->error))
		return;
	copy = pg_calls_fetch_copy(frame->fetch);
	by.key = pg_calls_fetch_key(frame->fetch, &by.key_len);
	if (!doltgres_entries_rows(frame->caps, relation, frame->snapshot, &read, by, &rows, frame->error))
		return;
	frame->copies = std_Vec_IntoIterator_into_iter(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
												   doltgres_entries_Row_drop);
	frame->copies_live = true;
	if (again)
	{
		uint64		given = pg_calls_tid_value(frame->tid);
		DoltgresScanRow skipped;

		while (doltgres_entries_doltgres_index_fetch_tuple_next(frame, copy, &skipped))
		{
			bool		same = skipped.stamp.tuple == given;

			doltgres_scan_Row_drop(&skipped);
			if (same)
				break;
		}
	}
	if (!doltgres_entries_doltgres_index_fetch_tuple_next(frame, copy, &frame->row))
	{
		pg_calls_flag_set(frame->call_again, false);
		*frame->result = false;
		frame->ok = true;
		return;
	}
	frame->row_live = true;
	pg_calls_flag_set(frame->call_again,
					  !(read.kind == HOST_SNAPSHOT_KIND_MVCC || read.kind == HOST_SNAPSHOT_KIND_HISTORIC_MVCC));
	doltgres_entries_found_running(frame->snapshot, &read, &frame->row);
	pg_calls_tid_set(frame->tid, frame->row.stamp.tuple);
	if (!pg_calls_predicate_lock_tid(raise, relation, frame->row.stamp.tuple, frame->snapshot,
									 (uint32) frame->row.stamp.txn, frame->error) ||
		!doltgres_entries_relocked(frame->caps, relation, frame->snapshot, frame->row.stamp.tuple, frame->error) ||
		!doltgres_entries_stored(raise, frame->slot, relation, &frame->row, frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_fetch_tuple is the fork's
 * index_fetch_tuple: for the tid the fork's index method sets for a fetch by
 * row key (PLAN item 2's index scans), the first version of the key the
 * fetch holds that the snapshot sees, of the copy it names on a table without
 * a primary key, with its tid written into tid as heap's HOT search writes
 * the visible member's; when call_again is set, the version after the one
 * whose tid tid holds; and for any other tid, the row version it names, as
 * tuple_fetch_row_version fetches it. As heap's, a version found under a
 * snapshot that isn't MVCC sets call_again, since another version of the key
 * may be seen, and all_dead keeps the caller's false, since the fork reports
 * no version dead to every snapshot.
 */
bool
doltgres_entries_doltgres_index_fetch_tuple(const PgCatalogCaps *caps, PgIndexFetch *fetch, PgTid *tid,
											const PgSnapshot *snapshot, PgSlot *slot, PgIsNull *call_again,
											PgIsNull *all_dead, bool *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_fetch_tuple_frame_t frame = {.caps = caps, .fetch = fetch, .tid = tid,
		.snapshot = snapshot, .slot = slot, .call_again = call_again, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_fetch_tuple_body,
				doltgres_entries_doltgres_index_fetch_tuple_pad);
	doltgres_entries_doltgres_index_fetch_tuple_pad(&frame);
	return frame.ok;
}

/* state.rs' held at Option<values::Entry>, which relation_fetch_toast_slice takes. */
#define TXN_STATE_TS OptionValuesEntry
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnValuesEntry *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnValuesEntry *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_relation_fetch_toast_slice_captures_t is the TOAST
 * slice's lent call's captures: the reader and the value id.
 */
typedef struct doltgres_entries_doltgres_relation_fetch_toast_slice_captures_t
{
	const HostSharedReader *reading;
	uint32		valueid;
} doltgres_entries_doltgres_relation_fetch_toast_slice_captures_t;

/*
 * doltgres_entries_doltgres_relation_fetch_toast_slice_closure_0 is the TOAST
 * slice's lent call: the value id's entry in the value map.
 */
static bool
doltgres_entries_doltgres_relation_fetch_toast_slice_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															   const HostSharedHeld *held, bool *some,
															   TxnValuesEntry *result, HostError *error)
{
	doltgres_entries_doltgres_relation_fetch_toast_slice_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	return txn_values_entry(io, TREES_STORE_READER_NODE_STORE(&store), held, c->valueid, some, result, error);
}

/*
 * doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t is the TOAST
 * slice's state across its pad: what it opened, the packs' store, and the
 * blobs read through it, while live.
 */
typedef struct doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t
{
	doltgres_entries_opened_t opened;
	TreesStoreReader packs;
	TxnBlobsBlobs blobs;
	bool		blobs_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *toastrel;
	uint32		valueid;
	int32		attrsize;
	int32		sliceoffset;
	PgToastSliceResult *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t;

/* doltgres_entries_doltgres_relation_fetch_toast_slice_pad frees what the TOAST slice holds. */
static void
doltgres_entries_doltgres_relation_fetch_toast_slice_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t *frame = frame_;

	if (frame->blobs_live)
		txn_blobs_Blobs_drop(&frame->blobs);
	frame->blobs_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_relation_fetch_toast_slice_body reads the value's
 * entry and fills the slice from its blob.
 */
static void
doltgres_entries_doltgres_relation_fetch_toast_slice_body(void *frame_)
{
	doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t *frame = frame_;
	doltgres_entries_doltgres_relation_fetch_toast_slice_captures_t captures = {.reading = &frame->opened.reading,
	.valueid = frame->valueid};
	const RepoRepo *repo;
	TxnStateFound found;
	bool		has_entry;
	TxnValuesEntry entry;
	uint32		repository;
	TreesStoreTree tree;
	HostError	failed;
	ChunksErrorError storage;

	if (!frame->result->ok)
	{
		*frame->error = frame->result->u.error;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->toastrel, &found, frame->error))
		return;
	if (!txn_state_held_OptionValuesEntry(&frame->io, repo, &frame->opened.reading, found,
										  doltgres_entries_doltgres_relation_fetch_toast_slice_closure_0, &captures,
										  NULL, &has_entry, &entry, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!has_entry)
	{
		frame->ok = pg_calls_refuse_missing_toast(pg_CatalogCaps_may_raise(frame->caps), frame->toastrel,
												  frame->valueid, frame->sliceoffset, frame->error);
		return;
	}
	if (frame->attrsize < 0 || (uint64) frame->attrsize != entry.stored)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("toast value "), STD_FMT_ARG(0), STD_FMT_LIT("'s stored size "),
		STD_FMT_ARG(1), STD_FMT_LIT(" isn't its map entry's "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&frame->valueid, std_u32_Display_fmt}, {&frame->attrsize, std_i32_Display_fmt},
		{&entry.stored, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX001", &message);
		return;
	}
	frame->packs = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->opened.reading);
	repository = pg_calls_rel_storage(frame->toastrel).database;
	if (repository == 0)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a TOAST relation has no database");
		return;
	}
	if (!txn_blobs_Blobs_new(&frame->io, repository, TREES_STORE_READER_NODE_STORE(&frame->packs), &frame->blobs,
							 &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->blobs_live = true;
	if (frame->sliceoffset < 0)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a TOAST slice has a negative offset");
		return;
	}
	tree = txn_values_Entry_tree(&entry);
	if (!trees_blob_read_slice(&frame->io, TXN_BLOBS_BLOBS_NODE_STORE(&frame->blobs), TREES_STORE_TREE_NAMED(&tree),
							   (uint64) frame->sliceoffset, frame->result->u.slice.ptr, frame->result->u.slice.len,
							   &storage))
	{
		*frame->error = doltgres_entries_storage_error(&storage);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_fetch_toast_slice is the fork's
 * relation_fetch_toast_slice: it fills a TOAST slice from the blob the value
 * id's entry in the TOAST relation's value map names, read through the
 * verified read door, refusing an id the map doesn't hold as heap refuses a
 * value with no chunk, and an entry whose stored size isn't the pointer's as
 * data corruption.
 */
bool
doltgres_entries_doltgres_relation_fetch_toast_slice(const PgCatalogCaps *caps, const PgRel *toastrel, uint32 valueid,
													 int32 attrsize, int32 sliceoffset, PgToastSliceResult *result,
													 PgPgError *error)
{
	doltgres_entries_doltgres_relation_fetch_toast_slice_frame_t frame = {.caps = caps, .toastrel = toastrel,
		.valueid = valueid, .attrsize = attrsize, .sliceoffset = sliceoffset, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_fetch_toast_slice_body,
				doltgres_entries_doltgres_relation_fetch_toast_slice_pad);
	doltgres_entries_doltgres_relation_fetch_toast_slice_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_relation_set_new_filelocator_frame_t is
 * set_new_filelocator's state across its pad: what it opened.
 */
typedef struct doltgres_entries_doltgres_relation_set_new_filelocator_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgFileLocator *newrlocator;
	char		persistence;
	uint32	   *freeze_xid;
	uint32	   *minmulti;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_set_new_filelocator_frame_t;

/* doltgres_entries_doltgres_relation_set_new_filelocator_pad frees what set_new_filelocator holds. */
static void
doltgres_entries_doltgres_relation_set_new_filelocator_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_set_new_filelocator_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_relation_set_new_filelocator_body makes the new
 * file and records a new user relation's identity pairs.
 */
static void
doltgres_entries_doltgres_relation_set_new_filelocator_body(void *frame_)
{
	doltgres_entries_doltgres_relation_set_new_filelocator_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint32		command;

	if (!pg_calls_new_filelocator(raise, frame->newrlocator, frame->persistence, frame->freeze_xid, frame->minmulti,
								  frame->error))
		return;
	frame->io = pghost_io_catalog(frame->caps);
	if (doltgres_settle_kept_frozen(&frame->io, pg_calls_rel_id(frame->rel)))
	{
		uint64		frozen = pg_calls_rel_frozen(frame->rel);

		*frame->freeze_xid = (uint32) frozen;
		*frame->minmulti = (uint32) (frozen >> 32);
	}
	if (!doltgres_identity_users(frame->rel) ||
		doltgres_relation_rewrites(pg_calls_rel_id(frame->rel), pg_calls_rel_rewrite(frame->rel)) != 0)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!pg_calls_current_command(raise, true, &command, frame->error))
		return;
	frame->ok = doltgres_identity_recorded(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
										   &frame->opened.reading, frame->rel, command, frame->error);
}

/*
 * doltgres_entries_doltgres_relation_set_new_filelocator is the fork's
 * relation_set_new_filelocator: heap's own, the relation's new file made with
 * its delete at abort, an unlogged relation's init fork beside it, and
 * relfrozenxid and relminmxid as heap gives them, or the relation's own for a
 * settle's new relfilenode (section 6); the storage needs nothing more, since
 * one nothing wrote has no state (the rewrite text's section 2). A new user
 * relation records its identity pairs here, in its creating transaction
 * (design item 10); a rewrite's new heap records none, since the rewritten
 * table keeps its own.
 */
bool
doltgres_entries_doltgres_relation_set_new_filelocator(const PgCatalogCaps *caps, const PgRel *rel,
													   const PgFileLocator *newrlocator, char persistence,
													   uint32 *freezeXid, uint32 *minmulti, PgPgError *error)
{
	doltgres_entries_doltgres_relation_set_new_filelocator_frame_t frame = {.caps = caps, .rel = rel,
		.newrlocator = newrlocator, .persistence = persistence, .freeze_xid = freezeXid, .minmulti = minmulti,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_set_new_filelocator_body,
				doltgres_entries_doltgres_relation_set_new_filelocator_pad);
	doltgres_entries_doltgres_relation_set_new_filelocator_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_toast_created_frame_t is toast_created's state across its pad: what it opened. */
typedef struct doltgres_entries_doltgres_toast_created_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *relation;
	uint32		toast;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_toast_created_frame_t;

/* doltgres_entries_doltgres_toast_created_pad frees what toast_created holds. */
static void
doltgres_entries_doltgres_toast_created_pad(void *frame_)
{
	doltgres_entries_doltgres_toast_created_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_toast_created_body records the TOAST relation's identity pair for a stored user table. */
static void
doltgres_entries_doltgres_toast_created_body(void *frame_)
{
	doltgres_entries_doltgres_toast_created_frame_t *frame = frame_;
	bool		stored;

	if (!doltgres_identity_users(frame->relation))
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_relation_stored(frame->caps, frame->relation, &stored, frame->error))
		return;
	if (!stored)
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	frame->ok = doltgres_identity_toast_created(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
												&frame->opened.reading, frame->relation, frame->toast, frame->error);
}

/*
 * doltgres_entries_doltgres_toast_created records a user table's new TOAST
 * relation's identity pair in the transaction that made it, at the end of
 * create_toast_table (design item 10), for a table the fork stores.
 */
bool
doltgres_entries_doltgres_toast_created(const PgCatalogCaps *caps, const PgRel *relation, uint32 toast,
										PgPgError *error)
{
	doltgres_entries_doltgres_toast_created_frame_t frame = {.caps = caps, .relation = relation, .toast = toast,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_toast_created_body, doltgres_entries_doltgres_toast_created_pad);
	doltgres_entries_doltgres_toast_created_pad(&frame);
	return frame.ok;
}

/* txn::state::held at (), which layouts.c defines and nontransactional_truncate takes. */
#define TXN_STATE_TS Unit
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_relation_nontransactional_truncate_closure_0 is
 * the truncate's lent call: the held storage emptied.
 */
static bool
doltgres_entries_doltgres_relation_nontransactional_truncate_closure_0(void *captures, HostIoIo *io,
																	   const RepoRepo *repo,
																	   const HostSharedHeld *held, HostError *error)
{
	return txn_state_emptied(io, held, error);
}

/*
 * doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t is the
 * truncate's state across its pad: what it opened.
 */
typedef struct doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t;

/* doltgres_entries_doltgres_relation_nontransactional_truncate_pad frees what the truncate holds. */
static void
doltgres_entries_doltgres_relation_nontransactional_truncate_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_relation_nontransactional_truncate_body empties the storage under its held state. */
static void
doltgres_entries_doltgres_relation_nontransactional_truncate_body(void *frame_)
{
	doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t *frame = frame_;
	TxnStateFound found;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
		return;
	if (!txn_state_held_Unit(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading, found,
							 doltgres_entries_doltgres_relation_nontransactional_truncate_closure_0, NULL, NULL,
							 &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_nontransactional_truncate is the fork's
 * relation_nontransactional_truncate, which Postgres calls only for a
 * relfilenode the current subtransaction made: it empties the storage in
 * place (the rewrite text's section 3).
 */
bool
doltgres_entries_doltgres_relation_nontransactional_truncate(const PgCatalogCaps *caps, const PgRel *rel,
															 PgPgError *error)
{
	doltgres_entries_doltgres_relation_nontransactional_truncate_frame_t frame = {.caps = caps, .rel = rel,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_nontransactional_truncate_body,
				doltgres_entries_doltgres_relation_nontransactional_truncate_pad);
	doltgres_entries_doltgres_relation_nontransactional_truncate_pad(&frame);
	return frame.ok;
}

/* state.rs' held at rewrite's Read, which relation_copy_for_cluster takes. */
#define TXN_STATE_TS RewriteRead
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , DoltgresRewriteRead *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS DoltgresRewriteRead *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at bool, which entries.c defines. */
#define TXN_STATE_TS Bool
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* doltgres_entries_Carry_drop is Carry's drop glue, an element of carrying's answer: its row change's. */
static void
doltgres_entries_Carry_drop(void *carry)
{
	txn_record_RowChange_drop(&((TxnRewriteCarry *) carry)->f0);
}

/* doltgres_entries_Copied_drop is Copied's drop glue, an element of copied's answer. */
static void
doltgres_entries_Copied_drop(void *copied)
{
	txn_rewrite_Copied_drop(copied);
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_captures_t is the
 * copy's lent calls' captures: the reader, the old storage's key order and
 * whether it is keyless, the fold's horizon and the rewrite's, and for the
 * install, the versions it carries, the old base's entries, the copied
 * entries while live, the old base's fold points, the packing, the new
 * storage's nodes' xid, and the conflict horizon.
 */
typedef struct doltgres_entries_doltgres_relation_copy_for_cluster_captures_t
{
	const HostSharedReader *reading;
	const TreesKeyOrder *order;
	bool		keyless;
	uint64		folding;
	uint64		horizon;
	const StdVec *carried;
	StdVec	   *based;
	StdVec	   *copied;
	bool	   *copied_live;
	uint64		folded;
	uint64		max_folded;
	uint64		max_ended;
	TxnRewritePacked packed;
	bool		has_nodes;
	uint64		nodes;
	uint64		conflict;
	bool		logged;
} doltgres_entries_doltgres_relation_copy_for_cluster_captures_t;

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t is
 * the copy's read's state across its pad: the fold's warnings, while live.
 */
typedef struct doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t
{
	StdVec		warned;
	bool		warned_live;
	doltgres_entries_doltgres_relation_copy_for_cluster_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	DoltgresRewriteRead *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t;

/* doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_pad frees what the copy's read holds. */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t *frame = frame_;

	if (frame->warned_live)
		std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	frame->warned_live = false;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_body folds the
 * old storage now and reads its newest base.
 */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_body(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t *frame = frame_;
	doltgres_entries_doltgres_relation_copy_for_cluster_captures_t *c = frame->c;
	TxnInsertPage page = doltgres_entries_page();
	bool		has_point;
	HostLsn		point;
	bool		folded;
	bool		some;
	TxnBaseBase base;
	TreesStoreReader store;

	if (!txn_fold_point(frame->io, &has_point, &point, frame->error))
		return;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	if (!txn_fold_fold_now(frame->io, frame->repo, c->reading, frame->held, c->order, c->keyless, has_point, point,
						   c->folding, &frame->warned, &page, NULL, &folded, frame->error))
		return;
	frame->warned_live = false;
	std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (!txn_state_newest(frame->io, frame->held, &some, &base, frame->error))
		return;
	if (!some)
	{
		*frame->error = doltgres_entries_host_fork("XX000", "a rewrite found a storage state with no base");
		return;
	}
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	frame->ok = doltgres_rewrite_read(frame->io, TREES_STORE_READER_NODE_STORE(&store), frame->held, &base, c->horizon,
									  frame->result, frame->error);
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_0 is the copy's
 * read: the old storage folded and its rows read under the held state.
 */
static bool
doltgres_entries_doltgres_relation_copy_for_cluster_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															  const HostSharedHeld *held, DoltgresRewriteRead *result,
															  HostError *error)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_frame_t frame = {.c = captures, .io = io,
	.repo = repo, .held = held, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_body,
				doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_pad);
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t is
 * the install's state across its pad: the new storage's base record until
 * new_base takes it, and the carried puts' places, each while live.
 */
typedef struct doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t
{
	TxnBaseBase made;
	bool		made_live;
	StdVec		places;
	bool		places_live;
	doltgres_entries_doltgres_relation_copy_for_cluster_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	bool	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t;

/* doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_pad frees what the install holds. */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t *frame = frame_;

	if (frame->places_live)
		std_vec_free_Checked(&frame->places, sizeof(host_TupleRowU32_t), _Alignof(host_TupleRowU32_t));
	if (frame->made_live)
		txn_base_Base_drop(&frame->made);
	frame->places_live = frame->made_live = false;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_body builds
 * the new storage's base whole and installs it, then carries the delta's
 * versions it keeps.
 */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_body(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t *frame = frame_;
	doltgres_entries_doltgres_relation_copy_for_cluster_captures_t *c = frame->c;
	const TxnRewriteCarry *carried = c->carried->ptr;
	TxnInsertPage page = doltgres_entries_page();
	bool		some;
	bool		first = true;
	StdVec		based;
	TxnFoldTrees trees;
	TxnBaseBase base;
	bool		built;
	TreesStoreReader store;

	if (!txn_state_base_record(frame->io, frame->held, &some, &frame->made, frame->error))
		return;
	if (!some)
	{
		*frame->error = doltgres_entries_host_fork("XX000", "a rewrite's new storage has no base record");
		return;
	}
	frame->made_live = true;
	std_vec_new(&frame->places, sizeof(host_TupleRowU32_t), _Alignof(host_TupleRowU32_t));
	frame->places_live = true;
	for (size_t i = 0; i < c->carried->len; i++)
	{
		const TxnMemtablePut *put;
		host_TupleRowU32_t place;

		if (!txn_record_RowChange_put(&carried[i].f0, &put))
			continue;
		place.f0 = put->tuple;
		place.f1 = put->heap_bytes;
		if (first)
			std_vec_collect_first(&frame->places, &place, 0, sizeof(host_TupleRowU32_t),
								  _Alignof(host_TupleRowU32_t));
		else
			std_vec_extend_one(&frame->places, &place, 0, sizeof(host_TupleRowU32_t), _Alignof(host_TupleRowU32_t));
		first = false;
	}
	based = *c->based;
	*c->based = (StdVec) STD_VEC_NEW;
	frame->made_live = false;
	*c->copied_live = false;
	if (!txn_rewrite_new_base(&based, c->copied, c->keyless, c->folded, c->max_folded, c->max_ended, &frame->made,
							  c->packed, frame->places.ptr, frame->places.len, &trees, &base, frame->error))
		return;
	if (!txn_fold_built(frame->io, frame->repo, c->reading, frame->held, c->order, &trees, &base, c->folded,
						c->has_nodes, c->nodes, c->conflict, &built, frame->error))
		return;
	if (!built)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	if (!txn_rewrite_carried(frame->io, TREES_STORE_READER_NODE_STORE(&store), frame->held, NULL, 0, carried,
							 c->carried->len, c->logged, &page, pg_calls_max_heap_tuples_per_page(), frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_closure_5 is the copy's
 * install: the new storage's base under the held state.
 */
static bool
doltgres_entries_doltgres_relation_copy_for_cluster_closure_5(void *captures, HostIoIo *io, const RepoRepo *repo,
															  const HostSharedHeld *held, bool *result,
															  HostError *error)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_frame_t frame = {.c = captures, .io = io,
	.repo = repo, .held = held, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_body,
				doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_pad);
	doltgres_entries_doltgres_relation_copy_for_cluster_closure_5_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_frame_t is the copy's
 * state across its pad: what it opened, the old storage's key, its columns
 * until key_order takes them, its keyed columns, its key order, the read,
 * the rewrite's results, the new TOAST storage's state, the carried
 * versions, the copied entries until the install takes them, and the new
 * TOAST storage's base record and value entries until the build takes them,
 * each while live.
 */
typedef struct doltgres_entries_doltgres_relation_copy_for_cluster_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	key;
	bool		key_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		keyed;
	bool		keyed_live;
	TreesKeyOrder order;
	bool		order_live;
	DoltgresRewriteRead read;
	bool		read_live;
	DoltgresRewriteRewritten done;
	bool		done_live;
	TxnRecordToast toast;
	HostSharedHeld values;
	bool		toast_live;
	StdVec		carried;
	bool		carried_live;
	StdVec		copied;
	bool		copied_live;
	TxnBaseBase made;
	bool		made_live;
	StdVec		taken;
	bool		taken_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *old_table;
	const PgRel *new_table;
	const PgRel *old_index;
	bool		use_sort;
	uint32		oldest_xmin;
	uint32	   *xid_cutoff;
	float8	   *num_tuples;
	float8	   *tups_vacuumed;
	float8	   *tups_recently_dead;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_copy_for_cluster_frame_t;

/* doltgres_entries_doltgres_relation_copy_for_cluster_pad frees what the copy holds. */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_frame_t *frame = frame_;

	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, sizeof(txn_TupleU32TupleEntryU64_t),
							 _Alignof(txn_TupleU32TupleEntryU64_t));
	if (frame->made_live)
		txn_base_Base_drop(&frame->made);
	if (frame->copied_live)
		std_vec_drop_Checked(&frame->copied, sizeof(TxnRewriteCopied), _Alignof(TxnRewriteCopied),
							 doltgres_entries_Copied_drop);
	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry),
							 doltgres_entries_Carry_drop);
	if (frame->toast_live)
		host_shared_Held_drop(&frame->values);
	if (frame->done_live)
		doltgres_rewrite_Rewritten_drop(&frame->done);
	if (frame->read_live)
		doltgres_rewrite_Read_drop(&frame->read);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->taken_live = frame->made_live = frame->copied_live = frame->carried_live = frame->toast_live = false;
	frame->done_live = frame->read_live = frame->order_live = frame->keyed_live = frame->columns_live = false;
	frame->key_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_toast builds the new
 * TOAST storage's base whole from the value entries the rewrite kept.
 */
static bool
doltgres_entries_doltgres_relation_copy_for_cluster_toast(doltgres_entries_doltgres_relation_copy_for_cluster_frame_t *frame,
														  bool has_nodes, uint64 nodes)
{
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	bool		some;
	TxnFoldTrees trees;
	TxnBaseSizes sizes;
	bool		built;
	bool		mapped;
	HostError	failed;

	if (!txn_state_base_record(&frame->io, &frame->values, &some, &frame->made, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!some)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a rewrite's new TOAST storage has no base record");
		return false;
	}
	frame->made_live = true;
	frame->taken = frame->done.values;
	frame->done.values = (StdVec) STD_VEC_NEW;
	frame->taken_live = true;
	mapped = txn_rewrite_value_map(frame->taken.ptr, frame->taken.len, &trees, &sizes, &failed);
	frame->taken_live = false;
	std_vec_free_Checked(&frame->taken, sizeof(txn_TupleU32TupleEntryU64_t), _Alignof(txn_TupleU32TupleEntryU64_t));
	if (!mapped)
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->made.sizes = sizes;
	frame->made_live = false;
	if (!txn_fold_built(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading,
						&frame->values, &order, &trees, &frame->made, 0, has_nodes, nodes, 0, &built, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!built)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a rewrite's new TOAST storage changed while it was built");
		return false;
	}
	return true;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster_body reads and rewrites
 * the old storage, then installs the new storage's base and carries its
 * versions.
 */
static void
doltgres_entries_doltgres_relation_copy_for_cluster_body(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_entries_doltgres_relation_copy_for_cluster_captures_t captures = {.reading = &frame->opened.reading,
		.order = &frame->order, .carried = &frame->carried, .based = &frame->read.based, .copied = &frame->copied,
	.copied_live = &frame->copied_live};
	const RepoRepo *repo;
	TxnBaseBase recorded;
	TxnStateFound found;
	int32		target;
	DoltgresRewriteTable table;
	StdVec		key_columns;
	uint64		cutoff;
	bool		has_oldest = false;
	uint64		oldest = 0;
	TxnStateFind find;
	bool		installed;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->old_table, &recorded,
								   frame->error))
		return;
	frame->key = recorded.key;
	frame->key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->keyed);
	frame->keyed_live = true;
	captures.keyless = frame->columns.len == 0;
	frame->columns_live = false;
	if (!doltgres_layout_key_order(raise, catalog, &frame->key, &frame->columns, &frame->order, frame->error))
		return;
	frame->order_live = true;
	if (!pg_calls_widened_xid(raise, frame->oldest_xmin, &captures.horizon, frame->error) ||
		!pg_calls_fold_horizon(raise, frame->old_table, &captures.folding, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->old_table, &found, frame->error))
		return;
	if (!txn_state_held_RewriteRead(&frame->io, repo, &frame->opened.reading, found,
									doltgres_entries_doltgres_relation_copy_for_cluster_closure_0, &captures, NULL,
									&frame->read, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->read_live = true;
	target = pg_calls_rel_toast_target(frame->new_table);
	table.rel = frame->old_table;
	table.keyed = frame->keyed.ptr;
	table.keyed_len = frame->keyed.len;
	table.keyless = captures.keyless;
	table.target = target < 0 ? 0 : (size_t) target;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &key_columns, frame->error) ||
		!doltgres_rewrite_rewritten(frame->caps, repo, &frame->opened.reading, &table, &key_columns,
									frame->new_table, frame->old_index, frame->use_sort, &frame->read,
									captures.horizon, &frame->done, frame->error))
		return;
	frame->done_live = true;
	if (frame->done.values.len > 0)
	{
		if (!doltgres_entries_toast_state(frame->caps, &frame->io, repo, &frame->opened.reading, frame->new_table,
										  &frame->toast, &frame->values, frame->error))
			return;
		frame->toast_live = true;
	}
	if (!doltgres_rewrite_carrying(&frame->read, &frame->done, captures.keyless,
								   doltgres_entries_storage(frame->new_table), pg_calls_rel_id(frame->new_table),
								   frame->toast_live ? &frame->toast : NULL, &frame->carried, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->carried_live = true;
	doltgres_rewrite_copied(&frame->read, &frame->done, &frame->copied);
	frame->copied_live = true;
	if (!pg_calls_widened_xid(raise, *frame->xid_cutoff, &cutoff, frame->error))
		return;
	for (size_t i = 0; i < frame->carried.len; i++)
	{
		const TxnMemtableVersionWriter *writer = &STD_VEC_AT(&frame->carried, TxnRewriteCarry, i).f0.writer;
		uint64		xid = writer->has_top ? writer->top : writer->xid;

		if (xid < TXN_STATUS_FIRST_NORMAL || (has_oldest && xid >= oldest))
			continue;
		has_oldest = true;
		oldest = xid;
	}
	if (has_oldest && oldest < cutoff)
		*frame->xid_cutoff = (uint32) oldest;
	captures.logged = pg_calls_rel_permanent(frame->new_table);
	captures.has_nodes = pg_calls_rel_needs_wal(frame->new_table);
	if (captures.has_nodes && !pg_calls_top_xid(raise, &captures.nodes, frame->error))
		return;
	if (frame->toast_live &&
		!doltgres_entries_doltgres_relation_copy_for_cluster_toast(frame, captures.has_nodes, captures.nodes))
		return;
	captures.folded = frame->read.base.folded;
	captures.max_folded = frame->read.base.max_folded;
	captures.max_ended = frame->read.base.max_ended;
	captures.conflict = Max(frame->read.left_out, frame->read.base.max_folded);
	captures.packed = frame->done.packed;
	{
		HostStorage storage = doltgres_entries_storage(frame->new_table);

		if (!txn_state_holds(&frame->io, &storage, doltgres_entries_persistence(frame->new_table), &find, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
	}
	if (find.tag == TXN_STATE_FIND_HELD)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a rewrite's new relfilenode already holds a storage");
		return;
	}
	found.tag = TXN_STATE_FOUND_MADE;
	found.u.made.f0 = find.u.missing;
	found.u.made.f1.key = frame->read.base.key;
	frame->read.base.key.tag = TXN_BASE_KEY_NONE;
	found.u.made.f1.table = pg_calls_rel_id(frame->new_table);
	found.u.made.f1.has_index = false;
	found.u.made.f1.index = 0;
	if (!txn_state_held_Bool(&frame->io, repo, &frame->opened.reading, found,
							 doltgres_entries_doltgres_relation_copy_for_cluster_closure_5, &captures, NULL, &installed,
							 &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!installed)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a rewrite's new storage changed while it was built");
		return;
	}
	*frame->num_tuples = frame->done.counts.kept;
	*frame->tups_vacuumed = frame->done.counts.vacuumed;
	*frame->tups_recently_dead = frame->done.counts.recently_dead;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_copy_for_cluster is the fork's
 * relation_copy_for_cluster, CLUSTER's and VACUUM FULL's copy of a table into
 * the new relfilenode Postgres made (the rewrite text's section 4): it folds
 * the old storage under its horizon, reads the newest base, which every
 * snapshot may read, with the delta's versions that base doesn't apply,
 * rewrites them as heap's rewrite does, installs the new storage's base whole
 * under the old storage's key, at the old base's fold point, carries the
 * delta's versions it keeps, and lowers xid_cutoff to the oldest top-level
 * xid they name (item 7's relfrozenxid rule). The fork makes no multixacts,
 * so multi_cutoff stays. Postgres passes a snapshot only for REPACK
 * (CONCURRENTLY), which it runs on heap's tables alone.
 */
bool
doltgres_entries_doltgres_relation_copy_for_cluster(const PgCatalogCaps *caps, const PgRel *old_table,
													const PgRel *new_table, const PgRel *old_index, bool use_sort,
													uint32 oldest_xmin, const PgSnapshot *snapshot,
													uint32 *xid_cutoff, uint32 *multi_cutoff, float8 *num_tuples,
													float8 *tups_vacuumed, float8 *tups_recently_dead,
													PgPgError *error)
{
	doltgres_entries_doltgres_relation_copy_for_cluster_frame_t frame = {.caps = caps, .old_table = old_table,
		.new_table = new_table, .old_index = old_index, .use_sort = use_sort, .oldest_xmin = oldest_xmin,
		.xid_cutoff = xid_cutoff, .num_tuples = num_tuples, .tups_vacuumed = tups_vacuumed,
	.tups_recently_dead = tups_recently_dead, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_copy_for_cluster_body,
				doltgres_entries_doltgres_relation_copy_for_cluster_pad);
	doltgres_entries_doltgres_relation_copy_for_cluster_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_MapCarried_drop is MapCarried's drop glue, an element of spaces' answer. */
static void
doltgres_entries_MapCarried_drop(void *carried)
{
	txn_record_MapCarried_drop(carried);
}

/*
 * doltgres_entries_moved_base returns the base a move copies (the rewrite
 * text's section 5): the storage's newest base, as it is, its counter the
 * storage's live one, past every tuple id the delta took.
 */
static bool
doltgres_entries_moved_base(HostIoIo *io, const HostSharedHeld *held, TxnBaseBase *result, HostError *error)
{
	bool		some;

	if (!txn_state_newest(io, held, &some, result, error))
		return false;
	if (!some)
	{
		*error = doltgres_entries_host_fork("XX000", "a move found a storage state with no base");
		return false;
	}
	result->counter = txn_state_counter(host_shared_Held_record(held));
	return true;
}

/*
 * doltgres_entries_moved_into_captures_t is moved_into's lent call's
 * captures: the reader, the base it copies while live, the nodes' xid,
 * whether the relation is logged, and the map versions and versions it
 * carries.
 */
typedef struct doltgres_entries_moved_into_captures_t
{
	const HostSharedReader *reading;
	TxnBaseBase *newest;
	bool	   *newest_live;
	bool		has_nodes;
	uint64		nodes;
	bool		logged;
	const TxnRecordMapCarried *spaces;
	size_t		spaces_len;
	const TxnRewriteCarry *carried;
	size_t		carried_len;
} doltgres_entries_moved_into_captures_t;

/*
 * doltgres_entries_moved_into_closure_0_frame_t is moved_into's lent call's
 * state across its pad: the copied chunks, while live.
 */
typedef struct doltgres_entries_moved_into_closure_0_frame_t
{
	TxnFoldCopies copies;
	bool		copies_live;
	doltgres_entries_moved_into_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	bool	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_moved_into_closure_0_frame_t;

/* doltgres_entries_moved_into_closure_0_pad frees what moved_into's lent call holds. */
static void
doltgres_entries_moved_into_closure_0_pad(void *frame_)
{
	doltgres_entries_moved_into_closure_0_frame_t *frame = frame_;

	if (frame->copies_live)
		txn_fold_Copies_drop(&frame->copies);
	frame->copies_live = false;
}

/* doltgres_entries_moved_into_closure_0_body copies the base into the new storage and carries the versions. */
static void
doltgres_entries_moved_into_closure_0_body(void *frame_)
{
	doltgres_entries_moved_into_closure_0_frame_t *frame = frame_;
	doltgres_entries_moved_into_captures_t *c = frame->c;
	HostLsn		point = c->newest->folded;
	HostLsn		read;
	bool		copied;
	TreesStoreReader store;
	StdVec		writers;
	bool		ok;

	std_vec_new(&frame->copies.copies, sizeof(txn_TupleLocatorLocator_t), _Alignof(txn_TupleLocatorLocator_t));
	frame->copies_live = true;
	*c->newest_live = false;
	std_vec_new(&writers, sizeof(uint64), _Alignof(uint64));
	for (size_t i = 0; i < c->spaces_len; i++)
		for (size_t v = 0; v < c->spaces[i].versions.len; v++)
		{
			const TxnMemtableVersionWriter *writer = &STD_VEC_AT(&c->spaces[i].versions, TxnRecordCarriedTake,
																 v).writer;
			uint64		top = writer->has_top ? writer->top : writer->xid;

			std_vec_push_Checked(&writers, &top, sizeof(top), _Alignof(uint64));
		}
	for (size_t i = 0; i < c->carried_len; i++)
	{
		const TxnMemtableVersionWriter *writer = &c->carried[i].f0.writer;
		uint64		top = writer->has_top ? writer->top : writer->xid;

		std_vec_push_Checked(&writers, &top, sizeof(top), _Alignof(uint64));
	}
	ok = txn_rewrite_moved_point(frame->io, c->newest->read, writers.ptr, writers.len, &read, frame->error);
	std_vec_free_Checked(&writers, sizeof(uint64), _Alignof(uint64));
	if (!ok)
		return;
	if (!txn_fold_copied(frame->io, frame->repo, c->reading, frame->held, c->newest, point, read, c->has_nodes,
						 c->nodes, &frame->copies, &copied, frame->error))
		return;
	if (!copied)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	if (!txn_rewrite_carried(frame->io, TREES_STORE_READER_NODE_STORE(&store), frame->held, c->spaces, c->spaces_len,
							 c->carried, c->carried_len, c->logged, NULL, pg_calls_max_heap_tuples_per_page(),
							 frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/*
 * doltgres_entries_moved_into_closure_0 is moved_into's lent call: the move's
 * copy and carries under the new storage's held state.
 */
static bool
doltgres_entries_moved_into_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *result, HostError *error)
{
	doltgres_entries_moved_into_closure_0_frame_t frame = {.c = captures, .io = io, .repo = repo, .held = held,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_moved_into_closure_0_body, doltgres_entries_moved_into_closure_0_pad);
	doltgres_entries_moved_into_closure_0_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_moved_into_frame_t is moved_into's state across its pad: the base it copies, while live. */
typedef struct doltgres_entries_moved_into_frame_t
{
	TxnBaseBase newest;
	bool		newest_live;
	doltgres_entries_moved_into_captures_t captures;
	const PgMayRaise *raise;
	HostIoIo   *io;
	const RepoRepo *repo;
	const PgRel *rel;
	HostStorage moved;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_moved_into_frame_t;

/* doltgres_entries_moved_into_pad frees what moved_into holds. */
static void
doltgres_entries_moved_into_pad(void *frame_)
{
	doltgres_entries_moved_into_frame_t *frame = frame_;

	if (frame->newest_live)
		txn_base_Base_drop(&frame->newest);
	frame->newest_live = false;
}

/* doltgres_entries_moved_into_body makes the new storage and installs the copy under its held state. */
static void
doltgres_entries_moved_into_body(void *frame_)
{
	doltgres_entries_moved_into_frame_t *frame = frame_;
	doltgres_entries_moved_into_captures_t *c = &frame->captures;
	TxnStateFind find;
	TxnStateFound made;
	bool		installed;
	HostError	failed;

	c->logged = pg_calls_rel_permanent(frame->rel);
	c->has_nodes = c->logged && pg_calls_xlog_is_needed();
	if (c->has_nodes && !pg_calls_top_xid(frame->raise, &c->nodes, frame->error))
		return;
	if (!txn_state_holds(frame->io, &frame->moved, doltgres_entries_persistence(frame->rel), &find, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (find.tag == TXN_STATE_FIND_HELD)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a move's new relfilenode already holds a storage");
		return;
	}
	made.tag = TXN_STATE_FOUND_MADE;
	made.u.made.f0 = find.u.missing;
	txn_base_Key_Clone_clone(&frame->newest.key, &made.u.made.f1.key);
	made.u.made.f1.table = frame->newest.table;
	made.u.made.f1.has_index = frame->newest.has_index;
	made.u.made.f1.index = frame->newest.index;
	c->newest = &frame->newest;
	c->newest_live = &frame->newest_live;
	if (!txn_state_held_Bool(frame->io, frame->repo, c->reading, made, doltgres_entries_moved_into_closure_0, c, NULL,
							 &installed, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!installed)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a move's new storage changed while it was copied");
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_moved_into makes a moved relation's storage at its new
 * relfilenode with the old one's key, table, and index, copies into it chunk
 * by chunk the newest base a move found, which it takes, each logged as a
 * built base's nodes when the relation is permanent and WAL is needed, and
 * carries the delta's map versions and row or entry versions as they are,
 * logged when the relation is permanent (the rewrite text's section 5).
 */
static bool
doltgres_entries_moved_into(const PgMayRaise *raise, HostIoIo *io, const RepoRepo *repo,
							const HostSharedReader *reading, const PgRel *rel, HostStorage moved, TxnBaseBase *newest,
							const TxnRecordMapCarried *spaces, size_t spaces_len, const TxnRewriteCarry *carried,
							size_t carried_len, PgPgError *error)
{
	doltgres_entries_moved_into_frame_t frame = {.newest = *newest, .newest_live = true,
		.captures = {.reading = reading, .spaces = spaces, .spaces_len = spaces_len, .carried = carried,
		.carried_len = carried_len}, .raise = raise, .io = io, .repo = repo, .rel = rel, .moved = moved,
	.error = error};

	std_landing(&frame, doltgres_entries_moved_into_body, doltgres_entries_moved_into_pad);
	doltgres_entries_moved_into_pad(&frame);
	return frame.ok;
}

/* state.rs' held at (Base, Vec<Carry>, Vec<MapCarried>), which relation_copy_data takes. */
#define TXN_STATE_TS TupleBaseVecCarryVecMapCarried
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , TxnBaseBase *result_0, StdVec *result_1, StdVec *result_2, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS TxnBaseBase *result_0; StdVec *result_1; StdVec *result_2;
#define TXN_STATE_RESULT_INIT , result_0, result_1, result_2
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1, (c)->result_2
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_relation_copy_data_captures_t is the move's lent
 * call's captures: the reader, the key order and whether it is keyless, the
 * fold's horizon, the new storage, and the table.
 */
typedef struct doltgres_entries_doltgres_relation_copy_data_captures_t
{
	const HostSharedReader *reading;
	const TreesKeyOrder *order;
	bool		keyless;
	uint64		folding;
	HostStorage moved;
	uint32		table;
} doltgres_entries_doltgres_relation_copy_data_captures_t;

/*
 * doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t is the
 * move's lent call's state across its pad: the fold's warnings, the base, and
 * the carried versions, each while live.
 */
typedef struct doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t
{
	StdVec		warned;
	bool		warned_live;
	bool		base_live;
	bool		carried_live;
	doltgres_entries_doltgres_relation_copy_data_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	TxnBaseBase *base;
	StdVec	   *carried;
	StdVec	   *spaces;
	HostError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t;

/* doltgres_entries_doltgres_relation_copy_data_closure_0_pad frees what the move's lent call holds. */
static void
doltgres_entries_doltgres_relation_copy_data_closure_0_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t *frame = frame_;

	if (frame->carried_live)
		std_vec_drop_Checked(frame->carried, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry),
							 doltgres_entries_Carry_drop);
	if (frame->base_live)
		txn_base_Base_drop(frame->base);
	if (frame->warned_live)
		std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	frame->carried_live = frame->base_live = frame->warned_live = false;
}

/*
 * doltgres_entries_doltgres_relation_copy_data_closure_0_body folds the
 * storage now, then reads the base it copies, the versions it carries, and
 * its free space map's versions.
 */
static void
doltgres_entries_doltgres_relation_copy_data_closure_0_body(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t *frame = frame_;
	doltgres_entries_doltgres_relation_copy_data_captures_t *c = frame->c;
	TxnInsertPage page = doltgres_entries_page();
	bool		has_point;
	HostLsn		point;
	bool		folded;

	if (!txn_fold_point(frame->io, &has_point, &point, frame->error))
		return;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	if (!txn_fold_fold_now(frame->io, frame->repo, c->reading, frame->held, c->order, c->keyless, has_point, point,
						   c->folding, &frame->warned, &page, NULL, &folded, frame->error))
		return;
	frame->warned_live = false;
	std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (!doltgres_entries_moved_base(frame->io, frame->held, frame->base, frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_rewrite_moving(frame->io, frame->held, frame->base->read, c->moved, c->table, false, 0,
								 frame->carried, frame->error))
		return;
	frame->carried_live = true;
	if (!txn_rewrite_spaces(frame->io, frame->held, c->moved, frame->spaces, frame->error))
		return;
	frame->carried_live = frame->base_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_copy_data_closure_0 is the move's lent
 * call: what it copies and carries, under the held state.
 */
static bool
doltgres_entries_doltgres_relation_copy_data_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
													   const HostSharedHeld *held, TxnBaseBase *result_0,
													   StdVec *result_1, StdVec *result_2, HostError *error)
{
	doltgres_entries_doltgres_relation_copy_data_closure_0_frame_t frame = {.c = captures, .io = io, .repo = repo,
	.held = held, .base = result_0, .carried = result_1, .spaces = result_2, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_copy_data_closure_0_body,
				doltgres_entries_doltgres_relation_copy_data_closure_0_pad);
	doltgres_entries_doltgres_relation_copy_data_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_relation_copy_data_frame_t is the move's state
 * across its pad: what it opened, the key, the key order, the base until
 * moved_into takes it, the carried versions, the free space map's versions,
 * the TOAST storage's state, and a row's blobs, each while live.
 */
typedef struct doltgres_entries_doltgres_relation_copy_data_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	key;
	bool		key_live;
	TreesKeyOrder order;
	bool		order_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		carried;
	bool		carried_live;
	StdVec		spaces;
	bool		spaces_live;
	HostSharedHeld values;
	bool		values_live;
	StdVec		blobs;
	bool		blobs_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgFileLocator *newrlocator;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_copy_data_frame_t;

/* doltgres_entries_doltgres_relation_copy_data_pad frees what the move holds. */
static void
doltgres_entries_doltgres_relation_copy_data_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_data_frame_t *frame = frame_;

	if (frame->blobs_live)
		std_vec_free_Checked(&frame->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	if (frame->values_live)
		host_shared_Held_drop(&frame->values);
	if (frame->spaces_live)
		std_vec_drop_Checked(&frame->spaces, sizeof(TxnRecordMapCarried), _Alignof(TxnRecordMapCarried),
							 doltgres_entries_MapCarried_drop);
	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry),
							 doltgres_entries_Carry_drop);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->blobs_live = frame->values_live = frame->spaces_live = frame->carried_live = frame->base_live = false;
	frame->order_live = frame->key_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_relation_copy_data_named names each carried
 * row's out-of-line values by the TOAST relation and the blobs its value map
 * gives them, as the move carries them.
 */
static bool
doltgres_entries_doltgres_relation_copy_data_named(doltgres_entries_doltgres_relation_copy_data_frame_t *frame)
{
	TxnRecordToast toast;
	TreesStoreReader store;
	HostError	failed;

	if (!doltgres_entries_toast_state(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
									  &frame->opened.reading, frame->rel, &toast, &frame->values, frame->error))
		return false;
	frame->values_live = true;
	store = repo_Repo_reader(doltgres_entries_opened_repo(&frame->opened), HOST_CACHE_PLACEMENT_SERVER,
							 &frame->opened.reading);
	for (size_t i = 0; i < frame->carried.len; i++)
	{
		TxnRecordRowChange *change = &STD_VEC_AT(&frame->carried, TxnRewriteCarry, i).f0;
		const TxnMemtablePut *put;
		const uint32 *ids = NULL;
		size_t		ids_len = 0;

		if (txn_record_RowChange_put(change, &put))
		{
			ids = put->values.ptr;
			ids_len = put->values.len;
		}
		std_vec_with_capacity_Checked(&frame->blobs, ids_len, sizeof(chunks_TupleAddressU64_t),
									  _Alignof(chunks_TupleAddressU64_t));
		frame->blobs_live = true;
		for (size_t j = 0; j < ids_len; j++)
		{
			bool		has_entry;
			TxnValuesEntry entry;
			chunks_TupleAddressU64_t blob;

			if (!txn_values_entry(&frame->io, TREES_STORE_READER_NODE_STORE(&store), &frame->values, ids[j],
								  &has_entry, &entry, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return false;
			}
			if (!has_entry)
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("a moved row's value "), STD_FMT_ARG(0),
				STD_FMT_LIT(" has no map entry")};
				StdFmtArg	args[] = {{&ids[j], std_u32_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*frame->error = pg_PgError_fork((const uint8 *) "XX001", &message);
				return false;
			}
			blob.f0 = entry.address;
			blob.f1 = entry.stored;
			std_vec_push_Checked(&frame->blobs, &blob, sizeof(chunks_TupleAddressU64_t),
								 _Alignof(chunks_TupleAddressU64_t));
		}
		frame->blobs_live = false;
		if (frame->blobs.len == 0)
		{
			std_vec_free_Checked(&frame->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
			continue;
		}
		change->has_toast = true;
		change->toast = toast;
		std_vec_free_Checked(&change->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
		change->blobs = frame->blobs;
	}
	frame->values_live = false;
	host_shared_Held_drop(&frame->values);
	return true;
}

/* doltgres_entries_doltgres_relation_copy_data_body makes the new file, reads what the move copies, and copies it. */
static void
doltgres_entries_doltgres_relation_copy_data_body(void *frame_)
{
	doltgres_entries_doltgres_relation_copy_data_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_entries_doltgres_relation_copy_data_captures_t captures = {.reading = &frame->opened.reading,
	.order = &frame->order};
	uint32		freeze_xid = 0;
	uint32		min_multi = 0;
	const RepoRepo *repo;
	TxnBaseBase recorded;
	StdVec		columns;
	DgStorage	located;
	TxnStateFound found;
	bool		valued = false;
	HostError	failed;

	if (!pg_calls_new_filelocator(raise, frame->newrlocator, pg_calls_rel_persistence(frame->rel), &freeze_xid,
								  &min_multi, frame->error))
		return;
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &recorded,
								   frame->error))
		return;
	frame->key = recorded.key;
	frame->key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &columns, frame->error))
		return;
	captures.keyless = columns.len == 0;
	if (!doltgres_layout_key_order(raise, catalog, &frame->key, &columns, &frame->order, frame->error))
		return;
	frame->order_live = true;
	located = pg_calls_filelocator_storage(frame->newrlocator);
	captures.moved.tablespace = located.tablespace;
	captures.moved.database = located.database;
	captures.moved.relation = located.relation;
	captures.table = pg_calls_rel_id(frame->rel);
	if (!pg_calls_fold_horizon(raise, frame->rel, &captures.folding, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
		return;
	if (!txn_state_held_TupleBaseVecCarryVecMapCarried(&frame->io, repo, &frame->opened.reading, found,
													   doltgres_entries_doltgres_relation_copy_data_closure_0,
													   &captures, NULL, &frame->base, &frame->carried,
													   &frame->spaces, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->base_live = frame->carried_live = frame->spaces_live = true;
	for (size_t i = 0; i < frame->carried.len && !valued; i++)
	{
		const TxnMemtablePut *put;

		valued = txn_record_RowChange_put(&STD_VEC_AT(&frame->carried, TxnRewriteCarry, i).f0, &put) &&
			put->values.len > 0;
	}
	if (valued && !doltgres_entries_doltgres_relation_copy_data_named(frame))
		return;
	frame->base_live = false;
	if (!doltgres_entries_moved_into(raise, &frame->io, repo, &frame->opened.reading, frame->rel, captures.moved,
									 &frame->base, frame->spaces.ptr, frame->spaces.len, frame->carried.ptr,
									 frame->carried.len, frame->error))
		return;
	frame->ok = pg_calls_drop_storage(raise, frame->rel, frame->error);
}

/*
 * doltgres_entries_doltgres_relation_copy_data is the fork's
 * relation_copy_data, ALTER TABLE SET TABLESPACE's move of a storage into the
 * new relfilenode (the rewrite text's section 5): it makes the new file as
 * heap does, an unlogged relation's init fork beside it, folds the old
 * storage under its horizon, copies chunk by chunk into packs of the new
 * tablespace its newest base as it is, which every snapshot reads, its
 * counter the old storage's, past every tuple id the delta took, carries the
 * delta's map versions and every row version the base doesn't apply as they
 * are, a row's values named in the TOAST relation at its old locator, which
 * moves after it in the same command, and drops the old file at commit. A
 * TOAST relation's storage moves by the same code, its value map its base and
 * its map versions.
 */
bool
doltgres_entries_doltgres_relation_copy_data(const PgCatalogCaps *caps, const PgRel *rel,
											 const PgFileLocator *newrlocator, PgPgError *error)
{
	doltgres_entries_doltgres_relation_copy_data_frame_t frame = {.caps = caps, .rel = rel,
	.newrlocator = newrlocator, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_copy_data_body,
				doltgres_entries_doltgres_relation_copy_data_pad);
	doltgres_entries_doltgres_relation_copy_data_pad(&frame);
	return frame.ok;
}

/* state.rs' held at (Base, Vec<Carry>), which index_copy_data takes. */
#define TXN_STATE_TS TupleBaseVecCarry
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , TxnBaseBase *result_0, StdVec *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS TxnBaseBase *result_0; StdVec *result_1;
#define TXN_STATE_RESULT_INIT , result_0, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_index_copy_data_captures_t is the index's move's
 * lent call's captures: the new storage, the table, and the index.
 */
typedef struct doltgres_entries_doltgres_index_copy_data_captures_t
{
	HostStorage moved;
	uint32		table;
	uint32		index;
} doltgres_entries_doltgres_index_copy_data_captures_t;

/*
 * doltgres_entries_doltgres_index_copy_data_closure_0_frame_t is the index's
 * move's lent call's state across its pad: the base, while live.
 */
typedef struct doltgres_entries_doltgres_index_copy_data_closure_0_frame_t
{
	bool		base_live;
	doltgres_entries_doltgres_index_copy_data_captures_t *c;
	HostIoIo   *io;
	const HostSharedHeld *held;
	TxnBaseBase *base;
	StdVec	   *carried;
	HostError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_copy_data_closure_0_frame_t;

/* doltgres_entries_doltgres_index_copy_data_closure_0_pad frees what the index's move's lent call holds. */
static void
doltgres_entries_doltgres_index_copy_data_closure_0_pad(void *frame_)
{
	doltgres_entries_doltgres_index_copy_data_closure_0_frame_t *frame = frame_;

	if (frame->base_live)
		txn_base_Base_drop(frame->base);
	frame->base_live = false;
}

/*
 * doltgres_entries_doltgres_index_copy_data_closure_0_body reads the base the
 * index's move copies and the versions it carries.
 */
static void
doltgres_entries_doltgres_index_copy_data_closure_0_body(void *frame_)
{
	doltgres_entries_doltgres_index_copy_data_closure_0_frame_t *frame = frame_;
	doltgres_entries_doltgres_index_copy_data_captures_t *c = frame->c;

	if (!doltgres_entries_moved_base(frame->io, frame->held, frame->base, frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_rewrite_moving(frame->io, frame->held, frame->base->read, c->moved, c->table, true, c->index,
								 frame->carried, frame->error))
		return;
	frame->base_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_copy_data_closure_0 is the index's move's
 * lent call: what it copies and carries, under the held state.
 */
static bool
doltgres_entries_doltgres_index_copy_data_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
													const HostSharedHeld *held, TxnBaseBase *result_0,
													StdVec *result_1, HostError *error)
{
	doltgres_entries_doltgres_index_copy_data_closure_0_frame_t frame = {.c = captures, .io = io, .held = held,
	.base = result_0, .carried = result_1, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_copy_data_closure_0_body,
				doltgres_entries_doltgres_index_copy_data_closure_0_pad);
	doltgres_entries_doltgres_index_copy_data_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_index_copy_data_frame_t is the index's move's
 * state across its pad: what it opened, the base moved_into takes, and the
 * carried versions, while live.
 */
typedef struct doltgres_entries_doltgres_index_copy_data_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseBase base;
	StdVec		carried;
	bool		carried_live;
	const PgCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgFileLocator *newrlocator;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_copy_data_frame_t;

/* doltgres_entries_doltgres_index_copy_data_pad frees what the index's move holds. */
static void
doltgres_entries_doltgres_index_copy_data_pad(void *frame_)
{
	doltgres_entries_doltgres_index_copy_data_frame_t *frame = frame_;

	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry),
							 doltgres_entries_Carry_drop);
	frame->carried_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_copy_data_body reads what the index's move copies and copies it. */
static void
doltgres_entries_doltgres_index_copy_data_body(void *frame_)
{
	doltgres_entries_doltgres_index_copy_data_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	doltgres_entries_doltgres_index_copy_data_captures_t captures;
	DgStorage	located;
	TxnStateFound found;
	HostError	failed;

	if (!pg_calls_index_routine_forks(frame->rel))
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io(raise);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	located = pg_calls_filelocator_storage(frame->newrlocator);
	captures.moved.tablespace = located.tablespace;
	captures.moved.database = located.database;
	captures.moved.relation = located.relation;
	captures.table = pg_calls_index_table(frame->rel);
	captures.index = pg_calls_rel_id(frame->rel);
	if (!doltgres_entries_index_making(&frame->io, frame->rel, &found, frame->error))
		return;
	if (!txn_state_held_TupleBaseVecCarry(&frame->io, doltgres_entries_opened_repo(&frame->opened),
										  &frame->opened.reading, found,
										  doltgres_entries_doltgres_index_copy_data_closure_0, &captures, NULL,
										  &frame->base, &frame->carried, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->carried_live = true;
	frame->ok = doltgres_entries_moved_into(raise, &frame->io, doltgres_entries_opened_repo(&frame->opened),
											&frame->opened.reading, frame->rel, captures.moved, &frame->base, NULL, 0,
											frame->carried.ptr, frame->carried.len, frame->error);
}

/*
 * doltgres_entries_doltgres_index_copy_data is the fork's part of
 * index_copy_data, ALTER INDEX SET TABLESPACE's move of an index into the new
 * relfilenode whose file Postgres made (the rewrite text's section 5): for an
 * index whose routine is the fork's, it copies its newest base into packs of
 * the new tablespace and carries every entry version the base doesn't apply
 * as it is (moved_into), and the old file's unlink at commit frees the old
 * storage; Postgres' page copy alone moves any other index.
 */
bool
doltgres_entries_doltgres_index_copy_data(const PgCaps *caps, const PgRel *rel, const PgFileLocator *newrlocator,
										  PgPgError *error)
{
	doltgres_entries_doltgres_index_copy_data_frame_t frame = {.caps = caps, .rel = rel,
	.newrlocator = newrlocator, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_copy_data_body, doltgres_entries_doltgres_index_copy_data_pad);
	doltgres_entries_doltgres_index_copy_data_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_key_named_frame_t is key_named's state across its
 * pad: what it opened and the recorded key, while live.
 */
typedef struct doltgres_entries_doltgres_key_named_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	key;
	bool		key_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *table;
	bool		primary;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_key_named_frame_t;

/* doltgres_entries_doltgres_key_named_pad frees what key_named holds. */
static void
doltgres_entries_doltgres_key_named_pad(void *frame_)
{
	doltgres_entries_doltgres_key_named_frame_t *frame = frame_;

	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->key_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_key_named_body pins the table's old key and names the table for the settle. */
static void
doltgres_entries_doltgres_key_named_body(void *frame_)
{
	doltgres_entries_doltgres_key_named_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const RepoRepo *repo;
	HostStorage storage = doltgres_entries_storage(frame->table);
	uint32		table = pg_calls_rel_id(frame->table);
	bool		stored;
	bool		some;
	TxnBaseBase base;
	HostError	failed;

	if (!frame->primary)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_relation_stored(frame->caps, frame->table, &stored, frame->error))
		return;
	if (!stored)
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!txn_state_stored_base(&frame->io, repo, &frame->opened.reading, &storage, &some, &base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!some)
	{
		frame->ok = true;
		return;
	}
	txn_base_Base_drop(&base);
	if (!doltgres_settle_is_pinned(&frame->io, table))
	{
		StdVec		columns;

		if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->table, &base,
									   frame->error))
			return;
		frame->key = base.key;
		frame->key_live = true;
		base.key.tag = TXN_BASE_KEY_NONE;
		txn_base_Base_drop(&base);
		if (!doltgres_layout_key_columns(raise, pg_CatalogCaps_catalog(frame->caps), &frame->key, &columns,
										 frame->error))
			return;
		if (!doltgres_settle_pin(&frame->io, table, &columns, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->key_live = false;
		txn_base_Key_drop(&frame->key);
	}
	if (!doltgres_settle_named(&frame->io, table, (uint32) pg_calls_transaction_level(), &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_key_named names a table whose primary key the
 * statement creates, marks, or drops, when the index its site creates,
 * marks, or drops is a primary key's (primary), for the settle at the
 * statement's end (the rewrite text's section 6), first pinning its storage's
 * key while the catalog still gives the old one, so a state redo made with no
 * key never takes the new key the settle compares against, and its old key's
 * columns while every one has its type, which a drop in the same statement
 * may take from the catalog; a table whose storage nothing wrote has no key
 * to change and isn't named.
 */
bool
doltgres_entries_doltgres_key_named(const PgCatalogCaps *caps, const PgRel *table, bool primary, PgPgError *error)
{
	doltgres_entries_doltgres_key_named_frame_t frame = {.caps = caps, .table = table, .primary = primary,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_key_named_body, doltgres_entries_doltgres_key_named_pad);
	doltgres_entries_doltgres_key_named_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_key_level_opened opens a utility statement's level
 * of the settle's list, before its dispatch.
 */
bool
doltgres_entries_doltgres_key_level_opened(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (doltgres_settle_opened(&io, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_settle_each settles each table of a list the settle gave, which it takes. */
static bool
doltgres_entries_settle_each(const PgCatalogCaps *caps, StdVec *tables, PgPgError *error)
{
	bool		ok = true;

	for (size_t i = 0; i < tables->len && ok; i++)
		ok = pg_calls_key_settle_open(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps),
									  STD_VEC_AT(tables, uint32, i), error);
	std_vec_free_Checked(tables, sizeof(uint32), _Alignof(uint32));
	return ok;
}

/*
 * doltgres_entries_doltgres_key_level_settled closes a utility statement's
 * level of the settle's list after its dispatch and settles each table it
 * named; an empty level reads nothing.
 */
bool
doltgres_entries_doltgres_key_level_settled(const PgCatalogCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io_catalog(caps);
	StdVec		tables;

	doltgres_settle_closed(&io, &tables);
	return doltgres_entries_settle_each(caps, &tables, error);
}

/*
 * doltgres_entries_doltgres_key_settle settles a table ALTER TABLE altered
 * and doesn't rewrite, in its phase 3 before the table's constraints are
 * checked (the rewrite text's section 6).
 */
bool
doltgres_entries_doltgres_key_settle(const PgCatalogCaps *caps, uint32 table, PgPgError *error)
{
	return pg_calls_key_settle_open(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), table, error);
}

/*
 * doltgres_entries_doltgres_key_settle_commit settles every table the
 * transaction's levels still name before its commit or prepare, for a drop
 * made outside any utility statement (PRevPF 8's K2).
 */
bool
doltgres_entries_doltgres_key_settle_commit(const PgCatalogCaps *caps, int32 event, void *arg, PgPgError *error)
{
	HostIoIo	io;
	StdVec		tables;
	HostError	failed;

	if (event != XACT_EVENT_PRE_COMMIT && event != XACT_EVENT_PRE_PREPARE)
		return true;
	io = pghost_io_catalog(caps);
	if (!doltgres_settle_pending(&io, &tables, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	return doltgres_entries_settle_each(caps, &tables, error);
}

/*
 * doltgres_entries_doltgres_new_heap records the key a transient table
 * make_new_heap made takes at its storage's first make: its old table's
 * catalog key, which is the new key when the command changed it (the rewrite
 * text's section 6), with the old table's primary key index, whose rebuild
 * refuses a key the copy writes twice.
 */
bool
doltgres_entries_doltgres_new_heap(const PgCatalogCaps *caps, uint32 new_table, const PgRel *old_table,
								   PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(caps);
	TxnBaseKey	key;
	uint32		index;
	HostIoIo	io;
	HostError	failed;

	if (!doltgres_layout_catalog_key(raise, catalog, old_table, &key, error))
		return false;
	if (!pg_calls_rel_key_index(raise, catalog, old_table, &index, error))
	{
		txn_base_Key_drop(&key);
		return false;
	}
	io = pghost_io_catalog(caps);
	if (doltgres_settle_transient(&io, new_table, &key, index, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* state.rs' held at u64, which relation_size takes. */
#define TXN_STATE_TS U64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS uint64 *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at Sizes, which index_blocks takes. */
#define TXN_STATE_TS Sizes
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , TxnBaseSizes *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS TxnBaseSizes *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_blocks returns the block count of a relation's storage
 * from its state: its tree's totals and its tuple id counter's block, the
 * fresh page past every row's ctid, base's and delta's alike; a TOAST
 * relation's from its value map's totals.
 */
uint64
doltgres_entries_blocks(const PgRel *relation, const HostSharedHeld *held)
{
	TxnBaseSizes totals;
	uint64		counter;

	if (pg_calls_rel_kind(relation) == RELKIND_TOASTVALUE)
	{
		totals = txn_state_totals(held);
		return doltgres_sizes_toast_blocks(&totals, pg_calls_rel_usable_bytes(relation));
	}
	counter = txn_state_counter(host_shared_Held_record(held));
	totals = txn_state_totals(held);
	return doltgres_sizes_blocks(&totals, counter, pg_calls_rel_usable_bytes(relation));
}

/* doltgres_entries_doltgres_relation_size_closure_0 is relation_size's lent call: the storage's block count. */
static bool
doltgres_entries_doltgres_relation_size_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
												  const HostSharedHeld *held, uint64 *result, HostError *error)
{
	*result = doltgres_entries_blocks(captures, held);
	return true;
}

/* doltgres_entries_doltgres_relation_size_frame_t is relation_size's state across its pad: what it opened. */
typedef struct doltgres_entries_doltgres_relation_size_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	uint64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_size_frame_t;

/* doltgres_entries_doltgres_relation_size_pad frees what relation_size holds. */
static void
doltgres_entries_doltgres_relation_size_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_size_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_relation_size_body counts the storage's totals when unknown and reads its block count. */
static void
doltgres_entries_doltgres_relation_size_body(void *frame_)
{
	doltgres_entries_doltgres_relation_size_frame_t *frame = frame_;
	const RepoRepo *repo;
	TxnStateFound found;
	uint64		blocks;
	HostError	failed;

	frame->io = pghost_io(pg_CatalogCaps_may_raise(frame->caps));
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_counted(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->rel, false,
								  frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
		return;
	if (!txn_state_held_U64(&frame->io, repo, &frame->opened.reading, found,
							doltgres_entries_doltgres_relation_size_closure_0, (void *) frame->rel, NULL, &blocks,
							&failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	*frame->result = STD_MUL(u64, blocks, (uint64) BLCKSZ, 3846, 8);
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_relation_size is the fork's relation_size: the
 * main fork's block count times the block size, from the storage's tree's
 * totals and its tuple id counter (PLAN item 5's sizes for costs), the free
 * space map fork's own pages, which Postgres keeps (bugs row 87), and 0 for
 * every other fork, which the fork's relations don't have.
 */
bool
doltgres_entries_doltgres_relation_size(const PgCatalogCaps *caps, const PgRel *rel, int32 forkNumber, uint64 *result,
										PgPgError *error)
{
	doltgres_entries_doltgres_relation_size_frame_t frame = {.caps = caps, .rel = rel, .result = result,
	.error = error};

	if (forkNumber == FSM_FORKNUM)
	{
		uint32		blocks;

		if (!pg_calls_fork_blocks(pg_CatalogCaps_may_raise(caps), rel, forkNumber, &blocks, error))
			return false;
		*result = (uint64) blocks * BLCKSZ;
		return true;
	}
	if (forkNumber != MAIN_FORKNUM)
	{
		*result = 0;
		return true;
	}
	std_landing(&frame, doltgres_entries_doltgres_relation_size_body, doltgres_entries_doltgres_relation_size_pad);
	doltgres_entries_doltgres_relation_size_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_index_blocks_closure_1 is index_blocks' lent call: the index storage's totals. */
static bool
doltgres_entries_doltgres_index_blocks_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
												 const HostSharedHeld *held, TxnBaseSizes *result, HostError *error)
{
	*result = txn_state_totals(held);
	return true;
}

/*
 * doltgres_entries_doltgres_index_blocks_frame_t is index_blocks' state
 * across its pad: what it opened and the index's table, while live.
 */
typedef struct doltgres_entries_doltgres_index_blocks_frame_t
{
	doltgres_entries_opened_t opened;
	PgRefRel	table;
	bool		table_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *relation;
	uint32	   *blocks;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_blocks_frame_t;

/* doltgres_entries_doltgres_index_blocks_pad frees what index_blocks holds. */
static void
doltgres_entries_doltgres_index_blocks_pad(void *frame_)
{
	doltgres_entries_doltgres_index_blocks_frame_t *frame = frame_;

	if (frame->table_live)
		pg_RefRel_drop(&frame->table);
	frame->table_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_blocks_body counts the index storage's totals when unknown and derives its blocks. */
static void
doltgres_entries_doltgres_index_blocks_body(void *frame_)
{
	doltgres_entries_doltgres_index_blocks_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const RepoRepo *repo;
	bool		has_table;
	TxnStateFound made;
	TxnBaseSizes totals;
	TxnBaseBlockCounts counts;
	uint64		blocks;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!pg_RefRel_opened(raise, pg_CatalogCaps_catalog(frame->caps), pg_calls_index_table(frame->relation),
						  &has_table, &frame->table, frame->error))
		return;
	if (!has_table)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a fork index's table is gone");
		return;
	}
	frame->table_live = true;
	if (!doltgres_entries_counted(frame->caps, &frame->io, repo, &frame->opened.reading, frame->relation,
								  pg_RefRel_rel(&frame->table), true, frame->error) ||
		!doltgres_entries_index_making(&frame->io, frame->relation, &made, frame->error))
		return;
	if (!txn_state_held_Sizes(&frame->io, repo, &frame->opened.reading, made,
							  doltgres_entries_doltgres_index_blocks_closure_1, NULL, NULL, &totals, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	counts = txn_base_Sizes_counts(&totals, TREES_CHUNKER_DEFAULT_TARGET);
	blocks = STD_ADD(u64, STD_ADD(u64, counts.leaves, counts.internal, 3878, 27), 1, 3878, 27);
	*frame->blocks = blocks > MaxBlockNumber ? MaxBlockNumber : (uint32) blocks;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_blocks gives a fork index's block count, in
 * RelationGetNumberOfBlocksInFork: its tree's leaves and the levels above
 * them as its entries' totals derive them, with the metapage nbtree keeps,
 * and none for another fork (design item 5's "Sizes for costs"), so every
 * size function and cost reads the same count; it answers nothing for an
 * index the fork doesn't store.
 */
bool
doltgres_entries_doltgres_index_blocks(const PgCatalogCaps *caps, const PgRel *relation, int32 forkNum,
									   uint32 *blocks, bool *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_blocks_frame_t frame = {.caps = caps, .relation = relation, .blocks = blocks,
	.error = error};

	if (!doltgres_index_ported(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), relation, result, error))
		return false;
	if (!*result)
		return true;
	if (forkNum != MAIN_FORKNUM)
	{
		*blocks = 0;
		return true;
	}
	std_landing(&frame, doltgres_entries_doltgres_index_blocks_body, doltgres_entries_doltgres_index_blocks_pad);
	doltgres_entries_doltgres_index_blocks_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_size_added returns a size Postgres summed with bytes the
 * fork adds, saturating as Rust's i64 add does.
 */
static int64
doltgres_entries_size_added(int64 size, uint64 added)
{
	int64		sum;

	if (pg_add_s64_overflow(size, added > PG_INT64_MAX ? PG_INT64_MAX : (int64) added, &sum))
		return PG_INT64_MAX;
	return sum;
}

/*
 * doltgres_entries_doltgres_relation_size_added adds a fork relation's main
 * fork's size to the size Postgres summed from its files, which hold
 * nothing, in pg_relation_size for the fork it names and in the sums over a
 * table's relations for every fork: a table's block count and an index's,
 * times BLCKSZ, and nothing for a relation the fork doesn't store or another
 * fork, which has no file but the free space map's, whose pages Postgres
 * summed.
 */
bool
doltgres_entries_doltgres_relation_size_added(const PgCatalogCaps *caps, const PgRel *rel, const PgText *forkName,
											  int64 size, int64 *result, PgPgError *error)
{
	int32		fork_number = MAIN_FORKNUM;
	uint64		added = 0;

	if (forkName != NULL && !pg_calls_fork_named(pg_CatalogCaps_may_raise(caps), forkName, &fork_number, error))
		return false;
	if (pg_calls_rel_kind(rel) == RELKIND_INDEX)
	{
		uint32		blocks = 0;
		bool		ported;

		if (!doltgres_entries_doltgres_index_blocks(caps, rel, fork_number, &blocks, &ported, error))
			return false;
		if (ported)
			added = (uint64) blocks * BLCKSZ;
	}
	else
	{
		bool		stored;

		if (!doltgres_relation_stored(caps, rel, &stored, error))
			return false;
		if (stored && fork_number == MAIN_FORKNUM &&
			!doltgres_entries_doltgres_relation_size(caps, rel, fork_number, &added, error))
			return false;
	}
	*result = doltgres_entries_size_added(size, added);
	return true;
}

/*
 * doltgres_entries_doltgres_visibility_counted counts every block of a table
 * the fork stores all-visible and all-frozen, in visibilitymap_count, since
 * the fork keeps no visibility map and every block is both (design items 5
 * and 16): its block count, as RelationGetNumberOfBlocks gives it, the
 * all-frozen count only where its caller asks for one; it answers nothing
 * for a relation the fork doesn't store.
 */
bool
doltgres_entries_doltgres_visibility_counted(const PgCatalogCaps *caps, const PgRel *rel, uint32 *all_visible,
											 uint32 *all_frozen, bool *result, PgPgError *error)
{
	uint64		bytes;

	if (!doltgres_relation_stored(caps, rel, result, error))
		return false;
	if (!*result)
		return true;
	if (!doltgres_entries_doltgres_relation_size(caps, rel, MAIN_FORKNUM, &bytes, error))
		return false;
	*all_visible = (uint32) ((bytes + (BLCKSZ - 1)) / BLCKSZ);
	if (all_frozen != NULL)
		*all_frozen = *all_visible;
	return true;
}

/*
 * doltgres_entries_doltgres_heap_only_refused answers whether the fork
 * stores a relation, a table by access's rule and an index by whether its
 * entries are the fork's, for the heap-only contrib modules' checks (PLAN
 * item 5), which refuse it as they refuse a relation of another access
 * method.
 */
bool
doltgres_entries_doltgres_heap_only_refused(const PgCatalogCaps *caps, const PgRel *rel, bool *result,
											PgPgError *error)
{
	if (pg_calls_rel_kind(rel) == RELKIND_INDEX)
		return doltgres_index_ported(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), rel, result,
									 error);
	return doltgres_relation_stored(caps, rel, result, error);
}

/*
 * doltgres_entries_scan_frame_t is a scan cost's state across its pad: what
 * it opened, the index and its table, and the table's recorded base, each
 * while live, and the table's shape once read.
 */
typedef struct doltgres_entries_scan_frame_t
{
	doltgres_entries_opened_t opened;
	PgRefRel	index;
	bool		index_live;
	PgRefRel	table;
	bool		table_live;
	TxnBaseBase recorded;
	bool		recorded_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;

	/*
	 * indexoid is the index a scan reads, or 0 for a bitmap scan's table
	 * alone.
	 */
	uint32		indexoid;
	uint32		tableoid;
	/* stored is whether the fork stores the table, and so answers its costs. */
	bool		stored;

	/*
	 * keyed is whether the index is the table's primary key's, whose leaves
	 * are the table's rows.
	 */
	bool		keyed;

	/*
	 * rows and tuple_ids are the blocks of the table's tree and of its tuple
	 * id tree.
	 */
	TxnBaseBlockCounts rows;
	TxnBaseBlockCounts tuple_ids;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_scan_frame_t;

/* doltgres_entries_scan_pad frees what a scan cost holds. */
static void
doltgres_entries_scan_pad(void *frame_)
{
	doltgres_entries_scan_frame_t *frame = frame_;

	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	if (frame->table_live)
		pg_RefRel_drop(&frame->table);
	if (frame->index_live)
		pg_RefRel_drop(&frame->index);
	frame->recorded_live = frame->table_live = frame->index_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_scan_opened opens a relation the planner holds a lock on, as a fork error when it is gone. */
static bool
doltgres_entries_scan_opened(doltgres_entries_scan_frame_t *frame, uint32 relid, PgRefRel *result, bool *live)
{
	bool		has;

	if (!pg_RefRel_opened(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps), relid, &has,
						  result, frame->error))
		return false;
	if (!has)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a relation the planner costs is gone");
		return false;
	}
	*live = true;
	return true;
}

/*
 * doltgres_entries_scan_bytes returns entries times a size each plus more
 * bytes, saturating: a tuple id tree's bytes, whose entries are a tuple id's
 * six bytes keying the row's key and its four bytes of heap length.
 */
static uint64
doltgres_entries_scan_bytes(uint64 entries, uint64 each, uint64 more)
{
	uint64		product;
	uint64		sum;

	if (pg_mul_u64_overflow(entries, each, &product) || pg_add_u64_overflow(product, more, &sum))
		return PG_UINT64_MAX;
	return sum;
}

/*
 * doltgres_entries_scan_body reads a scanned table's shape: whether the fork
 * stores it, the blocks of its tree and of its tuple id tree from its
 * storage's totals, and whether the index is its primary key's.
 */
static void
doltgres_entries_scan_body(void *frame_)
{
	doltgres_entries_scan_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgRel *table;
	const RepoRepo *repo;
	TxnStateFound found;
	TxnBaseSizes totals;
	TxnBaseSizes tuple_ids = {0};
	HostError	failed;

	if (frame->indexoid != 0)
	{
		if (!doltgres_entries_scan_opened(frame, frame->indexoid, &frame->index, &frame->index_live))
			return;
		frame->tableoid = pg_calls_index_table(pg_RefRel_rel(&frame->index));
	}
	if (!doltgres_entries_scan_opened(frame, frame->tableoid, &frame->table, &frame->table_live))
		return;
	table = pg_RefRel_rel(&frame->table);
	if (!doltgres_relation_stored(frame->caps, table, &frame->stored, frame->error))
		return;
	if (!frame->stored)
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_counted(frame->caps, &frame->io, repo, &frame->opened.reading, table, table, false,
								  frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, table, &found, frame->error))
		return;
	if (!txn_state_held_Sizes(&frame->io, repo, &frame->opened.reading, found,
							  doltgres_entries_doltgres_index_blocks_closure_1, NULL, NULL, &totals, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->rows = txn_base_Sizes_counts(&totals, TREES_CHUNKER_DEFAULT_TARGET);
	tuple_ids.entries = totals.entries;
	tuple_ids.key_bytes = doltgres_entries_scan_bytes(totals.entries, 6, 0);
	tuple_ids.leaf_bytes = doltgres_entries_scan_bytes(totals.entries, 10, totals.key_bytes);
	frame->tuple_ids = txn_base_Sizes_counts(&tuple_ids, TREES_CHUNKER_DEFAULT_TARGET);
	if (frame->indexoid != 0)
	{
		if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, table,
									   &frame->recorded, frame->error))
			return;
		frame->recorded_live = true;
		if (!doltgres_index_keyed(raise, pg_CatalogCaps_catalog(frame->caps), pg_RefRel_rel(&frame->index),
								  &frame->recorded.key, &frame->keyed, frame->error))
			return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_scanned reads a scanned table's shape for its scans'
 * costs, by the index it is scanned through, or by the table itself when the
 * index is 0.
 */
bool
doltgres_entries_scanned(const PgCatalogCaps *caps, uint32 indexoid, uint32 tableoid, DoltgresEntriesScanned *result,
						 PgPgError *error)
{
	doltgres_entries_scan_frame_t frame = {.caps = caps, .indexoid = indexoid, .tableoid = tableoid, .error = error};

	std_landing(&frame, doltgres_entries_scan_body, doltgres_entries_scan_pad);
	doltgres_entries_scan_pad(&frame);
	if (!frame.ok)
		return false;
	result->stored = frame.stored;
	result->keyed = frame.keyed;
	result->rows = frame.rows;
	result->tuple_ids = frame.tuple_ids;
	return true;
}

/* doltgres_entries_cached_leaves returns the node cache's size in leaves of the default block size. */
static float8
doltgres_entries_cached_leaves(void)
{
	return (float8) pg_calls_settings_node_cache_size() * 1024.0 / BLCKSZ;
}

/*
 * doltgres_entries_leaves_read is Mackert and Lohman's count of a tree's
 * distinct leaves that fetches of its rows read past the node cache, as
 * index_pages_fetched counts heap pages with the node cache in
 * effective_cache_size's place, and none when the tree's leaves fit the
 * cache, which then holds them.
 */
static float8
doltgres_entries_leaves_read(float8 fetches, uint64 tree_leaves, float8 cached)
{
	float8		leaves = Max((float8) tree_leaves, 1.0);
	float8		limit;

	if (leaves <= cached)
		return 0;
	limit = 2.0 * leaves * cached / (2.0 * leaves - cached);
	if (fetches <= limit)
		return ceil(2.0 * leaves * fetches / (2.0 * leaves + fetches));
	return ceil(cached + (fetches - limit) * (leaves - cached) / leaves);
}

/*
 * doltgres_entries_doltgres_index_scan_io is a fork table's part of an index
 * scan's costs (PLAN item 16): an index-only scan reads no row of the table;
 * the primary key's own index reads its leaves, the table's rows, in key
 * order, the first at random and the rest in sequence, when they don't fit
 * the node cache; and a secondary index's entry leads to a descent of the
 * table's tree by its row key, a keyless table's by the row's hash, its
 * levels above the leaves held by the node cache at a comparison each and
 * its leaf read at random when the leaves don't fit the cache. It leaves
 * heap's costs for a table the fork doesn't store.
 */
bool
doltgres_entries_doltgres_index_scan_io(const PgCatalogCaps *caps, uint32 indexoid, bool indexonly,
										float8 tuples_fetched, float8 selectivity, float8 loop_count,
										float8 random_page_cost, float8 seq_page_cost, float8 cpu_operator_cost,
										float8 *max_io, float8 *min_io, float8 *rand_heap_pages, PgPgError *error)
{
	DoltgresEntriesScanned frame;
	float8		cached = doltgres_entries_cached_leaves();
	float8		read;
	float8		io;

	if (!doltgres_entries_scanned(caps, indexoid, 0, &frame, error))
		return false;
	if (!frame.stored)
		return true;
	if (indexonly)
		read = io = 0;
	else if (frame.keyed)
	{
		read = ceil(selectivity * Max((float8) frame.rows.leaves, 1.0));
		if ((float8) frame.rows.leaves <= cached)
			read = io = 0;
		else if (loop_count > 1)
		{
			read = doltgres_entries_leaves_read(read * loop_count, frame.rows.leaves, cached);
			io = read * random_page_cost / loop_count;
		}
		else
			io = read > 0 ? random_page_cost + (read - 1) * seq_page_cost : 0;
	}
	else
	{
		read = doltgres_entries_leaves_read(tuples_fetched * loop_count, frame.rows.leaves, cached);
		io = tuples_fetched * frame.rows.height * cpu_operator_cost + read * random_page_cost / loop_count;
	}
	*max_io = *min_io = io;
	*rand_heap_pages = read;
	return true;
}

/*
 * doltgres_entries_doltgres_bitmap_page_cost is a fork table's cost per page
 * fetched of a bitmap heap scan (PLAN item 16): each row costs its tuple id's
 * resolution, a descent of the tuple id tree, and a descent of the table's
 * tree by its row key, each level above the leaves a comparison and each
 * leaf past the node cache read at random, and each lossy block, split from
 * the pages it fetches by the bitmap's entries as compute_bitmap_pages splits
 * them, costs a range scan of the tuple id tree: a descent and its leaf. It
 * leaves heap's cost for a table the fork doesn't store.
 */
bool
doltgres_entries_doltgres_bitmap_page_cost(const PgCatalogCaps *caps, uint32 relid, float8 tuples_fetched,
										   float8 pages_fetched, uint32 pages, float8 maxentries,
										   float8 random_page_cost, float8 cpu_operator_cost, float8 *cost_per_page,
										   PgPgError *error)
{
	DoltgresEntriesScanned frame;
	float8		cached = doltgres_entries_cached_leaves();
	float8		heap_pages = Min(pages_fetched, (float8) pages);
	float8		lossy = 0;
	float8		io;

	if (!doltgres_entries_scanned(caps, 0, relid, &frame, error))
		return false;
	if (!frame.stored)
		return true;
	if (maxentries < heap_pages)
		lossy = Max(0, heap_pages - maxentries / 2);
	io = tuples_fetched * (frame.tuple_ids.height + frame.rows.height) * cpu_operator_cost +
		(doltgres_entries_leaves_read(tuples_fetched, frame.tuple_ids.leaves, cached) +
		 doltgres_entries_leaves_read(tuples_fetched, frame.rows.leaves, cached)) * random_page_cost +
		lossy * (frame.tuple_ids.height * cpu_operator_cost +
				 ((float8) frame.tuple_ids.leaves > cached ? random_page_cost : 0));
	*cost_per_page = pages_fetched > 0 ? io / pages_fetched : 0;
	return true;
}

/*
 * doltgres_entries_doltgres_database_size_frame_t is database_size's state
 * across its pad: a directory and the tablespaces, each while live.
 */
typedef struct doltgres_entries_doltgres_database_size_frame_t
{
	StdPathBuf	dir;
	bool		dir_live;
	StdVec		tablespaces;
	bool		tablespaces_live;
	HostIoIo	io;
	uint32		database;
	uint64		bytes;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_database_size_frame_t;

/* doltgres_entries_doltgres_database_size_pad frees what database_size holds. */
static void
doltgres_entries_doltgres_database_size_pad(void *frame_)
{
	doltgres_entries_doltgres_database_size_frame_t *frame = frame_;

	if (frame->tablespaces_live)
		std_vec_free_Checked(&frame->tablespaces, sizeof(uint32), _Alignof(uint32));
	if (frame->dir_live)
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	frame->tablespaces_live = frame->dir_live = false;
}

/* doltgres_entries_doltgres_database_size_bytes adds the bytes of the repository's directory in a tablespace. */
static bool
doltgres_entries_doltgres_database_size_bytes(doltgres_entries_doltgres_database_size_frame_t *frame,
											  uint32 tablespace)
{
	uint64		bytes;
	HostError	failed;

	frame->dir = host_tablespace_dir(frame->database, tablespace);
	frame->dir_live = true;
	if (!host_tree_bytes(HOST_IO_IO_IO_DOORS(&frame->io), (const char *) frame->dir.bytes.ptr, frame->dir.bytes.len,
						 &bytes, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->dir_live = false;
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	frame->bytes = std_saturating_add_u64(frame->bytes, bytes);
	return true;
}

/* doltgres_entries_doltgres_database_size_body sums the repository's files in every tablespace. */
static void
doltgres_entries_doltgres_database_size_body(void *frame_)
{
	doltgres_entries_doltgres_database_size_frame_t *frame = frame_;
	HostError	failed;

	if (!doltgres_entries_doltgres_database_size_bytes(frame, HOST_PG_DEFAULT))
		return;
	if (!host_tablespaces(HOST_IO_IO_READ_DOORS(&frame->io), &frame->tablespaces, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->tablespaces_live = true;
	for (size_t i = 0; i < frame->tablespaces.len; i++)
		if (!doltgres_entries_doltgres_database_size_bytes(frame, STD_VEC_AT(&frame->tablespaces, uint32, i)))
			return;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_database_size adds a database's repository's
 * files in every tablespace to the size Postgres summed from its
 * directories, in calculate_database_size (design item 5).
 */
bool
doltgres_entries_doltgres_database_size(const PgCaps *caps, uint32 dbOid, int64 size, int64 *result,
										PgPgError *error)
{
	doltgres_entries_doltgres_database_size_frame_t frame = {.io = pghost_io(pg_Caps_may_raise(caps)),
	.database = dbOid, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_database_size_body, doltgres_entries_doltgres_database_size_pad);
	doltgres_entries_doltgres_database_size_pad(&frame);
	if (frame.ok)
		*result = doltgres_entries_size_added(size, frame.bytes);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tablespace_size_frame_t is tablespace_size's state
 * across its pad: the directory, while live.
 */
typedef struct doltgres_entries_doltgres_tablespace_size_frame_t
{
	StdPathBuf	dir;
	bool		dir_live;
	HostIoIo	io;
	uint32		tablespace;
	uint64		bytes;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tablespace_size_frame_t;

/* DOLTGRES_ENTRIES_EMPTIED_NAP is the milliseconds DROP TABLESPACE sleeps between its asks to empty a tablespace. */
#define DOLTGRES_ENTRIES_EMPTIED_NAP 1000

/* doltgres_entries_doltgres_tablespace_size_pad frees what tablespace_size holds. */
static void
doltgres_entries_doltgres_tablespace_size_pad(void *frame_)
{
	doltgres_entries_doltgres_tablespace_size_frame_t *frame = frame_;

	if (frame->dir_live)
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	frame->dir_live = false;
}

/* doltgres_entries_doltgres_tablespace_size_body sums the files of the tablespace's repositories. */
static void
doltgres_entries_doltgres_tablespace_size_body(void *frame_)
{
	doltgres_entries_doltgres_tablespace_size_frame_t *frame = frame_;
	HostError	failed;

	frame->dir = host_versioned_dir(frame->tablespace);
	frame->dir_live = true;
	if (!host_tree_bytes(HOST_IO_IO_IO_DOORS(&frame->io), (const char *) frame->dir.bytes.ptr, frame->dir.bytes.len,
						 &frame->bytes, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_tablespace_size adds the files of every
 * repository a tablespace holds to the size Postgres summed from its
 * directory, in calculate_tablespace_size (design item 5).
 */
bool
doltgres_entries_doltgres_tablespace_size(const PgCaps *caps, uint32 tblspcOid, int64 size, int64 *result,
										  PgPgError *error)
{
	doltgres_entries_doltgres_tablespace_size_frame_t frame = {.io = pghost_io(pg_Caps_may_raise(caps)),
	.tablespace = tblspcOid, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tablespace_size_body, doltgres_entries_doltgres_tablespace_size_pad);
	doltgres_entries_doltgres_tablespace_size_pad(&frame);
	if (frame.ok)
		*result = doltgres_entries_size_added(size, frame.bytes);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tablespace_emptied_frame_t is tablespace_emptied's
 * state across its pad: the tablespace's directory of repositories, a path,
 * and the names listed in each, while live.
 */
typedef struct doltgres_entries_doltgres_tablespace_emptied_frame_t
{
	StdPathBuf	versioned;
	bool		versioned_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		names;
	bool		names_live;
	StdVec		inner;
	bool		inner_live;
	const PgCaps *caps;
	HostIoIo	io;
	uint32		tablespace;
	bool		redo;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tablespace_emptied_frame_t;

/* doltgres_entries_doltgres_tablespace_emptied_pad frees what tablespace_emptied holds. */
static void
doltgres_entries_doltgres_tablespace_emptied_pad(void *frame_)
{
	doltgres_entries_doltgres_tablespace_emptied_frame_t *frame = frame_;

	if (frame->inner_live)
		std_vec_drop_Checked(&frame->inner, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (frame->names_live)
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->versioned_live)
		std_vec_free_Checked(&frame->versioned.bytes, 1, 1);
	frame->inner_live = frame->names_live = frame->path_live = frame->versioned_live = false;
}

/*
 * doltgres_entries_tablespace_held reports whether a directory of the
 * tablespace's version directory other than the fork's holds a file, as
 * Postgres' removal of its directories finds a relation's file there.
 */
static bool
doltgres_entries_tablespace_held(doltgres_entries_doltgres_tablespace_emptied_frame_t *frame, bool *result,
								 HostError *failed)
{
	StdStr		version = host_directory_of((const char *) frame->versioned.bytes.ptr, frame->versioned.bytes.len);
	bool		has;

	*result = false;
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(&frame->io), version.ptr, version.len, &has, &frame->names,
								failed))
		return false;
	if (!has)
		return true;
	frame->names_live = true;
	for (size_t k = 0, k_len = frame->names.len; k < k_len && !*result; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		bool		listed;

		if (name->len == strlen(HOST_VERSIONED) && memcmp(name->ptr, HOST_VERSIONED, name->len) == 0)
			continue;
		std_path_join(version.ptr, version.len, (const char *) name->ptr, name->len, &frame->path);
		frame->path_live = true;
		if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(&frame->io), (const char *) frame->path.bytes.ptr,
									frame->path.bytes.len, &listed, &frame->inner, failed))
			return false;
		if (listed)
		{
			frame->inner_live = true;
			*result = frame->inner.len != 0;
			frame->inner_live = false;
			std_vec_drop_Checked(&frame->inner, sizeof(StdString), _Alignof(StdString), std_String_drop);
		}
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	frame->names_live = false;
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	return true;
}

/*
 * doltgres_entries_tablespace_asked answers whether a repository's directory
 * is in the tablespace's directory of repositories, asking the collector
 * worker first to empty the tablespace out of each such repository when
 * asking, and removes the directory of repositories once none is.
 */
static bool
doltgres_entries_tablespace_asked(doltgres_entries_doltgres_tablespace_emptied_frame_t *frame, bool asking,
								  bool *result, HostError *failed)
{
	bool		has;
	bool		removed;

	*result = false;
	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(&frame->io), (const char *) frame->versioned.bytes.ptr,
								frame->versioned.bytes.len, &has, &frame->names, failed))
		return false;
	if (!has)
		return true;
	frame->names_live = true;
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		uint32		repository;
		uint64		counts[3];
		StdParseIntError parsed;

		if (!std_str_parse_u32(name->ptr, name->len, &repository, &parsed) || repository == 0)
			continue;
		*result = true;
		if (!asking)
			continue;
		if (!host_collection_ask(&frame->io, repository, frame->tablespace, counts, failed))
			return false;
		if (counts[0] == PG_UINT64_MAX)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("the collector's collection of database "), STD_FMT_ARG(0),
			STD_FMT_LIT(" failed")};
			StdFmtArg	args[] = {{&repository, std_u32_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*failed = host_Error_fork((const uint8 *) "XX000", &message);
			return false;
		}
	}
	frame->names_live = false;
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (*result)
		return true;
	return host_remove_tree(HOST_IO_IO_IO_DOORS(&frame->io), (const char *) frame->versioned.bytes.ptr,
							frame->versioned.bytes.len, &removed, failed);
}

/*
 * doltgres_entries_tablespace_marked leaves the dropped mark, durably, in the
 * tablespace's directory of repositories when redo's attempt left it there.
 */
static bool
doltgres_entries_tablespace_marked(doltgres_entries_doltgres_tablespace_emptied_frame_t *frame, HostError *failed)
{
	bool		there;
	bool		made;
	HostFile	mark;

	if (!host_list_dir_if_there(HOST_IO_IO_READ_DOORS(&frame->io), (const char *) frame->versioned.bytes.ptr,
								frame->versioned.bytes.len, &there, &frame->names, failed))
		return false;
	if (!there)
		return true;
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_path_join((const char *) frame->versioned.bytes.ptr, frame->versioned.bytes.len, DOLTGRES_DROPPED_MARK,
				  strlen(DOLTGRES_DROPPED_MARK), &frame->path);
	frame->path_live = true;
	if (!host_create_if_absent(HOST_IO_IO_IO_DOORS(&frame->io), (const char *) frame->path.bytes.ptr,
							   frame->path.bytes.len, &made, &mark, failed))
		return false;
	if (made)
		host_File_drop(&mark);
	return host_sync_path(HOST_IO_IO_IO_DOORS(&frame->io), (const char *) frame->versioned.bytes.ptr,
						  frame->versioned.bytes.len, true, failed);
}

/*
 * doltgres_entries_doltgres_tablespace_emptied_body empties the fork's packs
 * out of the tablespace, round by round until its directory of repositories
 * is gone or a relation's file appears in Postgres' directories there, for
 * Postgres to refuse the DROP, or once in redo, which then leaves the dropped
 * mark on what is left for the collector's sweep.
 */
static void
doltgres_entries_doltgres_tablespace_emptied_body(void *frame_)
{
	doltgres_entries_doltgres_tablespace_emptied_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostError	failed;
	bool		held;
	bool		left;

	frame->versioned = host_versioned_dir(frame->tablespace);
	frame->versioned_live = true;
	if (!doltgres_entries_tablespace_held(frame, &held, &failed))
		goto raised;
	if (held && !frame->redo)
	{
		if (!pg_calls_checkpoint_now(raise, frame->error))
			return;
		if (!doltgres_entries_tablespace_held(frame, &held, &failed))
			goto raised;
	}
	for (bool asking = !held && (!frame->redo || host_collector_running()); asking;)
	{
		if (!doltgres_entries_tablespace_asked(frame, true, &left, &failed) ||
			(left && !doltgres_entries_tablespace_asked(frame, false, &left, &failed)))
			goto raised;
		if (!left || frame->redo)
			break;
		if (!pg_calls_latch_sleep(raise, DOLTGRES_ENTRIES_EMPTIED_NAP, frame->error))
			return;
		if (!doltgres_entries_tablespace_held(frame, &held, &failed))
			goto raised;
		asking = !held;
	}
	if (frame->redo && !doltgres_entries_tablespace_marked(frame, &failed))
		goto raised;
	frame->ok = true;
	return;
raised:
	*frame->error = pghost_raised(&failed);
}

/*
 * doltgres_entries_doltgres_tablespace_emptied empties the fork's packs out of
 * a tablespace and removes its directory of them before Postgres removes the
 * tablespace's directories (design item 5, bugs row 99): when no directory of
 * Postgres' there holds a file, after the checkpoint that removes the files
 * of dropped relations, the collector worker copies every live chunk of
 * each repository's packs there into the repository's own directory and
 * retires them, the files go once their readers end, and a base backup
 * keeps the collection from running, so DROP TABLESPACE waits for each,
 * answering cancel. Redo makes one attempt, and none when no collector
 * worker runs to answer, leaving the dropped mark on what is left, which
 * the worker's sweep removes.
 */
bool
doltgres_entries_doltgres_tablespace_emptied(const PgCaps *caps, uint32 tablespaceoid, bool redo, PgPgError *error)
{
	doltgres_entries_doltgres_tablespace_emptied_frame_t frame = {.caps = caps, .io = pghost_io(pg_Caps_may_raise(caps)),
	.tablespace = tablespaceoid, .redo = redo, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tablespace_emptied_body,
				doltgres_entries_doltgres_tablespace_emptied_pad);
	doltgres_entries_doltgres_tablespace_emptied_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_relation_estimate_size is the fork's
 * relation_estimate_size: Postgres' block-based estimate over the fork's
 * block count, with heap's overhead per tuple and usable bytes per page, as
 * heap's estimate makes it.
 */
bool
doltgres_entries_doltgres_relation_estimate_size(const PgCatalogCaps *caps, const PgRel *rel, PgWidths attr_widths,
												 uint32 *pages, float8 *tuples, float8 *allvisfrac, PgPgError *error)
{
	return pg_calls_estimate_size(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), rel, attr_widths,
								  pages, tuples, allvisfrac, error);
}

/*
 * DOLTGRES_ENTRIES_BYPASS_THRESHOLD_PAGES is heap's share of a table's pages
 * with dead items below which an INDEX_CLEANUP of auto skips the index pass,
 * its threshold a whole number of pages, as heap's BlockNumber truncates it.
 */
#define DOLTGRES_ENTRIES_BYPASS_THRESHOLD_PAGES 0.02

/* DOLTGRES_ENTRIES_BYPASS_MEMORY is the dead items' memory below which heap's skip of the index pass applies. */
#define DOLTGRES_ENTRIES_BYPASS_MEMORY ((size_t) 32 * 1024 * 1024)

/*
 * doltgres_entries_Freed is Freed: a run of the tuple ids VACUUM frees, by
 * their places, and whether only their bytes come back (D5).
 */
typedef struct doltgres_entries_Freed
{
	StdRangeUsize f0;
	bool		f1;
} doltgres_entries_Freed;

/* doltgres_entries_Run is a run of tids' entries in one block, a &[Freed] of txn's vacuum. */
typedef struct doltgres_entries_Run
{
	const TxnVacuumFreed *ptr;
	size_t		len;
} doltgres_entries_Run;

/* doltgres_entries_VacuumFreed_drop is txn's Freed's drop glue, an element of never_held's answer: its value ids'. */
static void
doltgres_entries_VacuumFreed_drop(void *freed)
{
	std_vec_free_Checked(&((TxnVacuumFreed *) freed)->f3, sizeof(uint32), _Alignof(uint32));
}

/*
 * doltgres_entries_bulk runs the bulk delete of each index at a place in a
 * table's list of indexes over the dead items of a count of tuple ids,
 * carrying its statistics.
 */
static bool
doltgres_entries_bulk(const PgCatalogCaps *caps, const PgRel *rel, const StdVec *places, const PgStrategy *bstrategy,
					  const PgDeadItems *items, size_t count, StdVec *stats, PgPgError *error)
{
	for (size_t i = 0; i < places->len; i++)
	{
		int32		at = STD_VEC_AT(places, int32, i);
		PgIndexStatsOption *slot = &STD_VEC_AT(stats, PgIndexStatsOption, at);
		PgIndexStatsOption carried = *slot;
		PgIndexStatsOption deleted;

		slot->has = false;
		if (!pg_calls_bulk_deleted(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), rel, at, bstrategy,
								   items, (int64) count, &carried, &deleted, error))
			return false;
		*slot = deleted;
	}
	return true;
}

/*
 * doltgres_entries_indexed_frame_t is indexed's state across its pad: the
 * places of the indexes of methods the fork doesn't know, the indexes'
 * statistics, the dead items, the freed runs, the tids' runs by block, and a
 * block's offsets, each while live.
 */
typedef struct doltgres_entries_indexed_frame_t
{
	StdVec		unported;
	bool		unported_live;
	StdVec		stats;
	bool		stats_live;
	PgDeadItems items;
	bool		items_live;
	StdVec		freed;
	bool		freed_live;
	StdVec		runs;
	bool		runs_live;
	StdVec		offsets;
	bool		offsets_live;
	const PgCatalogCaps *caps;
	const PgRel *rel;
	const PgVacuumParams *params;
	const PgStrategy *bstrategy;
	const TxnVacuumFreed *tids;
	size_t		tids_len;
	uint64		pages;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_indexed_frame_t;

/* doltgres_entries_indexed_pad frees what indexed holds. */
static void
doltgres_entries_indexed_pad(void *frame_)
{
	doltgres_entries_indexed_frame_t *frame = frame_;

	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, 1, 1);
	if (frame->runs_live)
		std_vec_free_Checked(&frame->runs, sizeof(doltgres_entries_Run), _Alignof(doltgres_entries_Run));
	if (frame->freed_live)
		std_vec_free_Checked(&frame->freed, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
	if (frame->items_live)
		pg_DeadItems_drop(&frame->items);
	if (frame->stats_live)
		std_vec_free_Checked(&frame->stats, sizeof(PgIndexStatsOption), _Alignof(PgIndexStatsOption));
	if (frame->unported_live)
		std_vec_free_Checked(&frame->unported, sizeof(int32), _Alignof(int32));
	frame->offsets_live = frame->runs_live = frame->freed_live = frame->items_live = frame->stats_live = false;
	frame->unported_live = false;
}

/* doltgres_entries_indexed_items makes the dead items a cycle fills, dropping the last cycle's. */
static bool
doltgres_entries_indexed_items(doltgres_entries_indexed_frame_t *frame, size_t most)
{
	PgDeadItems items;

	if (!pg_calls_make_dead_items(pg_CatalogCaps_may_raise(frame->caps), most, &items, frame->error))
		return false;
	if (frame->items_live)
		pg_DeadItems_drop(&frame->items);
	frame->items = items;
	frame->items_live = true;
	return true;
}

/* doltgres_entries_indexed_body runs the index pass over the tids block by block and cleans each index. */
static void
doltgres_entries_indexed_body(void *frame_)
{
	doltgres_entries_indexed_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	int32		cleanup = pg_calls_vacuum_index_cleanup(frame->params);
	int32		indexes;
	int32		work_mem;
	size_t		most;
	size_t		start = 0;
	uint64		blocks = 0;
	size_t		at = 0;
	bool		first = true;

	if (!pg_calls_index_count(raise, catalog, frame->rel, &indexes, frame->error))
		return;
	if (indexes == 0 || cleanup == VACOPTVALUE_DISABLED)
	{
		std_vec_new(frame->result, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
		if (frame->tids_len > 0)
		{
			doltgres_entries_Freed all = {{0, frame->tids_len}, indexes > 0};

			std_vec_with_capacity_Checked(frame->result, 1, sizeof(doltgres_entries_Freed),
										  _Alignof(doltgres_entries_Freed));
			std_vec_push_Checked(frame->result, &all, sizeof(doltgres_entries_Freed),
								 _Alignof(doltgres_entries_Freed));
		}
		frame->ok = true;
		return;
	}
	std_vec_new(&frame->unported, sizeof(int32), _Alignof(int32));
	frame->unported_live = true;
	for (int32 i = 0; i < indexes; i++)
	{
		bool		forks;

		if (!pg_calls_index_forks_at(raise, catalog, frame->rel, i, &forks, frame->error))
			return;
		if (!forks)
			std_vec_push_Checked(&frame->unported, &i, sizeof(int32), _Alignof(int32));
	}
	work_mem = pg_calls_vacuum_work_mem();
	most = (size_t) Max(work_mem, 0) * 1024;
	std_vec_with_capacity_Checked(&frame->stats, (size_t) indexes, sizeof(PgIndexStatsOption),
								  _Alignof(PgIndexStatsOption));
	frame->stats_live = true;
	for (int32 i = 0; i < indexes; i++)
	{
		PgIndexStatsOption none = {0};

		std_vec_push_Checked(&frame->stats, &none, sizeof(PgIndexStatsOption), _Alignof(PgIndexStatsOption));
	}
	if (!doltgres_entries_indexed_items(frame, most))
		return;
	std_vec_new(&frame->freed, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
	frame->freed_live = true;
	std_vec_new(&frame->runs, sizeof(doltgres_entries_Run), _Alignof(doltgres_entries_Run));
	frame->runs_live = true;
	for (size_t i = 0; i < frame->tids_len;)
	{
		doltgres_entries_Run run = {&frame->tids[i], 1};

		while (i + run.len < frame->tids_len && frame->tids[i + run.len].f0 >> 16 == frame->tids[i].f0 >> 16)
			run.len++;
		i += run.len;
		if (first)
			std_vec_collect_first(&frame->runs, &run, i < frame->tids_len ? 1 : 0, sizeof(doltgres_entries_Run),
								  _Alignof(doltgres_entries_Run));
		else
			std_vec_extend_one(&frame->runs, &run, i < frame->tids_len ? 1 : 0, sizeof(doltgres_entries_Run),
							   _Alignof(doltgres_entries_Run));
		first = false;
	}
	for (size_t r = 0; r < frame->runs.len; r++)
	{
		const doltgres_entries_Run *run = &STD_VEC_AT(&frame->runs, doltgres_entries_Run, r);

		if (at > start && pg_calls_dead_items_memory(&frame->items) > most)
		{
			doltgres_entries_Freed cycle = {{start, at}, false};

			if (!doltgres_entries_bulk(frame->caps, frame->rel, &frame->unported, frame->bstrategy, &frame->items,
									   at - start, &frame->stats, frame->error))
				return;
			std_vec_push_Checked(&frame->freed, &cycle, sizeof(doltgres_entries_Freed),
								 _Alignof(doltgres_entries_Freed));
			if (!doltgres_entries_indexed_items(frame, most))
				return;
			start = at;
			blocks = 0;
		}
		std_vec_with_capacity_Checked(&frame->offsets, run->len * 2, 1, 1);
		frame->offsets_live = true;
		for (size_t k = 0; k < run->len; k++)
		{
			uint16		offset = (uint16) (run->ptr[k].f0 & 0xffff);

			std_vec_push_Checked(&frame->offsets, &((const uint8 *) &offset)[0], 1, 1);
			std_vec_push_Checked(&frame->offsets, &((const uint8 *) &offset)[1], 1, 1);
		}
		if (!pg_calls_dead_items_set(raise, &frame->items, (uint32) (run->ptr[0].f0 >> 16), frame->offsets.ptr,
									 frame->offsets.len, frame->error))
			return;
		frame->offsets_live = false;
		std_vec_free_Checked(&frame->offsets, 1, 1);
		at += run->len;
		blocks++;
	}
	if (start < frame->tids_len)
	{
		size_t		memory = pg_calls_dead_items_memory(&frame->items);
		uint64		next = 0;
		bool		bypass;
		doltgres_entries_Freed last;

		if (frame->runs.len > 0)
			next = (STD_VEC_AT(&frame->runs, doltgres_entries_Run, frame->runs.len - 1).ptr[0].f0 >> 16) + 1;
		bypass = frame->freed.len == 0 && !(memory > most && next < frame->pages) && cleanup == VACOPTVALUE_AUTO &&
			frame->pages > 0 && blocks < (uint64) ((double) frame->pages * DOLTGRES_ENTRIES_BYPASS_THRESHOLD_PAGES) &&
			memory < DOLTGRES_ENTRIES_BYPASS_MEMORY;
		if (!bypass && !doltgres_entries_bulk(frame->caps, frame->rel, &frame->unported, frame->bstrategy,
											  &frame->items, frame->tids_len - start, &frame->stats, frame->error))
			return;
		last.f0.start = start;
		last.f0.end = frame->tids_len;
		last.f1 = bypass;
		std_vec_push_Checked(&frame->freed, &last, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
	}
	for (int32 i = 0; i < indexes; i++)
		if (!pg_calls_index_cleaned(raise, catalog, frame->rel, i, frame->bstrategy,
									&STD_VEC_AT(&frame->stats, PgIndexStatsOption, i), frame->error))
			return;
	frame->freed_live = false;
	*frame->result = frame->freed;
	frame->ok = true;
}

/*
 * doltgres_entries_indexed runs heap's index pass over the tuple ids VACUUM
 * frees, in tuple id order, on a table's indexes, and answers which of them
 * are then freed and which only give back their bytes (dead, D5), as
 * lazy_scan_heap and lazy_vacuum make heap's line pointers unused or leave
 * them dead, so the fork places tuple ids as heap does (the unknown methods'
 * text, section 4): a table with no index frees them all; with INDEX_CLEANUP
 * off they are all dead when it has one, and no index is cleaned; otherwise
 * the tuple ids go into the dead items block by block, and before a block,
 * when the items pass the work memory, the bulk delete of each index of a
 * method the fork doesn't know, the only ones whose entries name tuple ids,
 * drops their entries and they are freed (a cycle, as heap's before its next
 * block); the last items are freed the same way, unless no cycle ran and
 * heap's skip applies (auto, dead items on fewer pages than 2% of the
 * table's, truncated, and in under 32 MB), when they are dead. Each index's
 * cleanup then runs, the fork's own with no statistics and the others with
 * their bulk deletes', and a result that isn't an estimate is written to the
 * index's pg_class row, as heap's update_relstats_all_indexes does (bugs row
 * 102). The caller frees the runs after the pass, which no index method
 * reads.
 */
static bool
doltgres_entries_indexed(const PgCatalogCaps *caps, const PgRel *rel, const PgVacuumParams *params,
						 const PgStrategy *bstrategy, const TxnVacuumFreed *tids, size_t tids_len, uint64 pages,
						 StdVec *result, PgPgError *error)
{
	doltgres_entries_indexed_frame_t frame = {.caps = caps, .rel = rel, .params = params, .bstrategy = bstrategy,
	.tids = tids, .tids_len = tids_len, .pages = pages, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_indexed_body, doltgres_entries_indexed_pad);
	doltgres_entries_indexed_pad(&frame);
	return frame.ok;
}

/* state.rs' held at (Vec<Freed>, u64), which relation_vacuum takes. */
#define TXN_STATE_TS TupleVecFreedU64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , StdVec *result_0, uint64 *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS StdVec *result_0; uint64 *result_1;
#define TXN_STATE_RESULT_INIT , result_0, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at Vec<u64>, which relation_vacuum takes. */
#define TXN_STATE_TS VecU64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , StdVec *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS StdVec *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at (u64, u64), which relation_vacuum takes. */
#define TXN_STATE_TS TupleU64U64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result_0, uint64 *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS uint64 *result_0; uint64 *result_1;
#define TXN_STATE_RESULT_INIT , result_0, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_relation_vacuum_captures_t is the vacuum's lent
 * calls' captures: the reader, the relation, the page, the horizon, the
 * tids, a run of them and whether only its bytes come back, and the TOAST
 * storage's state and storage, none for a table with no TOAST relation.
 */
typedef struct doltgres_entries_doltgres_relation_vacuum_captures_t
{
	const HostSharedReader *reading;
	const PgRel *rel;
	const TxnInsertPage *page;
	const HostSnapshot *horizon;
	const StdVec *tids;
	StdRangeUsize range;
	bool		dead;
	const HostSharedHeld *toast;
	HostStorage toast_storage;
} doltgres_entries_doltgres_relation_vacuum_captures_t;

/*
 * doltgres_entries_doltgres_relation_vacuum_closure_0 is the vacuum's first
 * lent call: the tids it frees and the storage's blocks.
 */
static bool
doltgres_entries_doltgres_relation_vacuum_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
													const HostSharedHeld *held, StdVec *result_0, uint64 *result_1,
													HostError *error)
{
	doltgres_entries_doltgres_relation_vacuum_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	if (!txn_vacuum_never_held(io, TREES_STORE_READER_NODE_STORE(&store), held, c->page, c->horizon, result_0,
							   error))
		return false;
	*result_1 = doltgres_entries_blocks(c->rel, held);
	return true;
}

/* doltgres_entries_doltgres_relation_vacuum_closure_1 is the vacuum's lent call per run: the run's tids freed. */
static bool
doltgres_entries_doltgres_relation_vacuum_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
													const HostSharedHeld *held, StdVec *result, HostError *error)
{
	doltgres_entries_doltgres_relation_vacuum_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	return txn_vacuum_freed(io, TREES_STORE_READER_NODE_STORE(&store), held, c->toast, c->toast_storage,
							doltgres_entries_storage(c->rel), pg_calls_rel_permanent(c->rel), c->page,
							&STD_VEC_AT(c->tids, TxnVacuumFreed, c->range.start), c->range.end - c->range.start,
							c->dead, result, error);
}

/* doltgres_entries_doltgres_relation_vacuum_closure_2 is the vacuum's last lent call: the storage's blocks and rows. */
static bool
doltgres_entries_doltgres_relation_vacuum_closure_2(void *captures, HostIoIo *io, const RepoRepo *repo,
													const HostSharedHeld *held, uint64 *result_0, uint64 *result_1,
													HostError *error)
{
	*result_0 = doltgres_entries_blocks(captures, held);
	*result_1 = txn_state_totals(held).rows;
	return true;
}

/*
 * doltgres_entries_doltgres_relation_vacuum_frame_t is the vacuum's state
 * across its pad: what it opened, its NonVacuumable snapshot, the tids it
 * frees, the TOAST storage's state, and the runs, each while live.
 */
typedef struct doltgres_entries_doltgres_relation_vacuum_frame_t
{
	doltgres_entries_opened_t opened;
	PgOwnedSnapshot owned;
	bool		owned_live;
	StdVec		tids;
	bool		tids_live;
	HostSharedHeld toast;
	bool		toast_live;
	StdVec		runs;
	bool		runs_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgVacuumParams *params;
	const PgStrategy *bstrategy;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_relation_vacuum_frame_t;

/* doltgres_entries_doltgres_relation_vacuum_pad frees what the vacuum holds. */
static void
doltgres_entries_doltgres_relation_vacuum_pad(void *frame_)
{
	doltgres_entries_doltgres_relation_vacuum_frame_t *frame = frame_;

	if (frame->runs_live)
		std_vec_free_Checked(&frame->runs, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
	if (frame->toast_live)
		host_shared_Held_drop(&frame->toast);
	if (frame->tids_live)
		std_vec_drop_Checked(&frame->tids, sizeof(TxnVacuumFreed), _Alignof(TxnVacuumFreed),
							 doltgres_entries_VacuumFreed_drop);
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	frame->runs_live = frame->toast_live = frame->tids_live = frame->owned_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_relation_vacuum_body frees the tids no layer maps, then writes the table's statistics. */
static void
doltgres_entries_doltgres_relation_vacuum_body(void *frame_)
{
	doltgres_entries_doltgres_relation_vacuum_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	int64		started = pg_calls_current_timestamp();
	TxnInsertPage page = doltgres_entries_page();
	doltgres_entries_doltgres_relation_vacuum_captures_t captures = {.reading = &frame->opened.reading,
	.rel = frame->rel, .page = &page, .tids = &frame->tids};
	const RepoRepo *repo;
	int32		indexes;
	HostSnapshot horizon;
	TxnStateFound found;
	uint64		pages;
	uint64		rows;
	HostError	failed;

	if (!pg_calls_index_count(raise, catalog, frame->rel, &indexes, frame->error))
		return;
	if (pg_calls_vacuum_nworkers(frame->params) > 0 && indexes > 1 &&
		pg_calls_vacuum_index_cleanup(frame->params) != VACOPTVALUE_DISABLED && pg_calls_rel_local(frame->rel) &&
		!pg_calls_warn_parallel_temporary(raise, frame->rel, frame->error))
		return;
	frame->io = pghost_io(raise);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!pg_OwnedSnapshot_nonvacuumable(raise, frame->rel, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &horizon, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
		return;
	captures.horizon = &horizon;
	if (!txn_state_held_TupleVecFreedU64(&frame->io, repo, &frame->opened.reading, found,
										 doltgres_entries_doltgres_relation_vacuum_closure_0, &captures, NULL,
										 &frame->tids, &pages, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->tids_live = true;
	if (pg_calls_rel_toast_id(frame->rel) != 0)
	{
		TxnRecordToast toast;

		if (!doltgres_entries_toast_state(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &toast,
										  &frame->toast, frame->error))
			return;
		frame->toast_live = true;
		captures.toast = &frame->toast;
		captures.toast_storage.tablespace = toast.tablespace;
		captures.toast_storage.database = doltgres_entries_storage(frame->rel).database;
		captures.toast_storage.relation = toast.relation;
	}
	if (!doltgres_entries_indexed(frame->caps, frame->rel, frame->params, frame->bstrategy, frame->tids.ptr,
								  frame->tids.len, pages, &frame->runs, frame->error))
		return;
	frame->runs_live = true;
	for (size_t i = 0; i < frame->runs.len; i++)
	{
		const doltgres_entries_Freed *run = &STD_VEC_AT(&frame->runs, doltgres_entries_Freed, i);
		StdVec		claimed;

		if (!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
			return;
		captures.range = run->f0;
		captures.dead = run->f1;
		if (!txn_state_held_VecU64(&frame->io, repo, &frame->opened.reading, found,
								   doltgres_entries_doltgres_relation_vacuum_closure_1, &captures, NULL, &claimed,
								   &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		std_vec_free_Checked(&claimed, sizeof(uint64), _Alignof(uint64));
	}
	frame->runs_live = false;
	std_vec_free_Checked(&frame->runs, sizeof(doltgres_entries_Freed), _Alignof(doltgres_entries_Freed));
	if (!doltgres_entries_counted(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->rel, false,
								  frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &found, frame->error))
		return;
	if (!txn_state_held_TupleU64U64(&frame->io, repo, &frame->opened.reading, found,
									doltgres_entries_doltgres_relation_vacuum_closure_2, (void *) frame->rel, NULL,
									&pages, &rows, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!pg_calls_vacuum_relstats(raise, catalog, frame->rel, pages > MaxBlockNumber ? MaxBlockNumber : (uint32) pages,
								  (float8) rows, indexes > 0, frame->error))
		return;
	frame->ok = pg_calls_report_vacuum(raise, frame->rel, (float8) rows, started, frame->error);
}

/*
 * doltgres_entries_doltgres_relation_vacuum is the fork's relation_vacuum,
 * its part before the fold (PLAN item 2's Freeing; bugs rows 44, 73, and
 * 101): it finds the tuple ids a free space take took that no layer maps,
 * whose takers have ended, and whose rows' enders the relation's horizon has
 * passed, runs heap's index pass over them on the indexes of methods the fork
 * doesn't know, and frees them when that pass lets heap reuse its line
 * pointers, else gives back only their bytes, as heap's LP_DEAD (D5), logged
 * for a permanent relation. The fold comes with the fold's piece. It warns
 * of a temporary table's parallel option as heap's dead_items_alloc does,
 * and ends as heap_vacuum_rel does, writing the table's pages and rows to
 * pg_class and counting the vacuum in the cumulative statistics.
 */
bool
doltgres_entries_doltgres_relation_vacuum(const PgCatalogCaps *caps, const PgRel *rel, const PgVacuumParams *params,
										  const PgStrategy *bstrategy, PgPgError *error)
{
	doltgres_entries_doltgres_relation_vacuum_frame_t frame = {.caps = caps, .rel = rel, .params = params,
	.bstrategy = bstrategy, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_relation_vacuum_body, doltgres_entries_doltgres_relation_vacuum_pad);
	doltgres_entries_doltgres_relation_vacuum_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_next_unit gives a parallel scan its next unit from the
 * shared scan, as heap's parallel scans take blocks, and reports whether
 * there was one: a sequential scan's next ordinal range of the base's
 * entries, or a tid range scan's next ctid block of its range, its blocks
 * limited as heap's heap_set_tidrange limits them.
 */
bool
doltgres_entries_next_unit(const PgMayRaise *raise, HostIoIo *io, PgScan *scan, HostOwnedHandle owned,
						   DoltgresEntriesReading reading, bool *result, PgPgError *error)
{
	uint32		start = InvalidBlockNumber;
	uint32		count = InvalidBlockNumber;
	uint32		unit;
	bool		started;
	HostError	failed;

	if (reading == DOLTGRES_ENTRIES_READING_RANGE &&
		!doltgres_scan_range_blocks(io, owned, (uint64) pg_calls_parallel_nblocks(scan), &start, &count, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (!pg_calls_parallel_next_unit(raise, scan, start, count, &unit, error))
		return false;
	if (unit == InvalidBlockNumber)
	{
		*result = false;
		return true;
	}
	if (reading == DOLTGRES_ENTRIES_READING_ALL)
		started = doltgres_scan_start_unit(io, owned, (uint64) unit, &failed);
	else
		started = doltgres_scan_start_range_unit(io, owned, (uint64) unit, &failed);
	if (!started)
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = true;
	return true;
}

/*
 * doltgres_entries_root returns the tree a parallel scan's shared root names,
 * or none for an absent one: the leader stored it from the base its state
 * held, which keeps the tree until its transaction ends, after every
 * worker's.
 */
bool
doltgres_entries_root(const DgRoot *root, TreesStoreTree *result)
{
	ChunksAddressAddress address;

	if (!root->present)
		return false;
	memcpy(address.f0, root->address, sizeof(address.f0));
	*result = trees_store_Tree_decoded(address, chunks_trailer_Locator_read(root->locator, sizeof(root->locator)));
	return true;
}

/*
 * doltgres_entries_shared_root returns a tree's root as a parallel scan's
 * shared memory keeps it, its address and its locator's stored bytes, or
 * absent for none (tree NULL).
 */
static DgRoot
doltgres_entries_shared_root(const TreesStoreTree *tree)
{
	DgRoot		root = {.present = tree != NULL};
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	StdVec		bytes;

	if (tree == NULL)
		return root;
	TREES_STORE_TREE_NAMED_OPS.parts((void *) tree, &address, &locator);
	std_vec_with_capacity_Checked(&bytes, CHUNKS_TRAILER_LOCATOR_LEN, 1, 1);
	chunks_trailer_Locator_write(&locator, &bytes);
	memcpy(root.address, address.f0, sizeof(root.address));
	memcpy(root.locator, bytes.ptr, sizeof(root.locator));
	std_vec_free_Checked(&bytes, 1, 1);
	return root;
}

/*
 * doltgres_entries_doltgres_parallelscan_estimate is the fork's
 * parallelscan_estimate: the size of the fork's parallel scan.
 */
bool
doltgres_entries_doltgres_parallelscan_estimate(const PgCaps *caps, const PgRel *rel, size_t *result,
												PgPgError *error)
{
	*result = pg_calls_parallel_scan_size();
	return true;
}

/*
 * doltgres_entries_doltgres_parallelscan_initialize is the fork's
 * parallelscan_initialize: it initializes Postgres' block-based parallel
 * scan, whose block count is the table's size for costs, then stores the
 * base every worker reads through parallel_based, which the leader's active
 * snapshot reaches, and returns the fork's parallel scan's size.
 */
bool
doltgres_entries_doltgres_parallelscan_initialize(const PgCatalogCaps *caps, const PgRel *rel,
												  const PgParallelScan *pscan, size_t *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(caps);

	if (!pg_calls_parallel_init(raise, catalog, rel, pscan, error) ||
		!pg_calls_parallel_active(raise, catalog, rel, pscan, error))
		return false;
	*result = pg_calls_parallel_scan_size();
	return true;
}

/* state.rs' held at Vec<Vec<u8>>, which split takes. */
#define TXN_STATE_TS VecVecU8
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , StdVec *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS StdVec *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* doltgres_entries_VecU8_drop is Vec<u8>'s drop glue, an element of range_starts' answer. */
static void
doltgres_entries_VecU8_drop(void *bytes)
{
	std_vec_free_Checked(bytes, 1, 1);
}

/* doltgres_entries_split_closure_0 is split's lent call: the delta's range starts. */
static bool
doltgres_entries_split_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								 StdVec *result, HostError *error)
{
	return txn_memtable_table_range_starts(io, host_shared_Held_record(held), result, error);
}

/*
 * doltgres_entries_split_frame_t is split's state across its pad: the delta's
 * range starts and the split keys, each while live.
 */
typedef struct doltgres_entries_split_frame_t
{
	StdVec		starts;
	bool		starts_live;
	StdVec		splits;
	bool		splits_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	const PgParallelScan *pscan;
	uint64		entries;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_split_frame_t;

/* doltgres_entries_split_pad frees what split holds. */
static void
doltgres_entries_split_pad(void *frame_)
{
	doltgres_entries_split_frame_t *frame = frame_;

	if (frame->splits_live)
		std_vec_free_Checked(&frame->splits, 1, 1);
	if (frame->starts_live)
		std_vec_drop_Checked(&frame->starts, sizeof(StdVec), _Alignof(StdVec), doltgres_entries_VecU8_drop);
	frame->splits_live = frame->starts_live = false;
}

/*
 * doltgres_entries_split_body reads the delta's range starts for a storage
 * with no base entry and stores the split keys.
 */
static void
doltgres_entries_split_body(void *frame_)
{
	doltgres_entries_split_frame_t *frame = frame_;
	TxnStateFound found;
	HostError	failed;

	if (frame->entries == 0)
	{
		if (!doltgres_entries_making(frame->caps, frame->io, frame->rel, &found, frame->error))
			return;
		if (!txn_state_held_VecVecU8(frame->io, frame->repo, frame->reading, found, doltgres_entries_split_closure_0,
									 NULL, NULL, &frame->starts, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
	}
	else
		std_vec_new(&frame->starts, sizeof(StdVec), _Alignof(StdVec));
	frame->starts_live = true;
	doltgres_scan_splits(frame->starts.ptr, frame->starts.len, &frame->splits);
	frame->splits_live = true;
	pg_calls_parallel_set_splits(frame->pscan, frame->splits.ptr, frame->splits.len);
	frame->ok = true;
}

/*
 * doltgres_entries_split stores in a parallel scan's shared scan the split
 * keys its units divide the delta's keys by when the storage's base has no
 * entry: up to scan's splits' count of the delta's range starts, read from
 * the storage's state the backend's top-level transaction holds; with a
 * base, none, since the base's units divide the keys.
 */
static bool
doltgres_entries_split(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					   const PgRel *rel, const PgParallelScan *pscan, uint64 entries, PgPgError *error)
{
	doltgres_entries_split_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
	.pscan = pscan, .entries = entries, .error = error};

	std_landing(&frame, doltgres_entries_split_body, doltgres_entries_split_pad);
	doltgres_entries_split_pad(&frame);
	return frame.ok;
}

/* state.rs' held at Option<Base>, which entries.c defines. */
#define TXN_STATE_TS OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseBase *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_parallel_based_closure_0 is parallel_based's lent
 * call: the base the snapshot may read, or the newest for none.
 */
static bool
doltgres_entries_doltgres_parallel_based_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
												   const HostSharedHeld *held, bool *some, TxnBaseBase *result,
												   HostError *error)
{
	const HostSnapshot *read = captures;

	if (read != NULL)
		return txn_state_readable(io, held, read, some, result, error);
	return txn_state_newest(io, held, some, result, error);
}

/*
 * doltgres_entries_doltgres_parallel_based_frame_t is parallel_based's state
 * across its pad: what it opened and the base, while live.
 */
typedef struct doltgres_entries_doltgres_parallel_based_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseBase base;
	bool		base_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgParallelScan *pscan;
	const PgSnapshot *snapshot;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_parallel_based_frame_t;

/* doltgres_entries_doltgres_parallel_based_pad frees what parallel_based holds. */
static void
doltgres_entries_doltgres_parallel_based_pad(void *frame_)
{
	doltgres_entries_doltgres_parallel_based_frame_t *frame = frame_;

	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->base_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_parallel_based_body reads the base the workers read and stores its parts and split keys. */
static void
doltgres_entries_doltgres_parallel_based_body(void *frame_)
{
	doltgres_entries_doltgres_parallel_based_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const RepoRepo *repo;
	TxnStateFound made;
	HostSnapshot read;
	DgParallelBase parts = {0};
	const TxnBaseBase *base = NULL;
	bool		some;
	uint32		epoch;
	bool		has_chain;
	TreesStoreTree chain;
	TreesStoreReader store;
	HostError	failed;

	frame->io = pghost_io(raise);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &made, frame->error))
		return;
	if (frame->snapshot != NULL && !pghost_snapshot(raise, frame->snapshot, &read, frame->error))
	{
		if (made.tag == TXN_STATE_FOUND_MADE)
			txn_base_Key_drop(&made.u.made.f1.key);
		return;
	}
	if (!txn_state_held_OptionBase(&frame->io, repo, &frame->opened.reading, made,
								   doltgres_entries_doltgres_parallel_based_closure_0,
								   frame->snapshot != NULL ? &read : NULL, NULL, &some, &frame->base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (some)
	{
		frame->base_live = true;
		base = &frame->base;
		parts.fresh = base->fresh_end;
		parts.folded = base->read;
	}
	doltgres_scan_Epochs_based(base, &epoch, &has_chain, &chain);
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->opened.reading);
	if (base != NULL && base->has_root)
	{
		ChunksErrorError counted;

		if (!trees_count(&frame->io, TREES_STORE_READER_NODE_STORE(&store),
						 TREES_STORE_TREE_NAMED((void *) &base->root), &parts.entries, &counted))
		{
			HostError	storage = txn_base_storage(&counted);

			*frame->error = pghost_raised(&storage);
			return;
		}
	}
	parts.table = doltgres_entries_shared_root(base != NULL && base->has_root ? &base->root : NULL);
	parts.tuples = doltgres_entries_shared_root(base != NULL && base->has_tuple_ids ? &base->tuple_ids : NULL);
	parts.chain = doltgres_entries_shared_root(has_chain ? &chain : NULL);
	parts.epoch = epoch;
	pg_calls_parallel_set_base(frame->pscan, parts);
	frame->ok = doltgres_entries_split(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel,
									   frame->pscan, parts.entries, frame->error);
}

/*
 * doltgres_entries_doltgres_parallel_based stores in a parallel scan the base
 * every worker reads, the trees of the storage's newest base, which every
 * snapshot the leader holds may read (state's readable), its table tree's
 * count of entries, its fresh end, its fold point, and its epoch.
 */
bool
doltgres_entries_doltgres_parallel_based(const PgCatalogCaps *caps, const PgRel *rel, const PgParallelScan *pscan,
										 const PgSnapshot *snapshot, PgPgError *error)
{
	doltgres_entries_doltgres_parallel_based_frame_t frame = {.caps = caps, .rel = rel, .pscan = pscan,
	.snapshot = snapshot, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_parallel_based_body, doltgres_entries_doltgres_parallel_based_pad);
	doltgres_entries_doltgres_parallel_based_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_parallelscan_reinitialize_frame_t is
 * parallelscan_reinitialize's state across its pad: what it opened.
 */
typedef struct doltgres_entries_doltgres_parallelscan_reinitialize_frame_t
{
	doltgres_entries_opened_t opened;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	const PgParallelScan *pscan;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_parallelscan_reinitialize_frame_t;

/* doltgres_entries_doltgres_parallelscan_reinitialize_pad frees what parallelscan_reinitialize holds. */
static void
doltgres_entries_doltgres_parallelscan_reinitialize_pad(void *frame_)
{
	doltgres_entries_doltgres_parallelscan_reinitialize_frame_t *frame = frame_;

	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_parallelscan_reinitialize_body starts the shared
 * scan's units over and reads the split keys again.
 */
static void
doltgres_entries_doltgres_parallelscan_reinitialize_body(void *frame_)
{
	doltgres_entries_doltgres_parallelscan_reinitialize_frame_t *frame = frame_;

	pg_calls_parallel_reinit(frame->rel, frame->pscan);
	frame->io = pghost_io(pg_CatalogCaps_may_raise(frame->caps));
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	frame->ok = doltgres_entries_split(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
									   &frame->opened.reading, frame->rel, frame->pscan,
									   pg_calls_parallel_entries(frame->pscan), frame->error);
}

/*
 * doltgres_entries_doltgres_parallelscan_reinitialize is the fork's
 * parallelscan_reinitialize: it starts the shared scan's units over, as
 * heap's does, over the same base, with the delta's split keys read again.
 */
bool
doltgres_entries_doltgres_parallelscan_reinitialize(const PgCatalogCaps *caps, const PgRel *rel,
													const PgParallelScan *pscan, PgPgError *error)
{
	doltgres_entries_doltgres_parallelscan_reinitialize_frame_t frame = {.caps = caps, .rel = rel, .pscan = pscan,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_parallelscan_reinitialize_body,
				doltgres_entries_doltgres_parallelscan_reinitialize_pad);
	doltgres_entries_doltgres_parallelscan_reinitialize_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_Insert_drop is Insert's drop: its key's, its put's, and its kept places'. */
static void
doltgres_entries_Insert_drop(TxnInsertInsert *row)
{
	std_vec_free_Checked(&row->key, 1, 1);
	txn_memtable_Put_drop(&row->put);
	std_vec_free_Checked(&row->kept, sizeof(size_t), _Alignof(size_t));
}

/*
 * doltgres_entries_Written is Written: the row a slot holds, formed for its
 * table's storage: its insert, its room, its values out of line, and how its
 * key's stripe is taken: the stripe, whether every key's equal values are
 * equal bytes, and for an immediate key the key's order and primary key
 * index.
 */
typedef struct doltgres_entries_Written
{
	TxnInsertInsert row;
	TxnInsertRoom room;
	bool		has_toasted;
	DoltgresEntriesToasted toasted;
	uint32		stripe;
	bool		by_bytes;
	bool		has_order;
	TreesKeyOrder order;
	uint32		index;
} doltgres_entries_Written;

/*
 * doltgres_entries_written_frame_t is written's state across its pad: the
 * recorded key, its columns until the layout takes them, the key's column
 * places, what an update keeps until toasted takes it, the slot's columns
 * until the layout takes them and their held values, the values, the layout,
 * the kept values, the placing until formed takes it, and the row, its
 * out-of-line values, and its key order as the result takes them, each while
 * live.
 */
typedef struct doltgres_entries_written_frame_t
{
	TxnBaseKey	key;
	bool		key_live;
	StdVec		key_columns;
	bool		key_columns_live;
	StdVec		keyed;
	bool		keyed_live;
	DoltgresEntriesKeeping keeping;
	bool		keeping_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		held;
	bool		held_live;
	StdVec		values;
	bool		values_live;
	AccessHeaptupleRowLayout layout;
	bool		layout_live;
	StdVec		kept;
	bool		kept_live;
	AccessHeaptuplePlacing placing;
	bool		placing_live;
	bool		row_live;
	bool		toasted_live;
	bool		order_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	PgSlot	   *slot;
	const PgSlot *replaced;
	TxnMemtableVersionWriter writer;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	doltgres_entries_Written *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_written_frame_t;

/* doltgres_entries_written_pad frees what written holds. */
static void
doltgres_entries_written_pad(void *frame_)
{
	doltgres_entries_written_frame_t *frame = frame_;

	if (frame->order_live)
		trees_KeyOrder_drop(&frame->result->order);
	if (frame->toasted_live)
		doltgres_entries_Toasted_drop(&frame->result->toasted);
	if (frame->row_live)
		doltgres_entries_Insert_drop(&frame->result->row);
	if (frame->placing_live)
		access_heaptuple_Placing_drop(&frame->placing);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	if (frame->layout_live)
		access_heaptuple_RowLayout_drop(&frame->layout);
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->held_live)
		doltgres_layout_VecHeld_drop(&frame->held);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	if (frame->keeping_live)
		doltgres_entries_Keeping_drop(&frame->keeping);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, sizeof(size_t), _Alignof(size_t));
	if (frame->key_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->key_columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->order_live = frame->toasted_live = frame->row_live = frame->placing_live = frame->kept_live = false;
	frame->layout_live = frame->values_live = frame->held_live = frame->columns_live = frame->keeping_live = false;
	frame->keyed_live = frame->key_columns_live = frame->key_live = false;
}

/* doltgres_entries_written_closure_3 is written's outside: whether a slot holds any value out of line. */
static bool
doltgres_entries_written_closure_3(const PgSlot *row)
{
	int32		natts = pg_calls_slot_natts(row);

	for (int32 column = 0; column < natts; column++)
		if (pg_calls_slot_value_outside(row, column))
			return true;
	return false;
}

/* doltgres_entries_written_body forms the slot's row for the table's storage and its insert. */
static void
doltgres_entries_written_body(void *frame_)
{
	doltgres_entries_written_frame_t *frame = frame_;
	doltgres_entries_Written *result = frame->result;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, (void *) frame->caps};
	AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
	TxnBaseBase recorded;
	bool		keyless;
	bool		immediate = false;
	bool		keeping = false;
	int32		toast_target;
	bool		external;
	AccessHeaptupleFormed formed;
	uint64		length;
	uint64		page;

	if (!doltgres_entries_recorded(frame->caps, frame->io, frame->repo, frame->reading, frame->rel, &recorded,
								   frame->error))
		return;
	frame->key = recorded.key;
	frame->key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->key_columns, frame->error))
		return;
	frame->key_columns_live = true;
	keyless = frame->key_columns.len == 0;
	if (!keyless && !pg_calls_rel_key_immediate(raise, catalog, frame->rel, &immediate, frame->error))
		return;
	if (!pg_calls_slot_getallattrs(raise, frame->slot, frame->error))
		return;
	std_vec_with_capacity_Checked(&frame->keyed, frame->key_columns.len, sizeof(size_t), _Alignof(size_t));
	frame->keyed_live = true;
	for (size_t i = 0; i < frame->key_columns.len; i++)
		std_vec_push_Checked(&frame->keyed,
							 &STD_VEC_AT(&frame->key_columns, access_TupleUsizeOrderKeyType_t, i).f0, sizeof(size_t),
							 _Alignof(size_t));
	if (frame->replaced != NULL)
	{
		if (!doltgres_entries_keeping(frame->caps, frame->io, frame->repo, frame->reading, frame->rel,
									  frame->replaced, frame->slot, frame->keyed.ptr, frame->keyed.len, &keeping,
									  &frame->keeping, frame->error))
			return;
		frame->keeping_live = keeping;
	}
	if (!doltgres_layout_slot_row(raise, catalog, frame->rel, frame->slot, &frame->columns, &frame->held,
								  frame->error))
		return;
	frame->columns_live = frame->held_live = true;
	doltgres_layout_values(frame->columns.ptr, frame->columns.len, frame->held.ptr, frame->held.len, &frame->values);
	frame->values_live = true;
	frame->columns_live = frame->key_columns_live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &frame->key_columns, &frame->layout);
	frame->layout_live = true;
	toast_target = pg_calls_rel_toast_target(frame->rel);
	std_vec_new(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	frame->kept_live = true;
	for (size_t i = 0; keeping && i < frame->keeping.values.len; i++)
	{
		const doltgres_TupleUsizeU32Entry_t *value = &STD_VEC_AT(&frame->keeping.values,
																 doltgres_TupleUsizeU32Entry_t, i);
		AccessHeaptupleKept kept = {value->f0, value->f2.stored, value->f2.address.f0};
		size_t		lower = frame->keeping.values.len - i - 1;

		if (i == 0)
			std_vec_collect_first(&frame->kept, &kept, lower, sizeof(AccessHeaptupleKept),
								  _Alignof(AccessHeaptupleKept));
		else
			std_vec_extend_one(&frame->kept, &kept, lower, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	}
	external = doltgres_entries_written_closure_3(frame->slot) ||
		(frame->replaced != NULL && doltgres_entries_written_closure_3(frame->replaced));
	if (!access_heaptuple_RowLayout_placing(&frame->layout, sorter, names, frame->values.ptr, frame->values.len,
											frame->kept.ptr, frame->kept.len, external,
											toast_target < 0 ? 0 : (size_t) toast_target, &frame->placing,
											frame->error))
		return;
	frame->placing_live = true;
	frame->kept_live = false;
	std_vec_free_Checked(&frame->kept, sizeof(AccessHeaptupleKept), _Alignof(AccessHeaptupleKept));
	frame->keeping_live = false;
	if (!doltgres_entries_toasted(frame->caps, frame->io, frame->repo, frame->reading, frame->rel, &frame->placing,
								  keeping ? &frame->keeping : NULL, &result->has_toasted, &result->toasted,
								  frame->error))
		return;
	frame->toasted_live = result->has_toasted;
	frame->placing_live = false;
	if (!access_heaptuple_Placing_formed(&frame->placing, &formed, frame->error))
		return;
	length = std_saturating_sub_u64(formed.heap_bytes, 4);
	if (length > pg_calls_max_heap_tuple_size() &&
		!pg_calls_refuse_row_too_big(raise, (size_t) length, frame->error))
	{
		access_heaptuple_Formed_drop(&formed);
		return;
	}
	if (formed.heap_bytes > PG_UINT32_MAX)
	{
		access_heaptuple_Formed_drop(&formed);
		*frame->error = doltgres_entries_pg_fork("XX000", "a row's heap bytes pass 32 bits");
		return;
	}
	result->row.storage = doltgres_entries_storage(frame->rel);
	result->row.table = pg_calls_rel_id(frame->rel);
	result->row.key = formed.key;
	result->row.put.value = formed.tuple;
	result->row.put.addresses = formed.addresses;
	if (result->has_toasted)
		std_vec_clone(&result->row.put.values, &result->toasted.ids, sizeof(uint32), _Alignof(uint32));
	else
		std_vec_new(&result->row.put.values, sizeof(uint32), _Alignof(uint32));
	result->row.put.tuple.f0 = 0;
	result->row.put.heap_bytes = (uint32) formed.heap_bytes;
	result->row.keyless = keyless;
	result->row.deferred = !keyless && !immediate;
	result->row.writer = frame->writer;
	result->row.has_roomed = frame->has_roomed;
	result->row.roomed = frame->roomed;
	result->row.frozen = false;
	result->row.logged = pg_calls_rel_permanent(frame->rel);
	result->row.decoded = pg_calls_rel_logical_decoding(frame->rel);
	result->row.has_toast = result->has_toasted;
	if (result->has_toasted)
	{
		result->row.toast = result->toasted.toast;
		result->row.kept = result->toasted.kept;
		std_vec_new(&result->toasted.kept, sizeof(size_t), _Alignof(size_t));
	}
	else
		std_vec_new(&result->row.kept, sizeof(size_t), _Alignof(size_t));
	result->row.has_token = false;
	result->row.target = pg_calls_target_block(frame->rel);
	result->row.has_target = result->row.target != InvalidBlockNumber;
	frame->row_live = true;
	page = pg_calls_page_bytes();
	result->room.bytes = formed.heap_bytes;
	result->room.reserve = page - pg_calls_rel_usable_bytes(frame->rel);
	result->room.page = page;
	result->room.most = pg_calls_max_heap_tuples_per_page();
	result->has_order = false;
	if (immediate)
	{
		StdVec		columns;

		if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &columns, frame->error) ||
			!doltgres_layout_key_order(raise, catalog, &frame->key, &columns, &result->order, frame->error))
			return;
		result->has_order = frame->order_live = true;
	}
	result->index = 0;
	if (immediate && !pg_calls_rel_key_index(raise, catalog, frame->rel, &result->index, frame->error))
		return;
	result->by_bytes = keyless;
	if (!keyless && !pg_calls_rel_key_equal_image(raise, catalog, frame->rel, &result->by_bytes, frame->error))
		return;
	result->stripe = txn_change_stripe(&result->row.storage, result->row.key.ptr, result->row.key.len,
									   result->by_bytes);
	frame->order_live = frame->toasted_live = frame->row_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_written forms the row a slot holds by its table's keyed
 * layout at its toast target, keeping out of line what an update keeps from
 * the row the replaced slot holds (keeping; replaced NULL for none), writes
 * its other values out of line (toasted), refuses a row whose heap tuple is
 * longer than MaxHeapTupleSize with heap's 54000, and makes its insert by a
 * writer, which an insert written frozen marks (has_roomed false).
 */
static bool
doltgres_entries_written(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, PgSlot *slot, const PgSlot *replaced,
						 TxnMemtableVersionWriter writer, bool has_roomed, TxnStatusRoomed roomed,
						 doltgres_entries_Written *result, PgPgError *error)
{
	doltgres_entries_written_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
		.slot = slot, .replaced = replaced, .writer = writer, .has_roomed = has_roomed, .roomed = roomed,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_written_body, doltgres_entries_written_pad);
	doltgres_entries_written_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_speculated_frame_t is a push to SPECULATED's state across
 * its pad: the backend's list and its guard, and the insert it pushes, each
 * while live.
 */
typedef struct doltgres_entries_speculated_frame_t
{
	DoltgresEntriesSpeculated speculated;
	bool		speculated_live;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	HostIoIo   *io;
} doltgres_entries_speculated_frame_t;

/* doltgres_entries_speculated_pushed_pad releases what a push holds: the insert, the guard, then the list. */
static void
doltgres_entries_speculated_pushed_pad(void *frame_)
{
	doltgres_entries_speculated_frame_t *frame = frame_;

	if (frame->speculated_live)
		doltgres_entries_Speculated_drop(&frame->speculated);
	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, &DOLTGRES_ENTRIES_SPECULATED_TYPE);
	frame->speculated_live = frame->guard_live = frame->local_live = false;
}

/* doltgres_entries_speculated_pushed_body pushes the insert onto the backend's list under its lock. */
static void
doltgres_entries_speculated_pushed_body(void *frame_)
{
	doltgres_entries_speculated_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_SPECULATED.local, &DOLTGRES_ENTRIES_SPECULATED_TYPE);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, &DOLTGRES_ENTRIES_SPECULATED_TYPE);
	frame->guard = host_lock_IoMutex_lock(mutex, &DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	std_vec_push_Checked(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), &frame->speculated,
						 sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated));
	frame->speculated_live = false;
}

/* doltgres_entries_speculated_pushed keeps a speculative insert, which it takes, in SPECULATED for its completion. */
static void
doltgres_entries_speculated_pushed(HostIoIo *io, DoltgresEntriesSpeculated *speculated)
{
	doltgres_entries_speculated_frame_t frame = {.speculated = *speculated, .speculated_live = true, .io = io};

	std_landing(&frame, doltgres_entries_speculated_pushed_body, doltgres_entries_speculated_pushed_pad);
	doltgres_entries_speculated_pushed_pad(&frame);
}

/* state.rs' held at insert's Inserted, which inserted takes. */
#define TXN_STATE_TS Inserted
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , TxnInsertInserted *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS TxnInsertInserted *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_inserted_captures_t is inserted's lent call's captures: the
 * reader, the written row, and its row's live flag.
 */
typedef struct doltgres_entries_inserted_captures_t
{
	const HostSharedReader *reading;
	doltgres_entries_Written *written;
	bool	   *row_live;
} doltgres_entries_inserted_captures_t;

/*
 * doltgres_entries_inserted_closure_2 is inserted's lent call: the row
 * inserted as the storage's newest, which it takes.
 */
static bool
doltgres_entries_inserted_closure_2(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									TxnInsertInserted *result, HostError *error)
{
	doltgres_entries_inserted_captures_t *c = captures;
	doltgres_entries_Written *w = c->written;
	HostIoWaits waits = pghost_waits();
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	TxnInsertKeyed keyed = {.stripe = w->stripe};

	if (w->has_order)
	{
		keyed.has_unique = true;
		keyed.unique.order = &w->order;
		keyed.unique.index = w->index;
		keyed.unique.has_equal = doltgres_layout_equal(&w->order, &keyed.unique.equal);
	}
	*c->row_live = false;
	return txn_insert_insert(io, TREES_STORE_READER_NODE_STORE(&store), held, &w->row, &w->room, NULL,
							 w->has_toasted ? &w->toasted.state : NULL, w->has_toasted ? w->toasted.entries.ptr : NULL,
							 w->has_toasted ? w->toasted.entries.len : 0, keyed, &waits, result, error);
}

/*
 * doltgres_entries_inserted_frame_t is inserted's state across its pad: what
 * it opened, the written row's parts, and a speculative insert's key, each
 * while live.
 */
typedef struct doltgres_entries_inserted_frame_t
{
	doltgres_entries_opened_t opened;
	doltgres_entries_Written written;
	bool		row_live;
	bool		toasted_live;
	bool		order_live;
	StdVec		key;
	bool		key_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgSlot	   *slot;
	uint32		cid;
	uint32		options;
	bool		has_token;
	uint32		token;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_inserted_frame_t;

/* doltgres_entries_inserted_pad frees what inserted holds. */
static void
doltgres_entries_inserted_pad(void *frame_)
{
	doltgres_entries_inserted_frame_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->written.order);
	if (frame->toasted_live)
		doltgres_entries_Toasted_drop(&frame->written.toasted);
	if (frame->row_live)
		doltgres_entries_Insert_drop(&frame->written.row);
	frame->key_live = frame->order_live = frame->toasted_live = frame->row_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_inserted_body forms the slot's row, inserts it, and sets the slot's ctid and table. */
static void
doltgres_entries_inserted_body(void *frame_)
{
	doltgres_entries_inserted_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	doltgres_entries_inserted_captures_t captures = {.reading = &frame->opened.reading, .written = &frame->written,
	.row_live = &frame->row_live};
	const RepoRepo *repo;
	bool		frozen = (frame->options & TABLE_INSERT_FROZEN) != 0;
	TxnMemtableVersionWriter writer = {0};
	TxnStatusRoomed roomed = {0};
	TxnStateFound made;
	TxnInsertInserted inserted;
	uint64		placed;
	uint64		tuple;
	bool		killed;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (frozen)
	{
		writer.xid = pg_calls_frozen_xid();
		writer.command = frame->cid;
	}
	else if (!doltgres_entries_current(&frame->io, frame->cid, &writer, &roomed, frame->error))
		return;
	if (!doltgres_entries_written(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->slot,
								  NULL, writer, !frozen, roomed, &frame->written, frame->error))
		return;
	frame->row_live = true;
	frame->toasted_live = frame->written.has_toasted;
	frame->order_live = frame->written.has_order;
	frame->written.row.frozen = frozen;
	frame->written.row.has_token = frame->has_token;
	frame->written.row.token = frame->token;
	if (frame->has_token)
	{
		std_vec_clone(&frame->key, &frame->written.row.key, 1, 1);
		frame->key_live = true;
	}
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &made, frame->error))
		return;
	if (!txn_state_held_Inserted(&frame->io, repo, &frame->opened.reading, made, doltgres_entries_inserted_closure_2,
								 &captures, NULL, &inserted, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	Assert(inserted.tag != TXN_INSERT_INSERTED_STOPPED);
	placed = inserted.tag == TXN_INSERT_INSERTED_TOOK ? inserted.u.took.tuple : inserted.u.exists.spent;
	if (!pg_calls_set_target_block(raise, frame->rel, (uint32) (placed >> 16), frame->error))
		return;
	if (inserted.tag == TXN_INSERT_INSERTED_TOOK)
	{
		tuple = inserted.u.took.tuple;
		killed = false;
	}
	else if (frame->has_token)
	{
		tuple = inserted.u.exists.spent;
		killed = true;
	}
	else
	{
		const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
		uint32		index;

		if (doltgres_settle_rebuilt(&frame->io, pg_calls_rel_id(frame->rel), &index))
			frame->ok = pg_calls_refuse_unique_build(raise, catalog, frame->rel, index,
													 inserted.u.exists.has_live ? inserted.u.exists.live : 0,
													 frame->error);
		else
			frame->ok = pg_calls_refuse_duplicate_key(raise, catalog, frame->rel, frame->slot, frame->error);
		return;
	}
	pg_calls_slot_set_tid(frame->slot, tuple, pg_calls_rel_id(frame->rel));
	pg_calls_slot_set_written(frame->slot, writer.xid, writer.command, 0);
	if (frame->has_token)
	{
		DoltgresEntriesSpeculated speculated = {.token = frame->token, .relation = pg_calls_rel_id(frame->rel),
		.tuple = tuple, .key = frame->key, .killed = killed};

		std_vec_new(&speculated.entries, sizeof(doltgres_TupleU32VecU8_t), _Alignof(doltgres_TupleU32VecU8_t));
		frame->key_live = false;
		doltgres_entries_speculated_pushed(&frame->io, &speculated);
	}
	frame->ok = true;
}

/*
 * doltgres_entries_inserted inserts the row a slot holds through the table's
 * storage, as written forms it, written by the current subtransaction's xid
 * and its top-level transaction's, or by FrozenTransactionId for an insert
 * written frozen, and sets the slot's ctid and table from the tuple id the
 * insert took. A speculative insert, by its token (has_token), holds the
 * token in its version and is kept for its completion in SPECULATED; one
 * whose key holds a live row spends its place instead of raising, as heap's
 * speculative tuple is written before the btree finds the conflict.
 */
static bool
doltgres_entries_inserted(const PgCatalogCaps *caps, const PgRel *rel, PgSlot *slot, uint32 cid, uint32 options,
						  bool has_token, uint32 token, PgPgError *error)
{
	doltgres_entries_inserted_frame_t frame = {.caps = caps, .rel = rel, .slot = slot, .cid = cid,
	.options = options, .has_token = has_token, .token = token, .error = error};

	std_landing(&frame, doltgres_entries_inserted_body, doltgres_entries_inserted_pad);
	doltgres_entries_inserted_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tuple_insert is the fork's tuple_insert (PLAN
 * item 5; step (4a)): it inserts the row a slot holds as the table's newest,
 * sets the slot's ctid and table, and counts the insert, as heap's does.
 */
bool
doltgres_entries_doltgres_tuple_insert(const PgCatalogCaps *caps, const PgRel *rel, PgSlot *slot, uint32 cid,
									   uint32 options, const PgBulkInsert *bistate, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);

	return pg_calls_serial_conflict_in(raise, rel, error) &&
		doltgres_entries_inserted(caps, rel, slot, cid, options, false, 0, error) &&
		pg_calls_count_inserts(raise, rel, 1, error);
}

/*
 * doltgres_entries_doltgres_multi_insert is the fork's multi_insert: it
 * inserts each slot's row in order as tuple_insert inserts one, and counts
 * them together, as heap's heap_multi_insert does. Its slots come as the
 * binding read Postgres' array, or as its refusal of a count no slice holds,
 * which the entry raises.
 */
bool
doltgres_entries_doltgres_multi_insert(const PgCatalogCaps *caps, const PgRel *rel, PgSlotsResult *slots, uint32 cid,
									   uint32 options, const PgBulkInsert *bistate, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);

	if (!slots->ok)
	{
		*error = slots->u.error;
		return false;
	}
	if (!pg_calls_serial_conflict_in(raise, rel, error))
		return false;
	for (size_t i = 0; i < slots->u.slots.len; i++)
		if (!doltgres_entries_inserted(caps, rel, slots->u.slots.ptr[i], cid, options, false, 0, error))
			return false;
	return pg_calls_count_inserts(raise, rel, (int64) slots->u.slots.len, error);
}

/*
 * doltgres_entries_doltgres_tuple_insert_speculative is the fork's
 * tuple_insert_speculative ((4b)'s section 4): it inserts the row a slot
 * holds as tuple_insert does, its version holding the speculative token,
 * which a Dirty reader reports and a resolve waits on, as heap's heap_insert
 * with HEAP_INSERT_SPECULATIVE does.
 */
bool
doltgres_entries_doltgres_tuple_insert_speculative(const PgCatalogCaps *caps, const PgRel *rel, PgSlot *slot,
												   uint32 cid, uint32 options, const PgBulkInsert *bistate,
												   uint32 specToken, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);

	return pg_calls_serial_conflict_in(raise, rel, error) &&
		doltgres_entries_inserted(caps, rel, slot, cid, options, true, specToken, error) &&
		pg_calls_count_inserts(raise, rel, 1, error);
}

/* doltgres_entries_Record_drop is an entry's record change's drop: its key's and its op's. */
void
doltgres_entries_Record_drop(txn_TupleVecU8Op_t *record)
{
	std_vec_free_Checked(&record->f0, 1, 1);
	txn_memtable_Op_drop(&record->f1);
}

/*
 * doltgres_entries_record_identity returns a row's identity, which its
 * record in an index that keeps records is keyed by (entry's record): its
 * row key, or a keyless table's copy's tuple id, big-endian.
 */
void
doltgres_entries_record_identity(const DoltgresIndexDescribed *described, const uint8 *row_key, size_t row_key_len,
								 uint64 tuple, StdVec *result)
{
	uint8		bytes[sizeof(uint64)];

	if (!doltgres_index_Described_keyless(described))
	{
		std_slice_to_vec_Checked(result, row_key, row_key_len, 1, 1);
		return;
	}
	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8) (tuple >> (8 * (sizeof(bytes) - 1 - i)));
	std_slice_to_vec_Checked(result, bytes, sizeof(bytes), 1, 1);
}

/* doltgres_entries_record_change_frame_t is record_change's state across its pad: the record's op, while live. */
typedef struct doltgres_entries_record_change_frame_t
{
	TxnMemtableOp op;
	bool		op_live;
	const uint8 *identity;
	size_t		identity_len;
	txn_TupleVecU8Op_t *result;
} doltgres_entries_record_change_frame_t;

/* doltgres_entries_record_change_pad frees what record_change holds. */
static void
doltgres_entries_record_change_pad(void *frame_)
{
	doltgres_entries_record_change_frame_t *frame = frame_;

	if (frame->op_live)
		txn_memtable_Op_drop(&frame->op);
	frame->op_live = false;
}

/* doltgres_entries_record_change_body makes the record's key and pairs it with the op. */
static void
doltgres_entries_record_change_body(void *frame_)
{
	doltgres_entries_record_change_frame_t *frame = frame_;

	txn_entry_record(frame->identity, frame->identity_len, &frame->result->f0);
	frame->result->f1 = frame->op;
	frame->op_live = false;
}

/*
 * doltgres_entries_record_change returns the change of a row's record an
 * index that keeps records pushes with its entry's change: at the record's
 * key, a put of the entry's key, which it takes, or a delete for none (entry
 * NULL), a keyless table's as the copy's added or removed copy under its
 * tuple id, as its entries count copies.
 */
void
doltgres_entries_record_change(const DoltgresIndexDescribed *described, const uint8 *identity, size_t identity_len,
							   uint64 tuple, StdVec *entry, txn_TupleVecU8Op_t *result)
{
	bool		keyless = doltgres_index_Described_keyless(described);
	doltgres_entries_record_change_frame_t frame = {.op_live = true, .identity = identity,
	.identity_len = identity_len, .result = result};

	if (entry != NULL)
	{
		TxnMemtablePut *put = keyless ? &frame.op.u.add : &frame.op.u.put;

		frame.op.tag = keyless ? TXN_MEMTABLE_OP_ADD : TXN_MEMTABLE_OP_PUT;
		put->value = *entry;
		std_vec_new(&put->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_new(&put->values, sizeof(uint32), _Alignof(uint32));
		put->tuple.f0 = keyless ? tuple : 0;
		put->heap_bytes = 0;
	}
	else if (keyless)
	{
		frame.op.tag = TXN_MEMTABLE_OP_REMOVE;
		frame.op.u.remove.f0.f0 = tuple;
		frame.op.u.remove.has_f1 = false;
	}
	else
		frame.op.tag = TXN_MEMTABLE_OP_DELETE;
	std_landing(&frame, doltgres_entries_record_change_body, doltgres_entries_record_change_pad);
	doltgres_entries_record_change_pad(&frame);
}

/*
 * doltgres_entries_entry_pushed_captures_t is entry_pushed's lent call's
 * captures: the entry change, its record change, and their live flags.
 */
typedef struct doltgres_entries_entry_pushed_captures_t
{
	TxnEntryEntry entry;
	bool		entry_live;
	bool		has_record;
	txn_TupleVecU8Op_t record;
	bool		record_live;
} doltgres_entries_entry_pushed_captures_t;

/*
 * doltgres_entries_entry_pushed_closure_0 is entry_pushed's lent call: the
 * entry and its record changed, which it takes.
 */
static bool
doltgres_entries_entry_pushed_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
										HostError *error)
{
	doltgres_entries_entry_pushed_captures_t *c = captures;

	c->entry_live = c->record_live = false;
	return txn_entry_change(io, held, &c->entry, c->has_record, &c->record, error);
}

/* doltgres_entries_entry_pushed_frame_t is entry_pushed's state across its pad: its captures. */
typedef struct doltgres_entries_entry_pushed_frame_t
{
	doltgres_entries_entry_pushed_captures_t c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entry_pushed_frame_t;

/* doltgres_entries_entry_pushed_pad frees what entry_pushed holds. */
static void
doltgres_entries_entry_pushed_pad(void *frame_)
{
	doltgres_entries_entry_pushed_frame_t *frame = frame_;

	if (frame->c.record_live)
		doltgres_entries_Record_drop(&frame->c.record);
	if (frame->c.entry_live)
	{
		std_vec_free_Checked(&frame->c.entry.key, 1, 1);
		txn_memtable_Op_drop(&frame->c.entry.op);
	}
	frame->c.record_live = frame->c.entry_live = false;
}

/* doltgres_entries_entry_pushed_body changes the entry under the index storage's held state. */
static void
doltgres_entries_entry_pushed_body(void *frame_)
{
	doltgres_entries_entry_pushed_frame_t *frame = frame_;
	TxnStateFound made;
	HostError	failed;

	if (!doltgres_entries_index_making(frame->io, frame->index, &made, frame->error))
		return;
	if (!txn_state_held_Unit(frame->io, frame->repo, frame->reading, made, doltgres_entries_entry_pushed_closure_0,
							 &frame->c, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_entry_pushed changes an index's entry at a key, and with
 * it its record when the index keeps records (record NULL for none), in the
 * index's storage, under the state the backend's top-level transaction holds
 * (entry's change), taking the key, the op, and the record.
 */
bool
doltgres_entries_entry_pushed(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const PgRel *index,
							  StdVec *key, TxnMemtableOp *op, txn_TupleVecU8Op_t *record,
							  TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, PgPgError *error)
{
	doltgres_entries_entry_pushed_frame_t frame = {.io = io, .repo = repo, .reading = reading, .index = index,
	.error = error};

	frame.c.entry.writer = writer;
	frame.c.entry.roomed = roomed;
	frame.c.entry.storage = doltgres_entries_storage(index);
	frame.c.entry.table = pg_calls_index_table(index);
	frame.c.entry.index = pg_calls_rel_id(index);
	frame.c.entry.key = *key;
	frame.c.entry.op = *op;
	frame.c.entry.logged = pg_calls_rel_permanent(index);
	frame.c.entry_live = true;
	frame.c.has_record = frame.c.record_live = record != NULL;
	if (record != NULL)
		frame.c.record = *record;
	std_landing(&frame, doltgres_entries_entry_pushed_body, doltgres_entries_entry_pushed_pad);
	doltgres_entries_entry_pushed_pad(&frame);
	return frame.ok;
}

/* txn::state::held at Option<Row>, which searches.c defines and recorded_entry and entry_held take. */
#define TXN_STATE_TS OptionRow
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, DoltgresScanRow *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_index_read_captures_t is an index read's lent calls'
 * captures: the Dirty snapshot it reads under, the base generation its base
 * was read at, and the scan reader it steps.
 */
typedef struct doltgres_entries_index_read_captures_t
{
	const HostSnapshot *read;
	uint64		generation;
	HostOwnedHandle reader;
} doltgres_entries_index_read_captures_t;

/*
 * doltgres_entries_index_read_readable is an index read's first lent call's
 * body: the base the Dirty snapshot may read, with the generation read
 * before it.
 */
static bool
doltgres_entries_index_read_readable(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									 bool *some, TxnBaseBase *result, HostError *error)
{
	doltgres_entries_index_read_captures_t *c = captures;

	c->generation = txn_state_generation(host_shared_Held_record(held));
	return txn_state_readable(io, held, c->read, some, result, error);
}

/* doltgres_entries_index_read_step is an index read's step's body: the scan reader's next row under the held state. */
static bool
doltgres_entries_index_read_step(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								 bool *some, DoltgresScanRow *result, HostError *error)
{
	doltgres_entries_index_read_captures_t *c = captures;
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, repo};
	DoltgresScanUnder under = {.state = host_shared_Held_record(held), .snapshot = c->read};

	return doltgres_scan_next(io, &stores, &under, c->reader, DOLTGRES_SCAN_DIRECTION_FORWARD, some, result, error);
}

/*
 * doltgres_entries_index_read_frame_t is an index read's state across its
 * pad: the record's key, the Dirty snapshot, the base, the key order until
 * the reader takes it, and a row read, each while live, and the reader's two
 * lent calls, its readable base and its step.
 */
typedef struct doltgres_entries_index_read_frame_t
{
	StdVec		record;
	bool		record_live;
	PgOwnedSnapshot owned;
	bool		owned_live;
	TxnBaseBase base;
	bool		base_live;
	TreesKeyOrder order;
	bool		order_live;
	DoltgresScanRow row;
	bool		row_live;
	doltgres_entries_index_read_captures_t captures;
	bool		(*readable) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
							 bool *some, TxnBaseBase *result, HostError *error);
	bool		(*step) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held, bool *some,
						 DoltgresScanRow *result, HostError *error);
	HostSnapshot read;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *index;
	const uint8 *keyed;
	size_t		keyed_len;
	const uint8 *key;
	size_t		key_len;
	bool		has_copy;
	uint64		copy;
	const uint8 *row_key;
	size_t		row_key_len;
	bool	   *has_result;
	StdVec	   *result;
	bool	   *held;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_index_read_frame_t;

/* doltgres_entries_index_read_pad frees what an index read holds. */
static void
doltgres_entries_index_read_pad(void *frame_)
{
	doltgres_entries_index_read_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	if (frame->record_live)
		std_vec_free_Checked(&frame->record, 1, 1);
	frame->row_live = frame->order_live = frame->base_live = frame->owned_live = frame->record_live = false;
}

/*
 * doltgres_entries_index_read_begin begins an index read: its Dirty
 * snapshot, the base it may read, and the scan reader over the index's
 * storage sought to a key.
 */
static bool
doltgres_entries_index_read_begin(doltgres_entries_index_read_frame_t *frame, const uint8 *from, size_t from_len)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, frame->repo};
	TxnStateFound found;
	bool		some;
	TreesStoreReader store;
	StdVec		keyed;
	HostError	failed;

	if (!pg_OwnedSnapshot_dirty(raise, &frame->owned, frame->error))
		return false;
	frame->owned_live = true;
	if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &frame->read, frame->error) ||
		!doltgres_entries_index_making(frame->io, frame->index, &found, frame->error))
		return false;
	frame->captures.read = &frame->read;
	if (!txn_state_held_OptionBase(frame->io, frame->repo, frame->reading, found,
								   frame->readable, &frame->captures, NULL, &some, &frame->base,
								   &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->base_live = some;
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	std_slice_to_vec_Checked(&keyed, frame->keyed, frame->keyed_len, 1, 1);
	frame->order_live = false;
	if (!doltgres_scan_scan_begin(frame->io, TREES_STORE_READER_NODE_STORE(&store), 0, some ? &frame->base : NULL,
								  frame->captures.generation, &keyed, pg_calls_index_table(frame->index),
								  &frame->order, &frame->captures.reader, &failed) ||
		!doltgres_scan_seek(frame->io, &stores, frame->captures.reader, from, from_len,
							DOLTGRES_SCAN_DIRECTION_FORWARD, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_index_read_next reads the scan reader's next row into the frame. */
static bool
doltgres_entries_index_read_next(doltgres_entries_index_read_frame_t *frame, bool *some)
{
	TxnStateFound found;
	HostError	failed;

	if (!doltgres_entries_index_making(frame->io, frame->index, &found, frame->error))
		return false;
	if (!txn_state_held_OptionRow(frame->io, frame->repo, frame->reading, found,
								  frame->step, &frame->captures, NULL, some, &frame->row,
								  &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->row_live = *some;
	return true;
}

/* doltgres_entries_index_read_end ends the index read's scan reader. */
static bool
doltgres_entries_index_read_end(doltgres_entries_index_read_frame_t *frame)
{
	HostError	failed;

	if (doltgres_scan_scan_end(frame->io, frame->captures.reader, &failed))
		return true;
	*frame->error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_recorded_entry_closure_0 is recorded_entry's first lent
 * call: the base the Dirty snapshot may read.
 */
static bool
doltgres_entries_recorded_entry_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
										  const HostSharedHeld *held, bool *some, TxnBaseBase *result,
										  HostError *error)
{
	return doltgres_entries_index_read_readable(captures, io, repo, held, some, result, error);
}

/* doltgres_entries_recorded_entry_closure_1 is recorded_entry's step: the scan reader's next row. */
static bool
doltgres_entries_recorded_entry_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
										  const HostSharedHeld *held, bool *some, DoltgresScanRow *result,
										  HostError *error)
{
	return doltgres_entries_index_read_step(captures, io, repo, held, some, result, error);
}

/* doltgres_entries_recorded_entry_body reads the row's record and answers its value. */
static void
doltgres_entries_recorded_entry_body(void *frame_)
{
	doltgres_entries_index_read_frame_t *frame = frame_;
	bool		some;

	txn_entry_record(frame->key, frame->key_len, &frame->record);
	frame->record_live = true;
	if (!doltgres_entries_index_read_begin(frame, frame->record.ptr, frame->record.len) ||
		!doltgres_entries_index_read_next(frame, &some) || !doltgres_entries_index_read_end(frame))
		return;
	*frame->has_result = some && frame->row.key.len == frame->record.len &&
		memcmp(frame->row.key.ptr, frame->record.ptr, frame->record.len) == 0;
	if (*frame->has_result)
	{
		*frame->result = frame->row.tuple;
		std_vec_new(&frame->row.tuple, 1, 1);
	}
	frame->ok = true;
}

/*
 * doltgres_entries_recorded_entry returns the key of a row's entry an index
 * that keeps records holds, the value of the row's record, read under a
 * Dirty snapshot through the scan reader sought to the record's key, or none
 * when the index holds no record of the row, which then has no entry; the
 * index's table's keyed layout tells whether its entries count copies. It
 * takes the key order.
 */
static bool
doltgres_entries_recorded_entry(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
								const HostSharedReader *reading, const PgRel *index, const uint8 *keyed,
								size_t keyed_len, TreesKeyOrder *order, const uint8 *identity, size_t identity_len,
								bool *has_result, StdVec *result, PgPgError *error)
{
	doltgres_entries_index_read_frame_t frame = {.order = *order, .order_live = true,
		.readable = doltgres_entries_recorded_entry_closure_0, .step = doltgres_entries_recorded_entry_closure_1,
		.caps = caps, .io = io, .repo = repo, .reading = reading, .index = index, .keyed = keyed,
		.keyed_len = keyed_len, .key = identity, .key_len = identity_len, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_recorded_entry_body, doltgres_entries_index_read_pad);
	doltgres_entries_index_read_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_entry_held_closure_0 is entry_held's first lent call: the base the Dirty snapshot may read. */
static bool
doltgres_entries_entry_held_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *some, TxnBaseBase *result, HostError *error)
{
	return doltgres_entries_index_read_readable(captures, io, repo, held, some, result, error);
}

/* doltgres_entries_entry_held_closure_1 is entry_held's step: the scan reader's next row. */
static bool
doltgres_entries_entry_held_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *some, DoltgresScanRow *result, HostError *error)
{
	return doltgres_entries_index_read_step(captures, io, repo, held, some, result, error);
}

/* doltgres_entries_entry_held_body steps the reader over the key's rows until one is the entry asked for. */
static void
doltgres_entries_entry_held_body(void *frame_)
{
	doltgres_entries_index_read_frame_t *frame = frame_;
	bool		some;

	*frame->held = false;
	if (!doltgres_entries_index_read_begin(frame, frame->key, frame->key_len))
		return;
	for (;;)
	{
		bool		named = true;

		if (!doltgres_entries_index_read_next(frame, &some))
			return;
		if (!some || frame->row.key.len != frame->key_len ||
			(frame->key_len > 0 && memcmp(frame->row.key.ptr, frame->key, frame->key_len) != 0))
			break;
		if (frame->row_key != NULL)
		{
			StdBytes	row_key;

			if (!access_index_IndexKey_row_key(frame->row.tuple.ptr, frame->row.tuple.len, &row_key, frame->error))
				return;
			named = row_key.len == frame->row_key_len &&
				(row_key.len == 0 || memcmp(row_key.ptr, frame->row_key, row_key.len) == 0);
		}
		if (named && (!frame->has_copy || frame->copy == frame->row.stamp.tuple))
		{
			*frame->held = true;
			break;
		}
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
	}
	if (frame->row_live)
	{
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
	}
	frame->ok = doltgres_entries_index_read_end(frame);
}

/*
 * doltgres_entries_entry_held reports whether an index's storage holds an
 * entry at a key under a Dirty snapshot, through the scan reader's merge of
 * its base and delta in its tree's order, a keyless table's entry by the
 * copy a tuple id names (has_copy), and a unique index's entry, whose key
 * leaves its row's out, by the row key its value holds (row_key NULL for
 * none); the index's table's keyed layout tells whether its entries count
 * copies. It takes the key order.
 */
static bool
doltgres_entries_entry_held(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							const HostSharedReader *reading, const PgRel *index, const uint8 *keyed, size_t keyed_len,
							TreesKeyOrder *order, const uint8 *key, size_t key_len, bool has_copy, uint64 copy,
							const uint8 *row_key, size_t row_key_len, bool *result, PgPgError *error)
{
	doltgres_entries_index_read_frame_t frame = {.order = *order, .order_live = true,
		.readable = doltgres_entries_entry_held_closure_0, .step = doltgres_entries_entry_held_closure_1,
		.caps = caps, .io = io,
		.repo = repo, .reading = reading, .index = index, .keyed = keyed, .keyed_len = keyed_len, .key = key,
		.key_len = key_len, .has_copy = has_copy, .copy = copy, .row_key = row_key, .row_key_len = row_key_len,
	.held = result, .error = error};

	std_landing(&frame, doltgres_entries_entry_held_body, doltgres_entries_index_read_pad);
	doltgres_entries_index_read_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_removal_frame_t is removal's state across its pad: the
 * recorded key and its columns, each while live.
 */
typedef struct doltgres_entries_removal_frame_t
{
	TxnBaseKey	key;
	bool		key_live;
	StdVec		table_key;
	bool		table_key_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	HostOwnedHandle *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_removal_frame_t;

/* doltgres_entries_removal_pad frees what removal holds. */
static void
doltgres_entries_removal_pad(void *frame_)
{
	doltgres_entries_removal_frame_t *frame = frame_;

	if (frame->table_key_live)
		doltgres_layout_VecKeyed_drop(&frame->table_key);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->table_key_live = frame->key_live = false;
}

/* doltgres_entries_removal_body finds the backend's removal of the table, or makes one with its keyed layout. */
static void
doltgres_entries_removal_body(void *frame_)
{
	doltgres_entries_removal_frame_t *frame = frame_;
	TxnBaseBase recorded;
	bool		listed;
	HostError	failed;

	if (!doltgres_index_listed(frame->io, frame->rel, &listed, frame->result, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (listed)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_recorded(frame->caps, frame->io, frame->repo, frame->reading, frame->rel, &recorded,
								   frame->error))
		return;
	frame->key = recorded.key;
	frame->key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
									 &frame->key, &frame->table_key, frame->error))
		return;
	frame->table_key_live = true;
	frame->ok = doltgres_index_removal(frame->caps, frame->io, frame->rel, frame->table_key.ptr, frame->table_key.len,
									   &frame->key, frame->result, frame->error);
}

/*
 * doltgres_entries_removal returns the handle of a table's removal of old
 * entries the backend holds, made with the table's keyed layout when it
 * holds none.
 */
static bool
doltgres_entries_removal(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, HostOwnedHandle *result, PgPgError *error)
{
	doltgres_entries_removal_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_removal_body, doltgres_entries_removal_pad);
	doltgres_entries_removal_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_entries_removed_captures_t is entries_removed's remove's
 * captures: the repository and reader, the table's keyed layout, the old
 * row's tuple id and key, and the writer.
 */
typedef struct doltgres_entries_entries_removed_captures_t
{
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const StdVec *keyed;
	uint64		old;
	StdBytes	old_key;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
} doltgres_entries_entries_removed_captures_t;

/*
 * doltgres_entries_entries_removed_closure_0_frame_t is entries_removed's
 * remove's state across its pad: the index's key order until recorded_entry
 * takes it, the entry's key, the row's identity, and its record change, each
 * while live.
 */
typedef struct doltgres_entries_entries_removed_closure_0_frame_t
{
	TreesKeyOrder order;
	bool		order_live;
	StdVec		key;
	bool		key_live;
	StdVec		identity;
	bool		identity_live;
	txn_TupleVecU8Op_t record;
	bool		record_live;
	doltgres_entries_entries_removed_captures_t *c;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *index;
	DoltgresIndexOld *entry;
	const DoltgresIndexDescribed *described;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entries_removed_closure_0_frame_t;

/* doltgres_entries_entries_removed_closure_0_pad frees what entries_removed's remove holds. */
static void
doltgres_entries_entries_removed_closure_0_pad(void *frame_)
{
	doltgres_entries_entries_removed_closure_0_frame_t *frame = frame_;

	if (frame->record_live)
		doltgres_entries_Record_drop(&frame->record);
	if (frame->identity_live)
		std_vec_free_Checked(&frame->identity, 1, 1);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->record_live = frame->identity_live = frame->key_live = frame->order_live = false;
}

/* doltgres_entries_entries_removed_closure_0_body finds the entry's key and pushes its removal with its record's. */
static void
doltgres_entries_entries_removed_closure_0_body(void *frame_)
{
	doltgres_entries_entries_removed_closure_0_frame_t *frame = frame_;
	doltgres_entries_entries_removed_captures_t *c = frame->c;
	TxnMemtableOp op = {0};

	if (frame->entry->tag == DOLTGRES_INDEX_OLD_KEY)
	{
		frame->key = frame->entry->key;
		frame->key_live = true;
	}
	else
	{
		bool		has_key;

		doltgres_index_Described_order(frame->described, NULL, &frame->order);
		frame->order_live = true;
		doltgres_entries_record_identity(frame->described, c->old_key.ptr, c->old_key.len, c->old, &frame->identity);
		frame->identity_live = true;
		frame->order_live = false;
		if (!doltgres_entries_recorded_entry(frame->caps, frame->io, c->repo, c->reading, frame->index,
											 c->keyed->ptr, c->keyed->len, &frame->order, frame->identity.ptr,
											 frame->identity.len, &has_key, &frame->key, frame->error))
			return;
		if (!has_key)
		{
			frame->ok = true;
			return;
		}
		frame->key_live = true;
		doltgres_entries_record_change(frame->described, frame->identity.ptr, frame->identity.len, c->old, NULL,
									   &frame->record);
		frame->record_live = true;
		frame->identity_live = false;
		std_vec_free_Checked(&frame->identity, 1, 1);
	}
	if (doltgres_index_Described_keyless(frame->described))
	{
		op.tag = TXN_MEMTABLE_OP_REMOVE;
		op.u.remove.f0.f0 = c->old;
	}
	else
		op.tag = TXN_MEMTABLE_OP_DELETE;
	frame->key_live = false;
	frame->record_live = false;
	frame->ok = doltgres_entries_entry_pushed(frame->io, c->repo, c->reading, frame->index, &frame->key, &op,
											  frame->entry->tag == DOLTGRES_INDEX_OLD_KEY ? NULL : &frame->record,
											  c->writer, c->roomed, frame->error);
}

/*
 * doltgres_entries_entries_removed_closure_0 is entries_removed's remove: an
 * index's old entry removed, which it takes.
 */
static bool
doltgres_entries_entries_removed_closure_0(void *captures, const PgCatalogCaps *caps, HostIoIo *io,
										   const PgRel *index, DoltgresIndexOld *old,
										   const DoltgresIndexDescribed *described, PgPgError *error)
{
	doltgres_entries_entries_removed_closure_0_frame_t frame = {.c = captures, .caps = caps, .io = io,
	.index = index, .entry = old, .described = described, .error = error};

	std_landing(&frame, doltgres_entries_entries_removed_closure_0_body,
				doltgres_entries_entries_removed_closure_0_pad);
	doltgres_entries_entries_removed_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_entries_removed_frame_t is entries_removed's state across
 * its pad: what it opened, the table's recorded key, its columns while
 * keyed reads them, and its keyed layout, each while live.
 */
typedef struct doltgres_entries_entries_removed_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	table_key;
	bool		table_key_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		keyed;
	bool		keyed_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	DoltgresIndexRemoving *rows;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entries_removed_frame_t;

/* doltgres_entries_entries_removed_pad frees what entries_removed holds. */
static void
doltgres_entries_entries_removed_pad(void *frame_)
{
	doltgres_entries_entries_removed_frame_t *frame = frame_;

	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->table_key_live)
		txn_base_Key_drop(&frame->table_key);
	frame->keyed_live = frame->columns_live = frame->table_key_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_entries_removed_body removes the old row's entries from each index the fork stores. */
static void
doltgres_entries_entries_removed_body(void *frame_)
{
	doltgres_entries_entries_removed_frame_t *frame = frame_;
	doltgres_entries_entries_removed_captures_t captures = {.reading = &frame->opened.reading,
		.keyed = &frame->keyed, .old = frame->rows->old_tuple, .old_key = frame->rows->old_key,
	.writer = frame->writer, .roomed = frame->roomed};
	HostOwnedHandle handle;
	TxnBaseBase recorded;
	HostError	failed;

	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-entries-removing", &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!doltgres_entries_open(frame->io, &frame->opened, frame->error))
		return;
	captures.repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_removal(frame->caps, frame->io, captures.repo, &frame->opened.reading, frame->rel, &handle,
								  frame->error) ||
		!doltgres_entries_recorded(frame->caps, frame->io, captures.repo, &frame->opened.reading, frame->rel,
								   &recorded, frame->error))
		return;
	frame->table_key = recorded.key;
	frame->table_key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
									 &frame->table_key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->keyed);
	frame->keyed_live = true;
	frame->columns_live = false;
	doltgres_layout_VecKeyed_drop(&frame->columns);
	frame->ok = doltgres_index_removed(frame->caps, frame->io, frame->rel, handle, &frame->writer, frame->rows,
									   doltgres_entries_entries_removed_closure_0, &captures, frame->result,
									   frame->error);
}

/*
 * doltgres_entries_entries_removed removes the entries of the row a table's
 * change ends from each index of the table the fork stores, as index's
 * removed decides, each as a delete of its key, or for a keyless table's row
 * the removal of the copy the old row's tuple id names, written by a writer,
 * an index that keeps records removing the entry its record names with the
 * record (recorded_entry), and nothing when it holds no record of the row;
 * it answers whether an index must take an update's new row. Its callers run
 * it once the row's own change is pushed, outside every lock and
 * registration of the fork, and the row's change and its entries' removals
 * commit or abort together. It passes the pause doltgres-entries-removing
 * first, where a case holds a change between its row's push and its entries'
 * removals (orev-14's R1).
 */
static bool
doltgres_entries_entries_removed(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel,
								 DoltgresIndexRemoving *rows, TxnMemtableVersionWriter writer, TxnStatusRoomed roomed,
								 bool *result, PgPgError *error)
{
	doltgres_entries_entries_removed_frame_t frame = {.caps = caps, .io = io, .rel = rel, .rows = rows,
	.writer = writer, .roomed = roomed, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_entries_removed_body, doltgres_entries_entries_removed_pad);
	doltgres_entries_entries_removed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_entries_killed_captures_t is entries_killed's work's
 * captures: the repository and reader, the killed relation, the row's tuple
 * id and key, the entries its indexes wrote, and the writer.
 */
typedef struct doltgres_entries_entries_killed_captures_t
{
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	uint64		tuple;
	const uint8 *row_key;
	size_t		row_key_len;
	StdVec	   *entries;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
} doltgres_entries_entries_killed_captures_t;

/*
 * doltgres_entries_entries_killed_closure_0_frame_t is entries_killed's
 * work's state across its pad: an entry's key, the row's identity, and its
 * record change, each while live.
 */
typedef struct doltgres_entries_entries_killed_closure_0_frame_t
{
	StdVec		key;
	bool		key_live;
	StdVec		identity;
	bool		identity_live;
	txn_TupleVecU8Op_t record;
	bool		record_live;
	doltgres_entries_entries_killed_captures_t *c;
	HostIoIo   *io;
	int32		slot;
	const doltgres_TupleU32Described_t *indexes;
	size_t		indexes_len;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entries_killed_closure_0_frame_t;

/* doltgres_entries_entries_killed_closure_0_pad frees what entries_killed's work holds. */
static void
doltgres_entries_entries_killed_closure_0_pad(void *frame_)
{
	doltgres_entries_entries_killed_closure_0_frame_t *frame = frame_;

	if (frame->record_live)
		doltgres_entries_Record_drop(&frame->record);
	if (frame->identity_live)
		std_vec_free_Checked(&frame->identity, 1, 1);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	frame->record_live = frame->identity_live = frame->key_live = false;
}

/* doltgres_entries_entries_killed_closure_0_body pushes the removal of each entry the killed row's indexes wrote. */
static void
doltgres_entries_entries_killed_closure_0_body(void *frame_)
{
	doltgres_entries_entries_killed_closure_0_frame_t *frame = frame_;
	doltgres_entries_entries_killed_captures_t *c = frame->c;

	for (size_t at = 0; at < frame->indexes_len; at++)
	{
		const doltgres_TupleU32Described_t *each = &frame->indexes[at];
		const PgRel *index = pg_calls_old_entries_index(c->rel, frame->slot, (int32) at);

		for (;;)
		{
			size_t		found = 0;
			doltgres_TupleU32VecU8_t entry;
			TxnMemtableOp op = {0};
			bool		records;

			while (found < c->entries->len && STD_VEC_AT(c->entries, doltgres_TupleU32VecU8_t, found).f0 != each->f0)
				found++;
			if (found == c->entries->len)
				break;
			std_vec_swap_remove(c->entries, found, &entry, sizeof(doltgres_TupleU32VecU8_t),
								_Alignof(doltgres_TupleU32VecU8_t));
			frame->key = entry.f1;
			frame->key_live = true;
			if (doltgres_index_Described_keyless(&each->f1))
			{
				op.tag = TXN_MEMTABLE_OP_REMOVE;
				op.u.remove.f0.f0 = c->tuple;
			}
			else
				op.tag = TXN_MEMTABLE_OP_DELETE;
			records = doltgres_index_Described_records(&each->f1);
			if (records)
			{
				doltgres_entries_record_identity(&each->f1, c->row_key, c->row_key_len, c->tuple, &frame->identity);
				frame->identity_live = true;
				doltgres_entries_record_change(&each->f1, frame->identity.ptr, frame->identity.len, c->tuple, NULL,
											   &frame->record);
				frame->record_live = true;
				frame->identity_live = false;
				std_vec_free_Checked(&frame->identity, 1, 1);
			}
			frame->key_live = frame->record_live = false;
			if (!doltgres_entries_entry_pushed(frame->io, c->repo, c->reading, index, &frame->key, &op,
											   records ? &frame->record : NULL, c->writer, c->roomed, frame->error))
				return;
		}
	}
	frame->ok = true;
}

/* doltgres_entries_entries_killed_closure_0 is entries_killed's work over the table's opened indexes. */
static bool
doltgres_entries_entries_killed_closure_0(void *captures, const PgCatalogCaps *caps, HostIoIo *io, int32 slot,
										  const doltgres_TupleU32Described_t *indexes, size_t indexes_len,
										  PgPgError *error)
{
	doltgres_entries_entries_killed_closure_0_frame_t frame = {.c = captures, .io = io, .slot = slot,
	.indexes = indexes, .indexes_len = indexes_len, .error = error};

	std_landing(&frame, doltgres_entries_entries_killed_closure_0_body, doltgres_entries_entries_killed_closure_0_pad);
	doltgres_entries_entries_killed_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_entries_killed_frame_t is entries_killed's state across
 * its pad: what it opened and the entries the row's indexes wrote, each
 * while live.
 */
typedef struct doltgres_entries_entries_killed_frame_t
{
	doltgres_entries_opened_t opened;
	StdVec		entries;
	bool		entries_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	uint64		tuple;
	const uint8 *row_key;
	size_t		row_key_len;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_entries_killed_frame_t;

/* doltgres_entries_entries_killed_pad frees what entries_killed holds. */
static void
doltgres_entries_entries_killed_pad(void *frame_)
{
	doltgres_entries_entries_killed_frame_t *frame = frame_;

	if (frame->entries_live)
		std_vec_drop_Checked(&frame->entries, sizeof(doltgres_TupleU32VecU8_t), _Alignof(doltgres_TupleU32VecU8_t),
							 doltgres_entries_TupleU32VecU8_drop);
	frame->entries_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_entries_killed_body removes the killed row's entries through the table's removal. */
static void
doltgres_entries_entries_killed_body(void *frame_)
{
	doltgres_entries_entries_killed_frame_t *frame = frame_;
	doltgres_entries_entries_killed_captures_t captures = {.reading = &frame->opened.reading, .rel = frame->rel,
		.tuple = frame->tuple, .row_key = frame->row_key, .row_key_len = frame->row_key_len,
	.entries = &frame->entries, .writer = frame->writer, .roomed = frame->roomed};
	HostOwnedHandle handle;

	if (frame->entries.len == 0)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_open(frame->io, &frame->opened, frame->error))
		return;
	captures.repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_removal(frame->caps, frame->io, captures.repo, &frame->opened.reading, frame->rel, &handle,
								  frame->error))
		return;
	frame->ok = doltgres_index_opened(frame->caps, frame->io, handle, doltgres_entries_entries_killed_closure_0,
									  &captures, frame->error);
}

/*
 * doltgres_entries_entries_killed removes the entries a killed speculative
 * insert's row took, those its indexes' aminsert wrote (speculated_entry),
 * which it takes, each by the key it was written at, as a delete, or for a
 * keyless table's row the removal of the copy its tuple id names, with the
 * row's record for an index that keeps records, written by a writer. It
 * computes no entry, so no index expression or predicate runs, as
 * heap_abort_speculative runs none, and an index whose aminsert wrote
 * nothing, an arbiter whose partial check found another row, gets no
 * removal.
 */
static bool
doltgres_entries_entries_killed(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, uint64 tuple,
								const uint8 *row_key, size_t row_key_len, StdVec *entries,
								TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, PgPgError *error)
{
	doltgres_entries_entries_killed_frame_t frame = {.entries = *entries, .entries_live = true, .caps = caps,
		.io = io, .rel = rel, .tuple = tuple, .row_key = row_key, .row_key_len = row_key_len, .writer = writer,
	.roomed = roomed, .error = error};

	std_landing(&frame, doltgres_entries_entries_killed_body, doltgres_entries_entries_killed_pad);
	doltgres_entries_entries_killed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_speculated_taken_body takes the backend's speculative
 * insert of a token and relation from its list.
 */
static void
doltgres_entries_speculated_taken_body(void *frame_)
{
	doltgres_entries_speculated_frame_t *frame = frame_;
	HostLockIoMutex *mutex;
	StdVec	   *all;
	size_t		at = 0;

	frame->local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_SPECULATED.local, &DOLTGRES_ENTRIES_SPECULATED_TYPE);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, &DOLTGRES_ENTRIES_SPECULATED_TYPE);
	frame->guard = host_lock_IoMutex_lock(mutex, &DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	all = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	while (at < all->len && (STD_VEC_AT(all, DoltgresEntriesSpeculated, at).token != frame->speculated.token ||
							 STD_VEC_AT(all, DoltgresEntriesSpeculated, at).relation != frame->speculated.relation))
		at++;
	if (at == all->len)
		return;
	std_vec_swap_remove(all, at, &frame->speculated, sizeof(DoltgresEntriesSpeculated),
						_Alignof(DoltgresEntriesSpeculated));
	frame->speculated_live = true;
}

/*
 * doltgres_entries_speculated_taken takes the backend's speculative insert
 * of a token and relation from SPECULATED, reporting whether it held one.
 */
static bool
doltgres_entries_speculated_taken(HostIoIo *io, uint32 token, uint32 relation, DoltgresEntriesSpeculated *result)
{
	doltgres_entries_speculated_frame_t frame = {.speculated = {.token = token, .relation = relation}, .io = io};
	bool		taken;

	std_landing(&frame, doltgres_entries_speculated_taken_body, doltgres_entries_speculated_pushed_pad);
	taken = frame.speculated_live;
	frame.speculated_live = false;
	doltgres_entries_speculated_pushed_pad(&frame);
	if (taken)
		*result = frame.speculated;
	return taken;
}

/*
 * doltgres_entries_doltgres_tuple_complete_speculative_captures_t is the
 * completion's lent call's captures: the completion record, how the insert
 * completes, and whether its relation is logged.
 */
typedef struct doltgres_entries_doltgres_tuple_complete_speculative_captures_t
{
	const TxnRecordCompletion *completion;
	TxnInsertCompleted completed;
	bool		logged;
} doltgres_entries_doltgres_tuple_complete_speculative_captures_t;

/*
 * doltgres_entries_doltgres_tuple_complete_speculative_closure_2 is the
 * completion's lent call: the insert completed.
 */
static bool
doltgres_entries_doltgres_tuple_complete_speculative_closure_2(void *captures, HostIoIo *io, const RepoRepo *repo,
															   const HostSharedHeld *held, HostError *error)
{
	const doltgres_entries_doltgres_tuple_complete_speculative_captures_t *c = captures;
	bool		completed;

	return txn_insert_complete(io, held, c->completion, c->completed, c->logged, &completed, error);
}

/*
 * doltgres_entries_doltgres_tuple_complete_speculative_frame_t is the
 * completion's state across its pad: what it opened, the insert's key, the
 * entries its indexes wrote, and the completion record, each while live.
 */
typedef struct doltgres_entries_doltgres_tuple_complete_speculative_frame_t
{
	doltgres_entries_opened_t opened;
	DoltgresEntriesSpeculated speculated;
	bool		key_live;
	bool		entries_live;
	TxnRecordCompletion completion;
	bool		completion_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	uint32		token;
	bool		succeeded;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_complete_speculative_frame_t;

/* doltgres_entries_doltgres_tuple_complete_speculative_pad frees what the completion holds. */
static void
doltgres_entries_doltgres_tuple_complete_speculative_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_complete_speculative_frame_t *frame = frame_;

	if (frame->completion_live)
		std_vec_free_Checked(&frame->completion.key, 1, 1);
	if (frame->entries_live)
		std_vec_drop_Checked(&frame->speculated.entries, sizeof(doltgres_TupleU32VecU8_t),
							 _Alignof(doltgres_TupleU32VecU8_t), doltgres_entries_TupleU32VecU8_drop);
	if (frame->key_live)
		std_vec_free_Checked(&frame->speculated.key, 1, 1);
	frame->completion_live = frame->entries_live = frame->key_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_tuple_complete_speculative_body kills or confirms the insert its token names. */
static void
doltgres_entries_doltgres_tuple_complete_speculative_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_complete_speculative_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	doltgres_entries_doltgres_tuple_complete_speculative_captures_t captures = {.completion = &frame->completion};
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	TxnStateFound made;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_speculated_taken(&frame->io, frame->token, pg_calls_rel_id(frame->rel), &frame->speculated))
	{
		static const char message[] = "a speculative insert's completion names no insert of the backend's";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	frame->key_live = frame->entries_live = true;
	if (!frame->succeeded && !frame->speculated.killed)
	{
		uint32		command;

		if (!pg_calls_current_command(raise, true, &command, frame->error) ||
			!doltgres_entries_current(&frame->io, command, &writer, &roomed, frame->error))
			return;
		frame->entries_live = false;
		if (!doltgres_entries_entries_killed(frame->caps, &frame->io, frame->rel, frame->speculated.tuple,
											 frame->speculated.key.ptr, frame->speculated.key.len,
											 &frame->speculated.entries, writer, roomed, frame->error))
			return;
	}
	if (frame->speculated.killed)
	{
		static const char message[] = "a speculative insert whose key held a live row was confirmed";

		if (frame->succeeded)
			*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		frame->ok = !frame->succeeded;
		return;
	}
	captures.completed = frame->succeeded ? TXN_INSERT_COMPLETED_CONFIRMED : TXN_INSERT_COMPLETED_KILLED;
	if (!doltgres_entries_current(&frame->io, 0, &writer, &roomed, frame->error))
		return;
	frame->completion.writer = writer;
	frame->completion.storage = doltgres_entries_storage(frame->rel);
	frame->completion.key = frame->speculated.key;
	frame->completion.tuple.f0 = frame->speculated.tuple;
	frame->key_live = false;
	frame->completion_live = true;
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &made, frame->error))
		return;
	captures.logged = pg_calls_rel_permanent(frame->rel);
	if (!txn_state_held_Unit(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading, made,
							 doltgres_entries_doltgres_tuple_complete_speculative_closure_2, &captures, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_tuple_complete_speculative is the fork's
 * tuple_complete_speculative ((4b)'s section 4): it confirms a speculative
 * insert, clearing its token as heap_finish_speculative does, or kills it,
 * as heap_abort_speculative does, once, by what its insert left in
 * SPECULATED: an insert that found its key's live row spent its place and
 * has nothing to kill, and a kill removes the entries its indexes' aminsert
 * wrote (entries_killed).
 */
bool
doltgres_entries_doltgres_tuple_complete_speculative(const PgCatalogCaps *caps, const PgRel *rel, PgSlot *slot,
													 uint32 specToken, bool succeeded, PgPgError *error)
{
	doltgres_entries_doltgres_tuple_complete_speculative_frame_t frame = {.caps = caps, .rel = rel,
	.token = specToken, .succeeded = succeeded, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_complete_speculative_body,
				doltgres_entries_doltgres_tuple_complete_speculative_pad);
	doltgres_entries_doltgres_tuple_complete_speculative_pad(&frame);
	return frame.ok;
}

/*
 * DoltgresEntriesWaiting is Waiting: what a change does about a running
 * holder of its row: wait for it; answer TM_BeingModified, a delete's or an
 * update's told not to wait; or a row lock's SKIP LOCKED and NOWAIT, which
 * skip the row with TM_WouldBlock or raise heap's 55P03 when the tuple lock
 * or the holder's end can't be had at once.
 */
typedef enum DoltgresEntriesWaiting
{
	DOLTGRES_ENTRIES_WAITING_WAIT,
	DOLTGRES_ENTRIES_WAITING_ANSWER,
	DOLTGRES_ENTRIES_WAITING_SKIP,
	DOLTGRES_ENTRIES_WAITING_REFUSE,
} DoltgresEntriesWaiting;

/*
 * doltgres_entries_Changing is Changing: a delete's, an update's, or a row
 * lock's change of a row: the row's tuple id, the row lock it takes, the
 * operation its waits name, and what it does about a running holder.
 */
typedef struct doltgres_entries_Changing
{
	uint64		tuple;
	TxnMemtableLockMode mode;
	int32		oper;
	DoltgresEntriesWaiting waiting;
} doltgres_entries_Changing;

/*
 * doltgres_entries_stopped answers a change whose read stopped, or found no
 * row at its tuple id (stop NULL), as heap's heap_delete, heap_update, and
 * heap_lock_tuple do: a holder is waited for, after the row's tuple lock in
 * the change's mode, which locked keeps until the change ends, as
 * heap_acquire_tuplock takes it once, and answers none for the change to
 * read the row again; a change that doesn't wait answers or refuses as
 * Waiting says; a failure fills the failure data and answers its result; and
 * a row an update or a delete can't see raises heap's error, while a lock
 * answers TM_Invisible, as heap_lock_tuple does, for its caller to raise.
 */
static bool
doltgres_entries_stopped(const PgMayRaise *raise, HostIoIo *io, const PgRel *rel,
						 const doltgres_entries_Changing *changing, HostIoWaits *waits, bool *has_locked,
						 HostTupleGuard *locked, const DoltgresChangesStop *stop, PgFailure *tmfd, bool *has_result,
						 int32 *result, PgPgError *error)
{
	uint64		tuple = changing->tuple;
	DoltgresChangesFailed failed = {.result = DOLTGRES_CHANGES_TM_INVISIBLE, .ctid = tuple};
	HostError	host;

	*has_result = true;
	if (stop != NULL && stop->tag == DOLTGRES_CHANGES_STOP_FAILED)
		failed = stop->u.failed;
	else if (stop != NULL && stop->tag == DOLTGRES_CHANGES_STOP_HELD)
	{
		*result = DOLTGRES_CHANGES_TM_OK;
		return true;
	}
	else if (stop != NULL && changing->waiting == DOLTGRES_ENTRIES_WAITING_ANSWER)
	{
		failed.result = DOLTGRES_CHANGES_TM_BEING_MODIFIED;
		failed.xmax = (uint32) stop->u.wait.f0;
	}
	else if (stop != NULL)
	{
		bool		block = changing->waiting == DOLTGRES_ENTRIES_WAITING_WAIT;
		HostOperation on = {pg_calls_rel_id(rel), tuple, changing->oper};
		bool		waited;

		if (!*has_locked)
		{
			HostTupleLock lock = {doltgres_entries_storage(rel).database, pg_calls_rel_id(rel), tuple,
			(uint8) changing->mode};

			if (!host_lock_tuple(waits, HOST_IO_IO_IO_DOORS(io), lock, block, has_locked, locked, &host))
			{
				*error = pghost_raised(&host);
				return false;
			}
		}
		if (*has_locked)
		{
			if (!host_xact_wait(waits, HOST_IO_IO_IO_DOORS(io), stop->u.wait, true, on, block, &waited, &host))
			{
				*error = pghost_raised(&host);
				return false;
			}
			if (waited)
			{
				*has_result = false;
				return true;
			}
		}
		if (changing->waiting == DOLTGRES_ENTRIES_WAITING_REFUSE)
		{
			*has_result = false;
			return pg_calls_refuse_row_lock(raise, rel, error);
		}
		failed.result = DOLTGRES_CHANGES_TM_WOULD_BLOCK;
		failed.xmax = (uint32) stop->u.wait.f0;
	}
	if (failed.result == DOLTGRES_CHANGES_TM_INVISIBLE)
	{
		*result = DOLTGRES_CHANGES_TM_INVISIBLE;
		if (changing->oper == XLTW_Lock)
			return true;
		return pg_calls_refuse_invisible(raise, changing->oper, error);
	}
	pg_calls_set_failure(tmfd, failed.ctid, failed.xmax, failed.cmax);
	*result = failed.result;
	return true;
}

/*
 * doltgres_entries_Ends is Ends: what a delete or a row lock asks of its
 * table and of the row a tid names, read once for the entry: whether the
 * table is keyless or keyed by a deferrable key, whether its keys' equal
 * values are equal bytes, the change's writer, its command, its row lock,
 * whether it only locks, and the crosscheck snapshot, NULL for none.
 */
typedef struct doltgres_entries_Ends
{
	bool		keyless;
	bool		deferred;
	bool		by_bytes;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	uint32		command;
	TxnMemtableLockMode mode;
	bool		locks;
	const HostSnapshot *crosscheck;
} doltgres_entries_Ends;

/*
 * doltgres_entries_ends_frame_t is ends' state across its pad: the recorded
 * base and its key's columns, each while live.
 */
typedef struct doltgres_entries_ends_frame_t
{
	TxnBaseBase base;
	bool		base_live;
	StdVec		columns;
	bool		columns_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	doltgres_entries_Ends *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_ends_frame_t;

/* doltgres_entries_ends_pad frees what ends holds. */
static void
doltgres_entries_ends_pad(void *frame_)
{
	doltgres_entries_ends_frame_t *frame = frame_;

	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->columns_live = frame->base_live = false;
}

/* doltgres_entries_ends_body reads the table's key shape and the change's writer. */
static void
doltgres_entries_ends_body(void *frame_)
{
	doltgres_entries_ends_frame_t *frame = frame_;
	doltgres_entries_Ends *ends = frame->result;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	bool		immediate = false;

	if (!doltgres_entries_recorded(frame->caps, frame->io, frame->repo, frame->reading, frame->rel, &frame->base,
								   frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->base.key, &frame->columns, frame->error))
		return;
	ends->keyless = frame->columns.len == 0;
	frame->columns_live = false;
	doltgres_layout_VecKeyed_drop(&frame->columns);
	if (!ends->keyless && !pg_calls_rel_key_immediate(raise, catalog, frame->rel, &immediate, frame->error))
		return;
	ends->deferred = !ends->keyless && !immediate;
	ends->by_bytes = ends->keyless;
	if (!ends->keyless && !pg_calls_rel_key_equal_image(raise, catalog, frame->rel, &ends->by_bytes, frame->error))
		return;
	frame->ok = doltgres_entries_current(frame->io, ends->command, &ends->writer, &ends->roomed, frame->error);
}

/*
 * doltgres_entries_ends reads what a delete or a row lock asks of a table, by
 * a command, a row lock, and whether it only locks.
 */
static bool
doltgres_entries_ends(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					  const PgRel *rel, uint32 command, TxnMemtableLockMode mode, bool locks,
					  const HostSnapshot *crosscheck, doltgres_entries_Ends *result, PgPgError *error)
{
	doltgres_entries_ends_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
	.result = result, .error = error};

	result->command = command;
	result->mode = mode;
	result->locks = locks;
	result->crosscheck = crosscheck;
	std_landing(&frame, doltgres_entries_ends_body, doltgres_entries_ends_pad);
	doltgres_entries_ends_pad(&frame);
	return frame.ok;
}

/*
 * DoltgresEntriesOp is pushed's op: the version a change pushes, by whether
 * the table is keyless and its key deferrable.
 */
typedef TxnMemtableOp (*DoltgresEntriesOp) (void *captures, bool keyless, bool deferred);

/* state.rs' held at Option<Ended<Stop>>, which pushed takes. */
#define TXN_STATE_TS OptionEnded
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnChangeEnded *result, DoltgresChangesStop *stopped, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnChangeEnded *result; DoltgresChangesStop *stopped;
#define TXN_STATE_RESULT_INIT , some, result, stopped
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result, (c)->stopped
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_pushed_captures_t is pushed's lent calls' captures: the
 * reader, the relation, what the change asks, the row's tuple id, the
 * update it follows, the op, the base's store, and the held state.
 */
typedef struct doltgres_entries_pushed_captures_t
{
	const HostSharedReader *reading;
	const PgRel *rel;
	const doltgres_entries_Ends *ends;
	uint64		tuple;
	bool		has_follow;
	uint64		follow;
	DoltgresEntriesOp op;
	void	   *op_captures;
	DoltgresChangesAsked asked;
	TreesStoreReader store;
	const HostSharedHeld *held;
} doltgres_entries_pushed_captures_t;

/* doltgres_entries_pushed_closure_0_1_0 is pushed's based: whether the newest base holds the row. */
static bool
doltgres_entries_pushed_closure_0_1_0(void *captures, HostIoIo *io, bool *result, HostError *error)
{
	doltgres_entries_pushed_captures_t *c = captures;

	return doltgres_scan_based(io, TREES_STORE_READER_NODE_STORE(&c->store), c->held, c->tuple, result, error);
}

/* doltgres_entries_pushed_closure_0_1 is pushed's decide: the change's read of its key's versions. */
static bool
doltgres_entries_pushed_closure_0_1(void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions,
									size_t versions_len, TxnChangeRead *result, void *stopped, HostError *error)
{
	doltgres_entries_pushed_captures_t *c = captures;

	return doltgres_changes_decided(io, versions, versions_len, &c->asked, doltgres_entries_pushed_closure_0_1_0, c,
									result, stopped, error);
}

/*
 * doltgres_entries_pushed_closure_0 is pushed's lent call: the row's key found
 * and its version pushed under its stripe.
 */
static bool
doltgres_entries_pushed_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  bool *some, TxnChangeEnded *result, DoltgresChangesStop *stopped, HostError *error)
{
	doltgres_entries_pushed_captures_t *c = captures;
	const doltgres_entries_Ends *ends = c->ends;
	HostIoWaits waits = pghost_waits();
	bool		has_key;
	bool		keyless;
	TxnChangeEnding row;
	uint32		stripe;

	c->store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	c->held = held;
	if (!doltgres_scan_keyed(io, TREES_STORE_READER_NODE_STORE(&c->store), held, c->tuple, &has_key, &row.key,
							 &keyless, error))
		return false;
	*some = has_key;
	if (!has_key)
		return true;
	row.storage = doltgres_entries_storage(c->rel);
	stripe = txn_change_stripe(&row.storage, row.key.ptr, row.key.len, ends->by_bytes);
	row.table = pg_calls_rel_id(c->rel);
	row.op = c->op(c->op_captures, ends->keyless, ends->deferred);
	row.writer = ends->writer;
	row.roomed = ends->roomed;
	row.lock = ends->mode;
	row.no_key = false;
	row.logged = pg_calls_rel_permanent(c->rel);
	row.has_decoded = !ends->locks && pg_calls_rel_logical_decoding(c->rel);
	row.decoded.f0 = c->tuple;
	row.tuple.f0 = c->tuple;
	c->asked.tuple = c->tuple;
	c->asked.command = ends->command;
	c->asked.mode = ends->mode;
	c->asked.locks = ends->locks;
	c->asked.has_follow = c->has_follow;
	c->asked.follow = c->follow;
	c->asked.crosscheck = ends->crosscheck;
	return txn_change_end(io, held, &row, &waits, stripe, doltgres_entries_pushed_closure_0_1, c, NULL, stopped,
						  result, error);
}

/*
 * doltgres_entries_pushed pushes the version op gives, for a keyless table
 * and a deferrable key, on the row a tid names, under the table's state, as
 * change's end pushes it under the key's stripe once changes' decided read
 * the row, and answers what it did, or none (has_result false) for a tid no
 * layer holds; a stopped end writes its stop into stopped.
 */
static bool
doltgres_entries_pushed(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						const PgRel *rel, const doltgres_entries_Ends *ends, uint64 tuple, bool has_follow,
						uint64 follow, DoltgresEntriesOp op, void *op_captures, bool *has_result,
						TxnChangeEnded *result, DoltgresChangesStop *stopped, PgPgError *error)
{
	doltgres_entries_pushed_captures_t captures = {.reading = reading, .rel = rel, .ends = ends, .tuple = tuple,
	.has_follow = has_follow, .follow = follow, .op = op, .op_captures = op_captures};
	TxnStateFound made;
	HostError	failed;

	if (!doltgres_entries_making(caps, io, rel, &made, error))
		return false;
	if (txn_state_held_OptionEnded(io, repo, reading, made, doltgres_entries_pushed_closure_0, &captures, NULL,
								   has_result, result, stopped, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_doltgres_tuple_delete_closure_0 is tuple_delete's moved: a
 * move to another partition's successor, for a delete that changes partitions.
 */
static bool
doltgres_entries_doltgres_tuple_delete_closure_0(uint32 options, TxnMemtableSuccessor *result)
{
	if ((options & TABLE_DELETE_CHANGING_PARTITION) == 0)
		return false;
	result->tag = TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE;
	return true;
}

/*
 * doltgres_entries_doltgres_tuple_delete_captures_t is tuple_delete's op's
 * captures: the row's tuple id and the delete's options.
 */
typedef struct doltgres_entries_doltgres_tuple_delete_captures_t
{
	uint64		tuple;
	uint32		options;
} doltgres_entries_doltgres_tuple_delete_captures_t;

/*
 * doltgres_entries_doltgres_tuple_delete_closure_1 is tuple_delete's op: the
 * delete's version, a keyless table's removal of the copy, or a deferrable
 * key's of the row.
 */
static TxnMemtableOp
doltgres_entries_doltgres_tuple_delete_closure_1(void *captures, bool keyless, bool deferred)
{
	doltgres_entries_doltgres_tuple_delete_captures_t *c = captures;
	TxnMemtableOp op = {0};

	if (keyless)
	{
		op.tag = TXN_MEMTABLE_OP_REMOVE;
		op.u.remove.f0.f0 = c->tuple;
		op.u.remove.has_f1 = doltgres_entries_doltgres_tuple_delete_closure_0(c->options, &op.u.remove.f1);
	}
	else if (deferred)
	{
		op.tag = TXN_MEMTABLE_OP_DELETE_ROW;
		op.u.delete_row.f0.f0 = c->tuple;
		op.u.delete_row.has_f1 = doltgres_entries_doltgres_tuple_delete_closure_0(c->options, &op.u.delete_row.f1);
	}
	else if (doltgres_entries_doltgres_tuple_delete_closure_0(c->options, &op.u.move))
		op.tag = TXN_MEMTABLE_OP_MOVE;
	else
		op.tag = TXN_MEMTABLE_OP_DELETE;
	return op;
}

/*
 * doltgres_entries_doltgres_tuple_delete_frame_t is tuple_delete's state
 * across its pad: what it opened, the old row, the row's tuple lock, and the
 * slot of the removed row, each while live.
 */
typedef struct doltgres_entries_doltgres_tuple_delete_frame_t
{
	doltgres_entries_opened_t opened;
	DoltgresScanRow old;
	bool		old_live;
	HostTupleGuard locked;
	bool		locked_live;
	PgOwnedSlot removed;
	bool		removed_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgTid	   *tid;
	uint32		cid;
	uint32		options;
	const PgSnapshot *crosscheck;
	bool		wait;
	PgFailure  *tmfd;
	int32	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_delete_frame_t;

/* doltgres_entries_doltgres_tuple_delete_pad frees what tuple_delete holds. */
static void
doltgres_entries_doltgres_tuple_delete_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_delete_frame_t *frame = frame_;

	if (frame->removed_live)
		pg_OwnedSlot_drop(&frame->removed);
	if (frame->locked_live)
		host_TupleGuard_drop(&frame->locked);
	if (frame->old_live)
		doltgres_scan_Row_drop(&frame->old);
	frame->removed_live = frame->locked_live = frame->old_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_tuple_delete_removed removes the deleted row's entries from the table's indexes. */
static bool
doltgres_entries_doltgres_tuple_delete_removed(doltgres_entries_doltgres_tuple_delete_frame_t *frame,
											   const doltgres_entries_Ends *ends, uint64 tuple)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	DoltgresIndexRemoving rows = {.old_tuple = tuple};
	bool		taken;

	if (!pg_calls_make_slot(raise, frame->rel, &frame->removed, frame->error))
		return false;
	frame->removed_live = true;
	rows.old_slot = pg_OwnedSlot_slot(&frame->removed);
	rows.old_key.ptr = frame->old.key.ptr;
	rows.old_key.len = frame->old.key.len;
	if (!doltgres_entries_stored(raise, rows.old_slot, frame->rel, &frame->old, frame->error) ||
		!pg_calls_slot_getallattrs(raise, rows.old_slot, frame->error) ||
		!doltgres_entries_entries_removed(frame->caps, &frame->io, frame->rel, &rows, ends->writer, ends->roomed,
										  &taken, frame->error))
		return false;
	frame->removed_live = false;
	pg_OwnedSlot_drop(&frame->removed);
	return true;
}

/* doltgres_entries_doltgres_tuple_delete_body pushes the delete's version, waiting and failing as stopped says. */
static void
doltgres_entries_doltgres_tuple_delete_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_delete_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint64		tuple = pg_calls_tid_value(frame->tid);
	HostSnapshot crosscheck;
	const RepoRepo *repo;
	doltgres_entries_Ends ends;
	doltgres_entries_Changing changing = {.tuple = tuple, .mode = TXN_MEMTABLE_LOCK_MODE_UPDATE, .oper = XLTW_Delete,
	.waiting = frame->wait ? DOLTGRES_ENTRIES_WAITING_WAIT : DOLTGRES_ENTRIES_WAITING_ANSWER};
	doltgres_entries_doltgres_tuple_delete_captures_t captures = {.tuple = tuple, .options = frame->options};
	HostIoWaits waits;
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tuple};
	HostSnapshot any = pghost_any();
	StdVec		rows;

	if (frame->crosscheck != NULL && !pghost_snapshot(raise, frame->crosscheck, &crosscheck, frame->error))
		return;
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_ends(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->cid,
							   changing.mode, false, frame->crosscheck != NULL ? &crosscheck : NULL, &ends,
							   frame->error))
		return;
	waits = pghost_waits();
	if (!doltgres_entries_rows(frame->caps, frame->rel, NULL, &any, by, &rows, frame->error))
		return;
	frame->old_live = std_vec_pop(&rows, &frame->old, sizeof(DoltgresScanRow));
	std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	for (;;)
	{
		bool		has_ended;
		TxnChangeEnded ended;
		DoltgresChangesStop stop;
		bool		has_result;

		if (!doltgres_entries_pushed(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &ends, tuple,
									 false, 0, doltgres_entries_doltgres_tuple_delete_closure_1, &captures,
									 &has_ended, &ended, &stop, frame->error))
			return;
		if (has_ended && ended == TXN_CHANGE_ENDED_PUSHED)
		{
			if ((frame->old_live && !doltgres_entries_doltgres_tuple_delete_removed(frame, &ends, tuple)) ||
				!pg_calls_serial_conflict_in_tid(raise, frame->rel, tuple, frame->error) ||
				!pg_calls_count_deletes(raise, frame->rel, frame->error))
				return;
			*frame->result = DOLTGRES_CHANGES_TM_OK;
			frame->ok = true;
			return;
		}
		if (!doltgres_entries_stopped(raise, &frame->io, frame->rel, &changing, &waits, &frame->locked_live,
									  &frame->locked, has_ended ? &stop : NULL, frame->tmfd, &has_result,
									  frame->result, frame->error))
			return;
		if (has_result)
		{
			frame->ok = true;
			return;
		}
	}
}

/*
 * doltgres_entries_doltgres_tuple_delete is the fork's tuple_delete ((4b)'s
 * section 4): it ends the row a tid names with a delete version of its key,
 * of its row of a deferrable key, or of its copy of a keyless table, as
 * pushed pushes it, waiting and failing as stopped says, as heap's
 * heap_delete does; it then checks the serializable transactions' conflicts
 * in on its tuple and counts the delete.
 */
bool
doltgres_entries_doltgres_tuple_delete(const PgCatalogCaps *caps, const PgRel *rel, PgTid *tid, uint32 cid,
									   uint32 options, const PgSnapshot *snapshot, const PgSnapshot *crosscheck,
									   bool wait, PgFailure *tmfd, int32 *result, PgPgError *error)
{
	doltgres_entries_doltgres_tuple_delete_frame_t frame = {.caps = caps, .rel = rel, .tid = tid, .cid = cid,
		.options = options, .crosscheck = crosscheck, .wait = wait, .tmfd = tmfd, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_delete_body, doltgres_entries_doltgres_tuple_delete_pad);
	doltgres_entries_doltgres_tuple_delete_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_tid_row returns the row version a tuple id names under
 * SnapshotAny, as rows reads it, if any (has_result).
 */
static bool
doltgres_entries_tid_row(const PgCatalogCaps *caps, const PgRel *rel, uint64 tuple, bool *has_result,
						 DoltgresScanRow *result, PgPgError *error)
{
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tuple};
	HostSnapshot any = pghost_any();
	StdVec		rows;

	if (!doltgres_entries_rows(caps, rel, NULL, &any, by, &rows, error))
		return false;
	*has_result = std_vec_pop(&rows, result, sizeof(DoltgresScanRow));
	std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	return true;
}

/*
 * doltgres_entries_doltgres_tuple_lock_captures_t is tuple_lock's ops'
 * captures: the row's tuple id and the row lock.
 */
typedef struct doltgres_entries_doltgres_tuple_lock_captures_t
{
	uint64		tuple;
	TxnMemtableLockMode mode;
} doltgres_entries_doltgres_tuple_lock_captures_t;

/*
 * doltgres_entries_doltgres_tuple_lock_closure_2 is tuple_lock's op: a row
 * lock version, on the row for a keyless table or a deferrable key.
 */
static TxnMemtableOp
doltgres_entries_doltgres_tuple_lock_closure_2(void *captures, bool keyless, bool deferred)
{
	doltgres_entries_doltgres_tuple_lock_captures_t *c = captures;
	TxnMemtableOp op = {0};

	if (keyless || deferred)
	{
		op.tag = TXN_MEMTABLE_OP_LOCK_ROW;
		op.u.lock_row.f0.f0 = c->tuple;
		op.u.lock_row.f1 = c->mode;
	}
	else
	{
		op.tag = TXN_MEMTABLE_OP_LOCK;
		op.u.lock = c->mode;
	}
	return op;
}

/*
 * doltgres_entries_doltgres_tuple_lock_closure_3 is tuple_lock's following op:
 * a row lock version on a running update's new row.
 */
static TxnMemtableOp
doltgres_entries_doltgres_tuple_lock_closure_3(void *captures, bool keyless, bool deferred)
{
	doltgres_entries_doltgres_tuple_lock_captures_t *c = captures;
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_LOCK_ROW};

	op.u.lock_row.f0.f0 = c->tuple;
	op.u.lock_row.f1 = c->mode;
	return op;
}

/*
 * doltgres_entries_doltgres_tuple_lock_frame_t is tuple_lock's state across
 * its pad: what it opened, the row's tuple lock, and a row version read,
 * each while live.
 */
typedef struct doltgres_entries_doltgres_tuple_lock_frame_t
{
	doltgres_entries_opened_t opened;
	HostTupleGuard locked;
	bool		locked_live;
	DoltgresScanRow row;
	bool		row_live;
	doltgres_entries_Ends ends;
	doltgres_entries_Changing changing;
	HostIoWaits waits;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgTid	   *tid;
	PgSlot	   *slot;
	uint32		cid;
	int32		mode;
	int32		wait_policy;
	uint8		flags;
	PgFailure  *tmfd;
	int32	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_lock_frame_t;

/* doltgres_entries_doltgres_tuple_lock_pad frees what tuple_lock holds. */
static void
doltgres_entries_doltgres_tuple_lock_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_lock_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->locked_live)
		host_TupleGuard_drop(&frame->locked);
	frame->row_live = frame->locked_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_tuple_lock_stopped answers a stopped lock of a row as stopped does. */
static bool
doltgres_entries_doltgres_tuple_lock_stopped(doltgres_entries_doltgres_tuple_lock_frame_t *frame, uint64 tuple,
											 const DoltgresChangesStop *stop, bool *has_result, int32 *result)
{
	frame->changing.tuple = tuple;
	return doltgres_entries_stopped(pg_CatalogCaps_may_raise(frame->caps), &frame->io, frame->rel, &frame->changing,
									&frame->waits, &frame->locked_live, &frame->locked, stop, frame->tmfd, has_result,
									result, frame->error);
}

/*
 * doltgres_entries_doltgres_tuple_lock_followed follows an updated row to its
 * newest version, answering a result when the lock ends there (done) and
 * leaving the tuple at the version to lock again otherwise.
 */
static bool
doltgres_entries_doltgres_tuple_lock_followed(doltgres_entries_doltgres_tuple_lock_frame_t *frame,
											  const DoltgresChangesFailed *failed, uint64 *tuple, bool *done)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint32		prior = failed->xmax;

	*done = true;
	if (failed->ctid == DOLTGRES_CHANGES_MOVED_PARTITIONS && !pg_calls_refuse_moved_lock(raise, frame->error))
		return false;
	pg_calls_set_traversed(frame->tmfd, true);
	pg_calls_tid_set(frame->tid, failed->ctid);
	*tuple = failed->ctid;
	for (;;)
	{
		HostXid		writer;
		bool		has_next;
		std_TupleU64U64_t next;
		HostXid		ender;
		bool		in_progress;
		DoltgresChangesStop stop = {.tag = DOLTGRES_CHANGES_STOP_WAIT};
		bool		has_result;
		HostError	host;

		if (!doltgres_entries_tid_row(frame->caps, frame->rel, *tuple, &frame->row_live, &frame->row, frame->error))
			return false;
		if (!frame->row_live || (uint32) frame->row.stamp.txn != prior)
		{
			*frame->result = DOLTGRES_CHANGES_TM_DELETED;
			return true;
		}
		writer.f0 = frame->row.stamp.txn;
		if (!doltgres_entries_chained(frame->caps, frame->rel, *tuple, &has_next, &next, frame->error))
			return false;
		ender.f0 = has_next ? next.f1 : 0;
		if (has_next && host_is_current(HOST_IO_IO_IO_DOORS(&frame->io), ender))
		{
			if (next.f0 == *tuple)
			{
				*frame->result = DOLTGRES_CHANGES_TM_DELETED;
				return true;
			}
			pg_calls_tid_set(frame->tid, next.f0);
			*tuple = next.f0;
			prior = (uint32) next.f1;
			frame->row_live = false;
			doltgres_scan_Row_drop(&frame->row);
			continue;
		}
		if (host_is_current(HOST_IO_IO_IO_DOORS(&frame->io), writer))
		{
			if (frame->row.command >= frame->cid)
			{
				pg_calls_set_failure(frame->tmfd, *tuple, prior, frame->row.command);
				*frame->result = DOLTGRES_CHANGES_TM_SELF_MODIFIED;
				return true;
			}
			break;
		}
		if (!host_in_progress(HOST_IO_IO_IO_DOORS(&frame->io), writer, &in_progress, &host))
		{
			*frame->error = pghost_raised(&host);
			return false;
		}
		if (!in_progress)
			break;
		stop.u.wait = writer;
		if (!doltgres_entries_doltgres_tuple_lock_stopped(frame, *tuple, &stop, &has_result, frame->result))
			return false;
		if (has_result)
			return true;
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
	}
	frame->row_live = false;
	doltgres_scan_Row_drop(&frame->row);
	*done = false;
	return true;
}

/*
 * doltgres_entries_doltgres_tuple_lock_following locks the rows a running
 * update of a keyless table's row made, as heap_lock_updated_tuple does.
 */
static bool
doltgres_entries_doltgres_tuple_lock_following(doltgres_entries_doltgres_tuple_lock_frame_t *frame, uint64 at)
{
	doltgres_entries_doltgres_tuple_lock_captures_t captures = {.mode = frame->changing.mode};
	const RepoRepo *repo = doltgres_entries_opened_repo(&frame->opened);

	for (;;)
	{
		bool		has_next;
		std_TupleU64U64_t next;
		HostXid		writer;
		bool		in_progress;
		bool		has_ended;
		TxnChangeEnded ended;
		DoltgresChangesStop stop;
		bool		has_result;
		int32		result;
		HostError	host;

		if (!doltgres_entries_chained(frame->caps, frame->rel, at, &has_next, &next, frame->error))
			return false;
		if (!has_next)
			return true;
		writer.f0 = next.f1;
		if (next.f0 == at || host_is_current(HOST_IO_IO_IO_DOORS(&frame->io), writer))
			return true;
		if (!host_in_progress(HOST_IO_IO_IO_DOORS(&frame->io), writer, &in_progress, &host))
		{
			*frame->error = pghost_raised(&host);
			return false;
		}
		if (!in_progress)
			return true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(&frame->io), "doltgres-lock-following", &host))
		{
			*frame->error = pghost_raised(&host);
			return false;
		}
		captures.tuple = next.f0;
		if (!doltgres_entries_pushed(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &frame->ends,
									 next.f0, true, next.f1, doltgres_entries_doltgres_tuple_lock_closure_3,
									 &captures, &has_ended, &ended, &stop, frame->error))
			return false;
		if (has_ended && (ended == TXN_CHANGE_ENDED_PUSHED || stop.tag == DOLTGRES_CHANGES_STOP_HELD))
		{
			at = next.f0;
			continue;
		}
		if (!has_ended || stop.tag == DOLTGRES_CHANGES_STOP_FAILED)
			return true;
		if (!doltgres_entries_doltgres_tuple_lock_stopped(frame, next.f0, &stop, &has_result, &result))
			return false;
		if (has_result)
			return true;
	}
}

/*
 * doltgres_entries_doltgres_tuple_lock_body locks the row, following its
 * updates when asked, and stores it in the slot.
 */
static void
doltgres_entries_doltgres_tuple_lock_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_lock_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	doltgres_entries_doltgres_tuple_lock_captures_t captures;
	const RepoRepo *repo;
	uint64		tuple;

	pg_calls_set_traversed(frame->tmfd, false);
	if (frame->mode < 0 || frame->mode > TXN_MEMTABLE_LOCK_MODE_UPDATE)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a row lock asked for mode "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->mode, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return;
	}
	frame->changing.mode = (TxnMemtableLockMode) frame->mode;
	frame->changing.oper = XLTW_Lock;
	frame->changing.waiting = frame->wait_policy == LockWaitSkip ? DOLTGRES_ENTRIES_WAITING_SKIP :
		frame->wait_policy == LockWaitError ? DOLTGRES_ENTRIES_WAITING_REFUSE : DOLTGRES_ENTRIES_WAITING_WAIT;
	captures.mode = frame->changing.mode;
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_ends(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->cid,
							   frame->changing.mode, true, NULL, &frame->ends, frame->error))
		return;
	frame->waits = pghost_waits();
	tuple = pg_calls_tid_value(frame->tid);
	for (;;)
	{
		bool		has_ended;
		TxnChangeEnded ended;
		DoltgresChangesStop stop;
		bool		has_result;
		bool		done;

		captures.tuple = tuple;
		if (!doltgres_entries_pushed(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &frame->ends,
									 tuple, false, 0, doltgres_entries_doltgres_tuple_lock_closure_2, &captures,
									 &has_ended, &ended, &stop, frame->error))
			return;
		if (has_ended && (ended == TXN_CHANGE_ENDED_PUSHED || stop.tag == DOLTGRES_CHANGES_STOP_HELD))
		{
			*frame->result = DOLTGRES_CHANGES_TM_OK;
			break;
		}
		if (!has_ended || stop.tag != DOLTGRES_CHANGES_STOP_FAILED ||
			stop.u.failed.result != DOLTGRES_CHANGES_TM_UPDATED)
		{
			if (!doltgres_entries_doltgres_tuple_lock_stopped(frame, tuple, has_ended ? &stop : NULL, &has_result,
															  frame->result))
				return;
			if (has_result)
				break;
			continue;
		}
		if ((frame->flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) == 0 || stop.u.failed.ctid == tuple)
		{
			pg_calls_set_failure(frame->tmfd, stop.u.failed.ctid, stop.u.failed.xmax, stop.u.failed.cmax);
			*frame->result = stop.u.failed.ctid == tuple ? DOLTGRES_CHANGES_TM_DELETED : DOLTGRES_CHANGES_TM_UPDATED;
			break;
		}
		if (!doltgres_entries_doltgres_tuple_lock_followed(frame, &stop.u.failed, &tuple, &done))
			return;
		if (done)
		{
			frame->ok = true;
			return;
		}
	}
	if (*frame->result == DOLTGRES_CHANGES_TM_OK && frame->ends.keyless &&
		(frame->flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0 &&
		!doltgres_entries_doltgres_tuple_lock_following(frame, tuple))
		return;
	if (!doltgres_entries_tid_row(frame->caps, frame->rel, tuple, &frame->row_live, &frame->row, frame->error) ||
		(frame->row_live && !doltgres_entries_stored(raise, frame->slot, frame->rel, &frame->row, frame->error)))
		return;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_tuple_lock is the fork's tuple_lock ((4b)'s
 * section 4): it locks the row a tid names in a row lock mode with a row lock
 * version, a keyed table's on its key and a row-addressed one's on its row,
 * as pushed pushes it, as heap's heap_lock_tuple locks it: a lock the
 * transaction already holds as strong as the mode's is kept as it is, a
 * holder in a conflicting mode is waited for, skipped, or refused by the
 * wait policy, as stopped says, and a row a committed transaction updated is
 * followed to its newest version when the caller asks, as heapam_tuple_lock
 * follows it with SnapshotDirty, the failure data marked traversed. The row
 * it locked, or the one it stopped at, is stored in the slot.
 */
bool
doltgres_entries_doltgres_tuple_lock(const PgCatalogCaps *caps, const PgRel *rel, PgTid *tid,
									 const PgSnapshot *snapshot, PgSlot *slot, uint32 cid, int32 mode,
									 int32 wait_policy, uint8 flags, PgFailure *tmfd, int32 *result, PgPgError *error)
{
	doltgres_entries_doltgres_tuple_lock_frame_t frame = {.caps = caps, .rel = rel, .tid = tid, .slot = slot,
		.cid = cid, .mode = mode, .wait_policy = wait_policy, .flags = flags, .tmfd = tmfd, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_lock_body, doltgres_entries_doltgres_tuple_lock_pad);
	doltgres_entries_doltgres_tuple_lock_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_again_frame_t is again's state across its pad: the row it
 * copies, the copy, and how many of its parts are cloned.
 */
typedef struct doltgres_entries_again_frame_t
{
	const TxnInsertInsert *row;
	TxnInsertInsert *result;
	int			cloned;
} doltgres_entries_again_frame_t;

/* doltgres_entries_again_pad frees the parts of the copy cloned so far, on an unwind. */
static void
doltgres_entries_again_pad(void *frame_)
{
	doltgres_entries_again_frame_t *frame = frame_;
	TxnInsertInsert *result = frame->result;

	if (frame->cloned > 3)
		std_vec_free_Checked(&result->put.values, sizeof(uint32), _Alignof(uint32));
	if (frame->cloned > 2)
		std_vec_free_Checked(&result->put.addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	if (frame->cloned > 1)
		std_vec_free_Checked(&result->put.value, 1, 1);
	if (frame->cloned > 0)
		std_vec_free_Checked(&result->key, 1, 1);
	frame->cloned = 0;
}

/* doltgres_entries_again_body clones the row's key, its put's bytes, and its kept places. */
static void
doltgres_entries_again_body(void *frame_)
{
	doltgres_entries_again_frame_t *frame = frame_;
	const TxnInsertInsert *row = frame->row;
	TxnInsertInsert *result = frame->result;

	*result = *row;
	std_vec_clone(&result->key, &row->key, 1, 1);
	frame->cloned++;
	std_vec_clone(&result->put.value, &row->put.value, 1, 1);
	frame->cloned++;
	std_vec_clone(&result->put.addresses, &row->put.addresses, sizeof(chunks_TupleU32Locator_t),
				  _Alignof(chunks_TupleU32Locator_t));
	frame->cloned++;
	std_vec_clone(&result->put.values, &row->put.values, sizeof(uint32), _Alignof(uint32));
	frame->cloned++;
	std_vec_clone(&result->kept, &row->kept, sizeof(size_t), _Alignof(size_t));
	frame->cloned = 0;
}

/*
 * doltgres_entries_again returns a copy of an update's new row for one try,
 * which moves the row into the push it writes, since a try that waits or
 * finds its row moved leaves the next try to write it again.
 */
static void
doltgres_entries_again(const TxnInsertInsert *row, TxnInsertInsert *result)
{
	doltgres_entries_again_frame_t frame = {.row = row, .result = result};

	std_landing(&frame, doltgres_entries_again_body, doltgres_entries_again_pad);
	doltgres_entries_again_pad(&frame);
}

/* state.rs' held at Option<Inserted<Stop>>, which tuple_update takes. */
#define TXN_STATE_TS OptionInserted
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnInsertInserted *result, DoltgresChangesStop *stopped, \
	HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnInsertInserted *result; DoltgresChangesStop *stopped;
#define TXN_STATE_RESULT_INIT , some, result, stopped
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result, (c)->stopped
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_tuple_update_captures_t is the update's lent
 * calls' captures: the reader, the relation, the old row's tuple id, the
 * written row, its row lock and command, the prune and crosscheck
 * snapshots, what the change asks, the base's store, and the held state.
 */
typedef struct doltgres_entries_doltgres_tuple_update_captures_t
{
	const HostSharedReader *reading;
	const PgRel *rel;
	uint64		tuple;
	const doltgres_entries_Written *written;
	TxnMemtableLockMode mode;
	uint32		cid;
	const TxnVacuumPrune *prune;
	const HostSnapshot *crosscheck;
	DoltgresChangesAsked asked;
	TreesStoreReader store;
	const HostSharedHeld *held;
} doltgres_entries_doltgres_tuple_update_captures_t;

/*
 * doltgres_entries_doltgres_tuple_update_closure_1_3_0 is the update's based:
 * whether the newest base holds the old row.
 */
static bool
doltgres_entries_doltgres_tuple_update_closure_1_3_0(void *captures, HostIoIo *io, bool *result, HostError *error)
{
	doltgres_entries_doltgres_tuple_update_captures_t *c = captures;

	return doltgres_scan_based(io, TREES_STORE_READER_NODE_STORE(&c->store), c->held, c->tuple, result, error);
}

/* doltgres_entries_doltgres_tuple_update_closure_1_3 is the update's decide: its read of the old row's versions. */
static bool
doltgres_entries_doltgres_tuple_update_closure_1_3(void *captures, HostIoIo *io, TxnMemtableVersionVersion *versions,
												   size_t versions_len, TxnChangeRead *result, void *stopped,
												   HostError *error)
{
	doltgres_entries_doltgres_tuple_update_captures_t *c = captures;

	return doltgres_changes_decided(io, versions, versions_len, &c->asked,
									doltgres_entries_doltgres_tuple_update_closure_1_3_0, c, result, stopped, error);
}

/*
 * doltgres_entries_doltgres_tuple_update_closure_1_frame_t is the update's
 * lent call's state across its pad: what the update replaces, while live,
 * and the try's copy of the new row, which the update takes.
 */
typedef struct doltgres_entries_doltgres_tuple_update_closure_1_frame_t
{
	TxnInsertReplaces replaces;
	bool		replaces_live;
	TxnInsertInsert row;
	doltgres_entries_doltgres_tuple_update_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	bool	   *some;
	TxnInsertInserted *result;
	DoltgresChangesStop *stopped;
	HostError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_update_closure_1_frame_t;

/* doltgres_entries_doltgres_tuple_update_closure_1_pad frees what the update's lent call holds. */
static void
doltgres_entries_doltgres_tuple_update_closure_1_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_update_closure_1_frame_t *frame = frame_;

	if (frame->replaces_live)
		std_vec_free_Checked(&frame->replaces.key, 1, 1);
	frame->replaces_live = false;
}

/*
 * doltgres_entries_doltgres_tuple_update_closure_1_body finds the old row's
 * key and writes the update under its stripe.
 */
static void
doltgres_entries_doltgres_tuple_update_closure_1_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_update_closure_1_frame_t *frame = frame_;
	doltgres_entries_doltgres_tuple_update_captures_t *c = frame->c;
	const doltgres_entries_Written *w = c->written;
	HostStorage storage = doltgres_entries_storage(c->rel);
	bool		has_key;
	bool		keyless;
	TxnInsertKeyed keyed = {.stripe = w->stripe};
	HostIoWaits waits;

	c->store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	c->held = frame->held;
	if (!doltgres_scan_keyed(frame->io, TREES_STORE_READER_NODE_STORE(&c->store), frame->held, c->tuple, &has_key,
							 &frame->replaces.key, &keyless, frame->error))
		return;
	*frame->some = has_key;
	if (!has_key)
	{
		frame->ok = true;
		return;
	}
	frame->replaces_live = true;
	frame->replaces.stripe = txn_change_stripe(&storage, frame->replaces.key.ptr, frame->replaces.key.len,
											   w->by_bytes);
	frame->replaces.tuple.f0 = c->tuple;
	frame->replaces.lock = c->mode;
	if (w->has_order)
	{
		keyed.has_unique = true;
		keyed.unique.order = &w->order;
		keyed.unique.index = w->index;
		keyed.unique.has_equal = doltgres_layout_equal(&w->order, &keyed.unique.equal);
	}
	waits = pghost_waits();
	c->asked.tuple = c->tuple;
	c->asked.command = c->cid;
	c->asked.mode = c->mode;
	c->asked.crosscheck = c->crosscheck;
	doltgres_entries_again(&w->row, &frame->row);
	frame->replaces_live = false;
	frame->ok = txn_insert_update(frame->io, TREES_STORE_READER_NODE_STORE(&c->store), frame->held, &frame->row,
								  &frame->replaces, &w->room, c->prune, w->has_toasted ? &w->toasted.state : NULL,
								  w->has_toasted ? w->toasted.entries.ptr : NULL,
								  w->has_toasted ? w->toasted.entries.len : 0, keyed, &waits,
								  doltgres_entries_doltgres_tuple_update_closure_1_3, c, NULL, frame->stopped,
								  frame->result, frame->error);
}

/* doltgres_entries_doltgres_tuple_update_closure_1 is the update's lent call: the new row written over the old one. */
static bool
doltgres_entries_doltgres_tuple_update_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
												 const HostSharedHeld *held, bool *some, TxnInsertInserted *result,
												 DoltgresChangesStop *stopped, HostError *error)
{
	doltgres_entries_doltgres_tuple_update_closure_1_frame_t frame = {.c = captures, .io = io, .repo = repo,
	.held = held, .some = some, .result = result, .stopped = stopped, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_update_closure_1_body,
				doltgres_entries_doltgres_tuple_update_closure_1_pad);
	doltgres_entries_doltgres_tuple_update_closure_1_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_tuple_update_frame_t is the update's state across
 * its pad: what it opened, the old row and its slot, the written row's
 * parts, the NonVacuumable snapshot, and the row's tuple lock, each while
 * live.
 */
typedef struct doltgres_entries_doltgres_tuple_update_frame_t
{
	doltgres_entries_opened_t opened;
	DoltgresScanRow old;
	bool		old_live;
	PgOwnedSlot replaced;
	bool		replaced_live;
	doltgres_entries_Written written;
	bool		row_live;
	bool		toasted_live;
	bool		order_live;
	PgOwnedSnapshot owned;
	bool		owned_live;
	HostTupleGuard locked;
	bool		locked_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgTid	   *otid;
	PgSlot	   *slot;
	uint32		cid;
	const PgSnapshot *crosscheck;
	bool		wait;
	PgFailure  *tmfd;
	PgLockModeOut *lockmode;
	PgUpdateIndexesOut *update_indexes;
	int32	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_tuple_update_frame_t;

/* doltgres_entries_doltgres_tuple_update_pad frees what the update holds. */
static void
doltgres_entries_doltgres_tuple_update_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_update_frame_t *frame = frame_;

	if (frame->locked_live)
		host_TupleGuard_drop(&frame->locked);
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->written.order);
	if (frame->toasted_live)
		doltgres_entries_Toasted_drop(&frame->written.toasted);
	if (frame->row_live)
		doltgres_entries_Insert_drop(&frame->written.row);
	if (frame->replaced_live)
		pg_OwnedSlot_drop(&frame->replaced);
	if (frame->old_live)
		doltgres_scan_Row_drop(&frame->old);
	frame->locked_live = frame->owned_live = frame->order_live = frame->toasted_live = frame->row_live = false;
	frame->replaced_live = frame->old_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_tuple_update_took removes the old row's entries that change and finishes the update. */
static bool
doltgres_entries_doltgres_tuple_update_took(doltgres_entries_doltgres_tuple_update_frame_t *frame, uint64 tuple,
											uint64 new, uint64 locker, TxnMemtableVersionWriter writer,
											TxnStatusRoomed roomed)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	DoltgresIndexRemoving rows = {.old_slot = pg_OwnedSlot_slot(&frame->replaced), .old_tuple = tuple,
		.old_key = {frame->old.key.ptr, frame->old.key.len}, .has_new = true, .new_slot = frame->slot,
	.new_tuple = new, .new_key = {frame->written.row.key.ptr, frame->written.row.key.len}};
	bool		taken;

	if (!doltgres_entries_entries_removed(frame->caps, &frame->io, frame->rel, &rows, writer, roomed, &taken,
										  frame->error))
		return false;
	pg_calls_slot_set_tid(frame->slot, new, pg_calls_rel_id(frame->rel));
	pg_calls_slot_set_written(frame->slot, writer.xid, writer.command, locker);
	pg_calls_set_update_indexes(frame->update_indexes, taken ? TU_All : TU_None);
	if (!pg_calls_serial_conflict_in_tid(raise, frame->rel, tuple, frame->error) ||
		!pg_calls_count_updates(raise, frame->rel, new >> 16 != tuple >> 16, frame->error))
		return false;
	*frame->result = DOLTGRES_CHANGES_TM_OK;
	return true;
}

/* doltgres_entries_doltgres_tuple_update_body writes the update, waiting and failing as stopped says. */
static void
doltgres_entries_doltgres_tuple_update_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_update_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	uint64		tuple = pg_calls_tid_value(frame->otid);
	HostSnapshot crosscheck;
	PgSlot	   *replaced;
	bool		changed;
	const RepoRepo *repo;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	HostSnapshot horizon;
	HostSnapshot any = pghost_any();
	TxnVacuumPrune prune = {&horizon, &any};
	doltgres_entries_doltgres_tuple_update_captures_t captures = {.reading = &frame->opened.reading,
	.rel = frame->rel, .tuple = tuple, .written = &frame->written, .cid = frame->cid, .prune = &prune};
	doltgres_entries_Changing changing = {.tuple = tuple, .oper = XLTW_Update,
	.waiting = frame->wait ? DOLTGRES_ENTRIES_WAITING_WAIT : DOLTGRES_ENTRIES_WAITING_ANSWER};
	HostIoWaits waits;
	HostError	failed;

	if (frame->crosscheck != NULL && !pghost_snapshot(raise, frame->crosscheck, &crosscheck, frame->error))
		return;
	captures.crosscheck = frame->crosscheck != NULL ? &crosscheck : NULL;
	if (!doltgres_entries_tid_row(frame->caps, frame->rel, tuple, &frame->old_live, &frame->old, frame->error))
		return;
	if (!frame->old_live)
	{
		*frame->result = DOLTGRES_CHANGES_TM_INVISIBLE;
		frame->ok = pg_calls_refuse_invisible(raise, XLTW_Update, frame->error);
		return;
	}
	if (!pg_calls_make_slot(raise, frame->rel, &frame->replaced, frame->error))
		return;
	frame->replaced_live = true;
	replaced = pg_OwnedSlot_slot(&frame->replaced);
	if (!doltgres_entries_stored(raise, replaced, frame->rel, &frame->old, frame->error) ||
		!pg_calls_slot_getallattrs(raise, replaced, frame->error) ||
		!pg_calls_slot_getallattrs(raise, frame->slot, frame->error) ||
		!pg_calls_key_columns_changed(raise, catalog, frame->rel, replaced, frame->slot, &changed, frame->error))
		return;
	changing.mode = captures.mode = changed ? TXN_MEMTABLE_LOCK_MODE_UPDATE : TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
	pg_calls_set_lock_mode(frame->lockmode, (int32) changing.mode);
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_current(&frame->io, frame->cid, &writer, &roomed, frame->error) ||
		!doltgres_entries_written(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, frame->slot,
								  replaced, writer, true, roomed, &frame->written, frame->error))
		return;
	frame->row_live = true;
	frame->toasted_live = frame->written.has_toasted;
	frame->order_live = frame->written.has_order;
	if (!pg_OwnedSnapshot_nonvacuumable(raise, frame->rel, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &horizon, frame->error))
		return;
	waits = pghost_waits();
	if (!host_pause(HOST_SHARED_IO_PAUSES(&frame->io), "doltgres-update-reading", &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	for (;;)
	{
		TxnStateFound made;
		bool		has_updated;
		TxnInsertInserted updated;
		DoltgresChangesStop stop;
		bool		has_result;

		if (!doltgres_entries_making(frame->caps, &frame->io, frame->rel, &made, frame->error))
			return;
		if (!txn_state_held_OptionInserted(&frame->io, repo, &frame->opened.reading, made,
										   doltgres_entries_doltgres_tuple_update_closure_1, &captures, NULL,
										   &has_updated, &updated, &stop, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (has_updated && updated.tag != TXN_INSERT_INSERTED_STOPPED)
		{
			uint64		new = updated.tag == TXN_INSERT_INSERTED_TOOK ? updated.u.took.tuple : updated.u.exists.spent;

			if (new >> 16 != tuple >> 16 &&
				!pg_calls_set_target_block(raise, frame->rel, (uint32) (new >> 16), frame->error))
				return;
			if (updated.tag == TXN_INSERT_INSERTED_TOOK)
				frame->ok = doltgres_entries_doltgres_tuple_update_took(frame, tuple, new,
																		updated.u.took.has_carried ? updated.u.took.carried : 0,
																		writer, roomed);
			else
			{
				*frame->result = DOLTGRES_CHANGES_TM_OK;
				frame->ok = pg_calls_refuse_duplicate_key(raise, catalog, frame->rel, frame->slot, frame->error);
			}
			return;
		}
		if (!doltgres_entries_stopped(raise, &frame->io, frame->rel, &changing, &waits, &frame->locked_live,
									  &frame->locked, has_updated ? &stop : NULL, frame->tmfd, &has_result,
									  frame->result, frame->error))
			return;
		if (has_result)
		{
			frame->ok = true;
			return;
		}
	}
}

/*
 * doltgres_entries_doltgres_tuple_update is the fork's tuple_update ((4b)'s
 * section 4): it writes the row a slot holds, as written forms it, as the
 * update of the row a tid names, which insert's update writes once changes'
 * decided read the old row, waiting and failing as stopped says, as heap's
 * heap_update does. Its row lock is an update's when it changes a column a
 * unique index or a foreign key may name, and a no-key update's otherwise,
 * as heap's HeapDetermineColumnsInfo compares them with the old row, which
 * the executor fetched under SnapshotAny and this entry reads the same way;
 * and the old row's entries that change are removed (entries_removed), and
 * the indexes take the new row unless no entry changed and every index is
 * the fork's. It then checks the serializable transactions' conflicts in on
 * the old row's tuple and counts the update, on another page or on the old
 * row's.
 */
bool
doltgres_entries_doltgres_tuple_update(const PgCatalogCaps *caps, const PgRel *rel, PgTid *otid, PgSlot *slot,
									   uint32 cid, uint32 options, const PgSnapshot *snapshot,
									   const PgSnapshot *crosscheck, bool wait, PgFailure *tmfd,
									   PgLockModeOut *lockmode, PgUpdateIndexesOut *update_indexes, int32 *result,
									   PgPgError *error)
{
	doltgres_entries_doltgres_tuple_update_frame_t frame = {.caps = caps, .rel = rel, .otid = otid, .slot = slot,
		.cid = cid, .crosscheck = crosscheck, .wait = wait, .tmfd = tmfd, .lockmode = lockmode,
	.update_indexes = update_indexes, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_update_body, doltgres_entries_doltgres_tuple_update_pad);
	doltgres_entries_doltgres_tuple_update_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_locked_frame_t is a locked operation's state across its
 * pad: the backend's value and its guard, each while live.
 */
typedef struct doltgres_entries_locked_frame_t
{
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	HostLocalBackendLocal *of;
	const StdDefaultType *type;
	const HostLockType *lock_type;
	HostIoIo   *io;
	DoltgresEntriesLockedOp op;
	void	   *captures;
} doltgres_entries_locked_frame_t;

/* doltgres_entries_locked_pad releases what a locked operation holds: its guard, then the backend's value. */
static void
doltgres_entries_locked_pad(void *frame_)
{
	doltgres_entries_locked_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, frame->type);
	frame->guard_live = frame->local_live = false;
}

/* doltgres_entries_locked_body takes the backend's value and its lock, then runs the operation. */
static void
doltgres_entries_locked_body(void *frame_)
{
	doltgres_entries_locked_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(frame->of, frame->type);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, frame->type);
	frame->guard = host_lock_IoMutex_lock(mutex, frame->lock_type, &frame->io->held);
	frame->guard_live = true;
	frame->op(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), frame->captures);
}

/*
 * doltgres_entries_locked runs an operation on a backend's value under its
 * IoMutex, as LOCAL.get().lock(io) holds it, releasing the guard and then
 * the value.
 */
void
doltgres_entries_locked(HostIoIo *io, HostLocalBackendLocal *of, const StdDefaultType *type,
						const HostLockType *lock_type, DoltgresEntriesLockedOp op, void *captures)
{
	doltgres_entries_locked_frame_t frame = {.of = of, .type = type, .lock_type = lock_type, .io = io, .op = op,
	.captures = captures};

	std_landing(&frame, doltgres_entries_locked_body, doltgres_entries_locked_pad);
	doltgres_entries_locked_pad(&frame);
}

/* doltgres_entries_speculated_clear_op drops every speculative insert in the backend's list. */
static void
doltgres_entries_speculated_clear_op(void *value, void *captures)
{
	std_vec_clear(value, sizeof(DoltgresEntriesSpeculated), doltgres_entries_Speculated_drop);
}

/*
 * doltgres_entries_speculated_clear clears SPECULATED under its lock, as a
 * transaction's end clears the backend's speculative inserts
 * (SPECULATED.get().lock(io).clear(), which entries.c's xact_ended runs).
 */
void
doltgres_entries_speculated_clear(HostIoIo *io)
{
	doltgres_entries_locked(io, &DOLTGRES_ENTRIES_SPECULATED.local, &DOLTGRES_ENTRIES_SPECULATED_TYPE,
							&DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE, doltgres_entries_speculated_clear_op, NULL);
}

/*
 * doltgres_entries_Building is Building: the row an index build's scan names
 * to its callback: its tuple id, its key, its writer's xid, and its writer's
 * top-level xid, zero for a base row's.
 */
typedef struct doltgres_entries_Building
{
	uint64		f0;
	StdVec		f1;
	uint64		f2;
	uint64		f3;
} doltgres_entries_Building;

/* doltgres_entries_OptionBuilding is an Option<Building>, BUILDING's value. */
typedef struct doltgres_entries_OptionBuilding
{
	bool		has;
	doltgres_entries_Building building;
} doltgres_entries_OptionBuilding;

/* doltgres_entries_OptionBuilding_Default_default is Option<Building>'s Default: none. */
static void
doltgres_entries_OptionBuilding_Default_default(void *place)
{
	((doltgres_entries_OptionBuilding *) place)->has = false;
}

/* doltgres_entries_OptionBuilding_drop is Option<Building>'s drop: its key's, when it has one. */
static void
doltgres_entries_OptionBuilding_drop(void *place)
{
	doltgres_entries_OptionBuilding *self = place;

	if (self->has)
		std_vec_free_Checked(&self->building.f1, 1, 1);
}

/* taken at Option<Building> and IoMutex<Option<Building>>, which BUILDING is. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS OptionBuilding
#define HOST_LOCK_TAKEN_T doltgres_entries_OptionBuilding
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_entries_OptionBuilding_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_OptionBuilding_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_BUILDING_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS OptionBuilding
#define HOST_LOCK_T doltgres_entries_OptionBuilding
#define HOST_LOCK_DEFAULT(place) doltgres_entries_OptionBuilding_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_entries_IoMutexOptionBuilding_drop is IoMutex<Option<Building>>' drop glue: its value's. */
static void
doltgres_entries_IoMutexOptionBuilding_drop(void *value)
{
	doltgres_entries_OptionBuilding_drop(&((HostLockIoMutexOptionBuilding *) value)->value);
}

/* DOLTGRES_ENTRIES_BUILDING_TYPE is IoMutex<Option<Building>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_BUILDING_TYPE = {sizeof(HostLockIoMutexOptionBuilding),
	_Alignof(HostLockIoMutexOptionBuilding), host_lock_IoMutexOptionBuilding_Default_default,
doltgres_entries_IoMutexOptionBuilding_drop};

/*
 * DOLTGRES_ENTRIES_BUILDING is BUILDING: the row an index build's scan hands
 * its callback, which the fork's own build reads beside the callback's
 * values (the btree text's section 6), as the index fetch names a row's key;
 * its BackendLocal header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexOptionBuilding process;
}			DOLTGRES_ENTRIES_BUILDING = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * doltgres_entries_building_replaced_t is a replace of BUILDING's captures:
 * the row's tuple id and the row, whose key it takes.
 */
typedef struct doltgres_entries_building_replaced_t
{
	uint64		tuple;
	DoltgresScanRow *row;
} doltgres_entries_building_replaced_t;

/* doltgres_entries_building_replaced_op puts a row in BUILDING, moving the row's key out, and drops the one there. */
static void
doltgres_entries_building_replaced_op(void *value, void *captures)
{
	doltgres_entries_OptionBuilding *self = value;
	doltgres_entries_building_replaced_t *c = captures;
	doltgres_entries_OptionBuilding old = *self;

	self->has = true;
	self->building.f0 = c->tuple;
	self->building.f1 = c->row->key;
	self->building.f2 = c->row->stamp.txn;
	self->building.f3 = c->row->top;
	std_vec_new(&c->row->key, 1, 1);
	doltgres_entries_OptionBuilding_drop(&old);
}

/* doltgres_entries_building_taken_op takes BUILDING's row into its captures, leaving none. */
static void
doltgres_entries_building_taken_op(void *value, void *captures)
{
	doltgres_entries_OptionBuilding *self = value;

	*(doltgres_entries_OptionBuilding *) captures = *self;
	self->has = false;
}

/*
 * DoltgresEntriesBuiltTag is Built's variants: how an index build takes a row
 * version, as heap's heapam_index_build_range_scan takes a tuple by
 * HeapTupleSatisfiesVacuum: an entry and whether the row is alive, none, or
 * none with a recently dead version that an older snapshot may still see
 * (the btree text's B2b), or a wait on a running writer or ender.
 */
typedef enum DoltgresEntriesBuiltTag
{
	DOLTGRES_ENTRIES_BUILT_INDEX,
	DOLTGRES_ENTRIES_BUILT_SKIP,
	DOLTGRES_ENTRIES_BUILT_RECENT,
	DOLTGRES_ENTRIES_BUILT_WAIT,
} DoltgresEntriesBuiltTag;

/* doltgres_entries_Built is Built: its variant, an entry's aliveness, and a wait's xid. */
typedef struct doltgres_entries_Built
{
	DoltgresEntriesBuiltTag tag;
	bool		alive;
	uint64		wait;
} doltgres_entries_Built;

/*
 * doltgres_entries_built classes a row version for an index build (the btree
 * text's B2a and B2b): a version an aborted transaction wrote is skipped; one
 * live by its writer's and its ender's outcomes, or the builder's own, is
 * indexed and alive; one a committed transaction ended is skipped when every
 * snapshot sees it ended (removable_in the build's horizon), and otherwise
 * recent; one the builder's own transaction ended is indexed, not alive, as
 * heap indexes its own delete in progress, since a subtransaction's abort
 * can bring it back; and one another transaction is writing or ending is
 * waited for, or with anyvisible indexed, as heap's BRIN summarization takes
 * it.
 */
static bool
doltgres_entries_built(HostIoIo *io, const DoltgresScanRow *row, const HostSnapshot *horizon, bool anyvisible,
					   doltgres_entries_Built *result, HostError *error)
{
	HostStatus	writer;
	HostStatus	changer;
	HostXid		xid = {row->stamp.txn};
	bool		removable;

	result->tag = DOLTGRES_ENTRIES_BUILT_INDEX;
	result->alive = true;
	if (!txn_outcome_stamped(io, row->stamp.txn, &writer, error))
		return false;
	if (writer == HOST_STATUS_ABORTED)
	{
		result->tag = DOLTGRES_ENTRIES_BUILT_SKIP;
		return true;
	}
	if (writer == HOST_STATUS_IN_PROGRESS && !host_is_current(HOST_IO_IO_IO_DOORS(io), xid))
	{
		if (!anyvisible)
		{
			result->tag = DOLTGRES_ENTRIES_BUILT_WAIT;
			result->wait = row->stamp.txn;
		}
		return true;
	}
	if (!row->has_changer || row->changer.locks)
		return true;
	xid.f0 = row->changer.xid;
	if (!txn_outcome_stamped(io, row->changer.xid, &changer, error))
		return false;
	if (changer == HOST_STATUS_ABORTED)
		return true;
	if (changer == HOST_STATUS_IN_PROGRESS)
	{
		if (host_is_current(HOST_IO_IO_IO_DOORS(io), xid))
			result->alive = false;
		else if (!anyvisible)
		{
			result->tag = DOLTGRES_ENTRIES_BUILT_WAIT;
			result->wait = row->changer.xid;
		}
		return true;
	}
	if (!host_removable_in(HOST_IO_IO_IO_DOORS(io), horizon, xid, &removable, error))
		return false;
	result->tag = removable ? DOLTGRES_ENTRIES_BUILT_SKIP : DOLTGRES_ENTRIES_BUILT_RECENT;
	return true;
}

/*
 * doltgres_entries_Scanned is Scanned: what an index build's scan reads from:
 * its own scan's handle (has_own), or a parallel build's scan (NULL for
 * none), the snapshot that sees every row version, and the build's horizon.
 */
typedef struct doltgres_entries_Scanned
{
	bool		has_own;
	HostOwnedHandle own;
	PgScan	   *scan;
	const HostSnapshot *any;
	const HostSnapshot *horizon;
} doltgres_entries_Scanned;

/*
 * doltgres_entries_build_read_captures_t is build_read's lent call's captures:
 * the reader, the snapshot, and the scan's handle.
 */
typedef struct doltgres_entries_build_read_captures_t
{
	const HostSharedReader *reading;
	const HostSnapshot *any;
	HostOwnedHandle handle;
} doltgres_entries_build_read_captures_t;

/* doltgres_entries_build_read_closure_0 is build_read's lent call: the own scan's next row in its tid range. */
static bool
doltgres_entries_build_read_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *some, DoltgresScanRow *result, HostError *error)
{
	doltgres_entries_build_read_captures_t *c = captures;
	DoltgresScanUnder under = {.state = host_shared_Held_record(held), .snapshot = c->any};
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	return doltgres_scan_next_in_range(io, TREES_STORE_READER_NODE_STORE(&store), &under, c->handle,
									   DOLTGRES_SCAN_DIRECTION_FORWARD, some, result, error);
}

/*
 * doltgres_entries_build_read returns the next row version an index build's
 * scan reads, from its own scan's tid range or the parallel build's, which
 * takes the shared scan's next unit when one ends.
 */
static bool
doltgres_entries_build_read(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							const HostSharedReader *reading, const PgRel *table, doltgres_entries_Scanned *scanned,
							bool *has_result, DoltgresScanRow *result, PgPgError *error)
{
	*has_result = false;
	if (scanned->has_own)
	{
		doltgres_entries_build_read_captures_t captures = {reading, scanned->any, scanned->own};
		TxnStateFound found;
		HostError	failed;

		if (!doltgres_entries_making(caps, io, table, &found, error))
			return false;
		if (txn_state_held_OptionRow(io, repo, reading, found, doltgres_entries_build_read_closure_0, &captures, NULL,
									 has_result, result, &failed))
			return true;
		*error = pghost_raised(&failed);
		return false;
	}
	while (scanned->scan != NULL)
	{
		HostOwnedHandle owned = doltgres_entries_handle(scanned->scan);
		bool		more;

		if (!doltgres_entries_serial(caps, io, repo, reading, scanned->scan, DOLTGRES_SCAN_DIRECTION_FORWARD,
									 DOLTGRES_ENTRIES_READING_ALL, true, has_result, result, error))
			return false;
		if (*has_result)
			return true;
		if (!doltgres_entries_next_unit(pg_CatalogCaps_may_raise(caps), io, scanned->scan, owned,
										DOLTGRES_ENTRIES_READING_ALL, &more, error))
			return false;
		if (!more)
			return true;
	}
	return true;
}

/*
 * doltgres_entries_build_scanned_frame_t is build_scanned's state across its
 * pad: the row version it reads, while live.
 */
typedef struct doltgres_entries_build_scanned_frame_t
{
	DoltgresScanRow row;
	bool		row_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *table;
	const PgRel *index;
	PgIndexInfo *info;
	doltgres_entries_Scanned *scanned;
	uint32		start;
	uint32		numblocks;
	bool		anyvisible;
	bool		progress;
	PgOwnedBuild *build;
	PgOwnedSlot *slot;
	PgBuildCallback callback;
	PgBuildState state;
	float8	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_build_scanned_frame_t;

/* doltgres_entries_build_scanned_pad frees what build_scanned holds. */
static void
doltgres_entries_build_scanned_pad(void *frame_)
{
	doltgres_entries_build_scanned_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->row_live = false;
}

/*
 * doltgres_entries_build_scanned_alive classes the frame's row, waiting on a
 * running writer or ender and reading it again.
 */
static bool
doltgres_entries_build_scanned_alive(doltgres_entries_build_scanned_frame_t *frame, HostIoWaits *waits,
									 doltgres_entries_Built *result)
{
	uint64		tuple = frame->row.stamp.tuple;

	for (;;)
	{
		HostXid		writer;
		HostOperation none = {0};
		bool		waited;
		HostError	failed;

		if (!doltgres_entries_built(frame->io, &frame->row, frame->scanned->horizon, frame->anyvisible, result,
									&failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		if (result->tag == DOLTGRES_ENTRIES_BUILT_RECENT)
			pg_calls_index_info_broken_chain(frame->info);
		if (result->tag != DOLTGRES_ENTRIES_BUILT_WAIT)
			return true;
		writer.f0 = result->wait;
		if (!host_xact_wait(waits, HOST_IO_IO_IO_DOORS(frame->io), writer, false, none, true, &waited, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
		if (!doltgres_entries_tid_row(frame->caps, frame->table, tuple, &frame->row_live, &frame->row, frame->error))
			return false;
		if (!frame->row_live)
		{
			result->tag = DOLTGRES_ENTRIES_BUILT_SKIP;
			return true;
		}
	}
}

/* doltgres_entries_build_scanned_body hands each indexed row its scan reads in range to the build's callback. */
static void
doltgres_entries_build_scanned_body(void *frame_)
{
	doltgres_entries_build_scanned_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint64		end = frame->numblocks == InvalidBlockNumber ? PG_UINT64_MAX :
		((uint64) frame->start + frame->numblocks) << 16;
	HostIoWaits waits = pghost_waits();
	float8		live = 0.0;
	uint64		done = 0;

	for (;;)
	{
		uint64		tuple;
		doltgres_entries_Built built;
		bool		taken;
		doltgres_entries_building_replaced_t building = {.row = &frame->row};

		if (!doltgres_entries_build_read(frame->caps, frame->io, frame->repo, frame->reading, frame->table,
										 frame->scanned, &frame->row_live, &frame->row, frame->error))
			return;
		if (!frame->row_live)
			break;
		tuple = frame->row.stamp.tuple;
		if (tuple >= (uint64) frame->start << 16 && tuple < end)
		{
			if (frame->progress && tuple >> 16 > done)
			{
				done = tuple >> 16;
				pg_calls_progress_update(PROGRESS_SCAN_BLOCKS_DONE, done > PG_INT64_MAX ? PG_INT64_MAX : (int64) done);
			}
			if (!doltgres_entries_build_scanned_alive(frame, &waits, &built))
				return;
			if (built.tag == DOLTGRES_ENTRIES_BUILT_INDEX)
			{
				if (!doltgres_entries_stored(raise, pg_OwnedSlot_slot(frame->slot), frame->table, &frame->row,
											 frame->error))
					return;
				building.tuple = tuple;
				doltgres_entries_locked(frame->io, &DOLTGRES_ENTRIES_BUILDING.local, &DOLTGRES_ENTRIES_BUILDING_TYPE,
										&DOLTGRES_ENTRIES_BUILDING_LOCK_TYPE, doltgres_entries_building_replaced_op,
										&building);
				if (!pg_calls_build_row(raise, pg_CatalogCaps_catalog(frame->caps), pg_OwnedBuild_build(frame->build),
										frame->index, frame->info, pg_OwnedSlot_slot(frame->slot), frame->callback,
										frame->state, built.alive, &taken, frame->error))
					return;
				if (taken && built.alive)
					live += 1.0;
			}
		}
		if (frame->row_live)
		{
			frame->row_live = false;
			doltgres_scan_Row_drop(&frame->row);
		}
	}
	*frame->result = live;
	frame->ok = true;
}

/*
 * doltgres_entries_build_scanned is the index build's scan over its rows
 * (index_build_range_scan), answering its count of live rows.
 */
static bool
doltgres_entries_build_scanned(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							   const HostSharedReader *reading, const PgRel *table, const PgRel *index,
							   PgIndexInfo *info, doltgres_entries_Scanned *scanned, uint32 start, uint32 numblocks,
							   bool anyvisible, bool progress, PgOwnedBuild *build, PgOwnedSlot *slot,
							   PgBuildCallback callback, PgBuildState state, float8 *result, PgPgError *error)
{
	doltgres_entries_build_scanned_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading,
		.table = table, .index = index, .info = info, .scanned = scanned, .start = start, .numblocks = numblocks,
		.anyvisible = anyvisible, .progress = progress, .build = build, .slot = slot, .callback = callback,
	.state = state, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_build_scanned_body, doltgres_entries_build_scanned_pad);
	doltgres_entries_build_scanned_pad(&frame);
	return frame.ok;
}

/* state.rs' held at (u64, Option<Base>), which entries.c defines and index_build_range_scan takes. */
#define TXN_STATE_TS TupleU64OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result_0, bool *has_result_1, TxnBaseBase *result_1, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_index_build_range_scan_captures_t is the build
 * scan's lent call's captures: the table, and the base generation its base
 * was read at.
 */
typedef struct doltgres_entries_doltgres_index_build_range_scan_captures_t
{
	const PgRel *table;
	uint64		generation;
} doltgres_entries_doltgres_index_build_range_scan_captures_t;

/*
 * doltgres_entries_doltgres_index_build_range_scan_closure_0 is the build
 * scan's lent call: the table's blocks and newest base, with the generation
 * read before it.
 */
static bool
doltgres_entries_doltgres_index_build_range_scan_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														   const HostSharedHeld *held, uint64 *result_0,
														   bool *has_result_1, TxnBaseBase *result_1,
														   HostError *error)
{
	doltgres_entries_doltgres_index_build_range_scan_captures_t *c = captures;

	*result_0 = doltgres_entries_blocks(c->table, held);
	c->generation = txn_state_generation(host_shared_Held_record(held));
	return txn_state_newest(io, held, has_result_1, result_1, error);
}

/*
 * doltgres_entries_doltgres_index_build_range_scan_frame_t is the build
 * scan's state across its pad: what it opened, the NonVacuumable snapshot,
 * the slot, the build's state, the table's recorded base, its key's columns
 * until key_order takes them, its keyed layout and order until the scan
 * takes them, and the newest base, each while live.
 */
typedef struct doltgres_entries_doltgres_index_build_range_scan_frame_t
{
	doltgres_entries_opened_t opened;
	PgOwnedSnapshot owned;
	bool		owned_live;
	PgOwnedSlot slot;
	bool		slot_live;
	PgOwnedBuild build;
	bool		build_live;
	TxnBaseBase recorded;
	bool		recorded_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		keyed;
	bool		keyed_live;
	TreesKeyOrder order;
	bool		order_live;
	TxnBaseBase base;
	bool		base_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *table;
	const PgRel *index;
	PgIndexInfo *info;
	bool		anyvisible;
	bool		progress;
	uint32		start;
	uint32		numblocks;
	PgBuildCallback callback;
	PgBuildState state;
	PgScan	   *scan;
	float8	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_build_range_scan_frame_t;

/* doltgres_entries_doltgres_index_build_range_scan_pad frees what the build scan holds. */
static void
doltgres_entries_doltgres_index_build_range_scan_pad(void *frame_)
{
	doltgres_entries_doltgres_index_build_range_scan_frame_t *frame = frame_;

	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	if (frame->build_live)
		pg_OwnedBuild_drop(&frame->build);
	if (frame->slot_live)
		pg_OwnedSlot_drop(&frame->slot);
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	frame->base_live = frame->order_live = frame->keyed_live = frame->columns_live = frame->recorded_live = false;
	frame->build_live = frame->slot_live = frame->owned_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_build_range_scan_own begins the build's own scan of the table's tid range. */
static bool
doltgres_entries_doltgres_index_build_range_scan_own(doltgres_entries_doltgres_index_build_range_scan_frame_t *frame,
													 HostOwnedHandle *handle, uint64 *blocks)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	const RepoRepo *repo = doltgres_entries_opened_repo(&frame->opened);
	TxnStateFound found;
	TreesStoreReader store;
	uint64		last;
	HostError	failed;
	doltgres_entries_doltgres_index_build_range_scan_captures_t c = {frame->table};

	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->table,
								   &frame->recorded, frame->error))
		return false;
	frame->recorded_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->recorded.key, &frame->columns, frame->error))
		return false;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->keyed);
	frame->keyed_live = true;
	frame->columns_live = false;
	if (!doltgres_layout_key_order(raise, catalog, &frame->recorded.key, &frame->columns, &frame->order,
								   frame->error))
		return false;
	frame->order_live = true;
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->table, &found, frame->error))
		return false;
	if (!txn_state_held_TupleU64OptionBase(&frame->io, repo, &frame->opened.reading, found,
										   doltgres_entries_doltgres_index_build_range_scan_closure_0, &c, NULL,
										   blocks, &frame->base_live, &frame->base, &failed))
	{
		frame->base_live = false;
		*frame->error = pghost_raised(&failed);
		return false;
	}
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->opened.reading);
	frame->keyed_live = frame->order_live = false;
	if (!doltgres_scan_scan_begin(&frame->io, TREES_STORE_READER_NODE_STORE(&store), *blocks,
								  frame->base_live ? &frame->base : NULL, c.generation, &frame->keyed,
								  pg_calls_rel_id(frame->table), &frame->order, handle, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	last = frame->numblocks == InvalidBlockNumber ? PG_UINT64_MAX :
		std_saturating_sub_u64(((uint64) frame->start + frame->numblocks) << 16, 1);
	if (!doltgres_scan_set_range(&frame->io, *handle, (uint64) frame->start << 16, last, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_doltgres_index_build_range_scan_body scans the table's rows for the build and ends its scans. */
static void
doltgres_entries_doltgres_index_build_range_scan_body(void *frame_)
{
	doltgres_entries_doltgres_index_build_range_scan_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostSnapshot horizon;
	HostSnapshot any = pghost_any();
	doltgres_entries_Scanned scanned = {.scan = frame->scan, .any = &any, .horizon = &horizon};
	uint64		blocks = 0;
	bool		built;
	doltgres_entries_OptionBuilding taken;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	if (!pg_OwnedSnapshot_nonvacuumable(raise, frame->table, &frame->owned, frame->error))
		return;
	frame->owned_live = true;
	if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &horizon, frame->error) ||
		!pg_calls_make_slot(raise, frame->table, &frame->slot, frame->error))
		return;
	frame->slot_live = true;
	if (!pg_calls_build_state(raise, frame->info, &frame->build, frame->error))
		return;
	frame->build_live = true;
	if (frame->scan == NULL)
	{
		if (!doltgres_entries_doltgres_index_build_range_scan_own(frame, &scanned.own, &blocks))
			return;
		scanned.has_own = true;
		if (frame->base_live)
		{
			frame->base_live = false;
			txn_base_Base_drop(&frame->base);
		}
		frame->recorded_live = false;
		txn_base_Base_drop(&frame->recorded);
	}
	if (frame->progress)
		pg_calls_progress_update(PROGRESS_SCAN_BLOCKS_TOTAL, blocks > PG_INT64_MAX ? PG_INT64_MAX : (int64) blocks);
	built = doltgres_entries_build_scanned(frame->caps, &frame->io, doltgres_entries_opened_repo(&frame->opened),
										   &frame->opened.reading, frame->table, frame->index, frame->info, &scanned,
										   frame->start, frame->numblocks, frame->anyvisible, frame->progress,
										   &frame->build, &frame->slot, frame->callback, frame->state, frame->result,
										   frame->error);
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILDING.local, &DOLTGRES_ENTRIES_BUILDING_TYPE,
							&DOLTGRES_ENTRIES_BUILDING_LOCK_TYPE, doltgres_entries_building_taken_op, &taken);
	doltgres_entries_OptionBuilding_drop(&taken);
	if (!built)
		return;
	pg_calls_index_info_cleared(frame->info);
	if (scanned.has_own && !doltgres_scan_scan_end(&frame->io, scanned.own, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (frame->scan != NULL)
	{
		if (!doltgres_scan_scan_end(&frame->io, doltgres_entries_handle(frame->scan), &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		pg_calls_scan_free(frame->scan);
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_build_range_scan is the fork's
 * index_build_range_scan (the btree text's section 6): every row version of
 * the table's storage, read under SnapshotAny through a scan of its own, over
 * the base that snapshot reads, in tuple id order through its tid range, as
 * heap's reads its blocks in order, which BRIN's build relies on, or through
 * a parallel build's, in its tuple ids' block range, is classed by built, a
 * wait on a running writer or ender reading the row again after it; each
 * indexed row that the index's predicate admits is handed to the build's
 * callback with its values, its tid, and whether it is alive, after its key
 * is named to the fork's own build (BUILDING); a recent version marks
 * ii_BrokenHotChain, so index_build sets indcheckxmin. It reports the scan's
 * blocks to the build's progress when asked, clears the index's expression
 * state, which lived in the scan's executor state, and answers the count of
 * live rows, as heap's does. It ends a parallel build's scan, as heap's does.
 */
bool
doltgres_entries_doltgres_index_build_range_scan(const PgCatalogCaps *caps, const PgRel *table_rel,
												 const PgRel *index_rel, PgIndexInfo *index_info, bool allow_sync,
												 bool anyvisible, bool progress, uint32 start_blockno,
												 uint32 numblocks, PgBuildCallback callback,
												 PgBuildState callback_state, PgScan *build_scan, float8 *result,
												 PgPgError *error)
{
	doltgres_entries_doltgres_index_build_range_scan_frame_t frame = {.caps = caps, .table = table_rel,
		.index = index_rel, .info = index_info, .anyvisible = anyvisible, .progress = progress,
		.start = start_blockno, .numblocks = numblocks, .callback = callback, .state = callback_state,
	.scan = build_scan, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_build_range_scan_body,
				doltgres_entries_doltgres_index_build_range_scan_pad);
	doltgres_entries_doltgres_index_build_range_scan_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_BuildSnapshot is a concurrent build's snapshot, which its
 * validation reads in the same backend: the index built, and the snapshot's
 * bounds, whether its subtransactions overflowed, and its running xids, each
 * a Vec<u32>.
 */
typedef struct doltgres_entries_BuildSnapshot
{
	uint32		index;
	uint64		xmin;
	uint64		xmax;
	bool		overflowed;
	StdVec		xip;
	StdVec		subxip;
} doltgres_entries_BuildSnapshot;

/* doltgres_entries_BuildSnapshot_drop is BuildSnapshot's drop: its running xids. */
static void
doltgres_entries_BuildSnapshot_drop(void *snapshot)
{
	doltgres_entries_BuildSnapshot *self = snapshot;

	std_vec_free_Checked(&self->xip, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&self->subxip, sizeof(uint32), _Alignof(uint32));
}

/* doltgres_entries_VecBuildSnapshot_drop is Vec<BuildSnapshot>'s drop. */
static void
doltgres_entries_VecBuildSnapshot_drop(void *snapshots)
{
	std_vec_drop_Checked(snapshots, sizeof(doltgres_entries_BuildSnapshot), _Alignof(doltgres_entries_BuildSnapshot),
						 doltgres_entries_BuildSnapshot_drop);
}

/* BUILD_SNAPSHOTS' IoMutex, host's lock at the backend's concurrent builds' snapshots. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecBuildSnapshot
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_entries_BuildSnapshot), _Alignof(doltgres_entries_BuildSnapshot))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_VecBuildSnapshot_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecBuildSnapshot
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_entries_BuildSnapshot), _Alignof(doltgres_entries_BuildSnapshot))
#include "doltgres/host/lock_template.h"

/* doltgres_entries_IoMutexVecBuildSnapshot_drop is IoMutex<Vec<BuildSnapshot>>' drop glue: its value's. */
static void
doltgres_entries_IoMutexVecBuildSnapshot_drop(void *value)
{
	doltgres_entries_VecBuildSnapshot_drop(&((HostLockIoMutexVecBuildSnapshot *) value)->value);
}

/* DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_TYPE is IoMutex<Vec<BuildSnapshot>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_TYPE = {sizeof(HostLockIoMutexVecBuildSnapshot),
	_Alignof(HostLockIoMutexVecBuildSnapshot), host_lock_IoMutexVecBuildSnapshot_Default_default,
doltgres_entries_IoMutexVecBuildSnapshot_drop};

/*
 * DOLTGRES_ENTRIES_BUILD_SNAPSHOTS is BUILD_SNAPSHOTS: each concurrent build's
 * snapshot from its build's end until its validation takes it, in backend
 * memory alone (bugs row 313); its BackendLocal header and the process's
 * value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecBuildSnapshot process;
}			DOLTGRES_ENTRIES_BUILD_SNAPSHOTS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* doltgres_entries_build_snapshot_kept_op moves a build's snapshot into BUILD_SNAPSHOTS, in place of its index's older one. */
static void
doltgres_entries_build_snapshot_kept_op(void *value, void *captures)
{
	StdVec	   *self = value;
	doltgres_entries_BuildSnapshot *kept = captures;

	for (size_t i = 0; i < self->len; i++)
	{
		doltgres_entries_BuildSnapshot *at = &STD_VEC_AT(self, doltgres_entries_BuildSnapshot, i);

		if (at->index == kept->index)
		{
			doltgres_entries_BuildSnapshot_drop(at);
			*at = *kept;
			return;
		}
	}
	std_vec_push_Checked(self, kept, sizeof(*kept), _Alignof(doltgres_entries_BuildSnapshot));
}

/* doltgres_entries_build_snapshot_taken_t is a take of BUILD_SNAPSHOTS' captures: the index, and its snapshot when it had one. */
typedef struct doltgres_entries_build_snapshot_taken_t
{
	uint32		index;
	bool		has;
	doltgres_entries_BuildSnapshot snapshot;
} doltgres_entries_build_snapshot_taken_t;

/* doltgres_entries_build_snapshot_taken_op takes an index's build snapshot out of BUILD_SNAPSHOTS into its captures. */
static void
doltgres_entries_build_snapshot_taken_op(void *value, void *captures)
{
	StdVec	   *self = value;
	doltgres_entries_build_snapshot_taken_t *taken = captures;

	for (size_t i = 0; i < self->len; i++)
		if (STD_VEC_AT(self, doltgres_entries_BuildSnapshot, i).index == taken->index)
		{
			std_vec_swap_remove(self, i, &taken->snapshot, sizeof(doltgres_entries_BuildSnapshot),
								_Alignof(doltgres_entries_BuildSnapshot));
			taken->has = true;
			return;
		}
}

/*
 * doltgres_entries_build_snapshot_copied copies the transaction's snapshot
 * for a concurrent build of an index, taken before the build reads a row, as
 * heap's concurrent build scan takes its own.
 */
static bool
doltgres_entries_build_snapshot_copied(const PgMayRaise *raise, uint32 index, doltgres_entries_BuildSnapshot *result,
									   PgPgError *error)
{
	PgSnapshotView view;

	if (!pg_transaction_snapshot_view(raise, &view, error))
		return false;
	result->index = index;
	result->xmin = view.xmin;
	result->xmax = view.xmax;
	result->overflowed = view.overflowed;
	std_slice_to_vec_Checked(&result->xip, view.xip, view.xip_len, sizeof(uint32), _Alignof(uint32));
	std_slice_to_vec_Checked(&result->subxip, view.subxip, view.subxip_len, sizeof(uint32), _Alignof(uint32));
	return true;
}

/*
 * doltgres_entries_build_snapshot_saw reports whether a concurrent build's
 * snapshot saw a row's writer ended, so that the build indexed the row: an
 * xid the snapshot counts as no longer running, which an overflowed snapshot
 * answers only below its xmin.
 */
static bool
doltgres_entries_build_snapshot_saw(const doltgres_entries_BuildSnapshot *built, uint64 writer)
{
	HostSnapshot snapshot = {.kind = HOST_SNAPSHOT_KIND_MVCC, .xmin = {built->xmin}, .xmax = {built->xmax},
	.overflowed = built->overflowed, .xip = built->xip.ptr, .xip_len = built->xip.len,
	.subxip = built->subxip.ptr, .subxip_len = built->subxip.len};

	if (writer == InvalidTransactionId)
		return false;
	if (built->overflowed)
		return writer < built->xmin;
	return !txn_visible_in_snapshot(&snapshot, writer, writer);
}

/* doltgres_entries_doltgres_index_validate_scan_closure_0 is the validate scan's lent call: the table's block count. */
static bool
doltgres_entries_doltgres_index_validate_scan_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														const HostSharedHeld *held, uint64 *result, HostError *error)
{
	*result = doltgres_entries_blocks(captures, held);
	return true;
}

/*
 * doltgres_entries_doltgres_index_validate_scan_frame_t is the validate
 * scan's state across its pad: what it opened, the slot, and the build's
 * state, each while live.
 */
typedef struct doltgres_entries_doltgres_index_validate_scan_frame_t
{
	doltgres_entries_opened_t opened;
	PgOwnedSlot slot;
	bool		slot_live;
	PgOwnedBuild build;
	bool		build_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *table;
	const PgRel *index;
	PgIndexInfo *info;
	const PgSnapshot *snapshot;
	PgValidateState *state;
	doltgres_entries_build_snapshot_taken_t built;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_validate_scan_frame_t;

/* doltgres_entries_doltgres_index_validate_scan_pad frees what the validate scan holds. */
static void
doltgres_entries_doltgres_index_validate_scan_pad(void *frame_)
{
	doltgres_entries_doltgres_index_validate_scan_frame_t *frame = frame_;

	if (frame->build_live)
		pg_OwnedBuild_drop(&frame->build);
	if (frame->slot_live)
		pg_OwnedSlot_drop(&frame->slot);
	if (frame->built.has)
		doltgres_entries_BuildSnapshot_drop(&frame->built.snapshot);
	frame->build_live = frame->slot_live = frame->built.has = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_index_validate_scan_body merges the rows the
 * validation sees with the index's sorted tids.
 */
static void
doltgres_entries_doltgres_index_validate_scan_body(void *frame_)
{
	doltgres_entries_doltgres_index_validate_scan_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	TxnStateFound found;
	uint64		total;
	PgScanDesc	desc;
	PgScan	   *scan;
	float8		scanned = 0.0;
	float8		inserted = 0.0;
	bool		has_block = false;
	uint64		block = 0;
	bool		has_sorted = false;
	uint64		sorted = 0;
	bool		ended = false;
	bool		more;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	frame->built.index = pg_calls_rel_id(frame->index);
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILD_SNAPSHOTS.local, &DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_TYPE,
							&DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_LOCK_TYPE, doltgres_entries_build_snapshot_taken_op,
							&frame->built);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->table, &found, frame->error))
		return;
	if (!txn_state_held_U64(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading, found,
							doltgres_entries_doltgres_index_validate_scan_closure_0, (void *) frame->table, NULL,
							&total, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	pg_calls_progress_update(PROGRESS_SCAN_BLOCKS_TOTAL, total > PG_INT64_MAX ? PG_INT64_MAX : (int64) total);
	if (!doltgres_entries_doltgres_scan_begin(frame->caps, frame->table, frame->snapshot, 0, NULL, NULL,
											  SO_TYPE_SEQSCAN | SO_ALLOW_STRAT | SO_ALLOW_PAGEMODE, &desc,
											  frame->error))
		return;
	scan = pg_ScanDesc_scan(&desc);
	if (!doltgres_scan_set_range(&frame->io, doltgres_entries_handle(scan), 0, PG_UINT64_MAX, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!pg_calls_make_slot(raise, frame->table, &frame->slot, frame->error))
		return;
	frame->slot_live = true;
	if (!pg_calls_build_state(raise, frame->info, &frame->build, frame->error))
		return;
	frame->build_live = true;
	for (;;)
	{
		PgSlot	   *slot = pg_OwnedSlot_slot(&frame->slot);
		uint64		tuple;
		bool		added = false;

		if (!doltgres_entries_doltgres_scan_getnextslot_tidrange(frame->caps, scan, ForwardScanDirection, slot, &more,
																 frame->error))
			return;
		if (!more)
			break;
		tuple = pg_calls_slot_tid(slot);
		scanned += 1.0;
		if (!has_block || block != tuple >> 16)
		{
			has_block = true;
			block = tuple >> 16;
			pg_calls_progress_update(PROGRESS_SCAN_BLOCKS_DONE, block > PG_INT64_MAX ? PG_INT64_MAX : (int64) block);
		}
		while (!ended && (!has_sorted || sorted < tuple))
		{
			int64		next;

			if (!pg_calls_validate_next(raise, frame->state, &next, frame->error))
				return;
			has_sorted = next >= 0;
			sorted = (uint64) next;
			ended = !has_sorted;
		}
		if ((!has_sorted || sorted != tuple) &&
			!(frame->built.has && doltgres_entries_build_snapshot_saw(&frame->built.snapshot, pg_calls_slot_writer(slot))) &&
			!pg_calls_validate_insert(raise, catalog, pg_OwnedBuild_build(&frame->build), frame->index, frame->table,
									  frame->info, slot, &added, frame->error))
			return;
		if (added)
			inserted += 1.0;
	}
	pg_calls_validate_counted(frame->state, scanned, inserted);
	if (!doltgres_scan_scan_end(&frame->io, doltgres_entries_handle(scan), &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	pg_calls_scan_free(scan);
	pg_calls_index_info_cleared(frame->info);
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_validate_scan is the fork's
 * index_validate_scan (the unknown methods' text, section 2): the second
 * scan of a concurrent index build reads the rows the validation's snapshot
 * sees in tuple id order, through its tid range, and merges them with the
 * index's sorted tids, as heap's merge does with one version a tid, so each
 * row whose tid the index lacks and the index's predicate admits is inserted
 * through the index's own aminsert, a unique index checking the row. An
 * index the fork stores hands the merge no tid, since its ambulkdelete
 * reports none, so each row but one the build indexed is checked against the
 * index's own entries instead (index_holds, bugs row 168): VACUUM's
 * callback, which would hand the merge those tids, would make every VACUUM
 * walk the whole index for nothing, since no entry the fork stores names a
 * dead row, and a keyed table's entries hold no tids. A row the build indexed is one whose writer
 * the build's snapshot saw ended (BUILD_SNAPSHOTS), and its key is not formed
 * again (bugs row 313). It reports the scan's blocks to the build's
 * progress, counts the rows it read and inserted in the validation's state,
 * and clears the index's expression state, which lived in the scan's
 * executor state.
 */
bool
doltgres_entries_doltgres_index_validate_scan(const PgCatalogCaps *caps, const PgRel *table_rel,
											  const PgRel *index_rel, PgIndexInfo *index_info,
											  const PgSnapshot *snapshot, PgValidateState *state, PgPgError *error)
{
	doltgres_entries_doltgres_index_validate_scan_frame_t frame = {.caps = caps, .table = table_rel,
		.index = index_rel, .info = index_info, .snapshot = snapshot, .state = state,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_validate_scan_body,
				doltgres_entries_doltgres_index_validate_scan_pad);
	doltgres_entries_doltgres_index_validate_scan_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_TreesEntry_drop is a tree Entry's drop glue, an element of a base tree's entries. */
static void
doltgres_entries_TreesEntry_drop(void *entry)
{
	trees_build_Entry_drop(entry);
}

/* doltgres_entries_Based_drop is rewrite's Based's drop glue, an element of based's answer. */
static void
doltgres_entries_Based_drop(void *based)
{
	txn_rewrite_Based_drop(based);
}

/*
 * doltgres_entries_rekeyed_closure_0_0_0 is the base entries' map: a based
 * entry as a tree entry of its key and value.
 */
static void
doltgres_entries_rekeyed_closure_0_0_0(void *elem, void *result)
{
	TxnRewriteBased *entry = elem;

	trees_build_Entry_new_VecU8VecU8(&entry->key, &entry->value, result);
	std_vec_free_Checked(&entry->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_free_Checked(&entry->values, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&entry->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
}

/* doltgres_entries_rekeyed_closure_0_0 is the key change's entries: a base tree's entries as tree entries. */
static bool
doltgres_entries_rekeyed_closure_0_0(HostIoIo *io, TreesStoreNodeStore store, bool has_tree, TreesStoreTree tree,
									 StdVec *result, HostError *error)
{
	StdVec		read;

	if (!txn_rewrite_based(io, store, has_tree, tree, &read, error))
		return false;
	std_vec_from_iter_in_place_map(&read, sizeof(TxnRewriteBased), _Alignof(TxnRewriteBased),
								   doltgres_entries_Based_drop, sizeof(TreesBuildEntry),
								   doltgres_entries_TreesEntry_drop, doltgres_entries_rekeyed_closure_0_0_0, result);
	return true;
}

/* state.rs' held at (Read, Vec<Entry>, Vec<Entry>, Vec<MapCarried>, u64), which rekeyed takes. */
#define TXN_STATE_TS RekeyedRead
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , DoltgresRewriteRead *result_0, StdVec *result_1, StdVec *result_2, \
	StdVec *result_3, uint64 *result_4, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS DoltgresRewriteRead *result_0; StdVec *result_1; StdVec *result_2; \
	StdVec *result_3; uint64 *result_4;
#define TXN_STATE_RESULT_INIT , result_0, result_1, result_2, result_3, result_4
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1, (c)->result_2, (c)->result_3, (c)->result_4
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_rekeyed_captures_t is rekeyed's lent calls' captures: the
 * reader, the old key's order and whether it is keyless, the fold's and the
 * rewrite's horizons, the old storage, and for the install, the new key's
 * order, the trees, the old base's fold points and fresh end, the counter,
 * the sizes, the nodes' xid, the free space versions, the carried versions,
 * and whether the storage is logged.
 */
typedef struct doltgres_entries_rekeyed_captures_t
{
	const HostSharedReader *reading;
	const TreesKeyOrder *old_order;
	bool		old_keyless;
	uint64		folding;
	uint64		horizon;
	HostStorage old_storage;
	const TreesKeyOrder *new_order;
	TxnFoldTrees *trees;
	bool	   *trees_live;
	HostLsn		point;
	uint64		max_folded;
	uint64		max_ended;
	uint64		fresh_end;
	uint64		counter;
	TxnBaseSizes sizes;
	bool		has_nodes;
	uint64		nodes;
	const StdVec *spaces;
	const StdVec *carried;
	bool		logged;
} doltgres_entries_rekeyed_captures_t;

/*
 * doltgres_entries_rekeyed_closure_0_frame_t is the key change's read's state
 * across its pad: the fold's warnings, the free space and pending entries,
 * and the free space versions, each while live.
 */
typedef struct doltgres_entries_rekeyed_closure_0_frame_t
{
	StdVec		warned;
	bool		warned_live;
	bool		free_space_live;
	bool		pending_live;
	bool		spaces_live;
	doltgres_entries_rekeyed_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	DoltgresRewriteRead *read;
	StdVec	   *free_space;
	StdVec	   *pending;
	StdVec	   *spaces;
	uint64	   *counter;
	HostError  *error;
	bool		ok;
} doltgres_entries_rekeyed_closure_0_frame_t;

/* doltgres_entries_rekeyed_closure_0_pad frees what the key change's read holds. */
static void
doltgres_entries_rekeyed_closure_0_pad(void *frame_)
{
	doltgres_entries_rekeyed_closure_0_frame_t *frame = frame_;

	if (frame->spaces_live)
		std_vec_drop_Checked(frame->spaces, sizeof(TxnRecordMapCarried), _Alignof(TxnRecordMapCarried),
							 doltgres_entries_MapCarried_drop);
	if (frame->pending_live)
		std_vec_drop_Checked(frame->pending, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
	if (frame->free_space_live)
		std_vec_drop_Checked(frame->free_space, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
	if (frame->warned_live)
		std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	frame->spaces_live = frame->pending_live = frame->free_space_live = frame->warned_live = false;
}

/*
 * doltgres_entries_rekeyed_closure_0_body folds the old storage now and reads
 * its base, rows, and free space versions.
 */
static void
doltgres_entries_rekeyed_closure_0_body(void *frame_)
{
	doltgres_entries_rekeyed_closure_0_frame_t *frame = frame_;
	doltgres_entries_rekeyed_captures_t *c = frame->c;
	TxnInsertPage page = doltgres_entries_page();
	bool		has_point;
	HostLsn		point;
	bool		folded;
	bool		some;
	TxnBaseBase base;
	TreesStoreReader store;

	if (!txn_fold_point(frame->io, &has_point, &point, frame->error))
		return;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	if (!txn_fold_fold_now(frame->io, frame->repo, c->reading, frame->held, c->old_order, c->old_keyless, has_point,
						   point, c->folding, &frame->warned, &page, NULL, &folded, frame->error))
		return;
	frame->warned_live = false;
	std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (!txn_state_newest(frame->io, frame->held, &some, &base, frame->error))
		return;
	if (!some)
	{
		*frame->error = doltgres_entries_host_fork("XX000", "a key change found a storage state with no base");
		return;
	}
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	if (!doltgres_entries_rekeyed_closure_0_0(frame->io, TREES_STORE_READER_NODE_STORE(&store), base.has_free_space,
											  base.free_space, frame->free_space, frame->error))
	{
		txn_base_Base_drop(&base);
		return;
	}
	frame->free_space_live = true;
	if (!doltgres_entries_rekeyed_closure_0_0(frame->io, TREES_STORE_READER_NODE_STORE(&store), base.has_pending,
											  base.pending, frame->pending, frame->error))
	{
		txn_base_Base_drop(&base);
		return;
	}
	frame->pending_live = true;
	if (!txn_rewrite_spaces(frame->io, frame->held, c->old_storage, frame->spaces, frame->error))
	{
		txn_base_Base_drop(&base);
		return;
	}
	frame->spaces_live = true;
	*frame->counter = txn_state_counter(host_shared_Held_record(frame->held));
	if (!doltgres_rewrite_read(frame->io, TREES_STORE_READER_NODE_STORE(&store), frame->held, &base, c->horizon,
							   frame->read, frame->error))
		return;
	frame->spaces_live = frame->pending_live = frame->free_space_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_rekeyed_closure_0 is the key change's read: the old storage
 * folded and its rows read under the held state.
 */
static bool
doltgres_entries_rekeyed_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   DoltgresRewriteRead *result_0, StdVec *result_1, StdVec *result_2,
								   StdVec *result_3, uint64 *result_4, HostError *error)
{
	doltgres_entries_rekeyed_closure_0_frame_t frame = {.c = captures, .io = io, .repo = repo, .held = held,
		.read = result_0, .free_space = result_1, .pending = result_2, .spaces = result_3, .counter = result_4,
	.error = error};

	std_landing(&frame, doltgres_entries_rekeyed_closure_0_body, doltgres_entries_rekeyed_closure_0_pad);
	doltgres_entries_rekeyed_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_rekeyed_closure_4 is the key change's install: the new
 * storage's base built and its versions carried.
 */
static bool
doltgres_entries_rekeyed_closure_4(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   bool *result, HostError *error)
{
	doltgres_entries_rekeyed_captures_t *c = captures;
	bool		some;
	TxnBaseBase installing;
	TreesStoreReader store;
	const TxnRecordMapCarried *spaces = c->spaces->ptr;
	const TxnRewriteCarry *carried = c->carried->ptr;

	if (!txn_state_base_record(io, held, &some, &installing, error))
		return false;
	if (!some)
	{
		*error = doltgres_entries_host_fork("XX000", "a key change's new storage has no base record");
		return false;
	}
	installing.folded = c->point;
	installing.max_folded = c->max_folded;
	installing.max_ended = c->max_ended;
	installing.counter = c->counter;
	installing.fresh_end = c->fresh_end;
	installing.sizes = c->sizes;
	*c->trees_live = false;
	if (!txn_fold_built(io, repo, c->reading, held, c->new_order, c->trees, &installing, c->point, c->has_nodes,
						c->nodes, 0, result, error))
		return false;
	if (!*result)
		return true;
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	return txn_rewrite_carried(io, TREES_STORE_READER_NODE_STORE(&store), held, spaces, c->spaces->len, carried,
							   c->carried->len, c->logged, NULL, pg_calls_max_heap_tuples_per_page(), error);
}

/* doltgres_entries_Rekeyed_carried_drop is a rekeyed version's drop glue: its key's and its op's. */
static void
doltgres_entries_Rekeyed_carried_drop(void *carried)
{
	doltgres_TupleVecU8OpWriterShownLockMode_t *self = carried;

	std_vec_free_Checked(&self->f0, 1, 1);
	txn_memtable_Op_drop(&self->f1);
}

/*
 * doltgres_entries_rekeyed_frame_t is rekeyed's state across its pad: the
 * old and new keys until they are taken, the old key's columns until
 * key_order takes them and its keyed layout, the old and new orders, the new
 * key's columns until the reformer takes them, the read, the free space and
 * pending entries, the free space versions, the reformer, the rekeyed
 * storage and the iterator of its versions, the TOAST storage's state, the
 * carried versions, a version's blobs, the TOAST base record, and the trees
 * until the build takes them, each while live.
 */
typedef struct doltgres_entries_rekeyed_frame_t
{
	TxnBaseKey	old;
	bool		old_live;
	TxnBaseKey	new_;
	bool		new_live;
	StdVec		old_columns;
	bool		old_columns_live;
	StdVec		keyed;
	bool		keyed_live;
	TreesKeyOrder old_order;
	bool		old_order_live;
	StdVec		new_columns;
	bool		new_columns_live;
	DoltgresRewriteRead read;
	bool		read_live;
	StdVec		free_space;
	StdVec		pending;
	StdVec		spaces;
	bool		parts_live;
	TreesKeyOrder new_order;
	bool		new_order_live;
	DoltgresRewriteReformer reformer;
	bool		reformer_live;
	DoltgresRekeyRekeyed rekeyed;
	bool		rekeyed_live;
	StdVecIntoIter versions;
	bool		versions_live;
	TxnRecordToast toast;
	HostSharedHeld values;
	bool		toast_live;
	StdVec		carried;
	bool		carried_live;
	StdVec		blobs;
	bool		blobs_live;
	TxnBaseBase made;
	bool		made_live;
	TxnFoldTrees trees;
	bool		trees_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_rekeyed_frame_t;

/* doltgres_entries_rekeyed_pad frees what rekeyed holds. */
static void
doltgres_entries_rekeyed_pad(void *frame_)
{
	doltgres_entries_rekeyed_frame_t *frame = frame_;

	if (frame->trees_live)
	{
		std_vec_drop_Checked(&frame->trees.rows, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
		std_vec_drop_Checked(&frame->trees.tuple_ids, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
		std_vec_drop_Checked(&frame->trees.free_space, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
		std_vec_drop_Checked(&frame->trees.pending, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
	}
	if (frame->made_live)
		txn_base_Base_drop(&frame->made);
	if (frame->blobs_live)
		std_vec_free_Checked(&frame->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	if (frame->carried_live)
		std_vec_drop_Checked(&frame->carried, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry),
							 doltgres_entries_Carry_drop);
	if (frame->toast_live)
		host_shared_Held_drop(&frame->values);
	if (frame->versions_live)
		std_VecIntoIter_drop(&frame->versions);
	if (frame->rekeyed_live)
		doltgres_rekey_Rekeyed_drop(&frame->rekeyed);
	if (frame->reformer_live)
		doltgres_rewrite_Reformer_drop(&frame->reformer);
	if (frame->new_order_live)
		trees_KeyOrder_drop(&frame->new_order);
	if (frame->parts_live)
	{
		std_vec_drop_Checked(&frame->spaces, sizeof(TxnRecordMapCarried), _Alignof(TxnRecordMapCarried),
							 doltgres_entries_MapCarried_drop);
		std_vec_drop_Checked(&frame->pending, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
		std_vec_drop_Checked(&frame->free_space, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry),
							 doltgres_entries_TreesEntry_drop);
	}
	if (frame->read_live)
		doltgres_rewrite_Read_drop(&frame->read);
	if (frame->new_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->new_columns);
	if (frame->old_order_live)
		trees_KeyOrder_drop(&frame->old_order);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->old_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->old_columns);
	if (frame->new_live)
		txn_base_Key_drop(&frame->new_);
	if (frame->old_live)
		txn_base_Key_drop(&frame->old);
	frame->trees_live = frame->made_live = frame->blobs_live = frame->carried_live = frame->toast_live = false;
	frame->versions_live = frame->rekeyed_live = frame->reformer_live = frame->new_order_live = false;
	frame->parts_live = frame->read_live = frame->new_columns_live = frame->old_order_live = false;
	frame->keyed_live = frame->old_columns_live = frame->new_live = frame->old_live = false;
}

/* doltgres_entries_rekeyed_value finds a value id's map entry among the rekeyed storage's values, sorted by id. */
static const TxnValuesEntry *
doltgres_entries_rekeyed_value(const StdVec *values, uint32 id)
{
	size_t		low = 0;
	size_t		high = values->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const txn_TupleU32TupleEntryU64_t *value = &STD_VEC_AT(values, txn_TupleU32TupleEntryU64_t, mid);

		if (value->f0 == id)
			return &value->f1.f0;
		if (value->f0 < id)
			low = mid + 1;
		else
			high = mid;
	}
	return NULL;
}

/*
 * doltgres_entries_rekeyed_carried turns the rekeyed storage's versions into
 * the row changes the new storage carries, each value out of line named by
 * its blob in the TOAST relation.
 */
static bool
doltgres_entries_rekeyed_carried(doltgres_entries_rekeyed_frame_t *frame, HostStorage moved, uint32 table_id,
								 bool has_named, TxnRecordToast named)
{
	doltgres_TupleVecU8OpWriterShownLockMode_t version;

	frame->versions = std_Vec_IntoIterator_into_iter(&frame->rekeyed.carried,
													 sizeof(doltgres_TupleVecU8OpWriterShownLockMode_t),
													 _Alignof(doltgres_TupleVecU8OpWriterShownLockMode_t),
													 doltgres_entries_Rekeyed_carried_drop);
	std_vec_new(&frame->rekeyed.carried, sizeof(doltgres_TupleVecU8OpWriterShownLockMode_t),
				_Alignof(doltgres_TupleVecU8OpWriterShownLockMode_t));
	frame->versions_live = true;
	std_vec_with_capacity_Checked(&frame->carried, std_VecIntoIter_len(&frame->versions),
								  sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry));
	frame->carried_live = true;
	while (std_VecIntoIter_Iterator_next(&frame->versions, &version))
	{
		const TxnMemtablePut *put = NULL;
		TxnRewriteCarry carry = {0};
		TxnRecordRowChange *change = &carry.f0;

		if (version.f1.tag == TXN_MEMTABLE_OP_PUT)
			put = &version.f1.u.put;
		else if (version.f1.tag == TXN_MEMTABLE_OP_ADD)
			put = &version.f1.u.add;
		else if (version.f1.tag == TXN_MEMTABLE_OP_ADD_ROW)
			put = &version.f1.u.add_row;
		else if (version.f1.tag == TXN_MEMTABLE_OP_UPDATE_ROW)
			put = &version.f1.u.update_row.f1;
		std_vec_with_capacity_Checked(&frame->blobs, put != NULL ? put->values.len : 0,
									  sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
		frame->blobs_live = true;
		for (size_t i = 0; put != NULL && i < put->values.len; i++)
		{
			uint32		id = STD_VEC_AT(&put->values, uint32, i);
			const TxnValuesEntry *entry = doltgres_entries_rekeyed_value(&frame->rekeyed.values, id);
			chunks_TupleAddressU64_t blob;

			if (entry == NULL)
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("a carried row's value "), STD_FMT_ARG(0),
				STD_FMT_LIT(" is in no kept version's map entries")};
				StdFmtArg	args[] = {{&id, std_u32_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};
				StdString	message;

				doltgres_entries_Rekeyed_carried_drop(&version);
				std_fmt_format_Checked(&message, &arguments);
				*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
				return false;
			}
			blob.f0 = entry->address;
			blob.f1 = entry->stored;
			std_vec_push_Checked(&frame->blobs, &blob, sizeof(chunks_TupleAddressU64_t),
								 _Alignof(chunks_TupleAddressU64_t));
		}
		change->heap_bytes = put != NULL ? put->heap_bytes : 0;
		change->has_replaced = version.f1.tag == TXN_MEMTABLE_OP_UPDATE_ROW;
		if (change->has_replaced)
			change->replaced = version.f1.u.update_row.f0;
		change->writer = version.f2;
		change->storage = moved;
		change->table = table_id;
		change->key = version.f0;
		change->op = version.f1;
		change->no_key = version.f4 == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
		change->has_toast = frame->blobs.len > 0 && has_named;
		change->toast = named;
		change->blobs = frame->blobs;
		frame->blobs_live = false;
		std_vec_new(&change->kept, sizeof(size_t), _Alignof(size_t));
		carry.f1 = version.f3;
		carry.f2 = version.f4;
		std_vec_push_Checked(&frame->carried, &carry, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry));
	}
	frame->versions_live = false;
	std_VecIntoIter_drop(&frame->versions);
	return true;
}

/* doltgres_entries_rekeyed_toast builds the new TOAST storage's base whole from the values the versions name. */
static bool
doltgres_entries_rekeyed_toast(doltgres_entries_rekeyed_frame_t *frame, bool has_nodes, uint64 nodes)
{
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	bool		some;
	TxnFoldTrees trees;
	TxnBaseSizes sizes;
	bool		mapped;
	bool		built;
	HostError	failed;

	if (!txn_state_base_record(frame->io, &frame->values, &some, &frame->made, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!some)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a key change's new TOAST storage has no base record");
		return false;
	}
	frame->made_live = true;
	mapped = txn_rewrite_value_map(frame->rekeyed.values.ptr, frame->rekeyed.values.len, &trees, &sizes, &failed);
	std_vec_free_Checked(&frame->rekeyed.values, sizeof(txn_TupleU32TupleEntryU64_t),
						 _Alignof(txn_TupleU32TupleEntryU64_t));
	std_vec_new(&frame->rekeyed.values, sizeof(txn_TupleU32TupleEntryU64_t), _Alignof(txn_TupleU32TupleEntryU64_t));
	if (!mapped)
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->made.sizes = sizes;
	frame->made_live = false;
	if (!txn_fold_built(frame->io, frame->repo, frame->reading, &frame->values, &order, &trees, &frame->made, 0,
						has_nodes, nodes, 0, &built, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!built)
	{
		*frame->error = doltgres_entries_pg_fork("XX000",
												 "a key change's new TOAST storage changed while it was built");
		return false;
	}
	return true;
}

/*
 * doltgres_entries_rekeyed_body reads the old storage, re-forms its rows under
 * the new key, and installs the new storage.
 */
static void
doltgres_entries_rekeyed_body(void *frame_)
{
	doltgres_entries_rekeyed_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_entries_rekeyed_captures_t captures = {.reading = frame->reading, .old_order = &frame->old_order,
		.new_order = &frame->new_order, .trees = &frame->trees, .trees_live = &frame->trees_live,
	.spaces = &frame->spaces, .carried = &frame->carried};
	bool		pinned;
	bool		new_keyless;
	uint32		oldest;
	TxnStateFound found;
	int32		target;
	DoltgresRewriteTable table;
	StdVec		columns;
	uint32		toast_id;
	HostStorage moved;
	uint32		table_id;
	TxnStateFind find;
	TxnStateFound made;
	bool		installed;
	HostError	failed;

	pinned = doltgres_settle_pinned(frame->io, pg_calls_rel_id(frame->rel), &frame->old_columns);
	if (!pinned && !doltgres_layout_key_columns(raise, catalog, &frame->old, &frame->old_columns, frame->error))
		return;
	frame->old_columns_live = true;
	access_slot_keyed(frame->old_columns.ptr, frame->old_columns.len, &frame->keyed);
	frame->keyed_live = true;
	captures.old_keyless = frame->old_columns.len == 0;
	frame->old_columns_live = false;
	if (!doltgres_layout_key_order(raise, catalog, &frame->old, &frame->old_columns, &frame->old_order,
								   frame->error))
		return;
	frame->old_order_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->new_, &frame->new_columns, frame->error))
		return;
	frame->new_columns_live = true;
	new_keyless = frame->new_columns.len == 0;
	if (!pg_calls_oldest_nonremovable(raise, frame->rel, &oldest, frame->error) ||
		!pg_calls_widened_xid(raise, oldest, &captures.horizon, frame->error) ||
		!pg_calls_fold_horizon(raise, frame->rel, &captures.folding, frame->error) ||
		!doltgres_entries_making(frame->caps, frame->io, frame->rel, &found, frame->error))
		return;
	captures.old_storage = doltgres_entries_storage(frame->rel);
	if (!txn_state_held_RekeyedRead(frame->io, frame->repo, frame->reading, found, doltgres_entries_rekeyed_closure_0,
									&captures, NULL, &frame->read, &frame->free_space, &frame->pending,
									&frame->spaces, &captures.counter, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->read_live = frame->parts_live = true;
	target = pg_calls_rel_toast_target(frame->rel);
	table.rel = frame->rel;
	table.keyed = frame->keyed.ptr;
	table.keyed_len = frame->keyed.len;
	table.keyless = captures.old_keyless;
	table.target = target < 0 ? 0 : (size_t) target;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->new_, &columns, frame->error) ||
		!doltgres_layout_key_order(raise, catalog, &frame->new_, &columns, &frame->new_order, frame->error))
		return;
	frame->new_order_live = true;
	frame->new_columns_live = false;
	if (!doltgres_rewrite_Reformer_new(raise, frame->repo, frame->reading, &table, &frame->read, &frame->new_columns,
									   &frame->reformer, frame->error))
		return;
	frame->reformer_live = true;
	if (!doltgres_rekey_rekeyed(frame->caps, frame->io, &frame->reformer, &frame->read, new_keyless,
								&frame->rekeyed, frame->error))
		return;
	frame->rekeyed_live = true;
	frame->reformer_live = false;
	doltgres_rewrite_Reformer_drop(&frame->reformer);
	toast_id = pg_calls_rel_toast_id(frame->rel);
	if (!doltgres_settle_keep_frozen(frame->io, pg_calls_rel_id(frame->rel), &failed) ||
		(toast_id != 0 && !doltgres_settle_keep_frozen(frame->io, toast_id, &failed)))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!pg_calls_key_new_file(raise, catalog, frame->rel, frame->error))
		return;
	moved = doltgres_entries_storage(frame->rel);
	table_id = pg_calls_rel_id(frame->rel);
	captures.logged = pg_calls_rel_permanent(frame->rel);
	captures.has_nodes = pg_calls_rel_needs_wal(frame->rel);
	if (captures.has_nodes && !pg_calls_top_xid(raise, &captures.nodes, frame->error))
		return;
	if (frame->rekeyed.values.len > 0)
	{
		if (!doltgres_entries_toast_state(frame->caps, frame->io, frame->repo, frame->reading, frame->rel,
										  &frame->toast, &frame->values, frame->error))
			return;
		frame->toast_live = true;
	}
	if (!doltgres_entries_rekeyed_carried(frame, moved, table_id, frame->toast_live, frame->toast) ||
		(frame->toast_live && !doltgres_entries_rekeyed_toast(frame, captures.has_nodes, captures.nodes)))
		return;
	for (size_t i = 0; i < frame->spaces.len; i++)
		STD_VEC_AT(&frame->spaces, TxnRecordMapCarried, i).storage = moved;
	captures.point = captures.logged ? frame->read.base.folded : 0;
	captures.max_folded = frame->read.base.max_folded;
	captures.max_ended = frame->read.base.max_ended;
	captures.fresh_end = frame->read.base.fresh_end;
	if (!txn_state_holds(frame->io, &moved, doltgres_entries_persistence(frame->rel), &find, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (find.tag == TXN_STATE_FIND_HELD)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a key change's new relfilenode already holds a storage");
		return;
	}
	made.tag = TXN_STATE_FOUND_MADE;
	made.u.made.f0 = find.u.missing;
	made.u.made.f1.key = frame->new_;
	frame->new_live = false;
	made.u.made.f1.table = table_id;
	made.u.made.f1.has_index = false;
	made.u.made.f1.index = 0;
	frame->trees.rows = frame->rekeyed.rows;
	frame->trees.tuple_ids = frame->rekeyed.tuple_ids;
	frame->trees.free_space = frame->free_space;
	frame->trees.pending = frame->pending;
	captures.sizes = frame->rekeyed.sizes;
	std_vec_new(&frame->rekeyed.rows, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry));
	std_vec_new(&frame->rekeyed.tuple_ids, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry));
	std_vec_new(&frame->free_space, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry));
	std_vec_new(&frame->pending, sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry));
	frame->trees_live = true;
	if (!txn_state_held_Bool(frame->io, frame->repo, frame->reading, made, doltgres_entries_rekeyed_closure_4,
							 &captures, NULL, &installed, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!installed)
	{
		*frame->error = doltgres_entries_pg_fork("XX000", "a key change's new storage changed while it was built");
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_rekeyed writes a table again under its new primary key
 * into a new relfilenode (the rewrite text's section 6, its §8 to §10), taking
 * the old and new keys: the old storage folded and its base chosen as
 * section 4 chooses it, every row version re-formed under the new key before
 * the new files are made, so a value moved out of line anew goes to the old
 * TOAST storage as section 4 sends it; then the table and its TOAST relation
 * take new relfilenodes keeping their frozen xids, the TOAST storage's value
 * map is built from the values the versions name, and the new storage's base
 * is installed with each new key's rows that stay in it, its tuple ids paired
 * with their new keys, the old base's free space map and pending tuple ids,
 * and the old storage's live counter, past every tuple id a carried version
 * took, then the delta's free space versions and the rows' versions are
 * carried. A storage whose versions carry WAL records keeps the old base's
 * fold point; one whose versions carry none (unlogged, temporary) installs at
 * point 0, since fold's applied takes a frozen version with no record as
 * applied at any point above it (CommPF 8's ruling).
 */
static bool
doltgres_entries_rekeyed(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, TxnBaseKey *old, TxnBaseKey *new_,
						 PgPgError *error)
{
	doltgres_entries_rekeyed_frame_t frame = {.old = *old, .old_live = true, .new_ = *new_, .new_live = true,
	.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel, .error = error};

	std_landing(&frame, doltgres_entries_rekeyed_body, doltgres_entries_rekeyed_pad);
	doltgres_entries_rekeyed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_key_settle_table_frame_t is key_settle_table's
 * state across its pad: what it opened and the old and new keys until
 * rekeyed takes them, each while live.
 */
typedef struct doltgres_entries_doltgres_key_settle_table_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	old;
	bool		old_live;
	TxnBaseKey	new_;
	bool		new_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_key_settle_table_frame_t;

/* doltgres_entries_doltgres_key_settle_table_pad frees what key_settle_table holds. */
static void
doltgres_entries_doltgres_key_settle_table_pad(void *frame_)
{
	doltgres_entries_doltgres_key_settle_table_frame_t *frame = frame_;

	if (frame->new_live)
		txn_base_Key_drop(&frame->new_);
	if (frame->old_live)
		txn_base_Key_drop(&frame->old);
	frame->new_live = frame->old_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/*
 * doltgres_entries_doltgres_key_settle_table_body rewrites the table under the
 * catalog's key when the storage's differs.
 */
static void
doltgres_entries_doltgres_key_settle_table_body(void *frame_)
{
	doltgres_entries_doltgres_key_settle_table_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	HostStorage storage = doltgres_entries_storage(frame->rel);
	const RepoRepo *repo;
	bool		stored;
	bool		some;
	bool		same;
	TxnBaseBase base;
	HostError	failed;

	if (!doltgres_relation_stored(frame->caps, frame->rel, &stored, frame->error))
		return;
	if (!stored)
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!txn_state_stored_base(&frame->io, repo, &frame->opened.reading, &storage, &some, &base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!some)
	{
		frame->ok = true;
		return;
	}
	txn_base_Base_drop(&base);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &base,
								   frame->error))
		return;
	frame->old = base.key;
	frame->old_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	if (!doltgres_layout_catalog_key(raise, catalog, frame->rel, &frame->new_, frame->error))
		return;
	frame->new_live = true;
	if (!doltgres_layout_same_key(raise, catalog, &frame->old, &frame->new_, &same, frame->error))
		return;
	if (same)
	{
		frame->ok = true;
		return;
	}
	frame->old_live = frame->new_live = false;
	if (!doltgres_entries_rekeyed(frame->caps, &frame->io, repo, &frame->opened.reading, frame->rel, &frame->old,
								  &frame->new_, frame->error))
		return;
	frame->ok = pg_calls_key_reindex(raise, catalog, frame->rel, frame->error);
}

/*
 * doltgres_entries_doltgres_key_settle_table rewrites a table the fork stores
 * whose storage's key differs from the catalog's primary key once, under the
 * catalog's key, into a new relfilenode, and rebuilds each of its indexes the
 * fork stores, whose entries end with the row's key (the rewrite text's
 * section 6); a storage with no base takes the catalog's key at its first
 * write and is left alone.
 */
bool
doltgres_entries_doltgres_key_settle_table(const PgCatalogCaps *caps, const PgRel *rel, PgPgError *error)
{
	doltgres_entries_doltgres_key_settle_table_frame_t frame = {.caps = caps, .rel = rel, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_key_settle_table_body,
				doltgres_entries_doltgres_key_settle_table_pad);
	doltgres_entries_doltgres_key_settle_table_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_OptionDescribing is BUILD_DESCRIBED's value, an
 * Option<Describing>: a build's description of its index (an Arc<Described>)
 * and the count of rows its callback put in the build's sort.
 */
typedef struct doltgres_entries_OptionDescribing
{
	bool		has;
	StdArcInner *described;
	uint64		put;
} doltgres_entries_OptionDescribing;

/* doltgres_entries_Described_drop is Described's drop glue, as an Arc's value takes it. */
static void
doltgres_entries_Described_drop(void *described)
{
	doltgres_index_Described_drop(described);
}

/* doltgres_entries_OptionDescribing_Default_default is Option<Describing>'s Default: none. */
static void
doltgres_entries_OptionDescribing_Default_default(void *place)
{
	((doltgres_entries_OptionDescribing *) place)->has = false;
}

/* doltgres_entries_OptionDescribing_drop is Option<Describing>'s drop: its description's reference, when it has one. */
static void
doltgres_entries_OptionDescribing_drop(void *place)
{
	doltgres_entries_OptionDescribing *self = place;

	if (self->has)
		std_arc_drop(self->described, sizeof(DoltgresIndexDescribed), _Alignof(DoltgresIndexDescribed),
					 doltgres_entries_Described_drop);
}

/* taken at Option<Describing> and IoMutex<Option<Describing>>, which BUILD_DESCRIBED is. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS OptionDescribing
#define HOST_LOCK_TAKEN_T doltgres_entries_OptionDescribing
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_entries_OptionDescribing_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_OptionDescribing_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_BUILD_DESCRIBED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS OptionDescribing
#define HOST_LOCK_T doltgres_entries_OptionDescribing
#define HOST_LOCK_DEFAULT(place) doltgres_entries_OptionDescribing_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_entries_IoMutexOptionDescribing_drop is IoMutex<Option<Describing>>' drop glue: its value's. */
static void
doltgres_entries_IoMutexOptionDescribing_drop(void *value)
{
	doltgres_entries_OptionDescribing_drop(&((HostLockIoMutexOptionDescribing *) value)->value);
}

/* DOLTGRES_ENTRIES_BUILD_DESCRIBED_TYPE is IoMutex<Option<Describing>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_BUILD_DESCRIBED_TYPE = {sizeof(HostLockIoMutexOptionDescribing),
	_Alignof(HostLockIoMutexOptionDescribing), host_lock_IoMutexOptionDescribing_Default_default,
doltgres_entries_IoMutexOptionDescribing_drop};

/*
 * DOLTGRES_ENTRIES_BUILD_DESCRIBED is BUILD_DESCRIBED: the description of the
 * index the backend is building, which its build callback forms each row's
 * entry by, and the count of rows the callback put in the build's sort, for
 * the build's call alone; its BackendLocal header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexOptionDescribing process;
}			DOLTGRES_ENTRIES_BUILD_DESCRIBED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * doltgres_entries_describing_replaced_op puts a description in
 * BUILD_DESCRIBED, cloning its reference, with no rows put.
 */
static void
doltgres_entries_describing_replaced_op(void *value, void *captures)
{
	doltgres_entries_OptionDescribing *self = value;
	doltgres_entries_OptionDescribing old = *self;

	self->described = std_arc_clone(captures);
	self->put = 0;
	self->has = true;
	doltgres_entries_OptionDescribing_drop(&old);
}

/* doltgres_entries_describing_taken_op takes BUILD_DESCRIBED's description into its captures, leaving none. */
static void
doltgres_entries_describing_taken_op(void *value, void *captures)
{
	doltgres_entries_OptionDescribing *self = value;

	*(doltgres_entries_OptionDescribing *) captures = *self;
	self->has = false;
}

/*
 * doltgres_entries_describing_lent_op counts a row put in BUILD_DESCRIBED and
 * lends its description's reference, when it has one.
 */
static void
doltgres_entries_describing_lent_op(void *value, void *captures)
{
	doltgres_entries_OptionDescribing *self = value;
	doltgres_entries_OptionDescribing *lent = captures;

	lent->has = self->has;
	if (!self->has)
		return;
	self->put++;
	lent->described = std_arc_clone(self->described);
}

/* doltgres_entries_sorted_frame_t is sorted's state across its pad: its bytes, while live. */
typedef struct doltgres_entries_sorted_frame_t
{
	bool		out_live;
	const uint8 *key;
	size_t		key_len;
	const uint8 *value;
	size_t		value_len;
	const chunks_TupleU32Locator_t *addresses;
	size_t		addresses_len;
	uint64		writer;
	uint64		tuple;
	uint64		top;
	bool		alive;
	StdVec	   *out;
} doltgres_entries_sorted_frame_t;

/* doltgres_entries_sorted_pad frees what sorted holds. */
static void
doltgres_entries_sorted_pad(void *frame_)
{
	doltgres_entries_sorted_frame_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(frame->out, 1, 1);
	frame->out_live = false;
}

/* doltgres_entries_sorted_word appends a word's bytes, big-endian. */
static void
doltgres_entries_sorted_word(StdVec *out, uint64 word)
{
	uint8		bytes[sizeof(uint64)];

	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8) (word >> (8 * (sizeof(bytes) - 1 - i)));
	std_vec_extend_from_slice_Checked(out, bytes, sizeof(bytes), 1, 1);
}

/* doltgres_entries_sorted_body writes the entry's sort bytes. */
static void
doltgres_entries_sorted_body(void *frame_)
{
	doltgres_entries_sorted_frame_t *frame = frame_;
	StdVec	   *out = frame->out;
	static const uint8 end[] = {0, 0};
	uint8		alive = frame->alive ? 1 : 0;

	std_vec_with_capacity_Checked(out, frame->key_len * 2 + frame->value_len + 64, 1, 1);
	frame->out_live = true;
	for (size_t i = 0; i < frame->key_len; i++)
	{
		static const uint8 escape = 255;

		std_vec_push_Checked(out, &frame->key[i], 1, 1);
		if (frame->key[i] == 0)
			std_vec_push_Checked(out, &escape, 1, 1);
	}
	std_vec_extend_from_slice_Checked(out, end, sizeof(end), 1, 1);
	doltgres_entries_sorted_word(out, frame->writer);
	doltgres_entries_sorted_word(out, frame->tuple);
	doltgres_entries_sorted_word(out, frame->top);
	std_vec_push_Checked(out, &alive, 1, 1);
	chunks_varint_put_varint(out, (uint64) frame->value_len);
	std_vec_extend_from_slice_Checked(out, frame->value, frame->value_len, 1, 1);
	chunks_varint_put_varint(out, (uint64) frame->addresses_len);
	for (size_t i = 0; i < frame->addresses_len; i++)
	{
		uint32		offset = frame->addresses[i].f0;
		uint8		le[sizeof(uint32)];

		for (size_t k = 0; k < sizeof(le); k++)
			le[k] = (uint8) (offset >> (8 * k));
		std_vec_extend_from_slice_Checked(out, le, sizeof(le), 1, 1);
		chunks_trailer_Locator_write(&frame->addresses[i].f1, out);
	}
	frame->out_live = false;
}

/*
 * doltgres_entries_sorted returns the bytes an index build's sort holds of an
 * entry: its key with each zero byte escaped as zero then 255 and ended by
 * two zeros, then its row's writer and tuple id big-endian, so the sort's
 * byte order is the keys' byte order and a key's copies come in PLAN's stamp
 * order, by writer and then tuple id as numbers, then its writer's top-level
 * xid big-endian, then whether the row is alive, then its value's length and
 * bytes and its addresses' count and each offset and locator.
 */
static void
doltgres_entries_sorted(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len,
						const chunks_TupleU32Locator_t *addresses, size_t addresses_len, uint64 writer, uint64 tuple,
						uint64 top, bool alive, StdVec *result)
{
	doltgres_entries_sorted_frame_t frame = {.key = key, .key_len = key_len, .value = value, .value_len = value_len,
		.addresses = addresses, .addresses_len = addresses_len, .writer = writer, .tuple = tuple, .top = top,
	.alive = alive, .out = result};

	std_landing(&frame, doltgres_entries_sorted_body, doltgres_entries_sorted_pad);
	doltgres_entries_sorted_pad(&frame);
}

/*
 * doltgres_entries_Sorted is Sorted: an entry as an index build's sort gives
 * it back: its key, value, and addresses, and its row's writer, tuple id,
 * writer's top-level xid, zero for a base row's, and whether it is alive.
 */
typedef struct doltgres_entries_Sorted
{
	StdVec		key;
	StdVec		value;
	StdVec		addresses;
	uint64		writer;
	uint64		tuple;
	uint64		top;
	bool		alive;
} doltgres_entries_Sorted;

/* doltgres_entries_Sorted_drop is Sorted's drop: its key's, its value's, and its addresses'. */
static void
doltgres_entries_Sorted_drop(doltgres_entries_Sorted *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	std_vec_free_Checked(&self->value, 1, 1);
	std_vec_free_Checked(&self->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
}

/*
 * doltgres_entries_unsorted_frame_t is unsorted's state across its pad: the
 * entry's key, value, and addresses as it reads them, each while live.
 */
typedef struct doltgres_entries_unsorted_frame_t
{
	bool		key_live;
	bool		value_live;
	bool		addresses_live;
	const uint8 *bytes;
	size_t		len;
	doltgres_entries_Sorted *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_unsorted_frame_t;

/* doltgres_entries_unsorted_pad frees what unsorted holds. */
static void
doltgres_entries_unsorted_pad(void *frame_)
{
	doltgres_entries_unsorted_frame_t *frame = frame_;

	if (frame->addresses_live)
		std_vec_free_Checked(&frame->result->addresses, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
	if (frame->value_live)
		std_vec_free_Checked(&frame->result->value, 1, 1);
	if (frame->key_live)
		std_vec_free_Checked(&frame->result->key, 1, 1);
	frame->addresses_live = frame->value_live = frame->key_live = false;
}

/*
 * doltgres_entries_unsorted_closure_1 is unsorted's word: the big-endian word
 * at a place, if the bytes hold one there.
 */
static bool
doltgres_entries_unsorted_closure_1(const uint8 *bytes, size_t len, size_t at, uint64 *result)
{
	if (at > len || len - at < sizeof(uint64))
		return false;
	*result = 0;
	for (size_t i = 0; i < sizeof(uint64); i++)
		*result = *result << 8 | bytes[at + i];
	return true;
}

/*
 * doltgres_entries_unsorted_closure_0 is unsorted's broken: the error of bytes
 * the build's sort gave back that sorted didn't write.
 */
static PgPgError
doltgres_entries_unsorted_closure_0(void)
{
	static const char message[] = "an index build's sort gave back bytes it wasn't given";

	return doltgres_entries_internal(message, sizeof(message) - 1);
}

/* doltgres_entries_unsorted_body reads the entry's key, words, value, and addresses back. */
static void
doltgres_entries_unsorted_body(void *frame_)
{
	doltgres_entries_unsorted_frame_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		len = frame->len;
	doltgres_entries_Sorted *result = frame->result;
	size_t		at = 0;
	size_t		width = 4 + CHUNKS_TRAILER_LOCATOR_LEN;
	uint64		value_len;
	uint64		count;
	ChunksVarintMalformed malformed;

	std_vec_new(&result->key, 1, 1);
	frame->key_live = true;
	for (;;)
	{
		if (at < len && bytes[at] == 0 && at + 1 < len && bytes[at + 1] == 0)
			break;
		if (at < len && bytes[at] == 0 && at + 1 < len && bytes[at + 1] == 255)
		{
			std_vec_push_Checked(&result->key, &bytes[at], 1, 1);
			at += 2;
		}
		else if (at < len && bytes[at] != 0)
		{
			std_vec_push_Checked(&result->key, &bytes[at], 1, 1);
			at += 1;
		}
		else
		{
			*frame->error = doltgres_entries_unsorted_closure_0();
			return;
		}
	}
	if (!doltgres_entries_unsorted_closure_1(bytes, len, at + 2, &result->writer) ||
		!doltgres_entries_unsorted_closure_1(bytes, len, at + 10, &result->tuple) ||
		!doltgres_entries_unsorted_closure_1(bytes, len, at + 18, &result->top) || at + 26 >= len)
	{
		*frame->error = doltgres_entries_unsorted_closure_0();
		return;
	}
	result->alive = bytes[at + 26] != 0;
	at += 27;
	if (!chunks_varint_decode(bytes, len, &at, &value_len, &malformed) || value_len > SIZE_MAX)
	{
		*frame->error = doltgres_entries_unsorted_closure_0();
		return;
	}
	if (STD_ADD(usize, at, (size_t) value_len, 6010, 31) > len)
	{
		*frame->error = doltgres_entries_unsorted_closure_0();
		return;
	}
	std_slice_to_vec_Checked(&result->value, bytes + at, (size_t) value_len, 1, 1);
	frame->value_live = true;
	at += (size_t) value_len;
	if (!chunks_varint_decode(bytes, len, &at, &count, &malformed))
	{
		*frame->error = doltgres_entries_unsorted_closure_0();
		return;
	}
	std_vec_new(&result->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	frame->addresses_live = true;
	for (uint64 i = 0; i < count; i++)
	{
		chunks_TupleU32Locator_t address;

		if (len - at < width)
		{
			*frame->error = doltgres_entries_unsorted_closure_0();
			return;
		}
		address.f0 = (uint32) bytes[at] | (uint32) bytes[at + 1] << 8 | (uint32) bytes[at + 2] << 16 |
			(uint32) bytes[at + 3] << 24;
		address.f1 = chunks_trailer_Locator_read(bytes + at + 4, width - 4);
		std_vec_push_Checked(&result->addresses, &address, sizeof(chunks_TupleU32Locator_t),
							 _Alignof(chunks_TupleU32Locator_t));
		at += width;
	}
	if (at != len)
	{
		*frame->error = doltgres_entries_unsorted_closure_0();
		return;
	}
	frame->key_live = frame->value_live = frame->addresses_live = false;
	frame->ok = true;
}

/* doltgres_entries_unsorted reads back an entry sorted wrote, refusing as an internal error bytes it didn't write. */
static bool
doltgres_entries_unsorted(const uint8 *bytes, size_t len, doltgres_entries_Sorted *result, PgPgError *error)
{
	doltgres_entries_unsorted_frame_t frame = {.bytes = bytes, .len = len, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_unsorted_body, doltgres_entries_unsorted_pad);
	doltgres_entries_unsorted_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_next_sorted_frame_t is next_sorted's state across its pad: the sort's bytes, while live. */
typedef struct doltgres_entries_next_sorted_frame_t
{
	StdVec		bytes;
	bool		bytes_live;
	const PgCatalogCaps *caps;
	PgOwnedSort *sort;
	bool	   *has_result;
	doltgres_entries_Sorted *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_next_sorted_frame_t;

/* doltgres_entries_next_sorted_pad frees what next_sorted holds. */
static void
doltgres_entries_next_sorted_pad(void *frame_)
{
	doltgres_entries_next_sorted_frame_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->bytes_live = false;
}

/* doltgres_entries_next_sorted_body reads the sort's next entry's bytes and the entry from them. */
static void
doltgres_entries_next_sorted_body(void *frame_)
{
	doltgres_entries_next_sorted_frame_t *frame = frame_;
	PgSort	   *sort = pg_OwnedSort_sort(frame->sort);
	int64		len;

	*frame->has_result = false;
	if (!pg_calls_sort_next_len(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps), sort,
								&len, frame->error))
		return;
	if (len < 0)
	{
		frame->ok = true;
		return;
	}
	std_vec_from_elem_zeroed(&frame->bytes, (size_t) len, 1, 1);
	frame->bytes_live = true;
	pg_calls_sort_read(sort, frame->bytes.ptr, frame->bytes.len);
	if (!doltgres_entries_unsorted(frame->bytes.ptr, frame->bytes.len, frame->result, frame->error))
		return;
	*frame->has_result = true;
	frame->ok = true;
}

/* doltgres_entries_next_sorted returns the next entry an index build's sort gives back, none past its last. */
static bool
doltgres_entries_next_sorted(const PgCatalogCaps *caps, PgOwnedSort *sort, bool *has_result,
							 doltgres_entries_Sorted *result, PgPgError *error)
{
	doltgres_entries_next_sorted_frame_t frame = {.caps = caps, .sort = sort, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_next_sorted_body, doltgres_entries_next_sorted_pad);
	doltgres_entries_next_sorted_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_late_entry returns the change an index build pushes as a
 * delta entry version for a row whose top-level writer is at or past the
 * build's horizon (the fold text's draft 12), under that writer, so the built
 * base folds no transaction a snapshot counts as running and an older
 * snapshot reads the entry through the delta as the writer's. It takes the
 * key and the op.
 */
static void
doltgres_entries_late_entry(const PgRel *index, StdVec *key, TxnMemtableOp *op, TxnMemtableVersionWriter writer,
							TxnRecordRowChange *result)
{
	memset(result, 0, sizeof(*result));
	result->writer = writer;
	result->storage = doltgres_entries_storage(index);
	result->table = pg_calls_index_table(index);
	result->key = *key;
	result->op = *op;
	std_vec_new(&result->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_new(&result->kept, sizeof(size_t), _Alignof(size_t));
	result->has_index = true;
	result->index = pg_calls_rel_id(index);
}

/* doltgres_entries_StampTop is a (Stamp, u64): a copy's stamp and its writer's top-level xid. */
typedef struct doltgres_entries_StampTop
{
	ChunksTrailerStamp f0;
	uint64		f1;
} doltgres_entries_StampTop;

/* doltgres_entries_RowChange_drop is RowChange's drop glue, an element of the late versions. */
static void
doltgres_entries_RowChange_drop(void *change)
{
	txn_record_RowChange_drop(change);
}

/* doltgres_entries_Published_drop is Published's drop glue, as a Vec's element takes it. */
static void
doltgres_entries_Published_drop(void *published)
{
	trees_published_Published_drop(published);
}

/*
 * doltgres_entries_loaded_closure_0 is loaded's first lent call, which only
 * takes the index storage's state into the transaction.
 */
static bool
doltgres_entries_loaded_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  HostError *error)
{
	return true;
}

/* doltgres_entries_loaded_closure_5 is the early copies' map: a copy's stamp, its top-level xid left out. */
static void
doltgres_entries_loaded_closure_5(void *elem, void *result)
{
	*(ChunksTrailerStamp *) result = ((doltgres_entries_StampTop *) elem)->f0;
}

/*
 * doltgres_entries_loaded_frame_t is loaded's state across its pad: the
 * records' sort, the index's key order, its storage's held state, its newest
 * base, the writer, the tree's shape, the builder, the sort's pending,
 * first, and next entries, a key's copies, its early and late copies, the
 * late versions, a late copy's op as it is cloned, an entry being added, the
 * records' identity and bytes, and the installed base's published roots and
 * packs, each while live.
 */
typedef struct doltgres_entries_loaded_frame_t
{
	PgOwnedSort records;
	bool		records_live;
	TreesKeyOrder order;
	bool		order_live;
	HostSharedHeld held;
	bool		held_live;
	TxnBaseBase base;
	bool		base_live;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	TreesShape	shape;
	bool		shape_live;
	TreesStoreBrandedBuilder builder;
	bool		builder_live;
	doltgres_entries_Sorted pending;
	bool		pending_live;
	doltgres_entries_Sorted first;
	bool		first_live;
	doltgres_entries_Sorted next;
	bool		next_live;
	StdVec		copies;
	bool		copies_live;
	StdVec		early;
	bool		early_live;
	StdVec		later;
	bool		later_live;
	StdVec		late;
	bool		late_live;
	TxnMemtableOp late_op;
	bool		late_op_live;
	TreesBuildEntry entry;
	bool		entry_live;
	StdVec		identity;
	bool		identity_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		published;
	bool		published_live;
	StdVec		packs;
	bool		packs_live;
	TxnBaseBase installed;
	bool		installed_live;
	size_t		root;
	HostLsn		point;
	uint64		max_folded;
	TxnBaseSizes sizes;
	uint64		generation;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *heap;
	const PgRel *index;
	PgIndexInfo *info;
	PgOwnedSort *sort;
	const DoltgresIndexDescribed *described;
	uint64		horizon;
	uint64		based;
	uint64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_loaded_frame_t;

/* doltgres_entries_loaded_pad frees what loaded holds. */
static void
doltgres_entries_loaded_pad(void *frame_)
{
	doltgres_entries_loaded_frame_t *frame = frame_;

	if (frame->installed_live)
		txn_base_Base_drop(&frame->installed);
	if (frame->packs_live)
		std_vec_free_Checked(&frame->packs, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	if (frame->published_live)
		std_vec_drop_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished),
							 doltgres_entries_Published_drop);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->identity_live)
		std_vec_free_Checked(&frame->identity, 1, 1);
	if (frame->entry_live)
		trees_build_Entry_drop(&frame->entry);
	if (frame->late_op_live)
		txn_memtable_Op_drop(&frame->late_op);
	if (frame->late_live)
		std_vec_drop_Checked(&frame->late, sizeof(TxnRecordRowChange), _Alignof(TxnRecordRowChange),
							 doltgres_entries_RowChange_drop);
	if (frame->later_live)
		std_vec_free_Checked(&frame->later, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	if (frame->early_live)
		std_vec_free_Checked(&frame->early, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	if (frame->copies_live)
		std_vec_free_Checked(&frame->copies, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	if (frame->next_live)
		doltgres_entries_Sorted_drop(&frame->next);
	if (frame->first_live)
		doltgres_entries_Sorted_drop(&frame->first);
	if (frame->pending_live)
		doltgres_entries_Sorted_drop(&frame->pending);
	if (frame->builder_live)
		trees_build_Builder_drop(&frame->builder.builder);
	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->records_live)
		pg_OwnedSort_drop(&frame->records);
	frame->installed_live = frame->packs_live = frame->published_live = frame->bytes_live = false;
	frame->identity_live = frame->entry_live = frame->late_op_live = frame->late_live = frame->later_live = false;
	frame->early_live = false;
	frame->copies_live = frame->next_live = frame->first_live = frame->pending_live = frame->builder_live = false;
	frame->shape_live = frame->writer_live = frame->base_live = frame->held_live = frame->order_live = false;
	frame->records_live = false;
}

/* doltgres_entries_loaded_failed makes a storage's error loaded's. */
static void
doltgres_entries_loaded_failed(doltgres_entries_loaded_frame_t *frame, ChunksErrorError *failed)
{
	*frame->error = doltgres_entries_storage_error(failed);
}

/* doltgres_entries_loaded_done counts one more loaded row in the build's progress. */
static void
doltgres_entries_loaded_done(int64 *done)
{
	*done += 1;
	pg_calls_progress_update(PROGRESS_CREATEIDX_TUPLES_DONE, *done);
}

/* doltgres_entries_loaded_copy starts a key's copies with one row's stamp. */
static void
doltgres_entries_loaded_copy(StdVec *copies, const doltgres_entries_Sorted *row)
{
	doltgres_entries_StampTop copy = {{row->writer, row->tuple}, row->top};

	std_vec_with_capacity_Checked(copies, 1, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	std_vec_push_Checked(copies, &copy, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
}

/*
 * doltgres_entries_loaded_reserved reserves room for one more late version,
 * refusing out of memory as an internal error.
 */
static bool
doltgres_entries_loaded_reserved(doltgres_entries_loaded_frame_t *frame)
{
	static const char message[] = "out of memory";

	if (std_vec_try_reserve(&frame->late, 1, sizeof(TxnRecordRowChange), _Alignof(TxnRecordRowChange)))
		return true;
	*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
	return false;
}

/* doltgres_entries_loaded_late pushes a late version of an entry, which it takes, under a copy's writer. */
static void
doltgres_entries_loaded_late(doltgres_entries_loaded_frame_t *frame, StdVec *key, TxnMemtableOp *op,
							 ChunksTrailerStamp copy, uint64 top)
{
	TxnMemtableVersionWriter writer = {.xid = copy.txn, .has_top = top != copy.txn, .top = top};
	TxnRecordRowChange change;

	doltgres_entries_late_entry(frame->index, key, op, writer, &change);
	std_vec_push_Checked(&frame->late, &change, sizeof(TxnRecordRowChange), _Alignof(TxnRecordRowChange));
}

/* doltgres_entries_loaded_added adds the frame's entry to the tree, counting its sizes first, its rows as given. */
static bool
doltgres_entries_loaded_added(doltgres_entries_loaded_frame_t *frame, size_t rows)
{
	ChunksErrorError failed;

	frame->sizes.entries += 1;
	frame->sizes.rows += rows;
	frame->sizes.key_bytes += frame->entry.key.len;
	frame->sizes.leaf_bytes += trees_chunker_leaf_entry_size(frame->entry.key.ptr, frame->entry.key.len,
															 frame->entry.value.ptr, frame->entry.value.len);
	frame->entry_live = false;
	if (!trees_build_Builder_add(&frame->builder.builder, frame->io, &frame->entry, &failed))
	{
		doltgres_entries_loaded_failed(frame, &failed);
		return false;
	}
	return true;
}

/* doltgres_entries_loaded_empty makes the frame's entry empty and live, for its parts to be filled in. */
static void
doltgres_entries_loaded_empty(doltgres_entries_loaded_frame_t *frame)
{
	std_vec_new(&frame->entry.key, 1, 1);
	std_vec_new(&frame->entry.value, 1, 1);
	std_vec_new(&frame->entry.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&frame->entry.stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	std_vec_new(&frame->entry.values, sizeof(uint32), _Alignof(uint32));
	frame->entry_live = true;
}

/*
 * doltgres_entries_loaded_copies reads the sort's rows of the first entry's
 * key after it: a unique index's equal keys refused when both are alive, else
 * the live row's kept, and a keyless table's copies gathered; it leaves the
 * row after them pending.
 */
static bool
doltgres_entries_loaded_copies(doltgres_entries_loaded_frame_t *frame, bool keyless, bool unique, bool checked,
							   bool has_equal, TxnInsertEqual equal, int64 *done, bool *refused)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);

	*refused = false;
	for (;;)
	{
		bool		same = false;
		bool		same_key;
		HostError	failed;

		if (!doltgres_entries_next_sorted(frame->caps, frame->sort, &frame->next_live, &frame->next, frame->error))
			return false;
		if (frame->next_live && has_equal)
		{
			if (!equal.ops->equal(equal.self, frame->io, frame->next.key.ptr, frame->next.key.len,
								  frame->first.key.ptr, frame->first.key.len, &same, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return false;
			}
		}
		same_key = frame->next_live && frame->next.key.len == frame->first.key.len &&
			(frame->first.key.len == 0 || memcmp(frame->next.key.ptr, frame->first.key.ptr, frame->first.key.len) == 0);
		if (frame->next_live && !has_equal)
			same = same_key;
		if (frame->next_live && unique && same)
		{
			if (checked && frame->first.alive && frame->next.alive)
			{
				uint64		named = Min(frame->first.tuple, frame->next.tuple);

				*refused = true;
				*frame->result = 0;
				return pg_calls_refuse_unique_build(raise, pg_CatalogCaps_catalog(frame->caps), frame->heap,
													pg_calls_rel_id(frame->index), named, frame->error);
			}
			doltgres_entries_loaded_done(done);
			if (frame->next.alive && !frame->first.alive)
			{
				StdVec		copies;

				doltgres_entries_loaded_copy(&copies, &frame->next);
				std_vec_free_Checked(&frame->copies, sizeof(doltgres_entries_StampTop),
									 _Alignof(doltgres_entries_StampTop));
				frame->copies = copies;
				frame->first_live = false;
				doltgres_entries_Sorted_drop(&frame->first);
				frame->first = frame->next;
				frame->first_live = true;
			}
			else
				doltgres_entries_Sorted_drop(&frame->next);
			frame->next_live = false;
			continue;
		}
		if (frame->next_live && keyless && same_key)
		{
			doltgres_entries_StampTop copy = {{frame->next.writer, frame->next.tuple}, frame->next.top};

			std_vec_push_Checked(&frame->copies, &copy, sizeof(doltgres_entries_StampTop),
								 _Alignof(doltgres_entries_StampTop));
			doltgres_entries_loaded_done(done);
			frame->next_live = false;
			doltgres_entries_Sorted_drop(&frame->next);
			continue;
		}
		frame->pending = frame->next;
		frame->pending_live = frame->next_live;
		frame->next_live = false;
		return true;
	}
}

/* doltgres_entries_loaded_recorded puts each copy's record in the records' sort, when the index keeps records. */
static bool
doltgres_entries_loaded_recorded(doltgres_entries_loaded_frame_t *frame)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);

	for (size_t i = 0; frame->records_live && i < frame->copies.len; i++)
	{
		const doltgres_entries_StampTop *copy = &STD_VEC_AT(&frame->copies, doltgres_entries_StampTop, i);
		StdBytes	row_key;
		StdVec		record;

		if (!access_index_IndexKey_row_key_bytes(doltgres_index_Described_entry_key(frame->described),
												 frame->first.key.ptr, frame->first.key.len, frame->first.value.ptr,
												 frame->first.value.len, &row_key, frame->error))
			return false;
		doltgres_entries_record_identity(frame->described, row_key.ptr, row_key.len, copy->f0.tuple,
										 &frame->identity);
		frame->identity_live = true;
		txn_entry_record(frame->identity.ptr, frame->identity.len, &record);
		frame->identity_live = false;
		std_vec_free_Checked(&frame->identity, 1, 1);
		frame->identity = record;
		frame->identity_live = true;
		doltgres_entries_sorted(frame->identity.ptr, frame->identity.len, frame->first.key.ptr, frame->first.key.len,
								NULL, 0, copy->f0.txn, copy->f0.tuple, copy->f1, true, &frame->bytes);
		frame->bytes_live = true;
		if (!pg_calls_sort_put(raise, pg_calls_sort_state(pg_OwnedSort_sort(&frame->records)), frame->bytes.ptr,
							   frame->bytes.len, frame->error))
			return false;
		frame->bytes_live = frame->identity_live = false;
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->identity, 1, 1);
	}
	return true;
}

/*
 * doltgres_entries_loaded_entry loads the first entry's key: its late copies
 * as delta versions and its early copies as one entry of the tree, a keyless
 * table's copies counted in it.
 */
static bool
doltgres_entries_loaded_entry(doltgres_entries_loaded_frame_t *frame, bool keyless)
{
	bool		has_newest = false;
	uint64		newest = 0;
	HostError	failed;

	std_vec_new(&frame->early, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	frame->early_live = true;
	std_vec_new(&frame->later, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	frame->later_live = true;
	for (size_t i = 0; i < frame->copies.len; i++)
	{
		const doltgres_entries_StampTop *copy = &STD_VEC_AT(&frame->copies, doltgres_entries_StampTop, i);

		std_vec_push_Checked(copy->f1 < frame->horizon ? &frame->early : &frame->later, copy,
							 sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	}
	frame->copies_live = false;
	std_vec_free_Checked(&frame->copies, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
	for (size_t i = 0; i < frame->later.len; i++)
	{
		const doltgres_entries_StampTop *copy = &STD_VEC_AT(&frame->later, doltgres_entries_StampTop, i);
		TxnMemtablePut *put = keyless ? &frame->late_op.u.add : &frame->late_op.u.put;
		StdVec		key;

		if (!txn_fold_kept_out(frame->io, copy->f1, frame->horizon, &frame->point, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		frame->late_op.tag = keyless ? TXN_MEMTABLE_OP_ADD : TXN_MEMTABLE_OP_PUT;
		std_vec_new(&put->value, 1, 1);
		std_vec_new(&put->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_new(&put->values, sizeof(uint32), _Alignof(uint32));
		put->tuple.f0 = keyless ? copy->f0.tuple : 0;
		put->heap_bytes = 0;
		frame->late_op_live = true;
		std_vec_clone(&put->value, &frame->first.value, 1, 1);
		std_vec_clone(&put->addresses, &frame->first.addresses, sizeof(chunks_TupleU32Locator_t),
					  _Alignof(chunks_TupleU32Locator_t));
		if (!doltgres_entries_loaded_reserved(frame))
			return false;
		std_vec_clone(&key, &frame->first.key, 1, 1);
		frame->late_op_live = false;
		doltgres_entries_loaded_late(frame, &key, &frame->late_op, copy->f0, copy->f1);
	}
	for (size_t i = 0; i < frame->early.len; i++)
	{
		uint64		top = STD_VEC_AT(&frame->early, doltgres_entries_StampTop, i).f1;
		uint64		at = top == 0 ? frame->based : top;

		if (!has_newest || at > newest)
			newest = at;
		has_newest = true;
	}
	if (!has_newest)
		return true;
	frame->max_folded = Max(frame->max_folded, newest);
	doltgres_entries_loaded_empty(frame);
	frame->early_live = false;
	std_vec_from_iter_in_place_map(&frame->early, sizeof(doltgres_entries_StampTop),
								   _Alignof(doltgres_entries_StampTop), NULL, sizeof(ChunksTrailerStamp), NULL,
								   doltgres_entries_loaded_closure_5, &frame->entry.stamps);
	if (keyless)
	{
		txn_counted_keyless_value(frame->entry.stamps.len, frame->first.value.ptr, frame->first.value.len,
								  &frame->entry.value);
		txn_counted_keyless_addresses(frame->entry.stamps.len, frame->first.addresses.ptr,
									  frame->first.addresses.len, &frame->entry.addresses);
	}
	else
	{
		frame->entry.value = frame->first.value;
		std_vec_new(&frame->first.value, 1, 1);
		frame->entry.addresses = frame->first.addresses;
		std_vec_new(&frame->first.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		std_vec_free_Checked(&frame->entry.stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		std_vec_new(&frame->entry.stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	}
	frame->entry.key = frame->first.key;
	std_vec_new(&frame->first.key, 1, 1);
	if (!doltgres_entries_loaded_added(frame, Max(frame->entry.stamps.len, 1)))
		return false;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-index-loaded", &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/*
 * doltgres_entries_loaded_records loads the records' sort after every entry, a
 * late writer's record as a delta version.
 */
static bool
doltgres_entries_loaded_records(doltgres_entries_loaded_frame_t *frame, bool keyless)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;

	if (!pg_calls_sort_performed(raise, pg_CatalogCaps_catalog(frame->caps), pg_OwnedSort_sort(&frame->records),
								 frame->error))
		return false;
	for (;;)
	{
		ChunksTrailerStamp copy;

		if (!doltgres_entries_next_sorted(frame->caps, &frame->records, &frame->next_live, &frame->next,
										  frame->error))
			return false;
		if (!frame->next_live)
			return true;
		copy.txn = frame->next.writer;
		copy.tuple = frame->next.tuple;
		if (frame->next.top >= frame->horizon)
		{
			txn_TupleVecU8Op_t change;

			if (!txn_fold_kept_out(frame->io, frame->next.top, frame->horizon, &frame->point, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return false;
			}
			if (!doltgres_entries_loaded_reserved(frame))
				return false;
			std_range_from_check(1, frame->next.key.len, RUST_FILE, 6190, 59);
			doltgres_entries_record_change(frame->described, (const uint8 *) frame->next.key.ptr + 1,
										   frame->next.key.len - 1, frame->next.tuple, &frame->next.value, &change);
			std_vec_new(&frame->next.value, 1, 1);
			doltgres_entries_loaded_late(frame, &change.f0, &change.f1, copy, frame->next.top);
			frame->next_live = false;
			doltgres_entries_Sorted_drop(&frame->next);
			continue;
		}
		frame->max_folded = Max(frame->max_folded, frame->next.top == 0 ? frame->based : frame->next.top);
		doltgres_entries_loaded_empty(frame);
		if (keyless)
		{
			txn_counted_keyless_value(1, frame->next.value.ptr, frame->next.value.len, &frame->entry.value);
			std_vec_with_capacity_Checked(&frame->entry.stamps, 1, sizeof(ChunksTrailerStamp),
										  _Alignof(ChunksTrailerStamp));
			std_vec_push_Checked(&frame->entry.stamps, &copy, sizeof(ChunksTrailerStamp),
								 _Alignof(ChunksTrailerStamp));
		}
		else
		{
			frame->entry.value = frame->next.value;
			std_vec_new(&frame->next.value, 1, 1);
		}
		frame->entry.key = frame->next.key;
		std_vec_new(&frame->next.key, 1, 1);
		if (!doltgres_entries_loaded_added(frame, 0))
			return false;
		frame->next_live = false;
		doltgres_entries_Sorted_drop(&frame->next);
	}
}

/*
 * doltgres_entries_loaded_closure_7 is loaded's register run: the tree's root
 * held at a slot and published, and the base installed.
 */
static bool
doltgres_entries_loaded_closure_7(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								  void *result, ChunksErrorError *error)
{
	doltgres_entries_loaded_frame_t *frame = captures;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, registering};
	ChunksPacksRootsRootSlot *slot;
	bool		installed;
	HostError	failed;

	chunks_packs_roots_RootSlots_one(slots, &slot);
	if (!trees_store_BrandedWriter_hold(&frame->writer, io, slot, frame->root, error))
		return false;
	frame->writer_live = false;
	if (!trees_store_BrandedWriter_publish(&frame->writer, io, &frame->published, &frame->packs, error))
		return false;
	frame->published_live = frame->packs_live = true;
	frame->installed = frame->base;
	frame->base_live = false;
	frame->installed_live = true;
	frame->installed.has_root = true;
	frame->installed.root =
		trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished, 0));
	frame->installed.folded = frame->point;
	frame->installed.read = frame->point;
	frame->installed.max_folded = frame->max_folded;
	frame->installed.sizes = frame->sizes;
	if (!txn_state_installed(io, &frame->held, frame->generation, &frame->installed, &installed, &failed))
	{
		*error = chunks_error_Error_FromError_from(&failed);
		return false;
	}
	*(bool *) result = installed;
	if (installed)
		txn_state_grown(host_shared_Held_record(&frame->held), &frame->installed);
	else
	{
		frame->installed_live = frame->packs_live = false;
		txn_base_Base_drop(&frame->installed);
		std_vec_free_Checked(&frame->packs, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	}
	frame->published_live = false;
	std_vec_drop_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished),
						 doltgres_entries_Published_drop);
	return true;
}

/* doltgres_entries_loaded_advanced logs the installed base as advanced, for a permanent index storage. */
static bool
doltgres_entries_loaded_advanced(doltgres_entries_loaded_frame_t *frame)
{
	HostStorage storage;
	TxnStatePersistence persistence;
	TxnRecordBaseAdvanced advanced = {0};
	StdVec		body;
	HostLsn		lsn;
	ChunksErrorError failed;
	bool		ok;

	if (!txn_state_described(host_shared_Held_record(&frame->held), &storage, &persistence) ||
		persistence != TXN_STATE_PERSISTENCE_PERMANENT)
		return true;
	if (!chunks_packs_backend_Packs_flush(&frame->repo->packs, frame->io, frame->reading, &failed))
	{
		doltgres_entries_loaded_failed(frame, &failed);
		return false;
	}
	{
		HostError	host;

		if (!host_wal_flush(HOST_IO_IO_IO_DOORS(frame->io), frame->point, &host))
		{
			*frame->error = pghost_raised(&host);
			return false;
		}
	}
	advanced.storage = storage;
	advanced.folded = frame->point;
	advanced.max_folded = frame->max_folded;
	advanced.base = frame->installed;
	advanced.packs = frame->packs;
	frame->installed_live = frame->packs_live = false;
	txn_record_BaseAdvanced_encode(&advanced, &body);
	{
		HostError	host;

		ok = host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_BASE_ADVANCED, body.ptr, body.len, &lsn,
							 &host);
		std_vec_free_Checked(&body, 1, 1);
		txn_record_BaseAdvanced_drop(&advanced);
		if (!ok)
		{
			*frame->error = pghost_raised(&host);
			return false;
		}
	}
	return true;
}

/*
 * doltgres_entries_loaded_body writes the sorted entries into a tree, installs
 * it as the base, and carries the late versions.
 */
static void
doltgres_entries_loaded_body(void *frame_)
{
	doltgres_entries_loaded_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	bool		checked = pg_calls_index_info_unique(frame->info);
	bool		keyless = doltgres_index_Described_keyless(frame->described);
	bool		unique = doltgres_index_Described_unique(frame->described);
	TxnStateFound made;
	bool		some;
	HostLsn		position;
	int64		done = 0;
	ChunksErrorError failed;
	HostError	host;
	TxnInsertEqual equal;
	bool		has_equal;
	bool		installed;
	size_t		late_rows;
	TreesStoreReader store;
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

	doltgres_index_Described_order(frame->described, NULL, &frame->order);
	frame->order_live = true;
	if (doltgres_index_Described_records(frame->described))
	{
		if (!pg_calls_sort_begin(raise, &frame->records, frame->error))
			return;
		frame->records_live = true;
	}
	if (!doltgres_entries_index_making(frame->io, frame->index, &made, frame->error))
		return;
	if (!txn_state_held_Unit(frame->io, frame->repo, frame->reading, made, doltgres_entries_loaded_closure_0, NULL,
							 NULL, &host))
	{
		*frame->error = pghost_raised(&host);
		return;
	}
	{
		HostStorage storage = doltgres_entries_storage(frame->index);

		if (!txn_state_state(frame->io, frame->repo, frame->reading, &storage,
							 doltgres_entries_persistence(frame->index), false, NULL, &frame->held, &host))
		{
			*frame->error = pghost_raised(&host);
			return;
		}
	}
	frame->held_live = true;
	frame->generation = txn_state_generation(host_shared_Held_record(&frame->held));
	if (!txn_state_newest(frame->io, &frame->held, &some, &frame->base, &host))
	{
		*frame->error = pghost_raised(&host);
		return;
	}
	if (!some)
	{
		static const char message[] = "an index build found its storage's state with no base";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	frame->base_live = true;
	if (!host_fold_point(HOST_IO_IO_IO_DOORS(frame->io), &position, &host) ||
		!txn_status_lowered(frame->io, position, &frame->point, &host))
	{
		*frame->error = pghost_raised(&host);
		return;
	}
	std_vec_new(&frame->late, sizeof(TxnRecordRowChange), _Alignof(TxnRecordRowChange));
	frame->late_live = true;
	if (!trees_store_Writer_new(frame->io, repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading),
								CHUNKS_PACKS_DEFAULT_SPACE, &frame->plain, &failed))
	{
		doltgres_entries_loaded_failed(frame, &failed);
		return;
	}
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	frame->shape.has_summarizer = false;
	trees_KeyOrder_Clone_clone(&frame->order, &frame->shape.order);
	frame->shape_live = true;
	trees_store_BrandedWriter_shaped(&frame->writer, TREES_CHUNKER_DEFAULT_TARGET, &frame->shape, &frame->builder);
	frame->builder_live = true;
	has_equal = doltgres_layout_equal(&frame->order, &equal);
	if (!doltgres_entries_next_sorted(frame->caps, frame->sort, &frame->pending_live, &frame->pending, frame->error))
		return;
	while (frame->pending_live)
	{
		bool		refused;

		frame->first = frame->pending;
		frame->first_live = true;
		frame->pending_live = false;
		doltgres_entries_loaded_copy(&frame->copies, &frame->first);
		frame->copies_live = true;
		doltgres_entries_loaded_done(&done);
		if (!doltgres_entries_loaded_copies(frame, keyless, unique, checked, has_equal, equal, &done, &refused))
			return;
		if (refused)
		{
			frame->ok = true;
			return;
		}
		if (!doltgres_entries_loaded_recorded(frame) || !doltgres_entries_loaded_entry(frame, keyless))
			return;
		if (frame->early_live)
		{
			frame->early_live = false;
			std_vec_free_Checked(&frame->early, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
		}
		frame->later_live = false;
		std_vec_free_Checked(&frame->later, sizeof(doltgres_entries_StampTop), _Alignof(doltgres_entries_StampTop));
		frame->first_live = false;
		doltgres_entries_Sorted_drop(&frame->first);
	}
	late_rows = frame->late.len;
	if (frame->records_live && !doltgres_entries_loaded_records(frame, keyless))
		return;
	frame->builder_live = false;
	if (!trees_store_BrandedBuilder_finish(&frame->builder, frame->io, &frame->root, &failed) ||
		!chunks_packs_roots_RootSlots_register(frame->io, doltgres_entries_loaded_closure_7, frame, NULL, &installed,
											   &failed))
	{
		doltgres_entries_loaded_failed(frame, &failed);
		return;
	}
	if (!installed)
	{
		static const char message[] = "an index build's install found its storage's base moved";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	if (!doltgres_entries_loaded_advanced(frame))
		return;
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	{
		StdVec		late;
		bool		ok;

		std_vec_with_capacity_Checked(&late, frame->late.len, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry));
		for (size_t i = 0; i < frame->late.len; i++)
		{
			TxnRewriteCarry carry = {STD_VEC_AT(&frame->late, TxnRecordRowChange, i), shown,
			TXN_MEMTABLE_LOCK_MODE_UPDATE};

			std_vec_push_Checked(&late, &carry, sizeof(carry), _Alignof(TxnRewriteCarry));
		}
		ok = txn_rewrite_carried(frame->io, TREES_STORE_READER_NODE_STORE(&store), &frame->held, NULL, 0, late.ptr,
								 late.len, pg_calls_rel_permanent(frame->index), NULL,
								 pg_calls_max_heap_tuples_per_page(), &host);
		std_vec_free_Checked(&late, sizeof(TxnRewriteCarry), _Alignof(TxnRewriteCarry));
		if (!ok)
		{
			*frame->error = pghost_raised(&host);
			return;
		}
	}
	*frame->result = frame->sizes.rows + late_rows;
	frame->ok = true;
}

/*
 * doltgres_entries_loaded writes an index build's sorted entries into a tree
 * bottom up and installs it as the index's base (the btree text's section 6
 * and PLAN's format row for an index storage's base leaf entry), as a fold
 * installs a base: a keyed table's entries with no stamp, and a keyless
 * table's equal keys, its row's copies, as one entry counting them, with a
 * stamp of each copy's writer and tuple id. The base names the index's table
 * and itself, holds as its maxFolded the newest top-level writer of its rows,
 * each below the build's horizon, its table's (the fold text's draft 12), a
 * row of the table's base counting as that base's maxFolded (based), raises
 * the state's totals by its sizes, and is logged with BASE_ADVANCED for a
 * permanent index; a row whose top-level writer is at or past the horizon
 * goes into the delta as an entry version under its writer instead
 * (late_entry), its value and addresses cloned for each such copy, since each
 * version owns its own, so no snapshot meets a base folded past it. The base
 * is folded at the fold point the build reads, lowered below the commit end
 * of each such writer (kept_out), so no fold's drop takes its versions out as
 * the base's. It counts each row it loads, a keyless table's copies each, in
 * the build's progress, as nbtree counts its index tuples, and pauses after
 * each entry it writes (doltgres-index-loaded). A unique index's two rows
 * with equal keys, both alive, refuse the build with nbtree's 23505
 * (refuse_unique_build), naming the one with the lower tuple id, as nbtree's
 * sort meets the earlier of the two in its heap first, when the build checks
 * uniqueness (ii_Unique, which a reindex that skips constraint checks
 * clears, as CLUSTER's and VACUUM FULL's do, leaving a bad index invalid);
 * when one isn't alive, as nbtree checks only its live rows, the key's entry
 * is the live row's, or the first's when neither is or the build checks
 * nothing. An index that keeps records gets each loaded row's record after
 * every entry, sorted by their keys in a sort of their own and written as
 * their entries are, in the base or, under a writer at or past the horizon,
 * in the delta, counted as no row. It answers the rows its entries hold.
 */
static bool
doltgres_entries_loaded(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						const HostSharedReader *reading, const PgRel *heap, const PgRel *index, PgIndexInfo *info,
						PgOwnedSort *sort, const DoltgresIndexDescribed *described, uint64 horizon, uint64 based,
						uint64 *result, PgPgError *error)
{
	doltgres_entries_loaded_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .heap = heap,
		.index = index, .info = info, .sort = sort, .described = described, .horizon = horizon, .based = based,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_loaded_body, doltgres_entries_loaded_pad);
	doltgres_entries_loaded_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_index_build_closure_1 is the build's lent call: the table storage's newest base. */
static bool
doltgres_entries_doltgres_index_build_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo,
												const HostSharedHeld *held, bool *some, TxnBaseBase *result,
												HostError *error)
{
	return txn_state_newest(io, held, some, result, error);
}

/*
 * doltgres_entries_doltgres_index_build_frame_t is the build's state across
 * its pad: what it opened, the table's recorded key and its columns, the
 * index's description, and the build's sort, each while live.
 */
typedef struct doltgres_entries_doltgres_index_build_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseKey	key;
	bool		key_live;
	StdVec		table_key;
	bool		table_key_live;
	StdArcInner *described;
	bool		described_live;
	PgOwnedSort sort;
	bool		sort_live;
	doltgres_entries_BuildSnapshot built;
	bool		built_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *heap;
	const PgRel *index;
	PgIndexInfo *info;
	PgBuildResult *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_build_frame_t;

/* doltgres_entries_doltgres_index_build_pad frees what the build holds. */
static void
doltgres_entries_doltgres_index_build_pad(void *frame_)
{
	doltgres_entries_doltgres_index_build_frame_t *frame = frame_;

	if (frame->sort_live)
		pg_OwnedSort_drop(&frame->sort);
	if (frame->described_live)
		std_arc_drop(frame->described, sizeof(DoltgresIndexDescribed), _Alignof(DoltgresIndexDescribed),
					 doltgres_entries_Described_drop);
	if (frame->table_key_live)
		doltgres_layout_VecKeyed_drop(&frame->table_key);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	if (frame->built_live)
		doltgres_entries_BuildSnapshot_drop(&frame->built);
	frame->sort_live = frame->described_live = frame->table_key_live = frame->key_live = frame->built_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_build_kept moves a concurrent build's snapshot into BUILD_SNAPSHOTS once the build succeeds. */
static void
doltgres_entries_doltgres_index_build_kept(doltgres_entries_doltgres_index_build_frame_t *frame)
{
	if (!frame->ok || !frame->built_live)
		return;
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILD_SNAPSHOTS.local, &DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_TYPE,
							&DOLTGRES_ENTRIES_BUILD_SNAPSHOTS_LOCK_TYPE, doltgres_entries_build_snapshot_kept_op,
							&frame->built);
	frame->built_live = false;
}

/* doltgres_entries_doltgres_index_build_body scans the table into the build's sort and loads the sorted entries. */
static void
doltgres_entries_doltgres_index_build_body(void *frame_)
{
	doltgres_entries_doltgres_index_build_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	const RepoRepo *repo;
	uint32		command;
	TxnBaseBase recorded;
	DoltgresIndexDescribed describing;
	const DoltgresIndexDescribed *described;
	bool		scanned;
	float8		heap_tuples;
	doltgres_entries_OptionDescribing taken;
	uint64		put;
	uint64		horizon;
	TxnStateFound made;
	bool		some;
	TxnBaseBase base;
	uint64		based = 0;
	uint64		entries;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!pg_calls_current_command(raise, true, &command, frame->error) ||
		!doltgres_identity_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->index, command,
									frame->error) ||
		!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->heap, &recorded,
								   frame->error))
		return;
	frame->key = recorded.key;
	frame->key_live = true;
	recorded.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&recorded);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->table_key, frame->error))
		return;
	frame->table_key_live = true;
	if (!doltgres_index_describe(raise, catalog, frame->index, frame->table_key.ptr, frame->table_key.len,
								 &frame->key, &describing, frame->error))
		return;
	frame->described = std_arc_new_dropping(&describing, sizeof(DoltgresIndexDescribed),
											_Alignof(DoltgresIndexDescribed), doltgres_entries_Described_drop);
	frame->described_live = true;
	described = std_arc_Deref_deref(frame->described, _Alignof(DoltgresIndexDescribed));
	if (doltgres_index_Described_compared(described) ?
		!pg_calls_sort_begin_index(raise, catalog, frame->index, &frame->sort, frame->error) :
		!pg_calls_sort_begin(raise, &frame->sort, frame->error))
		return;
	frame->sort_live = true;
	if (pg_calls_index_info_concurrent(frame->info))
	{
		if (!doltgres_entries_build_snapshot_copied(raise, pg_calls_rel_id(frame->index), &frame->built, frame->error))
			return;
		frame->built_live = true;
	}
	pg_calls_progress_update(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN);
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILD_DESCRIBED.local, &DOLTGRES_ENTRIES_BUILD_DESCRIBED_TYPE,
							&DOLTGRES_ENTRIES_BUILD_DESCRIBED_LOCK_TYPE, doltgres_entries_describing_replaced_op,
							frame->described);
	scanned = pg_calls_build_scan(raise, catalog, frame->heap, frame->index, frame->info,
								  pg_calls_sort_state(pg_OwnedSort_sort(&frame->sort)), &heap_tuples, frame->error);
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILD_DESCRIBED.local, &DOLTGRES_ENTRIES_BUILD_DESCRIBED_TYPE,
							&DOLTGRES_ENTRIES_BUILD_DESCRIBED_LOCK_TYPE, doltgres_entries_describing_taken_op, &taken);
	put = taken.has ? taken.put : 0;
	doltgres_entries_OptionDescribing_drop(&taken);
	if (!scanned)
		return;
	if (doltgres_index_Described_keyed(described))
	{
		frame->ok = pg_calls_build_result(raise, heap_tuples, (float8) put, frame->result, frame->error);
		doltgres_entries_doltgres_index_build_kept(frame);
		return;
	}
	pg_calls_progress_update_three(PROGRESS_CREATEIDX_TUPLES_TOTAL, put > PG_INT64_MAX ? PG_INT64_MAX : (int64) put,
								   PROGRESS_SCAN_BLOCKS_TOTAL, 0, PROGRESS_SCAN_BLOCKS_DONE, 0);
	pg_calls_progress_update(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	if (!pg_calls_sort_performed(raise, catalog, pg_OwnedSort_sort(&frame->sort), frame->error))
		return;
	pg_calls_progress_update(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_BTREE_PHASE_LEAF_LOAD);
	if (!pg_calls_fold_horizon(raise, frame->heap, &horizon, frame->error) ||
		!doltgres_entries_making(frame->caps, &frame->io, frame->heap, &made, frame->error))
		return;
	if (!txn_state_held_OptionBase(&frame->io, repo, &frame->opened.reading, made,
								   doltgres_entries_doltgres_index_build_closure_1, NULL, NULL, &some, &base,
								   &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (some)
	{
		based = base.max_folded;
		txn_base_Base_drop(&base);
	}
	if (!doltgres_entries_loaded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->heap, frame->index,
								 frame->info, &frame->sort, described, horizon, based, &entries, frame->error))
		return;
	frame->ok = pg_calls_build_result(raise, heap_tuples, (float8) entries, frame->result, frame->error);
	doltgres_entries_doltgres_index_build_kept(frame);
}

/*
 * doltgres_entries_doltgres_index_build is the fork's btree ambuild (the
 * btree text's section 6): the table's rows are read through its
 * index_build_range_scan, each entry formed by the build's callback and
 * sorted by its key's bytes in Postgres' tuplesort, which spills past
 * maintenance_work_mem, and the sorted entries are written into a tree bottom
 * up and installed as the index's base (loaded), reporting nbtree's
 * subphases and its counts: once the scan ends, the rows its callback put in
 * the sort as the load's total, with the scan's block counts reset in the
 * same update. It answers the table's live rows and the rows its entries
 * hold. A new index records its identity pairs first, in its creating
 * transaction (design item 10). A concurrent build keeps the snapshot it took
 * before its scan for its validation (BUILD_SNAPSHOTS).
 */
bool
doltgres_entries_doltgres_index_build(const PgCatalogCaps *caps, const PgRel *heap, const PgRel *index,
									  PgIndexInfo *info, PgBuildResult *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_build_frame_t frame = {.caps = caps, .heap = heap, .index = index, .info = info,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_build_body, doltgres_entries_doltgres_index_build_pad);
	doltgres_entries_doltgres_index_build_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_index_build_callback_frame_t is the build
 * callback's state across its pad: the row its scan named, the lent
 * description, what it opened, the row's values, its formed entry's key and
 * placing, the entry's value and addresses, and the sort's bytes, each while
 * live.
 */
typedef struct doltgres_entries_doltgres_index_build_callback_frame_t
{
	doltgres_entries_OptionBuilding building;
	doltgres_entries_OptionDescribing lent;
	doltgres_entries_opened_t opened;
	StdVec		read;
	bool		read_live;
	DoltgresIndexFormed formed;
	bool		key_live;
	bool		placing_live;
	AccessIndexEntry entry;
	bool		entry_live;
	StdVec		record;
	bool		record_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *index;
	PgTid	   *tid;
	const PgDatums *values;
	PgIsNull   *isnull;
	bool		alive;
	PgBuildState state;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_build_callback_frame_t;

/* doltgres_entries_doltgres_index_build_callback_pad frees what the build callback holds. */
static void
doltgres_entries_doltgres_index_build_callback_pad(void *frame_)
{
	doltgres_entries_doltgres_index_build_callback_frame_t *frame = frame_;

	if (frame->record_live)
		std_vec_free_Checked(&frame->record, 1, 1);
	if (frame->entry_live)
		access_index_Entry_drop(&frame->entry);
	if (frame->placing_live)
		access_heaptuple_Placing_drop(&frame->formed.placing);
	if (frame->key_live)
		access_index_EntryKey_drop(&frame->formed.key);
	if (frame->read_live)
		doltgres_index_values_drop(&frame->read);
	frame->record_live = frame->entry_live = frame->placing_live = frame->key_live = false;
	frame->read_live = false;
	doltgres_entries_opened_drop(&frame->opened);
	doltgres_entries_OptionDescribing_drop(&frame->lent);
	frame->lent.has = false;
	doltgres_entries_OptionBuilding_drop(&frame->building);
	frame->building.has = false;
}

/* doltgres_entries_doltgres_index_build_callback_body forms the row's entry and puts it in the build's sort. */
static void
doltgres_entries_doltgres_index_build_callback_body(void *frame_)
{
	doltgres_entries_doltgres_index_build_callback_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const DoltgresIndexDescribed *described;
	const doltgres_entries_Building *building = &frame->building.building;
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, (void *) frame->caps};
	AccessHeaptupleFormed formed;
	StdVec		placed;

	frame->io = pghost_io_catalog(frame->caps);
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILDING.local, &DOLTGRES_ENTRIES_BUILDING_TYPE,
							&DOLTGRES_ENTRIES_BUILDING_LOCK_TYPE, doltgres_entries_building_taken_op,
							&frame->building);
	if (!frame->building.has)
	{
		static const char message[] = "an index build's callback was handed a row its scan didn't name";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	if (building->f0 != pg_calls_tid_value(frame->tid))
	{
		static const char message[] = "an index build's callback was handed another row than its scan named";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	doltgres_entries_locked(&frame->io, &DOLTGRES_ENTRIES_BUILD_DESCRIBED.local, &DOLTGRES_ENTRIES_BUILD_DESCRIBED_TYPE,
							&DOLTGRES_ENTRIES_BUILD_DESCRIBED_LOCK_TYPE, doltgres_entries_describing_lent_op,
							&frame->lent);
	if (!frame->lent.has)
	{
		static const char message[] = "an index build's callback ran outside the fork's build";

		*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
		return;
	}
	described = std_arc_Deref_deref(frame->lent.described, _Alignof(DoltgresIndexDescribed));
	if (doltgres_index_Described_keyed(described))
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error) ||
		!doltgres_index_Described_values(described, frame->caps, frame->values, frame->isnull, &frame->read,
										 frame->error))
		return;
	frame->read_live = true;
	if (!doltgres_index_Described_formed(described, sorter, frame->read.ptr, frame->read.len, building->f1.ptr,
										 building->f1.len, &frame->formed, frame->error))
		return;
	frame->key_live = frame->placing_live = true;
	if (!doltgres_entries_placed(&frame->io, doltgres_entries_opened_repo(&frame->opened), &frame->opened.reading,
								 frame->index, &frame->formed.placing, &placed, frame->error))
		return;
	std_vec_free_Checked(&placed, sizeof(doltgres_TupleUsizeAddressU64_t), _Alignof(doltgres_TupleUsizeAddressU64_t));
	frame->placing_live = false;
	if (!access_heaptuple_Placing_formed(&frame->formed.placing, &formed, frame->error))
		return;
	frame->entry = access_index_IndexKey_entry(&formed);
	std_vec_free_Checked(&frame->entry.key, 1, 1);
	std_vec_new(&frame->entry.key, 1, 1);
	frame->entry_live = true;
	doltgres_entries_sorted(frame->formed.key.key.ptr, frame->formed.key.key.len, frame->entry.value.ptr,
							frame->entry.value.len, frame->entry.addresses.ptr, frame->entry.addresses.len,
							building->f2, building->f0, building->f3, frame->alive, &frame->record);
	frame->record_live = true;
	if (doltgres_index_Described_compared(described))
	{
		StdBytes	rest;

		if (!access_index_IndexKey_rest(doltgres_index_Described_entry_key(described), frame->formed.key.key.ptr,
										frame->formed.key.key.len, &rest, frame->error))
			return;
		frame->ok = pg_calls_sort_put_keyed(raise, pg_CatalogCaps_catalog(frame->caps), frame->state, frame->values,
											frame->isnull, rest.ptr, rest.len, frame->record.ptr, frame->record.len,
											frame->error);
	}
	else
		frame->ok = pg_calls_sort_put(raise, frame->state, frame->record.ptr, frame->record.len, frame->error);
}

/*
 * doltgres_entries_doltgres_index_build_callback is the fork's btree build's
 * callback over its table's rows: it forms the row's entry by the index's
 * description from the scan's values under the row's key the scan named
 * (BUILDING), writes its value's blobs (placed), and puts the entry in the
 * build's sort as sorted writes it.
 */
bool
doltgres_entries_doltgres_index_build_callback(const PgCatalogCaps *caps, const PgRel *index, PgTid *tid,
											   const PgDatums *values, PgIsNull *isnull, bool alive,
											   PgBuildState state, PgPgError *error)
{
	doltgres_entries_doltgres_index_build_callback_frame_t frame = {.caps = caps, .index = index, .tid = tid,
	.values = values, .isnull = isnull, .alive = alive, .state = state, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_build_callback_body,
				doltgres_entries_doltgres_index_build_callback_pad);
	doltgres_entries_doltgres_index_build_callback_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_Deltid_size is the bytes of one Deltid as Postgres' delete state lends and takes it. */
#define DOLTGRES_ENTRIES_DELTID_BYTES 16

/*
 * doltgres_entries_doltgres_index_delete_tuples_frame_t is the delete's state
 * across its pad: the lent bytes, the deletion, a row read, the finished
 * entries, and the bytes written back, each while live.
 */
typedef struct doltgres_entries_doltgres_index_delete_tuples_frame_t
{
	StdVec		bytes;
	bool		bytes_live;
	PgOwnedSnapshot owned;
	bool		owned_live;
	DoltgresDeletesDeleting deleting;
	bool		deleting_live;
	DoltgresScanRow row;
	bool		row_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		out;
	bool		out_live;
	const PgCatalogCaps *caps;
	const PgRel *rel;
	PgIndexDeleteOp *delstate;
	uint32	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_delete_tuples_frame_t;

/* doltgres_entries_doltgres_index_delete_tuples_pad frees what the delete holds. */
static void
doltgres_entries_doltgres_index_delete_tuples_pad(void *frame_)
{
	doltgres_entries_doltgres_index_delete_tuples_frame_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid));
	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->deleting_live)
		std_vec_free_Checked(&frame->deleting.entries, sizeof(DoltgresDeletesDeltid),
							 _Alignof(DoltgresDeletesDeltid));
	if (frame->owned_live)
		pg_OwnedSnapshot_drop(&frame->owned);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->out_live = frame->entries_live = frame->row_live = frame->deleting_live = frame->owned_live = false;
	frame->bytes_live = false;
}

/* doltgres_entries_doltgres_index_delete_tuples_looked reads what the table says of an entry's row. */
static bool
doltgres_entries_doltgres_index_delete_tuples_looked(doltgres_entries_doltgres_index_delete_tuples_frame_t *frame,
													 const HostSnapshot *read, uint64 tid,
													 DoltgresDeletesLooked *looked)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tid};
	StdVec		rows;

	memset(looked, 0, sizeof(*looked));
	if (!doltgres_entries_rows(frame->caps, frame->rel, pg_OwnedSnapshot_snapshot(&frame->owned), read, by, &rows,
							   frame->error))
		return false;
	looked->kept = std_vec_pop(&rows, &frame->row, sizeof(DoltgresScanRow));
	if (looked->kept)
		doltgres_scan_Row_drop(&frame->row);
	std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	if (!doltgres_entries_tid_row(frame->caps, frame->rel, tid, &frame->row_live, &frame->row, frame->error))
		return false;
	looked->has_row = frame->row_live;
	if (frame->row_live)
	{
		HostIoIo	io = pghost_io(raise);
		HostStatus	outcome;
		HostError	failed;

		if (!txn_outcome_stamped(&io, frame->row.stamp.txn, &outcome, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return false;
		}
		looked->row.f0 = (uint32) frame->row.stamp.txn;
		looked->row.f1 = outcome == HOST_STATUS_COMMITTED;
		looked->row.has_f2 = frame->row.has_changer && !frame->row.changer.locks;
		looked->row.f2 = looked->row.has_f2 ? (uint32) frame->row.changer.xid : 0;
		frame->row_live = false;
		doltgres_scan_Row_drop(&frame->row);
	}
	return true;
}

/*
 * doltgres_entries_doltgres_index_delete_tuples_body reads the entries,
 * answers each one's row, and writes them back.
 */
static void
doltgres_entries_doltgres_index_delete_tuples_body(void *frame_)
{
	doltgres_entries_doltgres_index_delete_tuples_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	int32		size = pg_calls_index_delete_size(frame->delstate);
	size_t		count = size < 0 ? 0 : (size_t) size;
	StdVec		entries;
	int32		target;
	HostSnapshot read;
	uint64		tid;
	uint32		horizon;

	std_vec_from_elem_zeroed(&frame->bytes, count * DOLTGRES_ENTRIES_DELTID_BYTES, 1, 1);
	frame->bytes_live = true;
	pg_calls_index_delete_read(frame->delstate, frame->bytes.ptr, frame->bytes.len);
	std_vec_with_capacity_Checked(&entries, count, sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid));
	for (size_t i = 0; i < count; i++)
	{
		const uint8 *at = (const uint8 *) frame->bytes.ptr + i * DOLTGRES_ENTRIES_DELTID_BYTES;
		DoltgresDeletesDeltid deltid;

		memcpy(&deltid.tid, at, sizeof(uint64));
		memcpy(&deltid.id, at + 8, sizeof(int16));
		deltid.known = at[10] != 0;
		deltid.promising = at[11] != 0;
		memcpy(&deltid.freespace, at + 12, sizeof(int16));
		std_vec_push_Checked(&entries, &deltid, sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid));
	}
	target = pg_calls_index_delete_target(frame->delstate);
	if (!pg_OwnedSnapshot_nonvacuumable(raise, frame->rel, &frame->owned, frame->error))
	{
		std_vec_free_Checked(&entries, sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid));
		return;
	}
	frame->owned_live = true;
	if (!pghost_snapshot(raise, pg_OwnedSnapshot_snapshot(&frame->owned), &read, frame->error))
	{
		std_vec_free_Checked(&entries, sizeof(DoltgresDeletesDeltid), _Alignof(DoltgresDeletesDeltid));
		return;
	}
	doltgres_deletes_Deleting_new(&entries, target >= 0, target, &frame->deleting);
	frame->deleting_live = true;
	while (doltgres_deletes_Deleting_next(&frame->deleting, &tid))
	{
		DoltgresDeletesLooked looked;

		if (!doltgres_entries_doltgres_index_delete_tuples_looked(frame, &read, tid, &looked))
			return;
		doltgres_deletes_Deleting_answer(&frame->deleting, looked);
	}
	frame->deleting_live = false;
	doltgres_deletes_Deleting_finish(&frame->deleting, &frame->entries, &horizon);
	frame->entries_live = true;
	std_vec_with_capacity_Checked(&frame->out, frame->entries.len * DOLTGRES_ENTRIES_DELTID_BYTES, 1, 1);
	frame->out_live = true;
	for (size_t i = 0; i < frame->entries.len; i++)
	{
		const DoltgresDeletesDeltid *entry = &STD_VEC_AT(&frame->entries, DoltgresDeletesDeltid, i);
		uint8		flags[2] = {entry->known ? 1 : 0, entry->promising ? 1 : 0};
		static const uint8 padding[2] = {0, 0};

		std_vec_extend_from_slice_Checked(&frame->out, &entry->tid, sizeof(uint64), 1, 1);
		std_vec_extend_from_slice_Checked(&frame->out, &entry->id, sizeof(int16), 1, 1);
		std_vec_extend_from_slice_Checked(&frame->out, flags, sizeof(flags), 1, 1);
		std_vec_extend_from_slice_Checked(&frame->out, &entry->freespace, sizeof(int16), 1, 1);
		std_vec_extend_from_slice_Checked(&frame->out, padding, sizeof(padding), 1, 1);
	}
	pg_calls_index_delete_write(frame->delstate, frame->out.ptr, frame->out.len);
	*frame->result = horizon;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_index_delete_tuples is the fork's
 * index_delete_tuples ((4b)'s section 4c): an index of a method the fork
 * doesn't know asks which of its entries' rows are dead to everyone, and the
 * fork answers as heap's heap_index_delete_tuples does over its tuple ids
 * (deletes' Deleting): an entry's row is kept while the relation's
 * NonVacuumable snapshot sees it, read by its tuple id, a tuple id no layer
 * holds has none, and the conflict horizon comes from each deletable row's
 * committed writer and the update or delete that ended it, read under
 * SnapshotAny.
 */
bool
doltgres_entries_doltgres_index_delete_tuples(const PgCatalogCaps *caps, const PgRel *rel, PgIndexDeleteOp *delstate,
											  uint32 *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_delete_tuples_frame_t frame = {.caps = caps, .rel = rel, .delstate = delstate,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_delete_tuples_body,
				doltgres_entries_doltgres_index_delete_tuples_pad);
	doltgres_entries_doltgres_index_delete_tuples_pad(&frame);
	return frame.ok;
}

/* state.rs' held at Option<(Vec<u8>, bool)>, which index_holds takes. */
#define TXN_STATE_TS OptionTupleVecU8Bool
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, StdVec *result_0, bool *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; StdVec *result_0; bool *result_1;
#define TXN_STATE_RESULT_INIT , some, result_0, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result_0, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_doltgres_index_holds_captures_t is index_holds' lent call's
 * captures: the reader and the row's tuple id.
 */
typedef struct doltgres_entries_doltgres_index_holds_captures_t
{
	const HostSharedReader *reading;
	uint64		tuple;
} doltgres_entries_doltgres_index_holds_captures_t;

/* doltgres_entries_doltgres_index_holds_closure_0 is index_holds' lent call: the row key of the row's tuple id. */
static bool
doltgres_entries_doltgres_index_holds_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
												const HostSharedHeld *held, bool *some, StdVec *result_0,
												bool *result_1, HostError *error)
{
	doltgres_entries_doltgres_index_holds_captures_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

	return doltgres_scan_keyed(io, TREES_STORE_READER_NODE_STORE(&store), held, c->tuple, some, result_0, result_1,
							   error);
}

/*
 * doltgres_entries_doltgres_index_holds_frame_t is index_holds' state across
 * its pad: what it opened, the table's recorded base and its key's columns,
 * the index's description, the row key, the row's values, the entry's key,
 * and the table's keyed layout, each while live.
 */
typedef struct doltgres_entries_doltgres_index_holds_frame_t
{
	doltgres_entries_opened_t opened;
	TxnBaseBase base;
	bool		base_live;
	StdVec		table_key;
	bool		table_key_live;
	DoltgresIndexDescribed described;
	bool		described_live;
	StdVec		row_key;
	bool		row_key_live;
	StdVec		read;
	bool		read_live;
	AccessIndexEntryKey key;
	bool		key_live;
	StdVec		layout;
	bool		layout_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *index;
	const PgDatums *values;
	PgIsNull   *isnull;
	PgTid	   *heap_tid;
	const PgRel *heap;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_doltgres_index_holds_frame_t;

/* doltgres_entries_doltgres_index_holds_pad frees what index_holds holds. */
static void
doltgres_entries_doltgres_index_holds_pad(void *frame_)
{
	doltgres_entries_doltgres_index_holds_frame_t *frame = frame_;

	if (frame->layout_live)
		std_vec_free_Checked(&frame->layout, 1, 1);
	if (frame->key_live)
		access_index_EntryKey_drop(&frame->key);
	if (frame->read_live)
		doltgres_index_values_drop(&frame->read);
	if (frame->row_key_live)
		std_vec_free_Checked(&frame->row_key, 1, 1);
	if (frame->described_live)
		doltgres_index_Described_drop(&frame->described);
	if (frame->table_key_live)
		doltgres_layout_VecKeyed_drop(&frame->table_key);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->layout_live = frame->key_live = frame->read_live = frame->row_key_live = frame->described_live = false;
	frame->table_key_live = frame->base_live = false;
	doltgres_entries_opened_drop(&frame->opened);
}

/* doltgres_entries_doltgres_index_holds_body reads the row's key and asks whether the index holds its entry. */
static void
doltgres_entries_doltgres_index_holds_body(void *frame_)
{
	doltgres_entries_doltgres_index_holds_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, (void *) frame->caps};
	doltgres_entries_doltgres_index_holds_captures_t captures = {.reading = &frame->opened.reading};
	const RepoRepo *repo;
	TxnStateFound made;
	bool		has_key;
	bool		keyless;
	TreesKeyOrder order;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_open(&frame->io, &frame->opened, frame->error))
		return;
	repo = doltgres_entries_opened_repo(&frame->opened);
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->opened.reading, frame->heap, &frame->base,
								   frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->base.key, &frame->table_key, frame->error))
		return;
	frame->table_key_live = true;
	if (!doltgres_index_describe(raise, catalog, frame->index, frame->table_key.ptr, frame->table_key.len,
								 &frame->base.key, &frame->described, frame->error))
		return;
	frame->described_live = true;
	if (doltgres_index_Described_keyed(&frame->described))
	{
		*frame->result = true;
		frame->ok = true;
		return;
	}
	captures.tuple = pg_calls_tid_value(frame->heap_tid);
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->heap, &made, frame->error))
		return;
	if (!txn_state_held_OptionTupleVecU8Bool(&frame->io, repo, &frame->opened.reading, made,
											 doltgres_entries_doltgres_index_holds_closure_0, &captures, NULL,
											 &has_key, &frame->row_key, &keyless, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!has_key)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("an index entry's row ("), STD_FMT_ARG(0), STD_FMT_LIT(","),
		STD_FMT_ARG(1), STD_FMT_LIT(") has no key")};
		uint64		block = captures.tuple >> 16;
		uint64		offset = captures.tuple & 0xffff;
		StdFmtArg	args[] = {{&block, std_u64_Display_fmt}, {&offset, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return;
	}
	frame->row_key_live = true;
	if (!doltgres_index_Described_values(&frame->described, frame->caps, frame->values, frame->isnull, &frame->read,
										 frame->error))
		return;
	frame->read_live = true;
	if (!doltgres_index_Described_key(&frame->described, sorter, frame->read.ptr, frame->read.len,
									  frame->row_key.ptr, frame->row_key.len, &frame->key, frame->error))
		return;
	frame->key_live = true;
	access_slot_keyed(frame->table_key.ptr, frame->table_key.len, &frame->layout);
	frame->layout_live = true;
	doltgres_index_Described_order(&frame->described, NULL, &order);
	frame->ok = doltgres_entries_entry_held(frame->caps, &frame->io, repo, &frame->opened.reading, frame->index,
											frame->layout.ptr, frame->layout.len, &order, frame->key.key.ptr,
											frame->key.key.len, doltgres_index_Described_keyless(&frame->described),
											captures.tuple, doltgres_index_Described_unique(&frame->described) ?
											frame->row_key.ptr : NULL, frame->row_key.len, frame->result,
											frame->error);
}

/*
 * doltgres_entries_doltgres_index_holds is a concurrent build's validation's
 * question of an index the fork stores (bugs row 168): whether the index
 * already holds a row's own entry, by the entry's key the executor's values
 * form under the row's key, a keyless table's by the copy its tuple id names
 * and a unique index's by the row key its value holds, read under a Dirty
 * snapshot (entry_held), so the validation inserts only the rows it lacks; a
 * primary key's index, whose entries are its table's rows, holds every row.
 */
bool
doltgres_entries_doltgres_index_holds(const PgCatalogCaps *caps, const PgRel *index, const PgDatums *values,
									  PgIsNull *isnull, PgTid *heap_tid, const PgRel *heap, PgIndexInfo *info,
									  bool *result, PgPgError *error)
{
	doltgres_entries_doltgres_index_holds_frame_t frame = {.caps = caps, .index = index, .values = values,
	.isnull = isnull, .heap_tid = heap_tid, .heap = heap, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_index_holds_body, doltgres_entries_doltgres_index_holds_pad);
	doltgres_entries_doltgres_index_holds_pad(&frame);
	return frame.ok;
}

#endif							/* FRONTEND */
