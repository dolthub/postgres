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
 * The fork's entries (doltgres' entries.rs): the functions Postgres calls,
 * each the body of the entry of its name in pg's entries.list, and the
 * helpers they and the crate's other files share.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "access/sdir.h"
#include "access/xact.h"
#include "catalog/pg_class_d.h"
#include "doltgres/access/slot.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/io.h"
#include "storage/block.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/std/fmt.h"
#include "doltgres/doltgres/entries/testing.h"
#include "doltgres/txn/values.h"
#include "doltgres/txn/vacuum.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/change.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/local.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/pghost/delta.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres/shmem/cache.h"
#include "doltgres/shmem/collection.h"
#include "doltgres/shmem/delta.h"
#include "doltgres/shmem/keys.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/store/collect.h"
#include "doltgres/store/databases.h"
#include "doltgres/store/lib.h"
#include "doltgres/store/open.h"
#include "doltgres/store/session.h"
#include "doltgres/std/sort.h"
#include "doltgres/trees/blob.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/blobs.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/written.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/kinds.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/layouts.h"
#include "doltgres/doltgres/reform.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/doltgres/settle.h"
#include "pg_calls.h"
#include "version.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "entries.rs"

/* DOLTGRES_ENTRIES_BYTES is the element layout of a Vec<u8>. */
#define DOLTGRES_ENTRIES_BYTES 1, 1

/* doltgres_entries_Repo_drop is Repo's drop glue, as an Arc's value takes it. */
void
doltgres_entries_Repo_drop(void *repo)
{
	repo_Repo_drop(repo);
}

/*
 * doltgres_entries_repository returns the backend's handle on its database's
 * repository (an Arc<Repo>), the one a relation it scans is in.
 */
bool
doltgres_entries_repository(HostIoIo *io, StdArcInner **result, PgPgError *error)
{
	HostError	failed;

	if (store_session_session_repo(io, result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_storage_error returns a storage's error as the entry raises it. */
PgPgError
doltgres_entries_storage_error(ChunksErrorError *error)
{
	HostError	storage = txn_base_storage(error);

	return pghost_raised(&storage);
}

/*
 * doltgres_entries_chunk_tree names the overflow chunk at an address a stored
 * row's tuple names, whose locator a read finds.
 */
TreesStoreTree
doltgres_entries_chunk_tree(const uint8 address[32])
{
	ChunksAddressAddress named;
	ChunksTrailerLocator none = {0};

	memcpy(named.f0, address, sizeof(named.f0));
	return trees_store_Tree_decoded(named, none);
}

/*
 * doltgres_entries_blob_bytes_frame_t is blob_bytes' state across its pad:
 * the repository's handle, the reader's published epoch, and the blobs it
 * reads through.
 */
typedef struct doltgres_entries_blob_bytes_frame_t
{
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnBlobsBlobs blobs;
	bool		blobs_live;
	HostIoIo   *io;
	const uint8 *address;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_blob_bytes_frame_t;

/* doltgres_entries_blob_bytes_pad frees what blob_bytes holds. */
static void
doltgres_entries_blob_bytes_pad(void *frame_)
{
	doltgres_entries_blob_bytes_frame_t *frame = frame_;

	if (frame->blobs_live)
		txn_blobs_Blobs_drop(&frame->blobs);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->blobs_live = frame->reading_live = frame->repo_live = false;
}

/*
 * doltgres_entries_blob_bytes_body reads the blob through the session's
 * repository's working state's delta and its packs.
 */
static void
doltgres_entries_blob_bytes_body(void *frame_)
{
	doltgres_entries_blob_bytes_frame_t *frame = frame_;
	HostError	failed;
	ChunksErrorError read_error;
	TreesStoreReader packs;
	TreesStoreTree tree;
	uint32		database;
	uint32		tablespace;

	if (!doltgres_entries_repository(frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	packs = repo_Repo_reader(std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), HOST_CACHE_PLACEMENT_SERVER,
							 &frame->reading);
	if (!host_session_database(&database, &tablespace, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!txn_blobs_Blobs_new(frame->io, database, TREES_STORE_READER_NODE_STORE(&packs), &frame->blobs, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->blobs_live = true;
	tree = doltgres_entries_chunk_tree(frame->address);
	if (!trees_blob_read_blob(frame->io, TXN_BLOBS_BLOBS_NODE_STORE(&frame->blobs), TREES_STORE_TREE_NAMED(&tree),
							  frame->result, &read_error))
	{
		*frame->error = doltgres_entries_storage_error(&read_error);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_blob_bytes returns the bytes of the blob or chunk at an
 * address that a stored row or key of the session's database names, read
 * through the verified read door from its repository's working state's
 * delta, else its packs.
 */
bool
doltgres_entries_blob_bytes(HostIoIo *io, const uint8 address[32], StdVec *result, PgPgError *error)
{
	doltgres_entries_blob_bytes_frame_t frame = {.io = io, .address = address, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_blob_bytes_body, doltgres_entries_blob_bytes_pad);
	doltgres_entries_blob_bytes_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_current returns the writer of a change at a command, the
 * current subtransaction's xid and its top-level transaction's, with the
 * slot of the top-level transaction's entry in the commit status table,
 * whose room it takes at the transaction's first write.
 */
bool
doltgres_entries_current(HostIoIo *io, uint32 command, TxnMemtableVersionWriter *result_0,
						 TxnStatusRoomed *result_1, PgPgError *error)
{
	HostXid		sub;
	HostXid		top;
	HostError	failed;

	if (!host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &failed) ||
		!txn_status_roomed(io, top, result_1, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	result_0->xid = sub.f0;
	result_0->has_top = sub.f0 != top.f0;
	result_0->top = result_0->has_top ? top.f0 : 0;
	result_0->command = command;
	return true;
}

/* doltgres_entries_storage returns the storage a relation's rows are kept in. */
HostStorage
doltgres_entries_storage(const PgRel *relation)
{
	DgStorage	located = pg_calls_rel_storage(relation);
	HostStorage storage = {located.tablespace, located.database, located.relation};

	return storage;
}

/* doltgres_entries_persistence returns the persistence a relation's storage's state keeps: permanent, or unlogged. */
TxnStatePersistence
doltgres_entries_persistence(const PgRel *relation)
{
	/*
	 * TODO: a temporary relation's delta is its backend's own and never a
	 * shared state or a run (bugs row 63, the temporary storage piece); until
	 * that piece, a temporary relation gets a state here as an unlogged one
	 * does.
	 */
	return pg_calls_rel_permanent(relation) ? TXN_STATE_PERSISTENCE_PERMANENT : TXN_STATE_PERSISTENCE_UNLOGGED;
}

/*
 * doltgres_entries_making returns how the backend holds a relation's
 * storage's state: the state it holds, or what making it takes, the key its
 * settle keeps for a table it is still making or else the catalog's.
 */
bool
doltgres_entries_making(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, TxnStateFound *result,
						PgPgError *error)
{
	HostStorage storage = doltgres_entries_storage(rel);
	TxnStateFind find;
	HostError	failed;

	if (!txn_state_holds(io, &storage, doltgres_entries_persistence(rel), &find, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (find.tag == TXN_STATE_FIND_HELD)
	{
		result->tag = TXN_STATE_FOUND_HELD;
		result->u.held = find.u.held;
		return true;
	}
	result->tag = TXN_STATE_FOUND_MADE;
	result->u.made.f0 = find.u.missing;
	result->u.made.f1.table = pg_calls_rel_id(rel);
	result->u.made.f1.has_index = false;
	result->u.made.f1.index = 0;
	if (!doltgres_settle_transient_key(io, pg_calls_rel_id(rel), &result->u.made.f1.key) &&
		!doltgres_layout_catalog_key(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), rel,
									 &result->u.made.f1.key, error))
		return false;
	return true;
}

/* txn::state::held at Option<Base>, which recorded takes. */
#define TXN_STATE_TS OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseBase *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnBaseBase *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* doltgres_entries_recorded_closure_0 is recorded's lent call: the held state's base record. */
static bool
doltgres_entries_recorded_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									bool *some, TxnBaseBase *result, HostError *error)
{
	return txn_state_base_record(io, held, some, result, error);
}

/*
 * doltgres_entries_recorded returns the base record of a relation's storage's
 * state, making the state when the backend doesn't hold it.
 */
bool
doltgres_entries_recorded(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						  const HostSharedReader *reading, const PgRel *rel, TxnBaseBase *result, PgPgError *error)
{
	TxnStateFound made;
	HostError	failed;
	bool		some;

	if (!doltgres_entries_making(caps, io, rel, &made, error))
		return false;
	if (!txn_state_held_OptionBase(io, repo, reading, made, doltgres_entries_recorded_closure_0, NULL, NULL, &some,
								   result, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (!some)
	{
		StdString	str;
		static const char message[] = "a backend's make of a storage's state left no base record";

		std_string_from_Checked(&str, message, sizeof(message) - 1);
		*error = pg_PgError_fork((const uint8 *) "XX000", &str);
		return false;
	}
	return true;
}

/* doltgres_entries_page returns what redo knows of a heap page: its bytes past its header and its most tuples. */
TxnInsertPage
doltgres_entries_page(void)
{
	TxnInsertPage page = {pg_calls_page_bytes(), pg_calls_max_heap_tuples_per_page()};

	return page;
}

/*
 * doltgres_entries_index_making returns what held takes for an index's
 * storage: the place of its state when the backend's top-level transaction
 * holds it, and otherwise a make with no key, its table's object id, and its
 * own, so its first base record names both, with no catalog read.
 */
bool
doltgres_entries_index_making(HostIoIo *io, const PgRel *index, TxnStateFound *result, PgPgError *error)
{
	HostStorage storage = doltgres_entries_storage(index);
	TxnStateFind find;
	HostError	failed;

	if (!txn_state_holds(io, &storage, doltgres_entries_persistence(index), &find, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (find.tag == TXN_STATE_FIND_HELD)
	{
		result->tag = TXN_STATE_FOUND_HELD;
		result->u.held = find.u.held;
		return true;
	}
	result->tag = TXN_STATE_FOUND_MADE;
	result->u.made.f0 = find.u.missing;
	result->u.made.f1.key.tag = TXN_BASE_KEY_NONE;
	result->u.made.f1.table = pg_calls_index_table(index);
	result->u.made.f1.has_index = true;
	result->u.made.f1.index = pg_calls_rel_id(index);
	return true;
}

/*
 * doltgres_entries_shape_frame_t is shape's state across its pad: the
 * table's recorded key and its key's columns, while live.
 */
typedef struct doltgres_entries_shape_frame_t
{
	TxnBaseKey	key;
	bool		key_live;
	StdVec		columns;
	bool		columns_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *table;
	const PgRel *index;
	TreesKeyOrder *result_0;
	bool	   *result_1;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_shape_frame_t;

/* doltgres_entries_shape_pad frees what shape holds. */
static void
doltgres_entries_shape_pad(void *frame_)
{
	doltgres_entries_shape_frame_t *frame = frame_;

	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->columns_live = frame->key_live = false;
}

/* doltgres_entries_shape_body reads the table's key and orders its storage's keys by it, or by the index's description. */
static void
doltgres_entries_shape_body(void *frame_)
{
	doltgres_entries_shape_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	TxnBaseBase base;

	if (!doltgres_entries_recorded(frame->caps, frame->io, frame->repo, frame->reading, frame->table, &base,
								   frame->error))
		return;
	frame->key = base.key;
	frame->key_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	*frame->result_1 = frame->columns.len == 0;
	if (frame->index != NULL)
	{
		DoltgresIndexDescribed described;

		if (!doltgres_index_describe(raise, catalog, frame->index, frame->columns.ptr, frame->columns.len,
									 &frame->key, &described, frame->error))
			return;
		doltgres_index_Described_order(&described, NULL, frame->result_0);
		doltgres_index_Described_drop(&described);
	}
	else
	{
		frame->columns_live = false;
		if (!doltgres_layout_key_order(raise, catalog, &frame->key, &frame->columns, frame->result_0, frame->error))
			return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_shape returns how a fold reads a storage's keys: their
 * order, and whether its table is keyless, by its table's relation and, for
 * an index's storage, the index (NULL for none), whose keys order as its
 * description does.
 */
bool
doltgres_entries_shape(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					   const PgRel *table, const PgRel *index, TreesKeyOrder *result_0, bool *result_1,
					   PgPgError *error)
{
	doltgres_entries_shape_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .table = table,
	.index = index, .result_0 = result_0, .result_1 = result_1, .error = error};

	std_landing(&frame, doltgres_entries_shape_body, doltgres_entries_shape_pad);
	doltgres_entries_shape_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_version returns the version a row's copy is stored with in
 * a slot, its writer and raw command, the writer of its changer and its
 * commander's raw command, its table, and its ctid under a page's most
 * tuples.
 */
static bool
doltgres_entries_version(const DoltgresScanRow *row, uint16 most_offset, AccessSlotVersion *result, PgPgError *error)
{
	result->writer = row->stamp.txn;
	result->command = row->command;
	result->deleter = row->has_changer ? row->changer.xid : 0;
	result->deleter_command = row->has_changer ? row->changer.command : 0;
	result->table = row->table;
	return access_slot_ctid(row->stamp.tuple, most_offset, &result->ctid, error);
}

/*
 * doltgres_entries_stored puts a row a read gives of a relation in a slot,
 * with its values out of line named in the relation's TOAST relation.
 */
bool
doltgres_entries_stored(const PgMayRaise *raise, PgSlot *slot, const PgRel *relation, const DoltgresScanRow *row,
						PgPgError *error)
{
	AccessSlotOutside outside = {{row->values.ptr, row->values.len}, pg_calls_rel_toast_id(relation)};
	AccessSlotVersion version;

	if (!doltgres_entries_version(row, pg_calls_max_heap_tuples_per_page(), &version, error))
		return false;
	return access_slot_store(raise, slot, row->keyed.ptr, row->keyed.len, row->key.ptr, row->key.len, row->tuple.ptr,
							 row->tuple.len, outside, row->transform.ptr, row->transform.len, &version, error);
}

/* txn::state::held at bool, which folded_now takes. */
#define TXN_STATE_TS Bool
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_folded_now_captures_t is folded_now's lent call's
 * captures: the reader, the storage's key order and keyless mark, the fold
 * point and horizon, the warnings it gathers, the page redo knows, and the
 * table's re-form, or NULL for an index's.
 */
typedef struct doltgres_entries_folded_now_captures_t
{
	const HostSharedReader *reading;
	const TreesKeyOrder *order;
	bool		keyless;
	bool		has_point;
	HostLsn		point;
	uint64		horizon;
	StdVec	   *warned;
	const TxnInsertPage *page;
	DoltgresReformCatalog *reform;
} doltgres_entries_folded_now_captures_t;

/* doltgres_entries_folded_now_closure_0 is folded_now's lent call: the fold of the held state now. */
static bool
doltgres_entries_folded_now_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *result, HostError *error)
{
	doltgres_entries_folded_now_captures_t *c = captures;
	TxnFoldReform reform = {&DOLTGRES_REFORM_CATALOG_REFORM_OPS, c->reform};

	return txn_fold_fold_now(io, repo, c->reading, held, c->order, c->keyless, c->has_point, c->point, c->horizon,
							 c->warned, c->page, c->reform != NULL ? &reform : NULL, result, error);
}

/*
 * doltgres_entries_folded_now_frame_t is folded_now's state across its pad:
 * the repository's handle, the reader, the table's key and its columns, the
 * storage's order, the table's re-form, what held takes, and the warnings
 * gathered, from the next one to raise, each while live.
 */
typedef struct doltgres_entries_folded_now_frame_t
{
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnBaseKey	key;
	bool		key_live;
	StdVec		columns;
	bool		columns_live;
	TreesKeyOrder order;
	bool		order_live;
	DoltgresReformCatalog reform;
	bool		reform_live;
	TxnStateFound found;
	bool		found_live;
	StdVec		warned;
	size_t		warned_next;
	bool		warned_live;
	const PgCatalogCaps *caps;
	const PgRel *target;
	const PgRel *heap;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_folded_now_frame_t;

/* doltgres_entries_folded_now_pad frees what folded_now holds, in the reverse of its making. */
static void
doltgres_entries_folded_now_pad(void *frame_)
{
	doltgres_entries_folded_now_frame_t *frame = frame_;

	if (frame->warned_live)
	{
		std_ptr_drop_in_place_slice((StdString *) frame->warned.ptr + frame->warned_next,
									frame->warned.len - frame->warned_next, sizeof(StdString), std_String_drop);
		std_vec_free_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString));
	}
	if (frame->found_live && frame->found.tag == TXN_STATE_FOUND_MADE)
		txn_base_Key_drop(&frame->found.u.made.f1.key);
	if (frame->reform_live)
		doltgres_reform_Catalog_drop(&frame->reform);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->warned_live = frame->found_live = frame->reform_live = frame->order_live = false;
	frame->columns_live = frame->key_live = frame->reading_live = frame->repo_live = false;
}

/*
 * doltgres_entries_folded_now_shape reads the order of the storage a fold
 * reads, what held takes for it, and for a table its re-form through the
 * catalog's layout, by the table's key.
 */
static bool
doltgres_entries_folded_now_shape(doltgres_entries_folded_now_frame_t *frame)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);

	if (pg_calls_rel_kind(frame->target) == RELKIND_INDEX)
	{
		DoltgresIndexDescribed described;

		if (!doltgres_index_describe(raise, catalog, frame->target, frame->columns.ptr, frame->columns.len,
									 &frame->key, &described, frame->error))
			return false;
		doltgres_index_Described_order(&described, NULL, &frame->order);
		doltgres_index_Described_drop(&described);
		frame->order_live = true;
		if (!doltgres_entries_index_making(&frame->io, frame->target, &frame->found, frame->error))
			return false;
	}
	else
	{
		StdVec		key_columns;

		if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &key_columns, frame->error))
			return false;
		doltgres_reform_Catalog_new(frame->target, key_columns.ptr, key_columns.len, &frame->reform);
		doltgres_layout_VecKeyed_drop(&key_columns);
		frame->reform_live = true;
		frame->columns_live = false;
		if (!doltgres_layout_key_order(raise, catalog, &frame->key, &frame->columns, &frame->order, frame->error))
			return false;
		frame->order_live = true;
		if (!doltgres_entries_making(frame->caps, &frame->io, frame->target, &frame->found, frame->error))
			return false;
	}
	frame->found_live = true;
	return true;
}

/*
 * doltgres_entries_folded_now_body folds the storage under the held state,
 * then reports each comparator tree's root a standby's fold found to differ
 * from its primary's.
 */
static void
doltgres_entries_folded_now_body(void *frame_)
{
	doltgres_entries_folded_now_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_entries_folded_now_captures_t captures = {0};
	TxnInsertPage page = doltgres_entries_page();
	TxnBaseBase base;
	HostError	failed;
	bool		folded;

	frame->io = pghost_io_catalog(frame->caps);
	if (!txn_fold_point(&frame->io, &captures.has_point, &captures.point, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_recorded(frame->caps, &frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
								   &frame->reading, frame->heap, &base, frame->error))
		return;
	frame->key = base.key;
	frame->key_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	captures.keyless = frame->columns.len == 0;
	if (!doltgres_entries_folded_now_shape(frame))
		return;
	if (!pg_calls_fold_horizon(raise, frame->heap, &captures.horizon, frame->error))
		return;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	captures.reading = &frame->reading;
	captures.order = &frame->order;
	captures.warned = &frame->warned;
	captures.page = &page;
	if (frame->reform_live)
		captures.reform = &frame->reform;
	frame->found_live = false;
	if (!txn_state_held_Bool(&frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
							 frame->found, doltgres_entries_folded_now_closure_0, &captures, NULL, &folded, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	while (frame->warned_next < frame->warned.len)
	{
		StdString	detail = STD_VEC_AT(&frame->warned, StdString, frame->warned_next);
		StdCString text;
		StdNulError nul;
		bool		warned;

		frame->warned_next++;
		if (!std_CString_new_String(&detail, &text, &nul))
		{
			std_NulError_drop(&nul);
			std_CString_default(&text);
		}
		warned = pg_calls_warn_fold_root(raise, frame->target, std_CString_as_ptr(&text), frame->error);
		std_CString_drop(&text);
		if (!warned)
			return;
	}
	*frame->result = folded;
	frame->ok = true;
}

/*
 * doltgres_entries_folded_now folds a relation's storage now, with its key
 * order and keyless mark from its catalog (an index's order from its
 * description and its mark from its table's key) and bounded by the horizon
 * of heap, the relation itself or an index's table, and answers whether it
 * installed a base: a fold worker's fold, and the tests'. A table's fold
 * re-forms its rows through the catalog's layout when its base's chain holds
 * a live transform. A standby's fold reports a comparator tree's root that
 * differs from its primary's at WARNING, naming the relation.
 */
bool
doltgres_entries_folded_now(const PgCatalogCaps *caps, const PgRel *target, const PgRel *heap, bool *result,
							PgPgError *error)
{
	doltgres_entries_folded_now_frame_t frame = {.caps = caps, .target = target, .heap = heap, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_folded_now_body, doltgres_entries_folded_now_pad);
	doltgres_entries_folded_now_pad(&frame);
	return frame.ok;
}

/* The element layouts of the out-of-line values' vectors. */
#define DOLTGRES_ENTRIES_MOVED sizeof(doltgres_TupleUsizeAddressU64_t), _Alignof(doltgres_TupleUsizeAddressU64_t)
#define DOLTGRES_ENTRIES_KEPT sizeof(doltgres_TupleUsizeU32Entry_t), _Alignof(doltgres_TupleUsizeU32Entry_t)
#define DOLTGRES_ENTRIES_COLUMN sizeof(doltgres_TupleUsizeU32_t), _Alignof(doltgres_TupleUsizeU32_t)
#define DOLTGRES_ENTRIES_ENTRY sizeof(TxnValuesEntry), _Alignof(TxnValuesEntry)
#define DOLTGRES_ENTRIES_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_ENTRIES_USIZE sizeof(size_t), _Alignof(size_t)

/* doltgres_entries_Toasted_drop is Toasted's drop glue: its state's hold, its entries, its ids, and its kept places. */
void
doltgres_entries_Toasted_drop(DoltgresEntriesToasted *self)
{
	host_shared_Held_drop(&self->state);
	std_vec_free_Checked(&self->entries, DOLTGRES_ENTRIES_ENTRY);
	std_vec_free_Checked(&self->ids, DOLTGRES_ENTRIES_U32);
	std_vec_free_Checked(&self->kept, DOLTGRES_ENTRIES_USIZE);
}

/* doltgres_entries_Keeping_drop is Keeping's drop glue: its state's hold and its kept values. */
void
doltgres_entries_Keeping_drop(DoltgresEntriesKeeping *self)
{
	host_shared_Held_drop(&self->state);
	std_vec_free_Checked(&self->values, DOLTGRES_ENTRIES_KEPT);
}

/* doltgres_entries_placed_closure_1 is placed's packed: whether a chunk is in the repository's packs already. */
static bool
doltgres_entries_placed_closure_1(void *captures, HostIoIo *io, const ChunksAddressAddress *address, bool *result,
								  HostError *error)
{
	TreesStoreNodeStore store = TREES_STORE_READER_NODE_STORE((TreesStoreReader *) captures);
	ChunksTrailerLocator unlocated = {0};
	ChunksChunkChunk chunk;
	ChunksErrorError failed;

	if (store.ops->read_record(store.self, io, address, unlocated, &chunk, &failed))
	{
		chunks_chunk_Chunk_drop(&chunk);
		*result = true;
		return true;
	}
	if (failed.tag == CHUNKS_ERROR_ERROR_MISSING)
	{
		chunks_error_Error_drop(&failed);
		*result = false;
		return true;
	}
	*error = txn_base_storage(&failed);
	return false;
}

/*
 * doltgres_entries_placed_frame_t is placed's state across its pad: the
 * chunks cut and the values moved so far, and the delta's hold, each while
 * live.
 */
typedef struct doltgres_entries_placed_frame_t
{
	TxnBlobsCut cut;
	bool		cut_live;
	HostSharedHeld deltas;
	bool		deltas_live;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	AccessHeaptuplePlacing *placing;
	StdVec	   *result;
	bool		result_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_placed_frame_t;

/* doltgres_entries_placed_pad frees what placed holds. */
static void
doltgres_entries_placed_pad(void *frame_)
{
	doltgres_entries_placed_frame_t *frame = frame_;

	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	if (frame->result_live)
		std_vec_free_Checked(frame->result, DOLTGRES_ENTRIES_MOVED);
	if (frame->cut_live)
		txn_blobs_Cut_drop(&frame->cut);
	frame->deltas_live = frame->result_live = frame->cut_live = false;
}

/* doltgres_entries_placed_body cuts each blob the placing asks for, then writes the chunks into the delta. */
static void
doltgres_entries_placed_body(void *frame_)
{
	doltgres_entries_placed_frame_t *frame = frame_;
	AccessHeaptupleBlob blob;
	ChunksErrorError failed;
	HostError	host_failed;
	ChunksTrailerLocator unlocated = {0};

	txn_blobs_Cut_Default_default(&frame->cut);
	frame->cut_live = true;
	std_vec_new(frame->result, DOLTGRES_ENTRIES_MOVED);
	frame->result_live = true;
	while (access_heaptuple_Placing_blob(frame->placing, &blob))
	{
		size_t		field;
		bool		has_field = access_heaptuple_Placing_field(frame->placing, &field);
		TreesStoreTreeRef root;

		if (blob.tag == ACCESS_HEAPTUPLE_BLOB_VALUE)
		{
			if (!trees_blob_write_blob(frame->io, TXN_BLOBS_CUT_NODE_STORE(&frame->cut), blob.u.value.ptr,
									   blob.u.value.len, TREES_CHUNKER_DEFAULT_TARGET, &root, &failed))
			{
				*frame->error = doltgres_entries_storage_error(&failed);
				return;
			}
			if (has_field)
			{
				doltgres_TupleUsizeAddressU64_t moved = {field, root.address, (uint64) blob.u.value.len};

				std_vec_push_Checked(frame->result, &moved, DOLTGRES_ENTRIES_MOVED);
			}
		}
		else
		{
			TreesStoreNodeStore cut = TXN_BLOBS_CUT_NODE_STORE(&frame->cut);
			ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
			StdVec		node;
			bool		wrote;

			std_slice_to_vec_Checked(&node, blob.u.chunk.node.ptr, blob.u.chunk.node.len, 1, 1);
			wrote = cut.ops->write(cut.self, frame->io, &node, blob.u.chunk.locators, blob.u.chunk.locators_len,
								   &stamps, &root, &failed);
			chunks_trailer_Stamps_drop(&stamps);
			if (!wrote)
			{
				*frame->error = doltgres_entries_storage_error(&failed);
				return;
			}
		}
		if (!access_heaptuple_Placing_wrote(frame->placing, root.address, unlocated, frame->error))
			return;
	}
	if (frame->cut.chunks.len > 0)
	{
		uint32		database = pg_calls_rel_storage(frame->rel).database;
		TreesStoreReader store;
		HostXid		sub;
		HostXid		top;
		TxnMemtableVersionWriter writer;
		bool		written;

		if (database == 0)
		{
			*frame->error = doltgres_entries_internal("a table has no database", sizeof("a table has no database") - 1);
			return;
		}
		if (!txn_memtable_branch_deltas(frame->io, database, repo_Repo_own(frame->repo), &frame->deltas,
										&host_failed))
		{
			*frame->error = pghost_raised(&host_failed);
			return;
		}
		frame->deltas_live = true;
		store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
		if (!host_current_writer(HOST_IO_IO_IO_DOORS(frame->io), &sub, &top, &host_failed))
		{
			*frame->error = pghost_raised(&host_failed);
			return;
		}
		writer.xid = sub.f0;
		writer.has_top = sub.f0 != top.f0;
		writer.top = writer.has_top ? top.f0 : 0;
		writer.command = 0;
		frame->cut_live = false;
		written = txn_blobs_written(frame->io, &frame->deltas, database, writer, &frame->cut.chunks,
									pg_calls_rel_permanent(frame->rel), doltgres_entries_placed_closure_1, &store,
									NULL, &host_failed);
		if (!written)
		{
			*frame->error = pghost_raised(&host_failed);
			return;
		}
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_placed writes the blobs a row's or an index entry's
 * placing asks for: each value is cut into its blob tree's chunks, each
 * overflow chunk is kept as it is, every chunk is written into the
 * repository's working state's delta through blobs' written before the
 * change's window, and each blob's root goes back to the placing with the
 * default locator of a chunk no pack holds yet. It returns each value's
 * field, root, and length.
 */
bool
doltgres_entries_placed(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const PgRel *rel,
						AccessHeaptuplePlacing *placing, StdVec *result, PgPgError *error)
{
	doltgres_entries_placed_frame_t frame = {.io = io, .repo = repo, .reading = reading, .rel = rel,
	.placing = placing, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_placed_body, doltgres_entries_placed_pad);
	doltgres_entries_placed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_toast_state opens the TOAST relation a table's out-of-line
 * values are mapped in, and its storage's state, which the transaction then
 * holds to its end, so its commit moves the state's totals.
 */
bool
doltgres_entries_toast_state(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
							 const HostSharedReader *reading, const PgRel *rel, TxnRecordToast *result_0,
							 HostSharedHeld *result_1, PgPgError *error)
{
	uint32		table = pg_calls_rel_toast_id(rel);
	DgStorage	located;
	HostStorage storage;
	TxnStateMade made = {.table = table};
	TxnStatePlace place;
	HostError	failed;
	bool		some;

	if (!pg_calls_toast_storage(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), table, &located,
								error))
		return false;
	storage.tablespace = located.tablespace;
	storage.database = located.database;
	storage.relation = located.relation;
	made.key.tag = TXN_BASE_KEY_NONE;
	if (!txn_state_state(io, repo, reading, &storage, doltgres_entries_persistence(rel), true, &made, result_1,
						 &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (!txn_state_held_to_end(io, &storage, &some, &place, &failed))
	{
		host_shared_Held_drop(result_1);
		*error = pghost_raised(&failed);
		return false;
	}
	result_0->tablespace = located.tablespace;
	result_0->relation = located.relation;
	result_0->table = table;
	return true;
}

/*
 * doltgres_entries_toasted_closure_0 is toasted's sort key, a moved value's
 * field, as sort_unstable_by_key compares by it.
 */
static StdOrdering
doltgres_entries_toasted_closure_0(void *captures, const void *a, const void *b)
{
	size_t		x = ((const doltgres_TupleUsizeAddressU64_t *) a)->f0;
	size_t		y = ((const doltgres_TupleUsizeAddressU64_t *) b)->f0;

	return x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/*
 * doltgres_entries_toasted_frame_t is toasted's state across its pad: the
 * values moved, the update's keeping, the TOAST state when made here, the
 * new values' ids and entries, the kept places, and the ids so far, each
 * while live.
 */
typedef struct doltgres_entries_toasted_frame_t
{
	StdVec		moved;
	bool		moved_live;
	DoltgresEntriesKeeping keeping;
	bool		keeping_live;
	StdVec		keeping_ids;
	bool		keeping_ids_live;
	DoltgresEntriesToasted made;
	bool		made_live;
	StdVec		stored;
	bool		stored_live;
	StdVec		kept;
	bool		kept_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	AccessHeaptuplePlacing *placing;
	bool	   *has_result;
	DoltgresEntriesToasted *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_toasted_frame_t;

/* doltgres_entries_toasted_pad frees what toasted holds. */
static void
doltgres_entries_toasted_pad(void *frame_)
{
	doltgres_entries_toasted_frame_t *frame = frame_;

	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, 1, 1);
	if (frame->keeping_ids_live)
		std_vec_free_Checked(&frame->keeping_ids, DOLTGRES_ENTRIES_U32);
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, DOLTGRES_ENTRIES_U32);
	if (frame->made_live)
		doltgres_entries_Toasted_drop(&frame->made);
	if (frame->keeping_live)
		doltgres_entries_Keeping_drop(&frame->keeping);
	if (frame->moved_live)
		std_vec_free_Checked(&frame->moved, DOLTGRES_ENTRIES_MOVED);
	frame->kept_live = frame->keeping_ids_live = frame->stored_live = frame->made_live = false;
	frame->keeping_live = frame->moved_live = false;
}

/* <[(usize, Address, u64)]>::sort_unstable_by_key, which toasted sorts its moved values by. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS TupleUsizeAddressU64
#define STD_SORT_T doltgres_TupleUsizeAddressU64_t
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_WITH_BY
#define STD_SORT_WITHOUT_UNSTABLE
#include "doltgres/std/sort_template.h"

/* doltgres_entries_toasted_body writes the blobs, gives each new value a free id, and sets each value's id in order. */
static void
doltgres_entries_toasted_body(void *frame_)
{
	doltgres_entries_toasted_frame_t *frame = frame_;
	DoltgresEntriesToasted *made = &frame->made;
	TreesStoreReader store;
	HostError	failed;
	size_t		next_stored = 0;
	size_t		next_kept = 0;

	if (!doltgres_entries_placed(frame->io, frame->repo, frame->reading, frame->rel, frame->placing, &frame->moved,
								 frame->error))
		return;
	frame->moved_live = true;
	if (frame->keeping_live)
	{
		std_vec_with_capacity_Checked(&frame->keeping_ids, frame->keeping.values.len, DOLTGRES_ENTRIES_U32);
		frame->keeping_ids_live = true;
		for (size_t i = 0; i < frame->keeping.values.len; i++)
			std_vec_push_Checked(&frame->keeping_ids, &STD_VEC_AT(&frame->keeping.values, doltgres_TupleUsizeU32Entry_t, i).f1,
								 DOLTGRES_ENTRIES_U32);
		made->toast = frame->keeping.toast;
		made->state = frame->keeping.state;
		frame->keeping_live = false;
		std_vec_free_Checked(&frame->keeping.values, DOLTGRES_ENTRIES_KEPT);
	}
	else if (frame->moved.len == 0)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	else if (!doltgres_entries_toast_state(frame->caps, frame->io, frame->repo, frame->reading, frame->rel,
										   &made->toast, &made->state, frame->error))
		return;
	std_vec_new(&made->entries, DOLTGRES_ENTRIES_ENTRY);
	std_vec_new(&made->ids, DOLTGRES_ENTRIES_U32);
	std_vec_new(&made->kept, DOLTGRES_ENTRIES_USIZE);
	frame->made_live = true;
	std_vec_new(&frame->stored, DOLTGRES_ENTRIES_U32);
	frame->stored_live = true;
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	std_slice_sort_unstable_by_TupleUsizeAddressU64(frame->moved.ptr, frame->moved.len,
													doltgres_entries_toasted_closure_0, NULL, NULL);
	for (size_t i = 0; i < frame->moved.len; i++)
	{
		const doltgres_TupleUsizeAddressU64_t *moved = &STD_VEC_AT(&frame->moved, doltgres_TupleUsizeAddressU64_t, i);
		TxnValuesEntry entry;
		uint32		id;
		bool		free;

		for (;;)
		{
			if (!pg_calls_new_object_id(pg_CatalogCaps_may_raise(frame->caps), &id, frame->error))
				return;
			if (!txn_values_free(frame->io, TREES_STORE_READER_NODE_STORE(&store), &made->state, id, &free,
								 &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			if (free)
				break;
		}
		std_vec_push_Checked(&frame->stored, &id, DOLTGRES_ENTRIES_U32);
		entry = txn_values_Entry_new(moved->f1, moved->f2);
		std_vec_push_Checked(&made->entries, &entry, DOLTGRES_ENTRIES_ENTRY);
	}
	access_heaptuple_Placing_kept(frame->placing, &frame->kept);
	frame->kept_live = true;
	for (size_t place = 0; place < frame->kept.len; place++)
	{
		bool		kept = STD_VEC_AT(&frame->kept, bool, place);
		const StdVec *from = kept ? &frame->keeping_ids : &frame->stored;
		size_t	   *next = kept ? &next_kept : &next_stored;

		if (kept)
			std_vec_push_Checked(&made->kept, &place, DOLTGRES_ENTRIES_USIZE);
		if (*next >= from->len)
		{
			static const char message[] = "a row's values out of line outnumber their ids";

			*frame->error = doltgres_entries_internal(message, sizeof(message) - 1);
			return;
		}
		std_vec_push_Checked(&made->ids, &STD_VEC_AT(from, uint32, (*next)++), DOLTGRES_ENTRIES_U32);
	}
	*frame->has_result = true;
	*frame->result = *made;
	frame->made_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_toasted writes the blobs a row's placing asks for, as
 * placed writes them, and each value takes a value id the table's TOAST
 * relation's value map doesn't hold. It returns the values' TOAST relation,
 * ids, and map entries, an update's kept values' ids at their places, or
 * none for a row with no value out of line; it takes the update's keeping,
 * NULL for none. A value's raw size is its length with its varlena header,
 * as Postgres' va_rawsize counts it, and its stored size its bytes.
 */
bool
doltgres_entries_toasted(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, AccessHeaptuplePlacing *placing,
						 DoltgresEntriesKeeping *keeping, bool *has_result, DoltgresEntriesToasted *result,
						 PgPgError *error)
{
	doltgres_entries_toasted_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
	.placing = placing, .has_result = has_result, .result = result, .error = error};

	if (keeping != NULL)
	{
		frame.keeping = *keeping;
		frame.keeping_live = true;
	}
	std_landing(&frame, doltgres_entries_toasted_body, doltgres_entries_toasted_pad);
	doltgres_entries_toasted_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_keeping_frame_t is keeping's state across its pad: the
 * kept columns, the TOAST state, and the values found so far, each while
 * live.
 */
typedef struct doltgres_entries_keeping_frame_t
{
	StdVec		columns;
	bool		columns_live;
	DoltgresEntriesKeeping made;
	bool		state_live;
	bool		values_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	const PgSlot *old;
	const PgSlot *new_;
	const size_t *key_columns;
	size_t		key_columns_len;
	bool	   *has_result;
	DoltgresEntriesKeeping *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_keeping_frame_t;

/* doltgres_entries_keeping_pad frees what keeping holds. */
static void
doltgres_entries_keeping_pad(void *frame_)
{
	doltgres_entries_keeping_frame_t *frame = frame_;

	if (frame->values_live)
		std_vec_free_Checked(&frame->made.values, DOLTGRES_ENTRIES_KEPT);
	if (frame->state_live)
		host_shared_Held_drop(&frame->made.state);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, DOLTGRES_ENTRIES_COLUMN);
	frame->values_live = frame->state_live = frame->columns_live = false;
}

/*
 * doltgres_entries_keeping_body finds the columns the new row keeps out of
 * line as the old row has them, then their entries.
 */
static void
doltgres_entries_keeping_body(void *frame_)
{
	doltgres_entries_keeping_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint32		table = pg_calls_rel_toast_id(frame->rel);
	TreesStoreReader store;
	HostError	failed;
	int32		natts;

	if (table == 0)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	std_vec_new(&frame->columns, DOLTGRES_ENTRIES_COLUMN);
	frame->columns_live = true;
	natts = pg_calls_slot_natts(frame->new_);
	for (int32 column = 0; column < natts; column++)
	{
		uint32		id;
		uint32		old_id;
		size_t		at = column < 0 ? SIZE_MAX : (size_t) column;
		bool		key = false;

		if (!pg_calls_slot_value_external(raise, frame->new_, column, table, &id, frame->error))
			return;
		for (size_t k = 0; k < frame->key_columns_len && !key; k++)
			key = frame->key_columns[k] == at;
		if (id == 0 || key)
			continue;
		if (!pg_calls_slot_value_external(raise, frame->old, column, table, &old_id, frame->error))
			return;
		if (old_id == id)
		{
			doltgres_TupleUsizeU32_t kept = {at, id};

			std_vec_push_Checked(&frame->columns, &kept, DOLTGRES_ENTRIES_COLUMN);
		}
	}
	if (frame->columns.len == 0)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_toast_state(frame->caps, frame->io, frame->repo, frame->reading, frame->rel,
									  &frame->made.toast, &frame->made.state, frame->error))
		return;
	frame->state_live = true;
	store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	std_vec_new(&frame->made.values, DOLTGRES_ENTRIES_KEPT);
	frame->values_live = true;
	for (size_t i = 0; i < frame->columns.len; i++)
	{
		const doltgres_TupleUsizeU32_t *column = &STD_VEC_AT(&frame->columns, doltgres_TupleUsizeU32_t, i);
		doltgres_TupleUsizeU32Entry_t value = {column->f0, column->f1};
		bool		has;

		if (!txn_values_entry(frame->io, TREES_STORE_READER_NODE_STORE(&store), &frame->made.state, column->f1,
							  &has, &value.f2, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (!has)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("a kept value's map entry "), STD_FMT_ARG(0),
			STD_FMT_LIT(" is missing")};
			StdFmtArg	args[] = {{&column->f1, std_u32_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*frame->error = pg_PgError_fork((const uint8 *) "XX001", &message);
			return;
		}
		std_vec_push_Checked(&frame->made.values, &value, DOLTGRES_ENTRIES_KEPT);
	}
	*frame->has_result = true;
	*frame->result = frame->made;
	frame->values_live = frame->state_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_keeping finds what an update keeps out of line from the
 * row it replaces, as heap's toast_tuple_init keeps an unchanged column's
 * toast pointer: each column other than a key column whose new value is an
 * on-disk toast pointer into the table's TOAST relation naming the same
 * value id as the old row's, with that value's map entry. A key column is
 * left out, since the key holds its value whole.
 */
bool
doltgres_entries_keeping(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, const PgSlot *old, const PgSlot *new_,
						 const size_t *key_columns, size_t key_columns_len, bool *has_result,
						 DoltgresEntriesKeeping *result, PgPgError *error)
{
	doltgres_entries_keeping_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
		.old = old, .new_ = new_, .key_columns = key_columns, .key_columns_len = key_columns_len,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_keeping_body, doltgres_entries_keeping_pad);
	doltgres_entries_keeping_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_serial_read returns a serializable read's check of a
 * relation's rows under a snapshot, its transaction's xmin and own xid, or
 * none when Postgres needs no check of the read
 * (CheckForSerializableConflictOutNeeded).
 */
static bool
doltgres_entries_serial_read(const PgMayRaise *raise, const PgRel *relation, const PgSnapshot *snapshot,
							 bool *has_result, TxnVisibleSerial *result, PgPgError *error)
{
	bool		needed;
	uint64		own;
	uint64		xmin;

	if (!pg_calls_serial_needed(raise, relation, snapshot, &needed, error))
		return false;
	*has_result = needed;
	if (!needed)
		return true;
	own = pg_calls_top_xid_if_any();
	if (!pg_calls_serial_xmin(raise, &xmin, error))
		return false;
	result->xmin.f0 = xmin;
	result->has_own = own != 0;
	result->own.f0 = own;
	return true;
}

/* doltgres_entries_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
doltgres_entries_u64_lt(void *captures, const void *a, const void *b)
{
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* doltgres_entries_u64_eq is u64's PartialEq, which conflicts_out's dedup takes. */
static bool
doltgres_entries_u64_eq(const void *self, const void *other)
{
	return *(const uint64 *) self == *(const uint64 *) other;
}

/* <[u64]>::sort_unstable, which conflicts_out sorts the writers by. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT doltgres_entries_u64_lt
#include "doltgres/std/sort_template.h"

/*
 * doltgres_entries_conflicts_out checks with Postgres each writer a
 * serializable read's examined row versions conflict out with, once each, as
 * heap's read checks each tuple it examines (CheckForSerializableConflictOut),
 * which raises Postgres' serialization failure; an xid is narrowed to 32
 * bits, exact for every unfolded writer. It takes the writers, a Vec<Xid>,
 * whose xids are its u64s.
 */
static bool
doltgres_entries_conflicts_out(const PgMayRaise *raise, const PgRel *relation, const PgSnapshot *snapshot,
							   StdVec *conflicts, PgPgError *error)
{
	bool		ok = true;

	std_slice_sort_unstable_U64(conflicts->ptr, conflicts->len);
	std_vec_dedup(conflicts, sizeof(uint64), _Alignof(uint64), doltgres_entries_u64_eq, NULL);
	for (size_t i = 0; i < conflicts->len && ok; i++)
		ok = pg_calls_serial_conflict_out(raise, relation, (uint32) STD_VEC_AT(conflicts, uint64, i), snapshot,
										  error);
	std_vec_free_Checked(conflicts, sizeof(uint64), _Alignof(uint64));
	return ok;
}

/* doltgres_entries_Row_drop is Row's drop glue, an element of rows' answer. */
void
doltgres_entries_Row_drop(void *row)
{
	doltgres_scan_Row_drop(row);
}

/* txn::state::held at Vec<Row>, which rows takes. */
#define TXN_STATE_TS VecRow
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , StdVec *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS StdVec *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_rows_captures_t is rows' lent call's captures: the
 * serializable read's check, the writers it conflicts out with, the snapshot
 * read under, the reader, the table's layout it moves in, and what the fetch
 * names its row by.
 */
typedef struct doltgres_entries_rows_captures_t
{
	bool		has_check;
	TxnVisibleSerial check;
	StdVec	   *conflicts;
	const HostSnapshot *read;
	const HostSharedReader *reading;
	DoltgresScanLayout layout;
	DoltgresEntriesBy by;
} doltgres_entries_rows_captures_t;

/* doltgres_entries_rows_closure_0_drop is rows' lent call's drop when held never lends it: its layout. */
static void
doltgres_entries_rows_closure_0_drop(void *captures)
{
	doltgres_scan_Layout_drop(&((doltgres_entries_rows_captures_t *) captures)->layout);
}

/*
 * doltgres_entries_rows_closure_0_frame_t is rows' lent call's state across
 * its pad: the layout until a fetch takes it, the base the snapshot reads,
 * and the epochs, each while live.
 */
typedef struct doltgres_entries_rows_closure_0_frame_t
{
	bool		layout_live;
	TxnBaseBase read_base;
	bool		read_base_live;
	DoltgresScanEpochs epochs;
	bool		epochs_live;
	doltgres_entries_rows_captures_t *c;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedHeld *held;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_rows_closure_0_frame_t;

/* doltgres_entries_rows_closure_0_pad frees what rows' lent call holds. */
static void
doltgres_entries_rows_closure_0_pad(void *frame_)
{
	doltgres_entries_rows_closure_0_frame_t *frame = frame_;

	if (frame->epochs_live)
		doltgres_scan_Epochs_drop(&frame->epochs);
	if (frame->read_base_live)
		txn_base_Base_drop(&frame->read_base);
	if (frame->layout_live)
		doltgres_entries_rows_closure_0_drop(frame->c);
	frame->epochs_live = frame->read_base_live = frame->layout_live = false;
}

/* doltgres_entries_rows_closure_0_body fetches the rows by the tuple id or the key under the held state. */
static void
doltgres_entries_rows_closure_0_body(void *frame_)
{
	doltgres_entries_rows_closure_0_frame_t *frame = frame_;
	doltgres_entries_rows_captures_t *c = frame->c;
	DoltgresScanUnder under = {.state = host_shared_Held_record(frame->held), .snapshot = c->read,
	.has_serial = c->has_check};
	TreesStoreReader store = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	DoltgresScanTrees trees = {0};
	uint32		epoch;
	bool		has_chain;
	TreesStoreTree chain;
	bool		some;

	under.serial.read = c->check;
	under.serial.conflicts = c->conflicts;
	if (!txn_state_readable(frame->io, frame->held, c->read, &some, &frame->read_base, frame->error))
		return;
	frame->read_base_live = some;
	if (some)
	{
		trees.has_root = frame->read_base.has_root;
		trees.root = frame->read_base.root;
		trees.has_tuple_ids = frame->read_base.has_tuple_ids;
		trees.tuple_ids = frame->read_base.tuple_ids;
		trees.folded = frame->read_base.folded;
	}
	doltgres_scan_Epochs_based(some ? &frame->read_base : NULL, &epoch, &has_chain, &chain);
	if (!doltgres_scan_Epochs_of(frame->io, TREES_STORE_READER_NODE_STORE(&store), epoch, has_chain, chain,
								 &frame->epochs, frame->error))
		return;
	frame->epochs_live = true;
	frame->layout_live = false;
	if (c->by.tag == DOLTGRES_ENTRIES_BY_TID)
	{
		DoltgresScanRow row;
		bool		has;

		if (!doltgres_scan_fetch(frame->io, TREES_STORE_READER_NODE_STORE(&store), &under, &frame->epochs, trees,
								 &c->layout, c->by.tuple_id, &has, &row, frame->error))
			return;
		std_vec_new(frame->result, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
		if (has)
			std_vec_push_Checked(frame->result, &row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	}
	else if (!doltgres_scan_fetch_key(frame->io, TREES_STORE_READER_NODE_STORE(&store), &under, &frame->epochs,
									  trees, &c->layout, c->by.key, c->by.key_len, frame->result, frame->error))
		return;
	frame->ok = true;
}

/* doltgres_entries_rows_closure_0 is rows' lent call: the fetch of the rows under the held state. */
static bool
doltgres_entries_rows_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								StdVec *result, HostError *error)
{
	doltgres_entries_rows_closure_0_frame_t frame = {.layout_live = true, .c = captures, .io = io, .repo = repo,
	.held = held, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_rows_closure_0_body, doltgres_entries_rows_closure_0_pad);
	doltgres_entries_rows_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_rows_frame_t is rows' state across its pad: the writers
 * the read conflicts out with, the repository's handle, the reader, the
 * recorded key, its columns until key_order takes them, the layout until the
 * lent call takes it, and what held takes, each while live.
 */
typedef struct doltgres_entries_rows_frame_t
{
	StdVec		conflicts;
	bool		conflicts_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnBaseKey	key;
	bool		key_live;
	StdVec		columns;
	bool		columns_live;
	doltgres_entries_rows_captures_t captures;
	bool		keyed_live;
	bool		layout_live;
	TxnStateFound found;
	bool		found_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *relation;
	const PgSnapshot *snapshot;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_rows_frame_t;

/* doltgres_entries_rows_pad frees what rows holds. */
static void
doltgres_entries_rows_pad(void *frame_)
{
	doltgres_entries_rows_frame_t *frame = frame_;

	if (frame->found_live && frame->found.tag == TXN_STATE_FOUND_MADE)
		txn_base_Key_drop(&frame->found.u.made.f1.key);
	if (frame->layout_live)
		doltgres_scan_Layout_drop(&frame->captures.layout);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->captures.layout.keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	if (frame->conflicts_live)
		std_vec_free_Checked(&frame->conflicts, sizeof(uint64), _Alignof(uint64));
	frame->found_live = frame->layout_live = frame->keyed_live = frame->columns_live = frame->key_live = false;
	frame->reading_live = frame->repo_live = frame->conflicts_live = false;
}

/*
 * doltgres_entries_rows_body reads the storage's layout and fetches its rows
 * under the held state, then checks conflicts.
 */
static void
doltgres_entries_rows_body(void *frame_)
{
	doltgres_entries_rows_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	TxnBaseBase base;
	HostError	failed;

	if (frame->snapshot != NULL &&
		!doltgres_entries_serial_read(raise, frame->relation, frame->snapshot, &frame->captures.has_check,
									  &frame->captures.check, frame->error))
		return;
	std_vec_new(&frame->conflicts, sizeof(uint64), _Alignof(uint64));
	frame->conflicts_live = true;
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
	if (!doltgres_entries_recorded(frame->caps, &frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
								   &frame->reading, frame->relation, &base, frame->error))
		return;
	frame->key = base.key;
	frame->key_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->captures.layout.keyed);
	frame->keyed_live = true;
	frame->columns_live = false;
	if (!doltgres_layout_key_order(raise, catalog, &frame->key, &frame->columns, &frame->captures.layout.order,
								   frame->error))
		return;
	frame->captures.layout.table = pg_calls_rel_id(frame->relation);
	frame->keyed_live = false;
	frame->layout_live = true;
	if (!doltgres_entries_making(frame->caps, &frame->io, frame->relation, &frame->found, frame->error))
		return;
	frame->found_live = true;
	frame->captures.conflicts = &frame->conflicts;
	frame->captures.reading = &frame->reading;
	frame->found_live = frame->layout_live = false;
	if (!txn_state_held_VecRow(&frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
							   frame->found, doltgres_entries_rows_closure_0, &frame->captures,
							   doltgres_entries_rows_closure_0_drop, frame->result, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (frame->snapshot != NULL)
	{
		frame->conflicts_live = false;
		if (!doltgres_entries_conflicts_out(raise, frame->relation, frame->snapshot, &frame->conflicts,
											frame->error))
		{
			std_vec_drop_Checked(frame->result, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
								 doltgres_entries_Row_drop);
			return;
		}
	}
	frame->ok = true;
}

/*
 * doltgres_entries_rows returns the row versions of a relation's storage a
 * snapshot sees by a tuple id, at most one, as scan's fetch reads it, or by a
 * row key, as scan's fetch_key reads them, through the storage's state the
 * backend's top-level transaction holds, with the key the storage's base
 * records, and checks the writers a serializable read's examined versions
 * conflict out with; a read with no snapshot of Postgres' own (snapshot NULL),
 * SnapshotAny's, is no serializable read.
 */
bool
doltgres_entries_rows(const PgCatalogCaps *caps, const PgRel *relation, const PgSnapshot *snapshot,
					  const HostSnapshot *read, DoltgresEntriesBy by, StdVec *result, PgPgError *error)
{
	doltgres_entries_rows_frame_t frame = {.captures = {.read = read, .by = by}, .caps = caps,
	.relation = relation, .snapshot = snapshot, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_rows_body, doltgres_entries_rows_pad);
	doltgres_entries_rows_pad(&frame);
	return frame.ok;
}

/*
 * DOLTGRES_ENTRIES_RESERVED_WORKERS is the worker slots the fork reserves
 * beside max_worker_processes for its static workers.
 */
#define DOLTGRES_ENTRIES_RESERVED_WORKERS 2

/*
 * doltgres_entries_prepare readies the process for an ordinary entry, which
 * every entry's thunk calls first: it installs the server's host when the
 * process has none, as in bootstrap mode, which runs no preload window, and
 * the re-form of a fold's row its reformed door calls.
 */
void
doltgres_entries_prepare(void)
{
	pghost_install();
	pghost_install_reform(doltgres_reform_reformed_row);
}

/* doltgres_entries_huge asks for an allocation no process can have, which fails as the binding's tests ask. */
static void
doltgres_entries_huge(void)
{
	StdVec		huge;

	std_vec_with_capacity_Checked(&huge, (size_t) (PTRDIFF_MAX / 2), DOLTGRES_ENTRIES_BYTES);
	std_vec_free_Checked(&huge, DOLTGRES_ENTRIES_BYTES);
}

/*
 * doltgres_entries_doltgres_shared_preload is the fork's first entry in every
 * process, inside the preload window; its thunk has installed the
 * allocation error hook and the reserve before it runs. It registers the
 * fork's resource manager, sets the worker slots it reserves, registers the
 * collector's worker and the fold launcher, and registers shared memory. The
 * developer setting debug_fail_preload_allocation fails its first allocation
 * in the postmaster or in each child, for the binding's tests.
 */
bool
doltgres_entries_doltgres_shared_preload(const PgCaps *caps, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	int32		fail = pg_calls_settings_debug_fail_preload_allocation();

	if ((fail == PG_CALLS_SETTINGS_DEBUG_FAIL_PRELOAD_ALLOCATION_POSTMASTER && !pg_calls_is_under_postmaster()) ||
		(fail == PG_CALLS_SETTINGS_DEBUG_FAIL_PRELOAD_ALLOCATION_CHILD && pg_calls_is_under_postmaster()))
		doltgres_entries_huge();
	pg_calls_register_rmgr();
	pg_calls_set_reserved_workers(STD_ADD(i32, DOLTGRES_ENTRIES_RESERVED_WORKERS, pg_calls_settings_fold_workers(),
										  72, 30));
	return pg_calls_register_collector(raise, error) && pg_calls_register_fold_launcher(raise, error) &&
		pg_calls_register_shmem(raise, error);
}

/*
 * doltgres_entries_doltgres_dolt_version is dolt_version(): the fork's
 * version as text, as Dolt's function returns it. Its injection points let
 * the binding's tests raise an error with interrupts held, wait where a
 * FATAL can end the backend, and raise a panic and an allocation failure
 * inside an entry.
 */
bool
doltgres_entries_doltgres_dolt_version(const PgCatalogCaps *caps, const PgCall *fcinfo, PgDatum *result,
									   PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	bool		attached;
	bool		reached;

	if (!pg_calls_injection_point_attached(raise, "doltgres-version-panic", &attached, error))
		return false;
	if (attached)
		STD_PANIC(83, 9, "injected panic in dolt_version");
	if (!pg_calls_injection_point_attached(raise, "doltgres-version-out-of-memory", &attached, error))
		return false;
	if (attached)
		doltgres_entries_huge();
	if (!pg_calls_injection_point(raise, "doltgres-version-wait", error))
		return false;
	pg_calls_hold_interrupts();
	reached = pg_calls_injection_point(raise, "doltgres-version", error);
	pg_calls_resume_interrupts();
	return reached && pg_calls_text_datum(raise, DOLTGRES_VERSION, result, error);
}

/* doltgres_entries_node_cache_layout returns the shared node cache's frames for node_cache_size. */
static ShmemCacheLayout
doltgres_entries_node_cache_layout(void)
{
	int32		size = pg_calls_settings_node_cache_size();
	size_t		kilobytes = size < 0 ? (size_t) 0 - (size_t) size : (size_t) size;

	return pghost_cache_layout(STD_MUL(usize, kilobytes, 1024, 113, 26));
}

/* doltgres_entries_kinds returns the shared records' kinds the server keeps, as shmem sizes and sets them up. */
static bool
doltgres_entries_kinds(const PgMayRaise *raise, StdVec *result, PgPgError *error)
{
	size_t		len;
	const HostSharedKind *kinds = doltgres_kinds_KINDS(&len);

	return pghost_shared_kinds(raise, kinds, len, result, error);
}

/* doltgres_entries_doltgres_shmem_request asks Postgres for the fork's shared memory, in the postmaster. */
bool
doltgres_entries_doltgres_shmem_request(const PgCaps *caps, void *arg, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	ShmemCacheLayout layout = doltgres_entries_node_cache_layout();
	uint64		bytes = (uint64) pghost_delta_bytes();
	uint64		blocks = host_delta_page_blocks(bytes);
	StdVec		kinds;
	bool		requested;

	if (!doltgres_entries_kinds(raise, &kinds, error))
		return false;
	requested = shmem_request(raise, kinds.ptr, kinds.len, error);
	std_vec_free_Checked(&kinds, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
	return requested && shmem_cache_request(raise, &layout, error) && shmem_keys_request(raise, error) &&
		shmem_collection_request(raise, error) &&
		shmem_delta_request(raise, (size_t) host_delta_page_directory_slots(bytes), (size_t) blocks,
							(size_t) STD_MUL(u64, blocks, HOST_DELTA_PAGE_BLOCK, 107, 10), error);
}

/* doltgres_entries_doltgres_shmem_init sets the fork's shared memory up, in the postmaster. */
bool
doltgres_entries_doltgres_shmem_init(const PgCaps *caps, void *arg, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	ShmemCacheLayout layout = doltgres_entries_node_cache_layout();
	uint64		bytes = (uint64) pghost_delta_bytes();
	StdVec		kinds;
	bool		initialized;

	if (!doltgres_entries_kinds(raise, &kinds, error))
		return false;
	initialized = shmem_initialize(raise, kinds.ptr, kinds.len, error);
	std_vec_free_Checked(&kinds, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
	return initialized && shmem_cache_initialize(raise, &layout, error) && shmem_keys_initialize(raise, error) &&
		shmem_collection_initialize(raise, error) &&
		shmem_delta_initialize(raise, (size_t) host_delta_page_directory_slots(bytes),
							   (size_t) host_delta_page_blocks(bytes), error);
}

/*
 * doltgres_entries_doltgres_process_start registers the fork's two exit
 * callbacks, at the end of every process's BaseInit: the release first, so
 * the callback that may raise runs before it, and both after every callback
 * Postgres registers later.
 */
bool
doltgres_entries_doltgres_process_start(const PgCaps *caps, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);

	return pg_calls_register_exit(raise, error) && pg_calls_register_xact_end(raise, error) &&
		pg_calls_register_subxact_end(raise, error) && pg_calls_register_key_settle(raise, error);
}

/* doltgres_entries_raised answers whether a host call succeeded, else gives its error as the entry raises it. */
static bool
doltgres_entries_raised(bool ok, HostError *failed, PgPgError *error)
{
	if (!ok)
		*error = pghost_raised(failed);
	return ok;
}

/*
 * doltgres_entries_doltgres_subxact_ended is the fork's subtransaction
 * callback over its list of the keys its transaction wrote to: a
 * subtransaction's start marks where its keys begin, its commit leaves them
 * to its parent, and its abort, which Postgres records before it calls this,
 * takes back its versions and its children's by pruning the keys since its
 * mark. It marks and ends each subtransaction in the settle's list too.
 */
bool
doltgres_entries_doltgres_subxact_ended(const PgCaps *caps, int32 event, uint32 mySubid, uint32 parentSubid,
										void *arg, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;
	bool		ok = true;

	if (event == SUBXACT_EVENT_START_SUB)
		ok = doltgres_settle_sub_started(&io, mySubid, &failed) && txn_written_sub_started(&io, mySubid, &failed);
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		doltgres_settle_sub_ended(&io, mySubid, false);
		ok = txn_written_sub_committed(&io, mySubid, &failed);
	}
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		doltgres_layouts_sub_aborted(&io, mySubid);
		doltgres_settle_sub_ended(&io, mySubid, true);
		ok = txn_written_sub_aborted(&io, mySubid, &failed);
	}
	return doltgres_entries_raised(ok, &failed, error);
}

/*
 * doltgres_entries_doltgres_checkpoint is the fork's work at each checkpoint
 * and restartpoint, at the end of CheckPointGuts, by its redo point: it
 * writes every repository's runs of the delta and saves the repository with
 * them, then writes each marked repository's newest manifest once more,
 * stores the redo point the WAL is kept from, and checks that WAL while it
 * is kept. It then asks the fold launcher for folds, so the delta the next
 * checkpoint writes as runs shrinks.
 */
bool
doltgres_entries_doltgres_checkpoint(const PgCaps *caps, uint64 redo, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (!doltgres_entries_raised(txn_runs_checkpoint(&io, redo, &failed) && store_checkpoint(&io, redo, &failed),
								 &failed, error))
		return false;
	shmem_delta_ask_fold();
	return true;
}

/*
 * doltgres_entries_doltgres_startup_open is the startup process's open of
 * every repository before the redo decision, in StartupXLOG, at the control
 * file's redo point and, at a start from a base backup's label, the label's
 * checkpoint, and its load of each repository's checkpoint runs into the
 * delta.
 */
bool
doltgres_entries_doltgres_startup_open(const PgCaps *caps, uint64 redo, uint64 label, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	TxnInsertPage page = doltgres_entries_page();
	HostError	failed;

	return doltgres_entries_raised(store_open_startup_open(&io, redo, label, &failed) &&
								   txn_runs_loaded(&io, &page, &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_unlogged_cleanup empties every unlogged storage,
 * in StartupXLOG beside ResetUnloggedRelations(UNLOGGED_RELATION_CLEANUP), on
 * exactly Postgres' condition for emptying unlogged relations in recovery, a
 * standby's start included.
 */
bool
doltgres_entries_doltgres_unlogged_cleanup(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(txn_runs_cleaned(&io, &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_createdb makes a new database's repository in its
 * default tablespace and logs it, in createdb's cleanup block after
 * Postgres' copy of the template.
 */
bool
doltgres_entries_doltgres_createdb(const PgCatalogCaps *caps, uint32 template_id, uint32 database_id,
								   uint32 tablespace_id, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(store_databases_created(&io, template_id, database_id, tablespace_id, &failed),
								   &failed, error);
}

/*
 * doltgres_entries_doltgres_dropdb frees a database's states, then drops its
 * repository and checkpoints after its removal, in dropdb after Postgres'
 * removal of the database's directories.
 */
bool
doltgres_entries_doltgres_dropdb(const PgCatalogCaps *caps, uint32 database_id, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;

	if (database_id == 0)
	{
		static const char message[] = "a database's id is 0";

		*error = doltgres_entries_internal(message, sizeof(message) - 1);
		return false;
	}
	return doltgres_entries_raised(txn_runs_dropped(&io, database_id, &failed) &&
								   store_databases_dropped(&io, database_id, &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_db_file_conflict reports whether a directory of
 * a repository of a database's id stands in any tablespace, in
 * check_db_file_conflict, so that createdb passes over the id.
 */
bool
doltgres_entries_doltgres_db_file_conflict(const PgCatalogCaps *caps, uint32 database_id, bool *result,
										   PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(store_databases_conflicts(HOST_IO_IO_READ_DOORS(&io), database_id, result,
															 &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_movedb_copy copies a moving database's repository
 * into the tablespace it moves to and logs it, in movedb's cleanup block
 * after Postgres' copy.
 */
bool
doltgres_entries_doltgres_movedb_copy(const PgCatalogCaps *caps, uint32 database_id, uint32 from_tablespace,
									  uint32 to_tablespace, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(store_databases_moved_copy(&io, database_id, from_tablespace, to_tablespace,
															  &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_movedb_finish makes a moved database's copy its
 * repository and removes the old one, in movedb after its commit and
 * Postgres' drop record, before the database's lock is released.
 */
bool
doltgres_entries_doltgres_movedb_finish(const PgCatalogCaps *caps, uint32 database_id, uint32 from_tablespace,
										uint32 to_tablespace, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(store_databases_moved_finish(&io, database_id, from_tablespace, to_tablespace,
																&failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_kept_segment returns the oldest WAL segment
 * KeepLogSeg keeps, given the oldest it would keep: lower while a
 * repository's previous manifest needs the WAL from the checkpoint before.
 */
bool
doltgres_entries_doltgres_kept_segment(const PgCaps *caps, int64 segment, int32 segment_size, int64 *result,
									   PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	uint64		size = segment_size < 0 ? 1 : (uint64) segment_size;
	uint64		kept;
	HostError	failed;

	if (!doltgres_entries_raised(store_kept_segment(&io, (uint64) segment, size, &kept, &failed), &failed, error))
		return false;
	*result = (int64) kept;
	return true;
}

/*
 * doltgres_entries_doltgres_backup_started counts a base backup in and waits
 * out a running collection, in do_pg_backup_start after Postgres counts it.
 */
bool
doltgres_entries_doltgres_backup_started(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(host_backup_begin(&io, &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_backup_checkpointed keeps each repository's
 * generation of its list files and manifest under the name of the backup's
 * checkpoint, in do_pg_backup_start after the checkpoint, which a server
 * restored from the backup settles from (§III.9, bugs row 178).
 */
bool
doltgres_entries_doltgres_backup_checkpointed(const PgCaps *caps, uint64 checkpoint, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(store_kept(&io, checkpoint, &failed), &failed, error);
}

/* doltgres_entries_doltgres_backup_ended counts a base backup out, in do_pg_backup_stop and do_pg_abort_backup. */
bool
doltgres_entries_doltgres_backup_ended(const PgCleanup *caps, PgPgError *error)
{
	shmem_collection_backup_end();
	return true;
}

/*
 * DOLTGRES_ENTRIES_ROW is the length of a row database_list writes: a
 * database's id, its tablespace, and whether its repository may be collected.
 */
#define DOLTGRES_ENTRIES_ROW 12

/* DOLTGRES_ENTRIES_LAUNCHER_NAPTIME is the longest time in milliseconds the fold launcher waits between its checks. */
#define DOLTGRES_ENTRIES_LAUNCHER_NAPTIME INT64CONST(1000)

/* doltgres_entries_unsigned is collector_settings' closure: a setting's magnitude as a u64. */
static uint64
doltgres_entries_unsigned(int32 value)
{
	return value < 0 ? (uint64) 0 - (uint64) value : (uint64) value;
}

/*
 * doltgres_entries_collector_settings returns the collector's settings in the
 * units store takes, each GUC's minimum keeping it positive.
 */
static StoreCollectSettings
doltgres_entries_collector_settings(void)
{
	int32		min_free_disk = pg_calls_settings_collector_min_free_disk();
	StoreCollectSettings settings;

	settings.target_percent = doltgres_entries_unsigned(pg_calls_settings_collector_target_percent());
	settings.first_packs = (size_t) doltgres_entries_unsigned(pg_calls_settings_collector_first_packs());
	settings.full_interval =
		std_Duration_from_secs(doltgres_entries_unsigned(pg_calls_settings_collector_full_interval()));
	settings.io_limit = STD_MUL(u64, doltgres_entries_unsigned(pg_calls_settings_collector_io_limit()), 1024, 573, 19);
	settings.has_min_free_disk = min_free_disk >= 0;
	settings.min_free_disk = settings.has_min_free_disk ? doltgres_entries_unsigned(min_free_disk) << 20 : 0;
	settings.young_size = STD_MUL(u64, doltgres_entries_unsigned(pg_calls_settings_collector_young_size()), 1024, 575,
								  21);
	settings.young_packs = (size_t) doltgres_entries_unsigned(pg_calls_settings_collector_young_packs());
	return settings;
}

/* doltgres_entries_word reads a 32-bit word of a database's row in the process's byte order. */
static uint32
doltgres_entries_word(const uint8 *row, size_t at)
{
	uint32		word;

	memcpy(&word, row + at, sizeof(word));
	return word;
}

/* doltgres_entries_databases_frame_t is databases' state across its pad: the read's buffer, while live. */
typedef struct doltgres_entries_databases_frame_t
{
	StdVec		buffer;
	bool		buffer_live;
	const PgCatalogCaps *caps;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_databases_frame_t;

/* doltgres_entries_databases_pad frees what databases holds. */
static void
doltgres_entries_databases_pad(void *frame_)
{
	doltgres_entries_databases_frame_t *frame = frame_;

	if (frame->buffer_live)
		std_vec_free_Checked(&frame->buffer, DOLTGRES_ENTRIES_BYTES);
	frame->buffer_live = false;
}

/* doltgres_entries_databases_body reads pg_database's rows, growing the buffer until a read fits. */
static void
doltgres_entries_databases_body(void *frame_)
{
	doltgres_entries_databases_frame_t *frame = frame_;
	static const uint8 zero = 0;

	std_vec_new(&frame->buffer, DOLTGRES_ENTRIES_BYTES);
	frame->buffer_live = true;
	for (;;)
	{
		size_t		count;

		if (!pg_calls_database_list(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
									frame->buffer.ptr, frame->buffer.len, &count, frame->error))
			return;
		if (STD_MUL(usize, count, DOLTGRES_ENTRIES_ROW, 586, 12) <= frame->buffer.len)
		{
			std_vec_with_capacity_Checked(frame->result, count, sizeof(StoreCollectDatabase),
										  _Alignof(StoreCollectDatabase));
			for (size_t i = 0; i < count; i++)
			{
				const uint8 *row = (const uint8 *) frame->buffer.ptr + i * DOLTGRES_ENTRIES_ROW;
				StoreCollectDatabase database = {doltgres_entries_word(row, 0), doltgres_entries_word(row, 4),
				doltgres_entries_word(row, 8) != 0};

				std_vec_push_Checked(frame->result, &database, sizeof(StoreCollectDatabase),
									 _Alignof(StoreCollectDatabase));
			}
			frame->ok = true;
			return;
		}
		std_vec_resize(&frame->buffer, STD_MUL(usize, count, DOLTGRES_ENTRIES_ROW, 598, 23), &zero,
					   DOLTGRES_ENTRIES_BYTES);
	}
}

/*
 * doltgres_entries_databases returns the rows of pg_database as the collector
 * worker reads them, growing its buffer until a read fits.
 */
static bool
doltgres_entries_databases(const PgCatalogCaps *caps, StdVec *result, PgPgError *error)
{
	doltgres_entries_databases_frame_t frame = {.caps = caps, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_databases_body, doltgres_entries_databases_pad);
	doltgres_entries_databases_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_collect_frame_t is collect's state across its pad: the
 * collector's schedule and the databases of a check, each while live.
 */
typedef struct doltgres_entries_collect_frame_t
{
	StoreCollectSchedule schedule;
	bool		schedule_live;
	StdVec		databases;
	bool		databases_live;
	const PgCatalogCaps *caps;
	PgPgError  *error;
} doltgres_entries_collect_frame_t;

/* doltgres_entries_collect_pad frees what collect holds. */
static void
doltgres_entries_collect_pad(void *frame_)
{
	doltgres_entries_collect_frame_t *frame = frame_;

	if (frame->databases_live)
		std_vec_free_Checked(&frame->databases, sizeof(StoreCollectDatabase), _Alignof(StoreCollectDatabase));
	if (frame->schedule_live)
		store_collect_Schedule_drop(&frame->schedule);
	frame->databases_live = frame->schedule_live = false;
}

/* doltgres_entries_collect_body runs the collector's checks until one fails or its SIGTERM ends it. */
static void
doltgres_entries_collect_body(void *frame_)
{
	doltgres_entries_collect_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);

	if (!pg_calls_worker_connect(raise, catalog, frame->error) ||
		!pg_calls_register_collector_exit(raise, frame->error) || !pg_calls_aux_resource_owner(raise, frame->error))
		return;
	host_collector_started();
	store_collect_Schedule_Default_default(&frame->schedule);
	frame->schedule_live = true;
	for (;;)
	{
		StoreCollectSettings settings;
		HostIoIo	io;
		HostError	failed;
		uint64		word;
		uint32		database;

		if (!pg_calls_latch_sleep(raise, (int64) pg_calls_settings_collector_naptime(), frame->error) ||
			!pg_calls_config_reload_if_pending(raise, catalog, frame->error))
			return;
		settings = doltgres_entries_collector_settings();
		if (!doltgres_entries_databases(frame->caps, &frame->databases, frame->error))
			return;
		frame->databases_live = true;
		io = pghost_io(raise);
		if (host_collection_asked(&word, &database))
		{
			uint64		counts[3] = {PG_UINT64_MAX, PG_UINT64_MAX, PG_UINT64_MAX};
			uint64		done[3];
			bool		ok = store_collect_collect_asked(&io, &frame->schedule, frame->databases.ptr,
														 frame->databases.len, database, &settings, done, &failed);

			host_collection_answer(word, ok ? done : counts);
			if (!ok)
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
		}
		if (!store_collect_collect_due(&io, &frame->schedule, frame->databases.ptr, frame->databases.len, &settings,
									   &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->databases_live = false;
		std_vec_free_Checked(&frame->databases, sizeof(StoreCollectDatabase), _Alignof(StoreCollectDatabase));
	}
}

/*
 * doltgres_entries_doltgres_collect is the collector's background worker:
 * connected to no database, it reads pg_database at each check, answers a
 * backend's ask of a collection, its counts each u64::MAX when it failed,
 * and collects each repository whose collection is due, until its SIGTERM.
 * The postmaster starts it at BgWorkerStart_ConsistentState, in hot standby
 * or once running, so no marking begins in a crash's recovery or on a
 * standby without hot standby. Since it runs outside a transaction, it makes
 * a resource owner of its own, as Postgres' auxiliary processes do, which
 * the node cache's pins name, as a collection's read of a spill's filter
 * pins its pages, and which each check's read of pg_database gives back.
 */
bool
doltgres_entries_doltgres_collect(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error)
{
	doltgres_entries_collect_frame_t frame = {.caps = caps, .error = error};

	std_landing(&frame, doltgres_entries_collect_body, doltgres_entries_collect_pad);
	doltgres_entries_collect_pad(&frame);
	return false;
}

/*
 * doltgres_entries_doltgres_collector_released gives back the session lock
 * the collector worker holds on a database it checks, and cancels a
 * condition variable's sleep it was in, as the worker exits by any path, a
 * FATAL from its SIGTERM or a recovery conflict included, since it runs
 * outside a transaction, whose abort would do both.
 */
bool
doltgres_entries_doltgres_collector_released(const PgCleanup *caps, int32 code, PgDatum arg, PgPgError *error)
{
	pg_calls_cv_cancel();
	pg_calls_session_locks_released();
	return true;
}

/*
 * doltgres_entries_fold_limits returns fold_threshold in bytes, a sixteenth
 * of delta_memory for its -1, and fold_timeout in microseconds, read anew at
 * each check so a reload's values apply.
 */
static void
doltgres_entries_fold_limits(uint64 *threshold, uint64 *timeout)
{
	int32		kilobytes = pg_calls_settings_fold_threshold();

	if (kilobytes == -1)
		kilobytes = pg_calls_settings_delta_memory() / 16;
	*threshold = STD_MUL(u64, doltgres_entries_unsigned(kilobytes), 1024, 507, 6);
	*timeout = STD_MUL(u64, doltgres_entries_unsigned(pg_calls_settings_fold_timeout()), 1000000, 507, 50);
}

/*
 * doltgres_TupleU32U64U64_t is a (u32, u64, u64): a running fold worker's
 * database, its handle, and the ask it serves.
 */
typedef struct doltgres_TupleU32U64U64_t
{
	uint32		f0;
	uint64		f1;
	uint64		f2;
} doltgres_TupleU32U64U64_t;

/* doltgres_TupleU32I64_t is a (u32, i64): a database and when the launcher last started a fold worker for it. */
typedef struct doltgres_TupleU32I64_t
{
	uint32		f0;
	int64		f1;
} doltgres_TupleU32I64_t;

#define DOLTGRES_ENTRIES_RUNNING sizeof(doltgres_TupleU32U64U64_t), _Alignof(doltgres_TupleU32U64U64_t)
#define DOLTGRES_ENTRIES_STARTED sizeof(doltgres_TupleU32I64_t), _Alignof(doltgres_TupleU32I64_t)
#define DOLTGRES_ENTRIES_DUE sizeof(TxnStateDue), _Alignof(TxnStateDue)

/*
 * doltgres_entries_fold_launch_frame_t is fold_launch's state across its
 * pad: the workers running, those started lately, the workers still running
 * at a check, and the storages due, each while live.
 */
typedef struct doltgres_entries_fold_launch_frame_t
{
	StdVec		running;
	StdVec		started;
	bool		lists_live;
	StdVec		still;
	bool		still_live;
	StdVec		due;
	bool		due_live;
	const PgCatalogCaps *caps;
	PgPgError  *error;
} doltgres_entries_fold_launch_frame_t;

/* doltgres_entries_fold_launch_pad frees what fold_launch holds. */
static void
doltgres_entries_fold_launch_pad(void *frame_)
{
	doltgres_entries_fold_launch_frame_t *frame = frame_;

	if (frame->due_live)
		std_vec_free_Checked(&frame->due, DOLTGRES_ENTRIES_DUE);
	if (frame->still_live)
		std_vec_free_Checked(&frame->still, DOLTGRES_ENTRIES_RUNNING);
	if (frame->lists_live)
	{
		std_vec_free_Checked(&frame->started, DOLTGRES_ENTRIES_STARTED);
		std_vec_free_Checked(&frame->running, DOLTGRES_ENTRIES_RUNNING);
	}
	frame->due_live = frame->still_live = frame->lists_live = false;
}

/* doltgres_entries_doltgres_fold_launch_closure_0 is fold_launch's retain: whether a start is within the naptime. */
static bool
doltgres_entries_doltgres_fold_launch_closure_0(void *captures, const void *started)
{
	int64		now = *(const int64 *) captures;

	return STD_SUB(i64, now, ((const doltgres_TupleU32I64_t *) started)->f1, 398, 35) <
		DOLTGRES_ENTRIES_LAUNCHER_NAPTIME * 1000;
}

/* doltgres_entries_fold_launch_body runs the launcher's checks until one fails or its SIGTERM ends it. */
static void
doltgres_entries_fold_launch_body(void *frame_)
{
	doltgres_entries_fold_launch_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	uint64		seen;

	if (!pg_calls_worker_connect(raise, catalog, frame->error))
		return;
	shmem_delta_launcher_started();
	seen = shmem_delta_fold_asked();
	std_vec_new(&frame->running, DOLTGRES_ENTRIES_RUNNING);
	std_vec_new(&frame->started, DOLTGRES_ENTRIES_STARTED);
	frame->lists_live = true;
	for (;;)
	{
		uint64		threshold;
		uint64		timeout;
		uint64		asked;
		int64		now;
		size_t		workers;
		bool		asking;
		bool		served = true;
		HostIoIo	io;
		HostError	failed;
		int32		folds = pg_calls_settings_fold_workers();

		std_vec_new(&frame->still, DOLTGRES_ENTRIES_RUNNING);
		frame->still_live = true;
		for (size_t i = 0; i < frame->running.len; i++)
		{
			doltgres_TupleU32U64U64_t worker = STD_VEC_AT(&frame->running, doltgres_TupleU32U64U64_t, i);
			bool		running;

			if (!pg_calls_fold_worker_running(raise, worker.f1, &running, frame->error))
				return;
			if (running)
				std_vec_push_Checked(&frame->still, &worker, DOLTGRES_ENTRIES_RUNNING);
			else
				pg_calls_free_worker_handle(worker.f1);
		}
		std_vec_free_Checked(&frame->running, DOLTGRES_ENTRIES_RUNNING);
		frame->running = frame->still;
		frame->still_live = false;
		now = pg_calls_current_timestamp();
		std_vec_retain_Checked(&frame->started, DOLTGRES_ENTRIES_STARTED, NULL, doltgres_entries_doltgres_fold_launch_closure_0,
							   &now);
		if (!pg_calls_injection_point(raise, "doltgres-fold-launch", frame->error))
			return;
		asked = shmem_delta_fold_asked();
		io = pghost_io(raise);
		asking = asked != seen;
		doltgres_entries_fold_limits(&threshold, &timeout);
		if (!txn_state_due(&io, threshold, timeout, asking, &frame->due, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->due_live = true;
		workers = (size_t) doltgres_entries_unsigned(folds);
		for (size_t i = 0; i < frame->due.len; i++)
		{
			uint32		database = STD_VEC_AT(&frame->due, TxnStateDue, i).storage.database;
			bool		serving = false;
			bool		busy = frame->running.len >= workers;
			uint64		handle;

			for (size_t k = 0; k < frame->running.len && !serving; k++)
			{
				const doltgres_TupleU32U64U64_t *worker = &STD_VEC_AT(&frame->running, doltgres_TupleU32U64U64_t, k);

				serving = worker->f0 == database && (!asking || worker->f2 == asked);
			}
			if (serving)
				continue;
			for (size_t k = 0; k < frame->running.len && !busy; k++)
				busy = STD_VEC_AT(&frame->running, doltgres_TupleU32U64U64_t, k).f0 == database;
			for (size_t k = 0; k < frame->started.len && !busy; k++)
				busy = STD_VEC_AT(&frame->started, doltgres_TupleU32I64_t, k).f0 == database;
			if (busy)
			{
				served = false;
				continue;
			}
			if (!pg_calls_start_fold_worker(raise, database, asking, &handle, frame->error))
				return;
			{
				doltgres_TupleU32I64_t start = {database, now};

				std_vec_push_Checked(&frame->started, &start, DOLTGRES_ENTRIES_STARTED);
			}
			if (handle == 0)
				served = false;
			else
			{
				doltgres_TupleU32U64U64_t worker = {database, handle, asked};

				std_vec_push_Checked(&frame->running, &worker, DOLTGRES_ENTRIES_RUNNING);
			}
		}
		frame->due_live = false;
		std_vec_free_Checked(&frame->due, DOLTGRES_ENTRIES_DUE);
		if (served)
			seen = asked;
		if (!pg_calls_latch_sleep(raise, Min((int64) pg_calls_settings_fold_timeout() * 1000,
											 DOLTGRES_ENTRIES_LAUNCHER_NAPTIME), frame->error) ||
			!pg_calls_config_reload_if_pending(raise, catalog, frame->error))
			return;
	}
}

/*
 * doltgres_entries_doltgres_fold_launch is the fold launcher's background
 * worker: connected to no database, at each check it starts, for each
 * database with a storage due a fold, largest first, a fold worker
 * connected to that database, while fewer than fold_workers run, none runs
 * for it, and none started for it within LAUNCHER_NAPTIME, so a worker that
 * fails is started again a naptime later. An ask stays owed until each
 * database due on it has a worker started since it, so an ask none could
 * serve at once is served at a later check. It checks when a writer at the
 * delta's budget or the checkpointer sets its latch, and at least every
 * LAUNCHER_NAPTIME, until its SIGTERM.
 */
bool
doltgres_entries_doltgres_fold_launch(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error)
{
	doltgres_entries_fold_launch_frame_t frame = {.caps = caps, .error = error};

	std_landing(&frame, doltgres_entries_fold_launch_body, doltgres_entries_fold_launch_pad);
	doltgres_entries_fold_launch_pad(&frame);
	return false;
}

/* doltgres_entries_fold_work_frame_t is fold_work's state across its pad: a pass's storages due, while live. */
typedef struct doltgres_entries_fold_work_frame_t
{
	StdVec		due;
	bool		due_live;
	const PgCatalogCaps *caps;
	PgDatum		main_arg;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_fold_work_frame_t;

/* doltgres_entries_fold_work_pad frees what fold_work holds. */
static void
doltgres_entries_fold_work_pad(void *frame_)
{
	doltgres_entries_fold_work_frame_t *frame = frame_;

	if (frame->due_live)
		std_vec_free_Checked(&frame->due, DOLTGRES_ENTRIES_DUE);
	frame->due_live = false;
}

/* doltgres_entries_fold_work_body folds the database's storages due, pass after pass, until a pass installs nothing. */
static void
doltgres_entries_fold_work_body(void *frame_)
{
	doltgres_entries_fold_work_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	uint64		word = pg_Datum_word(frame->main_arg);
	uint32		database = (uint32) word;
	bool		asked = word >> 32 != 0;

	if (!pg_calls_worker_connect_database(raise, catalog, database, frame->error))
		return;
	for (;;)
	{
		HostIoIo	io = pghost_io(raise);
		HostError	failed;
		uint64		threshold;
		uint64		timeout;
		bool		installed = false;

		doltgres_entries_fold_limits(&threshold, &timeout);
		if (!txn_state_due(&io, threshold, timeout, asked, &frame->due, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->due_live = true;
		for (size_t i = 0; i < frame->due.len; i++)
		{
			const TxnStateDue *due = &STD_VEC_AT(&frame->due, TxnStateDue, i);
			bool		folded;

			if (due->storage.database != database)
				continue;
			if (!pg_calls_fold_as_owner(raise, catalog, due->has_index ? due->index : due->table,
										due->storage.relation, &folded, frame->error))
				return;
			installed |= folded;
		}
		frame->due_live = false;
		std_vec_free_Checked(&frame->due, DOLTGRES_ENTRIES_DUE);
		if (!installed)
		{
			frame->ok = true;
			return;
		}
		if (!pg_calls_config_reload_if_pending(raise, catalog, frame->error))
			return;
	}
}

/*
 * doltgres_entries_doltgres_fold_work is a fold worker: connected to the
 * database its argument names, it folds, largest first, each of the
 * database's storages due a fold, every one holding a version when the
 * launcher started it on an ask, each in a transaction of its own as its
 * relation's owner with every setting at its built-in default, and ends
 * once a pass installs nothing.
 */
bool
doltgres_entries_doltgres_fold_work(const PgCatalogCaps *caps, PgDatum main_arg, PgPgError *error)
{
	doltgres_entries_fold_work_frame_t frame = {.caps = caps, .main_arg = main_arg, .error = error};

	std_landing(&frame, doltgres_entries_fold_work_body, doltgres_entries_fold_work_pad);
	doltgres_entries_fold_work_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_fold_relation folds a relation's storage now,
 * inside a fold worker's transaction as its owner, and answers whether it
 * installed a base; heap is the relation, or an index's table, whose horizon
 * bounds the fold.
 */
bool
doltgres_entries_doltgres_fold_relation(const PgCatalogCaps *caps, const PgRel *rel, const PgRel *heap, bool *result,
										PgPgError *error)
{
	return doltgres_entries_folded_now(caps, rel, heap, result, error);
}

/*
 * doltgres_entries_doltgres_fork_worker says whether a background worker is
 * one of the fork's own, whose entry is one of bgworker.c's internal
 * doltgres_ entries.
 */
bool
doltgres_entries_doltgres_fork_worker(const PgCleanup *caps, const PgBackgroundWorker *worker, bool *result,
									  PgPgError *error)
{
	*result = pg_calls_fork_worker_named(worker);
	return true;
}

/*
 * doltgres_entries_doltgres_worker_refused says whether the dynamic slot
 * search refuses a background worker: never one of the fork's own, and any
 * other once max_worker_processes slots hold workers that aren't the fork's,
 * so the slots DoltgresReservedWorkers adds stay the fork's and other
 * workers count as in Postgres.
 */
bool
doltgres_entries_doltgres_worker_refused(const PgCleanup *caps, const PgBackgroundWorker *worker, bool *result,
										 PgPgError *error)
{
	*result = !pg_calls_fork_worker_named(worker) && pg_calls_worker_slots_in_use() >= pg_calls_max_worker_processes();
	return true;
}

/*
 * DOLTGRES_ENTRIES_RELATION_ROW is the length of a dropped relation
 * aborted_relations writes: its tablespace, database, and relation number.
 */
#define DOLTGRES_ENTRIES_RELATION_ROW 12

/*
 * doltgres_entries_doltgres_recovery_refused ends the startup process with a
 * FATAL naming a storage and both roots once a standby's fold has published
 * a root other than its primary's, and does nothing before one.
 */
bool
doltgres_entries_doltgres_recovery_refused(const PgCaps *caps, PgPgError *error)
{
	StdCString	detail;
	bool		refused;

	if (!shmem_delta_refusal(&detail))
		return true;
	refused = pg_calls_refuse_recovery(pg_Caps_may_raise(caps), std_CString_as_ptr(&detail), error);
	std_CString_drop(&detail);
	return refused;
}

/*
 * doltgres_entries_restartpoint_held_frame_t is restartpoint_held's state
 * across its pad: the warning's message and detail, as strings and then as C
 * strings, each while live.
 */
typedef struct doltgres_entries_restartpoint_held_frame_t
{
	StdString	message;
	bool		message_live;
	StdString	detail;
	bool		detail_live;
	StdCString	message_c;
	bool		message_c_live;
	StdCString	detail_c;
	bool		detail_c_live;
	const PgCaps *caps;
	uint64		redo;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_restartpoint_held_frame_t;

/* doltgres_entries_restartpoint_held_pad frees what restartpoint_held holds. */
static void
doltgres_entries_restartpoint_held_pad(void *frame_)
{
	doltgres_entries_restartpoint_held_frame_t *frame = frame_;

	if (frame->detail_c_live)
		std_CString_drop(&frame->detail_c);
	if (frame->message_c_live)
		std_CString_drop(&frame->message_c);
	if (frame->detail_live)
		std_String_drop(&frame->detail);
	if (frame->message_live)
		std_String_drop(&frame->message);
	frame->detail_c_live = frame->message_c_live = frame->detail_live = frame->message_live = false;
}

/* doltgres_entries_restartpoint_held_body finds the hold and warns of one held longer than checkpoint_timeout. */
static void
doltgres_entries_restartpoint_held_body(void *frame_)
{
	doltgres_entries_restartpoint_held_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostIoIo	io = pghost_io(raise);
	txn_TupleU64StorageU64_t back;
	HostError	failed;
	bool		some;
	uint64		timeout;

	if (!txn_state_held_back(&io, &some, &back, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	*frame->result = some && frame->redo >= back.f0;
	if (!*frame->result)
	{
		frame->ok = true;
		return;
	}
	timeout = STD_MUL(u64, doltgres_entries_unsigned(pg_calls_checkpoint_timeout()), 1000000, 547, 15);
	if (back.f2 > timeout)
	{
		const char *why = shmem_delta_launcher_running() ? "waits for its fold" :
			"waits for a fold worker, none having started";
		size_t		why_len = strlen(why);
		uint64		seconds = back.f2 / 1000000;
		uint64		kept = STD_SUB(u64, frame->redo, back.f0, 557, 13);
		StdStr		why_str = {why, why_len};
		StdFmtPiece message_pieces[] = {STD_FMT_LIT("restartpoint held back by storage "), STD_FMT_ARG(0),
		STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"), STD_FMT_ARG(2)};
		StdFmtArg	message_args[] = {{&back.f1.tablespace, std_u32_Display_fmt},
		{&back.f1.database, std_u32_Display_fmt}, {&back.f1.relation, std_u32_Display_fmt}};
		StdFmtArguments message = {message_pieces, 6, message_args};
		StdFmtPiece detail_pieces[] = {STD_FMT_LIT("The storage's replayed fold point "), STD_FMT_ARG(0),
			STD_FMT_LIT(", for "), STD_FMT_ARG(1), STD_FMT_LIT(" s, keeping "), STD_FMT_ARG(2),
		STD_FMT_LIT(" bytes of WAL past it.")};
		StdFmtArg	detail_args[] = {{&why_str, std_str_Display_fmt}, {&seconds, std_u64_Display_fmt},
		{&kept, std_u64_Display_fmt}};
		StdFmtArguments detail = {detail_pieces, 7, detail_args};

		std_fmt_format_Checked(&frame->message, &message);
		frame->message_live = true;
		std_fmt_format_Checked(&frame->detail, &detail);
		frame->detail_live = true;
		frame->message_live = false;
		if (!doltgres_entries_c_string(&frame->message, &frame->message_c, frame->error))
			return;
		frame->message_c_live = true;
		frame->detail_live = false;
		if (!doltgres_entries_c_string(&frame->detail, &frame->detail_c, frame->error))
			return;
		frame->detail_c_live = true;
		if (!pg_calls_warn(raise, (const uint8 *) "01000", 5, std_CString_as_ptr(&frame->message_c),
						   std_CString_as_ptr(&frame->detail_c),
						   "Until the storage is folded, the standby keeps its WAL.", frame->error))
			return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_restartpoint_held says whether a restartpoint
 * whose redo point is the one given is held back, its redo point past the
 * record of a fold point a storage's slot holds unfolded or whose read past
 * it isn't done, so replay after a restart delivers every such record again,
 * in order. Once a hold has lasted longer than checkpoint_timeout, each
 * restartpoint it skips reports it at WARNING, naming the storage, why it is
 * unfolded, how long it has been held, and the WAL it keeps.
 */
bool
doltgres_entries_doltgres_restartpoint_held(const PgCaps *caps, uint64 redo, bool *result, PgPgError *error)
{
	doltgres_entries_restartpoint_held_frame_t frame = {.caps = caps, .redo = redo, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_restartpoint_held_body, doltgres_entries_restartpoint_held_pad);
	doltgres_entries_restartpoint_held_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_commit_registered stores a commit's top-level
 * xid and the end of its commit record, the backend's last inserted, in this
 * process's own slot, inside RecordTransactionCommit's critical section, for
 * a transaction whose room this process took.
 */
bool
doltgres_entries_doltgres_commit_registered(const PgCleanup *caps, PgPgError *error)
{
	HostXid		top = {pg_calls_top_xid_if_any()};

	txn_status_registered(&DOLTGRES_KINDS_COMMITS, top, pg_calls_last_record_end());
	return true;
}

/*
 * doltgres_entries_doltgres_prepared_registered stores the commit of the
 * prepared transaction prepared_committing readied, with the end of its
 * commit record, in this process's own slot, inside
 * RecordTransactionCommitPrepared's critical section.
 */
bool
doltgres_entries_doltgres_prepared_registered(const PgCleanup *caps, PgPgError *error)
{
	txn_status_prepared_registered(&DOLTGRES_KINDS_COMMITS, pg_calls_last_record_end());
	return true;
}

/*
 * doltgres_entries_doltgres_redo_committed stores a replayed commit record's
 * end in its transaction's entry, in xact_redo_commit ahead of the commit
 * log's marking, so a reader that sees the xid committed finds its end.
 */
bool
doltgres_entries_doltgres_redo_committed(const PgCaps *caps, uint32 xid, uint64 end, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io;
	HostXid		wide;
	HostError	failed;

	if (!pg_calls_widened_xid(raise, xid, &wide.f0, error))
		return false;
	io = pghost_io(raise);
	return doltgres_entries_raised(txn_status_redo_committed(&io, wide, end, &failed), &failed, error);
}

/* doltgres_entries_doltgres_redo_done_closure_0 is redo_done's picked: every built node. */
static bool
doltgres_entries_doltgres_redo_done_closure_0(void *captures, HostIoIo *io, uint32 repository, uint32 tablespace,
											  uint32 relation, uint64 point, uint64 writer, bool *result,
											  HostError *error)
{
	*result = true;
	return true;
}

/*
 * doltgres_entries_doltgres_redo_done drops from every working state's delta
 * each built node whose base record never came, in StartupXLOG once redo has
 * ended and before the end-of-recovery checkpoint, as a crash between a
 * built base's Chunk records and its base record leaves them.
 */
bool
doltgres_entries_doltgres_redo_done(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(txn_memtable_branch_every_nodes_taken(&io,
																		 doltgres_entries_doltgres_redo_done_closure_0,
																		 NULL, NULL, &failed), &failed, error);
}

/* DoltgresEntriesWrite is filled's write: the rows a wrapper writes into a buffer, answering how many it has. */
typedef size_t (*DoltgresEntriesWrite) (const PgWalRecord *record, uint8 *buffer, size_t buffer_len);

/*
 * doltgres_entries_filled returns the bytes a wrapper writes of a record into
 * a buffer, growing the buffer until they fit, the wrapper answering how
 * many rows of a length it has.
 */
static void
doltgres_entries_filled(size_t row, DoltgresEntriesWrite write, const PgWalRecord *record, StdVec *result)
{
	static const uint8 zero = 0;

	std_vec_new(result, DOLTGRES_ENTRIES_BYTES);
	for (;;)
	{
		size_t		count = write(record, result->ptr, result->len);

		if (STD_MUL(usize, count, row, 643, 12) <= result->len)
		{
			std_vec_truncate(result, STD_MUL(usize, count, row, 644, 29), 1, NULL);
			return;
		}
		std_vec_resize(result, STD_MUL(usize, count, row, 647, 23), &zero, DOLTGRES_ENTRIES_BYTES);
	}
}

/*
 * doltgres_entries_redo_aborted_captures_t is redo_aborted's picked's
 * captures: the dropped storages, three words each.
 */
typedef struct doltgres_entries_redo_aborted_captures_t
{
	const StdVec *rows;
} doltgres_entries_redo_aborted_captures_t;

/*
 * doltgres_entries_doltgres_redo_aborted_closure_3 is redo_aborted's picked:
 * whether a built node's storage was dropped.
 */
static bool
doltgres_entries_doltgres_redo_aborted_closure_3(void *captures, HostIoIo *io, uint32 repository, uint32 tablespace,
												 uint32 relation, uint64 point, uint64 writer, bool *result,
												 HostError *error)
{
	const StdVec *rows = ((doltgres_entries_redo_aborted_captures_t *) captures)->rows;

	*result = false;
	for (size_t at = 0; at + DOLTGRES_ENTRIES_RELATION_ROW <= rows->len && !*result;
		 at += DOLTGRES_ENTRIES_RELATION_ROW)
	{
		const uint8 *row = (const uint8 *) rows->ptr + at;

		*result = doltgres_entries_word(row, 0) == tablespace && doltgres_entries_word(row, 4) == repository &&
			doltgres_entries_word(row, 8) == relation;
	}
	return true;
}

/*
 * doltgres_entries_redo_aborted_frame_t is redo_aborted's state across its
 * pad: the dropped storages' rows, while live.
 */
typedef struct doltgres_entries_redo_aborted_frame_t
{
	StdVec		rows;
	bool		rows_live;
	const PgCaps *caps;
	const PgWalRecord *record;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_redo_aborted_frame_t;

/* doltgres_entries_redo_aborted_pad frees what redo_aborted holds. */
static void
doltgres_entries_redo_aborted_pad(void *frame_)
{
	doltgres_entries_redo_aborted_frame_t *frame = frame_;

	if (frame->rows_live)
		std_vec_free_Checked(&frame->rows, DOLTGRES_ENTRIES_BYTES);
	frame->rows_live = false;
}

/* doltgres_entries_redo_aborted_body reads the record's dropped storages and takes their built nodes out. */
static void
doltgres_entries_redo_aborted_body(void *frame_)
{
	doltgres_entries_redo_aborted_frame_t *frame = frame_;
	doltgres_entries_redo_aborted_captures_t captures = {&frame->rows};
	HostIoIo	io;
	HostError	failed;

	doltgres_entries_filled(DOLTGRES_ENTRIES_RELATION_ROW, pg_calls_aborted_relations, frame->record, &frame->rows);
	frame->rows_live = true;
	if (frame->rows.len < DOLTGRES_ENTRIES_RELATION_ROW)
	{
		frame->ok = true;
		return;
	}
	io = pghost_io(pg_Caps_may_raise(frame->caps));
	frame->ok = doltgres_entries_raised(txn_memtable_branch_every_nodes_taken(&io,
																			  doltgres_entries_doltgres_redo_aborted_closure_3,
																			  &captures, NULL, &failed), &failed,
										frame->error);
}

/*
 * doltgres_entries_doltgres_redo_aborted takes out of the working states'
 * deltas the built nodes of every storage a replayed abort drops, at the
 * start of xact_redo, before any branch's DropRelationFiles, so an aborted
 * rewrite's Chunk records leave nothing on a standby that never promotes; a
 * record of another kind drops none.
 */
bool
doltgres_entries_doltgres_redo_aborted(const PgCaps *caps, const PgWalRecord *record, PgPgError *error)
{
	doltgres_entries_redo_aborted_frame_t frame = {.caps = caps, .record = record, .error = error};

	std_landing(&frame, doltgres_entries_redo_aborted_body, doltgres_entries_redo_aborted_pad);
	doltgres_entries_redo_aborted_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_running_replayed_captures_t is running_replayed's picked's
 * captures: the record's next xid and running xids.
 */
typedef struct doltgres_entries_running_replayed_captures_t
{
	uint32		next;
	const uint8 *running;
	size_t		running_len;
} doltgres_entries_running_replayed_captures_t;

/*
 * doltgres_entries_doltgres_running_replayed_closure_2 is running_replayed's
 * picked: whether a writer is below the record's next xid, not among its
 * running ones, and not committed.
 */
static bool
doltgres_entries_doltgres_running_replayed_closure_2(void *captures, HostIoIo *io, uint32 repository,
													 uint32 tablespace, uint32 relation, uint64 point,
													 uint64 writer, bool *result, HostError *error)
{
	doltgres_entries_running_replayed_captures_t *c = captures;
	uint32		short_xid = (uint32) writer;
	bool		running = false;
	HostXid		xid = {writer};
	HostStatus	status;

	*result = false;
	if ((int32) (short_xid - c->next) >= 0)
		return true;
	for (size_t i = 0; i < c->running_len && !running; i++)
		running = doltgres_entries_word(c->running + i * 4, 0) == short_xid;
	if (running)
		return true;
	if (!host_status(HOST_IO_IO_IO_DOORS(io), xid, &status, error))
		return false;
	*result = status != HOST_STATUS_COMMITTED;
	return true;
}

/*
 * doltgres_entries_running_replayed_frame_t is running_replayed's state across
 * its pad: the record's words, while live.
 */
typedef struct doltgres_entries_running_replayed_frame_t
{
	StdVec		words;
	bool		words_live;
	const PgCaps *caps;
	const PgWalRecord *record;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_running_replayed_frame_t;

/* doltgres_entries_running_replayed_pad frees what running_replayed holds. */
static void
doltgres_entries_running_replayed_pad(void *frame_)
{
	doltgres_entries_running_replayed_frame_t *frame = frame_;

	if (frame->words_live)
		std_vec_free_Checked(&frame->words, DOLTGRES_ENTRIES_BYTES);
	frame->words_live = false;
}

/*
 * doltgres_entries_running_replayed_body reads the record's next and running
 * xids and takes the ended writers' nodes out.
 */
static void
doltgres_entries_running_replayed_body(void *frame_)
{
	doltgres_entries_running_replayed_frame_t *frame = frame_;
	doltgres_entries_running_replayed_captures_t captures;
	HostIoIo	io;
	HostError	failed;
	size_t		count;

	doltgres_entries_filled(4, pg_calls_running_xids, frame->record, &frame->words);
	frame->words_live = true;
	count = frame->words.len / 4;
	if (count == 0)
	{
		frame->ok = true;
		return;
	}
	captures.next = doltgres_entries_word(frame->words.ptr, 0);
	captures.running = (const uint8 *) frame->words.ptr + 4;
	captures.running_len = count - 1;
	io = pghost_io(pg_Caps_may_raise(frame->caps));
	frame->ok = doltgres_entries_raised(txn_memtable_branch_every_nodes_taken(&io,
																			  doltgres_entries_doltgres_running_replayed_closure_2,
																			  &captures, NULL, &failed), &failed,
										frame->error);
}

/*
 * doltgres_entries_doltgres_running_replayed drops from the working states'
 * deltas the built nodes of every writer a replayed running transactions
 * record shows ended uncommitted, at the start of standby_redo: one below
 * the record's next xid, not among its running ones, and not committed,
 * which a crash of the primary in a rewrite leaves, since it writes no abort
 * record; a committed writer's nodes are taken by its base record, which
 * comes before its commit, and a record of another kind drops none. The
 * record's xids are compared as the record holds them, 32 bits wide and in
 * Postgres' circular order, since this server's next xid can be behind the
 * record it replays.
 */
bool
doltgres_entries_doltgres_running_replayed(const PgCaps *caps, const PgWalRecord *record, PgPgError *error)
{
	doltgres_entries_running_replayed_frame_t frame = {.caps = caps, .record = record, .error = error};

	std_landing(&frame, doltgres_entries_running_replayed_body, doltgres_entries_running_replayed_pad);
	doltgres_entries_running_replayed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_prepared_committing readies a prepared
 * transaction's commit registration in RecordTransactionCommitPrepared,
 * ahead of its critical section: this process's last pair moved, and the
 * transaction's entry pointed at it.
 */
bool
doltgres_entries_doltgres_prepared_committing(const PgCaps *caps, uint32 xid, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io;
	HostXid		wide;
	HostError	failed;

	if (!pg_calls_widened_xid(raise, xid, &wide.f0, error))
		return false;
	io = pghost_io(raise);
	return doltgres_entries_raised(txn_status_prepared_committing(&io, wide, &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_shared_release releases what this process holds
 * in shared memory as it exits: its flags and its reader's epoch, then the
 * records it owns, holds open, or looks at, then its words, then the records
 * of the delta it made and didn't link and the pages of the delta it owns,
 * and last its attachment to the delta's area.
 */
bool
doltgres_entries_doltgres_shared_release(const PgCleanup *caps, int32 code, PgDatum arg, PgPgError *error)
{
	shmem_keys_exit();
	shmem_records_release();
	pghost_shared_exit();
	pghost_delta_exit();
	return true;
}

/*
 * doltgres_entries_doltgres_shared_exit runs this process's exit works as it
 * exits, one a call, and then gives up the node cache pins it holds, before
 * doltgres_shared_release releases what may not raise.
 */
bool
doltgres_entries_doltgres_shared_exit(const PgCaps *caps, int32 code, PgDatum arg, PgPgError *error)
{
	HostError	failed;

	return doltgres_entries_raised(pghost_shared_exit_step(pg_Caps_may_raise(caps), &failed), &failed, error);
}

/*
 * doltgres_entries_doltgres_cache_release unpins a node cache pin its
 * resource owner releases, inside an abort or at the owner's end, unless the
 * pin's drop already did.
 */
bool
doltgres_entries_doltgres_cache_release(const PgCleanup *caps, uint64 res, PgPgError *error)
{
	host_cache_released(res);
	return true;
}

/* DOLTGRES_ENTRIES_OWNED is the kinds of owned record owned_release releases, in its match's order. */
static const HostOwnedOwnedOps *const DOLTGRES_ENTRIES_OWNED[] = {
	&doltgres_scan_Scan_OWNED_OPS,
	&txn_state_Holds_OWNED_OPS,
	&txn_written_Written_OWNED_OPS,
	&doltgres_index_Inserting_OWNED_OPS,
	&doltgres_index_OldEntries_OWNED_OPS,
#ifdef USE_INJECTION_POINTS
	&doltgres_entries_testing_TestRecord_OWNED_OPS,
#endif
};

/*
 * doltgres_entries_doltgres_owned_release drops an owned record its resource
 * owner releases, inside an abort or at the owner's end, unless the record's
 * end already did.
 */
bool
doltgres_entries_doltgres_owned_release(const PgCleanup *caps, uint64 res, PgPgError *error)
{
	uint8		kind;
	uint32		slot;

	host_owned_remembered(res, &kind, &slot);
	for (size_t i = 0; i < lengthof(DOLTGRES_ENTRIES_OWNED); i++)
		if (DOLTGRES_ENTRIES_OWNED[i]->KIND == kind)
		{
			DOLTGRES_ENTRIES_OWNED[i]->release(slot);
			break;
		}
	return true;
}

/* doltgres_TupleU32ArcRepo_t is a (u32, Arc<Repo>): a database and the repository its redo opened. */
typedef struct doltgres_TupleU32ArcRepo_t
{
	uint32		f0;
	StdArcInner *f1;
} doltgres_TupleU32ArcRepo_t;

/* DOLTGRES_ENTRIES_REDONE_ENTRY is the element layout of Redone, a Vec<(u32, Arc<Repo>)>. */
#define DOLTGRES_ENTRIES_REDONE_ENTRY sizeof(doltgres_TupleU32ArcRepo_t), _Alignof(doltgres_TupleU32ArcRepo_t)

/* doltgres_entries_TupleU32ArcRepo_drop is a (u32, Arc<Repo>)'s drop glue: its repository's hold. */
static void
doltgres_entries_TupleU32ArcRepo_drop(void *redone)
{
	std_arc_drop(((doltgres_TupleU32ArcRepo_t *) redone)->f1, sizeof(RepoRepo), _Alignof(RepoRepo),
				 doltgres_entries_Repo_drop);
}

/* doltgres_entries_Redone_Default_default is Redone's Default: no repository opened. */
static void
doltgres_entries_Redone_Default_default(StdVec *self)
{
	std_vec_new(self, DOLTGRES_ENTRIES_REDONE_ENTRY);
}

/* doltgres_entries_Redone_drop is Redone's drop glue: each repository's hold. */
static void
doltgres_entries_Redone_drop(void *redone)
{
	std_vec_drop_Checked(redone, DOLTGRES_ENTRIES_REDONE_ENTRY, doltgres_entries_TupleU32ArcRepo_drop);
}

/* taken at Redone and IoMutex<Redone>, which entries.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Redone
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_entries_Redone_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_Redone_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_REDONE_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Redone
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) doltgres_entries_Redone_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_entries_IoMutexRedone_drop is IoMutex<Redone>'s drop glue: its value's. */
static void
doltgres_entries_IoMutexRedone_drop(void *value)
{
	doltgres_entries_Redone_drop(&((HostLockIoMutexRedone *) value)->value);
}

/* DOLTGRES_ENTRIES_REDONE_TYPE is IoMutex<Redone>'s size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_REDONE_TYPE = {sizeof(HostLockIoMutexRedone),
_Alignof(HostLockIoMutexRedone), host_lock_IoMutexRedone_Default_default, doltgres_entries_IoMutexRedone_drop};

/*
 * DOLTGRES_ENTRIES_REDONE is REDONE: the repositories a process's redo
 * opened, each held from the first record of its database the process
 * redoes until the process exits.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexRedone process;
}			DOLTGRES_ENTRIES_REDONE = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DoltgresEntriesOp is an operation on a backend record under its lock, with its captures. */
typedef void (*DoltgresEntriesOp) (void *value, void *captures);

/*
 * doltgres_entries_locked_frame_t is a locked operation's state across its
 * pad: the backend's record and its guard, each while live.
 */
typedef struct doltgres_entries_locked_frame_t
{
	HostIoIo   *io;
	DoltgresEntriesOp op;
	void	   *captures;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} doltgres_entries_locked_frame_t;

/* doltgres_entries_locked_body takes the backend's record and its lock, then runs the operation. */
static void
doltgres_entries_locked_body(void *frame_)
{
	doltgres_entries_locked_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_REDONE.local, &DOLTGRES_ENTRIES_REDONE_TYPE);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, &DOLTGRES_ENTRIES_REDONE_TYPE);
	frame->guard = host_lock_IoMutex_lock(mutex, &DOLTGRES_ENTRIES_REDONE_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	frame->op(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), frame->captures);
}

/* doltgres_entries_locked_pad releases what a locked operation holds: its guard, then the backend's record. */
static void
doltgres_entries_locked_pad(void *frame_)
{
	doltgres_entries_locked_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, &DOLTGRES_ENTRIES_REDONE_TYPE);
	frame->guard_live = frame->local_live = false;
}

/* doltgres_entries_redone_locked runs an operation on REDONE under its lock, as REDONE.get().lock(io) holds it. */
static void
doltgres_entries_redone_locked(HostIoIo *io, DoltgresEntriesOp op, void *captures)
{
	doltgres_entries_locked_frame_t frame = {.io = io, .op = op, .captures = captures};

	std_landing(&frame, doltgres_entries_locked_body, doltgres_entries_locked_pad);
	doltgres_entries_locked_pad(&frame);
}

/* doltgres_entries_redone_released_closure is redone_released's work on REDONE: every repository forgotten. */
static void
doltgres_entries_redone_released_clear(void *redone, void *captures)
{
	std_vec_truncate(redone, 0, sizeof(doltgres_TupleU32ArcRepo_t), doltgres_entries_TupleU32ArcRepo_drop);
}

/*
 * doltgres_entries_redone_released forgets every repository the process's
 * redo opened, closing each no one else holds, before a drop's or a move's
 * redo, so the dropped or moved repository's files close and a database made
 * again with its id is redone into its new repository; the others reopen at
 * their next record.
 */
static void
doltgres_entries_redone_released(HostIoIo *io)
{
	doltgres_entries_redone_locked(io, doltgres_entries_redone_released_clear, NULL);
}

/* doltgres_entries_redo_repository_found_t is redo_repository's search's captures: the database and its hold. */
typedef struct doltgres_entries_redo_repository_found_t
{
	uint32		database;
	StdArcInner *repo;
} doltgres_entries_redo_repository_found_t;

/* doltgres_entries_redo_repository_closure_0 is redo_repository's search of REDONE: a clone of its database's hold. */
static void
doltgres_entries_redo_repository_closure_0(void *redone, void *captures)
{
	const StdVec *table = redone;
	doltgres_entries_redo_repository_found_t *found = captures;

	for (size_t i = 0; i < table->len; i++)
		if (STD_VEC_AT(table, doltgres_TupleU32ArcRepo_t, i).f0 == found->database)
		{
			found->repo = std_arc_clone(STD_VEC_AT(table, doltgres_TupleU32ArcRepo_t, i).f1);
			return;
		}
}

/* doltgres_entries_redo_repository_push is redo_repository's push onto REDONE: its database and a clone of its hold. */
static void
doltgres_entries_redo_repository_push(void *redone, void *captures)
{
	const doltgres_entries_redo_repository_found_t *found = captures;
	doltgres_TupleU32ArcRepo_t entry = {found->database, std_arc_clone(found->repo)};

	std_vec_push_Checked(redone, &entry, DOLTGRES_ENTRIES_REDONE_ENTRY);
}

/*
 * doltgres_entries_redo_repository_frame_t is redo_repository's state across
 * its pad: the repositories' list, and the repository opened until REDONE
 * holds it, each while live.
 */
typedef struct doltgres_entries_redo_repository_frame_t
{
	HostIoIo   *io;
	uint32		database;
	StdArcInner **result;
	PgPgError  *error;
	bool		ok;
	StdVec		repositories;
	bool		repositories_live;
	StdArcInner *repo;
	bool		repo_live;
} doltgres_entries_redo_repository_frame_t;

/* doltgres_entries_redo_repository_pad frees the repository opened and the repositories' list. */
static void
doltgres_entries_redo_repository_pad(void *frame_)
{
	doltgres_entries_redo_repository_frame_t *frame = frame_;

	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	if (frame->repositories_live)
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	frame->repo_live = frame->repositories_live = false;
}

/* doltgres_entries_redo_repository_failed is redo_repository's failed: a chunks error raised as Postgres'. */
static PgPgError
doltgres_entries_redo_repository_failed(ChunksErrorError *error)
{
	HostError	raised = chunks_packs_shared_raised(error);

	return pghost_raised(&raised);
}

/* doltgres_entries_redo_repository_body finds the database's repository, or opens it from the repositories' list. */
static void
doltgres_entries_redo_repository_body(void *frame_)
{
	doltgres_entries_redo_repository_frame_t *frame = frame_;
	doltgres_entries_redo_repository_found_t found = {frame->database};
	const std_TupleU32U32_t *listed = NULL;
	ChunksErrorError failed;
	RepoRepo	repo;

	doltgres_entries_redone_locked(frame->io, doltgres_entries_redo_repository_closure_0, &found);
	if (found.repo != NULL)
	{
		*frame->result = found.repo;
		frame->ok = true;
		return;
	}
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
	{
		*frame->error = doltgres_entries_redo_repository_failed(&failed);
		return;
	}
	frame->repositories_live = true;
	for (size_t i = 0; i < frame->repositories.len && listed == NULL; i++)
		if (STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, i).f0 == frame->database)
			listed = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, i);
	if (listed == NULL)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0),
		STD_FMT_LIT("'s repository, which a WAL record names, is missing")};
		StdFmtArg	args[] = {{&frame->database, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString	missing;

		std_fmt_format_Checked(&missing, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX001", &missing);
		return;
	}
	if (!repo_Repo_open(frame->io, listed->f0, listed->f1, &repo, &failed))
	{
		*frame->error = doltgres_entries_redo_repository_failed(&failed);
		return;
	}
	frame->repo = std_arc_new_dropping(&repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->repo_live = true;
	found.repo = frame->repo;
	doltgres_entries_redone_locked(frame->io, doltgres_entries_redo_repository_push, &found);
	*frame->result = frame->repo;
	frame->repo_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_redo_repository returns the repository of a database a
 * record names, opened in the directory the repositories' list gives it at
 * the process's first record of that database and held after, and refuses a
 * database with no repository as data corruption. It clones the table's
 * Arc, since the table and the caller share the repository.
 */
static bool
doltgres_entries_redo_repository(HostIoIo *io, uint32 database, StdArcInner **result, PgPgError *error)
{
	doltgres_entries_redo_repository_frame_t frame = {.io = io, .database = database, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_redo_repository_body, doltgres_entries_redo_repository_pad);
	doltgres_entries_redo_repository_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_startup_folds_frame_t is startup_folds' state across its
 * pad: the storages due, and one's repository, reader, and state, each while
 * live.
 */
typedef struct doltgres_entries_startup_folds_frame_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	StdVec		due;
	bool		due_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	HostSharedHeld held;
	bool		held_live;
} doltgres_entries_startup_folds_frame_t;

/* doltgres_entries_startup_folds_pad frees the storages due and one's state, reader, and repository. */
static void
doltgres_entries_startup_folds_pad(void *frame_)
{
	doltgres_entries_startup_folds_frame_t *frame = frame_;

	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	if (frame->due_live)
		std_vec_free_Checked(&frame->due, sizeof(TxnStateDue), _Alignof(TxnStateDue));
	frame->held_live = frame->reading_live = frame->repo_live = frame->due_live = false;
}

/* doltgres_entries_startup_folds_body folds each storage due, largest first. */
static void
doltgres_entries_startup_folds_body(void *frame_)
{
	doltgres_entries_startup_folds_frame_t *frame = frame_;
	TxnInsertPage page = doltgres_entries_page();
	uint64		threshold;
	uint64		timeout;
	bool		installed = false;

	doltgres_entries_fold_limits(&threshold, &timeout);
	if (!txn_state_due(frame->io, threshold, timeout, true, &frame->due, frame->error))
		return;
	frame->due_live = true;
	for (size_t i = 0; i < frame->due.len; i++)
	{
		const TxnStateDue *due = &STD_VEC_AT(&frame->due, TxnStateDue, i);
		PgPgError	failed;
		bool		folded;

		if (!doltgres_entries_redo_repository(frame->io, due->storage.database, &frame->repo, &failed))
		{
			*frame->error = pghost_held(&failed);
			return;
		}
		frame->repo_live = true;
		if (!host_shared_read(frame->io, &frame->reading, frame->error))
			return;
		frame->reading_live = true;
		if (!txn_state_state(frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
							 &due->storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &frame->held, frame->error))
			return;
		frame->held_live = true;
		if (!txn_fold_startup_fold(frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
								   &frame->held, &page, 0, &folded, frame->error))
			return;
		installed |= folded;
		host_shared_Held_drop(&frame->held);
		frame->held_live = false;
		host_shared_Reader_drop(&frame->reading);
		frame->reading_live = false;
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
		frame->repo_live = false;
	}
	*frame->result = installed;
	frame->ok = true;
}

/*
 * doltgres_entries_startup_folds folds in the startup process, largest
 * first, each storage a replayed fold's record reached past its newest base
 * whose key orders its rows by their bytes (bugs row 107), at its wait at the
 * delta's budget while no fold worker can, answering whether any installed a
 * base.
 */
static bool
doltgres_entries_startup_folds(HostIoIo *io, bool *result, HostError *error)
{
	doltgres_entries_startup_folds_frame_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_startup_folds_body, doltgres_entries_startup_folds_pad);
	doltgres_entries_startup_folds_pad(&frame);
	return frame.ok;
}

/*
 * DoltgresEntriesRedo is a redo of one of a storage's records, replayed's
 * redo: its captures, the capability, the repository's store, the storage's
 * state, and its TOAST storage's, NULL for none.
 */
typedef bool (*DoltgresEntriesRedo) (void *captures, HostIoIo *io, TreesStoreNodeStore store,
									 const HostSharedHeld *held, const HostSharedHeld *toast, HostError *error);

/*
 * doltgres_entries_replayed_frame_t is replayed's state across its pad: the
 * repository, its reader, the storage's state, and its TOAST storage's, each
 * while live.
 */
typedef struct doltgres_entries_replayed_frame_t
{
	const PgCaps *caps;
	const HostStorage *storage;
	TxnStateMade *made;
	const HostStorage *toast;
	DoltgresEntriesRedo redo;
	void	   *captures;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	HostSharedHeld held;
	bool		held_live;
	HostSharedHeld toasted;
	bool		toasted_live;
} doltgres_entries_replayed_frame_t;

/* doltgres_entries_replayed_pad frees the states, the reader, and the repository. */
static void
doltgres_entries_replayed_pad(void *frame_)
{
	doltgres_entries_replayed_frame_t *frame = frame_;

	if (frame->toasted_live)
		host_shared_Held_drop(&frame->toasted);
	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->toasted_live = frame->held_live = frame->reading_live = frame->repo_live = false;
}

/* doltgres_entries_replayed_body opens the storage's states, marks them uncounted, and runs the redo. */
static void
doltgres_entries_replayed_body(void *frame_)
{
	doltgres_entries_replayed_frame_t *frame = frame_;
	const RepoRepo *repo;
	TreesStoreReader store;
	HostError	failed;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (!doltgres_entries_redo_repository(&frame->io, frame->storage->database, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	repo = std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo));
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!txn_state_state(&frame->io, repo, &frame->reading, frame->storage, TXN_STATE_PERSISTENCE_PERMANENT,
						 frame->made != NULL, frame->made, &frame->held, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->held_live = true;
	if (frame->toast != NULL)
	{
		if (!txn_state_state(&frame->io, repo, &frame->reading, frame->toast, TXN_STATE_PERSISTENCE_PERMANENT, false,
							 NULL, &frame->toasted, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		frame->toasted_live = true;
	}
	txn_state_totals_unknown(host_shared_Held_record(&frame->held));
	if (frame->toasted_live)
		txn_state_totals_unknown(host_shared_Held_record(&frame->toasted));
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	if (!frame->redo(frame->captures, &frame->io, TREES_STORE_READER_NODE_STORE(&store), &frame->held,
					 frame->toasted_live ? &frame->toasted : NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_replayed runs a redo of one of a storage's records on its
 * state, in the startup process or on a standby, outside any transaction,
 * with a reader of its repository's base and the state of the TOAST storage
 * its record names, if any; an index's storage's state is made with the
 * table and index its record names. The repository is the one of the
 * database the record names, since the process redoing is connected to none.
 * A replayed commit moves no totals, so the states it redoes are marked
 * uncounted, for a reader to count again.
 */
static bool
doltgres_entries_replayed(const PgCaps *caps, const HostStorage *storage, TxnStateMade *made,
						  const HostStorage *toast, DoltgresEntriesRedo redo, void *captures, PgPgError *error)
{
	doltgres_entries_replayed_frame_t frame = {.caps = caps, .storage = storage, .made = made, .toast = toast,
	.redo = redo, .captures = captures, .error = error};

	std_landing(&frame, doltgres_entries_replayed_body, doltgres_entries_replayed_pad);
	doltgres_entries_replayed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_redone_t is rmgr_redo's redos' captures: the record
 * decoded, its end, the page, and a row change's out-of-line values.
 */
typedef struct doltgres_entries_redone_t
{
	const void *record;
	uint64		end;
	TxnInsertPage page;
	const StdVec *entries;
} doltgres_entries_redone_t;

/* doltgres_entries_doltgres_rmgr_redo_closure_0 is a killed completion's redo: the speculative insert completed killed. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_0(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	bool		completed;

	return txn_insert_complete(io, held, ((const doltgres_entries_redone_t *) captures)->record,
							   TXN_INSERT_COMPLETED_KILLED, false, &completed, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_1 is a spent tuple id's redo. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_1(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_insert_redo_spent(io, store, held, c->record, &c->page, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_2 is an index's row change's redo: its version pushed. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_2(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_change_redo(io, held, c->record, c->end, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_3 is a row change's redo with no put: its version pushed. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_3(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_change_redo(io, held, c->record, c->end, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_6 is a row change's redo with a put: its row placed and pushed. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_6(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_insert_redo(io, store, held, c->record, c->end, &c->page, toast,
						   toast != NULL ? c->entries->ptr : NULL, toast != NULL ? c->entries->len : 0, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_7 is an emptied storage's redo. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_7(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	return txn_state_redo_emptied(io, held, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_8 is a left page's redo. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_8(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											  const HostSharedHeld *held, const HostSharedHeld *toast,
											  HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_insert_redo_left(io, store, held, c->record, &c->page, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_10 is freed tuple ids' redo. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_10(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											   const HostSharedHeld *held, const HostSharedHeld *toast,
											   HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_vacuum_redo_freed(io, store, held, toast, c->record, &c->page, c->end, error);
}

/* doltgres_entries_doltgres_rmgr_redo_closure_11 is a carried map's redo: its spaced value ids replayed. */
static bool
doltgres_entries_doltgres_rmgr_redo_closure_11(void *captures, HostIoIo *io, TreesStoreNodeStore store,
											   const HostSharedHeld *held, const HostSharedHeld *toast,
											   HostError *error)
{
	const doltgres_entries_redone_t *c = captures;

	return txn_rewrite_spaced(io, held, c->record, true, c->page.most, true, c->end, error);
}

/*
 * doltgres_entries_doltgres_rmgr_redo_frame_t is rmgr_redo's state across its
 * pad: the record decoded, each while live, a row change's out-of-line
 * values, and a base's fold's repository, reader, and state, or a chunk's
 * deltas.
 */
typedef struct doltgres_entries_doltgres_rmgr_redo_frame_t
{
	const PgCaps *caps;
	const PgWalRecord *record;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	TxnRecordCompletion completion;
	bool		completion_live;
	TxnRecordRowChange change;
	bool		change_live;
	StdVec		entries;
	bool		entries_live;
	TxnRecordBaseAdvanced advanced;
	bool		advanced_live;
	TxnRecordChunkWrite chunk;
	bool		chunk_live;
	TxnRecordDatabaseChange database_change;
	bool		database_change_live;
	TxnRecordTidsFreed freed;
	bool		freed_live;
	TxnRecordMapCarried carried;
	bool		carried_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	HostSharedHeld held;
	bool		held_live;
} doltgres_entries_doltgres_rmgr_redo_frame_t;

/* doltgres_entries_doltgres_rmgr_redo_pad frees what a redo holds and the record decoded. */
static void
doltgres_entries_doltgres_rmgr_redo_pad(void *frame_)
{
	doltgres_entries_doltgres_rmgr_redo_frame_t *frame = frame_;

	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, sizeof(TxnValuesEntry), _Alignof(TxnValuesEntry));
	if (frame->completion_live)
		txn_record_Completion_drop(&frame->completion);
	if (frame->change_live)
		txn_record_RowChange_drop(&frame->change);
	if (frame->advanced_live)
		txn_record_BaseAdvanced_drop(&frame->advanced);
	if (frame->chunk_live)
		txn_record_ChunkWrite_drop(&frame->chunk);
	if (frame->database_change_live)
		txn_record_DatabaseChange_drop(&frame->database_change);
	if (frame->freed_live)
		txn_record_TidsFreed_drop(&frame->freed);
	if (frame->carried_live)
		txn_record_MapCarried_drop(&frame->carried);
	frame->held_live = frame->reading_live = frame->repo_live = frame->entries_live = frame->completion_live =
		frame->change_live = frame->advanced_live = frame->chunk_live = frame->database_change_live =
		frame->freed_live = frame->carried_live = false;
}

/* doltgres_entries_redo_raised raises a redo's host error as Postgres', into the frame's error, answering false. */
static bool
doltgres_entries_redo_raised(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, HostError *failed)
{
	*frame->error = pghost_raised(failed);
	return false;
}

/* doltgres_entries_row_changed replays a row change: an index's, one with no row to place, or one with a put. */
static bool
doltgres_entries_row_changed(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body, size_t body_len)
{
	doltgres_entries_redone_t c = {.record = &frame->change, .page = doltgres_entries_page()};
	const TxnMemtablePut *put;
	HostStorage toast;
	HostError	failed;

	if (!txn_record_RowChange_decode(body, body_len, &frame->change, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->change_live = true;
	if (frame->change.has_index)
	{
		TxnStateMade made = {.key = {.tag = TXN_BASE_KEY_NONE}, .table = frame->change.table, .has_index = true,
		.index = frame->change.index};

		c.end = pg_calls_record_end(frame->record);
		return doltgres_entries_replayed(frame->caps, &frame->change.storage, &made, NULL,
										 doltgres_entries_doltgres_rmgr_redo_closure_2, &c, frame->error);
	}
	if (!txn_record_RowChange_put(&frame->change, &put))
	{
		c.end = pg_calls_record_end(frame->record);
		return doltgres_entries_replayed(frame->caps, &frame->change.storage, NULL, NULL,
										 doltgres_entries_doltgres_rmgr_redo_closure_3, &c, frame->error);
	}
	toast.tablespace = frame->change.toast.tablespace;
	toast.database = frame->change.storage.database;
	toast.relation = frame->change.toast.relation;
	std_vec_with_capacity_Checked(&frame->entries, frame->change.blobs.len, sizeof(TxnValuesEntry),
								  _Alignof(TxnValuesEntry));
	frame->entries_live = true;
	for (size_t i = 0; i < frame->change.blobs.len; i++)
	{
		const chunks_TupleAddressU64_t *blob = &STD_VEC_AT(&frame->change.blobs, chunks_TupleAddressU64_t, i);
		TxnValuesEntry entry = txn_values_Entry_new(blob->f0, blob->f1);

		std_vec_push_Checked(&frame->entries, &entry, sizeof(TxnValuesEntry), _Alignof(TxnValuesEntry));
	}
	c.end = pg_calls_record_end(frame->record);
	c.entries = &frame->entries;
	return doltgres_entries_replayed(frame->caps, &frame->change.storage, NULL,
									 frame->change.has_toast ? &toast : NULL,
									 doltgres_entries_doltgres_rmgr_redo_closure_6, &c, frame->error);
}

/*
 * doltgres_entries_base_advanced replays a base's advance: the conflict it
 * names resolved, then its fold's chain, or its base with the startup fold
 * that makes room for it when the delta's budget needs it.
 */
static bool
doltgres_entries_base_advanced(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body,
							   size_t body_len)
{
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	TxnRecordBaseAdvanced *advanced = &frame->advanced;
	const HostStorage *s;
	const RepoRepo *repo;
	TxnInsertPage page = doltgres_entries_page();
	HostError	failed;
	uint64		end;
	bool		kept;
	bool		folded;

	if (!txn_record_BaseAdvanced_decode(body, body_len, advanced, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->advanced_live = true;
	frame->io = pghost_io(raise);
	if (!doltgres_entries_redo_repository(&frame->io, advanced->storage.database, &frame->repo, frame->error))
		return false;
	frame->repo_live = true;
	repo = std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo));
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->reading_live = true;
	if (!txn_state_state(&frame->io, repo, &frame->reading, &advanced->storage, TXN_STATE_PERSISTENCE_PERMANENT,
						 false, NULL, &frame->held, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->held_live = true;
	s = &advanced->storage;
	if (advanced->horizon != 0 &&
		!pg_calls_resolve_conflict(raise, advanced->horizon, s->tablespace, s->database, s->relation, frame->error))
		return false;
	if (advanced->has_chain)
	{
		TxnBaseBase recorded = advanced->base;

		advanced->base.key.tag = TXN_BASE_KEY_UNKNOWN;
		if (!txn_fold_redo_chained(&frame->io, repo, &frame->reading, &frame->held, advanced->chain.ptr,
								   advanced->chain.len, &recorded, &failed))
			return doltgres_entries_redo_raised(frame, &failed);
		return true;
	}
	end = pg_calls_record_end(frame->record);
	if (!txn_fold_redo(&frame->io, repo, &frame->reading, &frame->held, advanced, end, &kept, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	if ((!kept || !shmem_delta_launcher_running()) &&
		!txn_fold_startup_fold(&frame->io, repo, &frame->reading, &frame->held, &page, end, &folded, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	if (!kept && !txn_fold_redo(&frame->io, repo, &frame->reading, &frame->held, advanced, end, &kept, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	return true;
}

/* doltgres_entries_redo_own returns the own number of a database's repository, held only for the call. */
static bool
doltgres_entries_redo_own(HostIoIo *io, uint32 database, uint32 *result, PgPgError *error)
{
	StdArcInner *repo;

	if (!doltgres_entries_redo_repository(io, database, &repo, error))
		return false;
	*result = repo_Repo_own(std_arc_Deref_deref(repo, _Alignof(RepoRepo)));
	std_arc_drop(repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	return true;
}

/* doltgres_entries_row_change_redone replays a row change record: a killed completion, a spent tuple id, or a change. */
static bool
doltgres_entries_row_change_redone(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body,
								   size_t body_len)
{
	doltgres_entries_redone_t c = {.page = doltgres_entries_page()};
	TxnRecordSpent spent;
	HostError	failed;
	bool		is;

	if (!txn_record_Completion_killed(body, body_len, &is, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	if (is)
	{
		if (!txn_record_Completion_decode(body, body_len, &frame->completion, &failed))
			return doltgres_entries_redo_raised(frame, &failed);
		frame->completion_live = true;
		c.record = &frame->completion;
		return doltgres_entries_replayed(frame->caps, &frame->completion.storage, NULL, NULL,
										 doltgres_entries_doltgres_rmgr_redo_closure_0, &c, frame->error);
	}
	if (!txn_record_Spent_is(body, body_len, &is, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	if (!is)
		return doltgres_entries_row_changed(frame, body, body_len);
	if (!txn_record_Spent_decode(body, body_len, &spent, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	c.record = &spent;
	return doltgres_entries_replayed(frame->caps, &spent.storage, NULL, NULL,
									 doltgres_entries_doltgres_rmgr_redo_closure_1, &c, frame->error);
}

/* doltgres_entries_chunk_redone replays a chunk's write into its repository's working state's deltas. */
static bool
doltgres_entries_chunk_redone(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body,
							  size_t body_len)
{
	HostError	failed;
	uint32		own;

	if (!txn_record_ChunkWrite_decode(body, body_len, &frame->chunk, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->chunk_live = true;
	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (!doltgres_entries_redo_own(&frame->io, frame->chunk.repository, &own, frame->error))
		return false;
	if (!txn_memtable_branch_deltas(&frame->io, frame->chunk.repository, own, &frame->held, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->held_live = true;
	frame->chunk_live = false;
	if (!txn_blobs_redo(&frame->io, &frame->held, frame->chunk, pg_calls_record_end(frame->record), &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	return true;
}

/* doltgres_entries_database_change_redone replays a database's identity change into its repository's deltas. */
static bool
doltgres_entries_database_change_redone(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body,
										size_t body_len)
{
	HostError	failed;
	uint32		own;

	if (!txn_record_DatabaseChange_decode(body, body_len, &frame->database_change, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->database_change_live = true;
	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (!doltgres_entries_redo_own(&frame->io, frame->database_change.repository, &own, frame->error))
		return false;
	frame->database_change_live = false;
	if (!txn_identity_redo(&frame->io, &frame->database_change, own, pg_calls_record_end(frame->record), &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	return true;
}

/* doltgres_entries_tids_freed_redone replays freed tuple ids, the conflict their record names resolved first. */
static bool
doltgres_entries_tids_freed_redone(doltgres_entries_doltgres_rmgr_redo_frame_t *frame, const uint8 *body,
								   size_t body_len)
{
	doltgres_entries_redone_t c = {.page = doltgres_entries_page()};
	const HostStorage *s;
	HostError	failed;

	if (!txn_record_TidsFreed_decode(body, body_len, c.page.most, c.page.bytes, &frame->freed, &failed))
		return doltgres_entries_redo_raised(frame, &failed);
	frame->freed_live = true;
	s = &frame->freed.storage;
	if (frame->freed.horizon != 0 &&
		!pg_calls_resolve_conflict(pg_Caps_may_raise(frame->caps), frame->freed.horizon, s->tablespace, s->database,
								   s->relation, frame->error))
		return false;
	c.end = pg_calls_record_end(frame->record);
	c.record = &frame->freed;
	return doltgres_entries_replayed(frame->caps, &frame->freed.storage, NULL,
									 frame->freed.has_values ? &frame->freed.values.f0 : NULL,
									 doltgres_entries_doltgres_rmgr_redo_closure_10, &c, frame->error);
}

/* doltgres_entries_store_redone runs a repository record's redo, after the process's repositories when asked. */
static bool
doltgres_entries_store_redone(doltgres_entries_doltgres_rmgr_redo_frame_t *frame,
							  bool (*redo) (HostIoIo *io, const uint8 *body, size_t body_len, HostError *error),
							  bool released, const uint8 *body, size_t body_len)
{
	HostError	failed;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (released)
		doltgres_entries_redone_released(&frame->io);
	return doltgres_entries_raised(redo(&frame->io, body, body_len, &failed), &failed, frame->error);
}

/* doltgres_entries_doltgres_rmgr_redo_body replays the record through the redo of the crate that owns its kind. */
static void
doltgres_entries_doltgres_rmgr_redo_body(void *frame_)
{
	doltgres_entries_doltgres_rmgr_redo_frame_t *frame = frame_;
	doltgres_entries_redone_t c = {.page = doltgres_entries_page()};
	uint8		info;
	size_t		body_len;
	const uint8 *body;
	HostWalKind kind;
	HostError	failed;
	TxnRecordStorageEmptied emptied;
	TxnRecordPageLeft left;

	pghost_delta_recovery_fold(doltgres_entries_startup_folds);
	info = pg_calls_record_info(frame->record);
	body = pg_calls_record_data(frame->record, &body_len);
	if (!host_WalKind_of(info >> 4, &kind))
	{
		pg_calls_unknown_kind(info);
		frame->ok = true;
		return;
	}
	switch (kind)
	{
		case HOST_WAL_KIND_ROW_CHANGE:
			frame->ok = doltgres_entries_row_change_redone(frame, body, body_len);
			return;
		case HOST_WAL_KIND_CONFIRM:
			if (!txn_record_Completion_decode(body, body_len, &frame->completion, &failed))
			{
				doltgres_entries_redo_raised(frame, &failed);
				return;
			}
			frame->completion_live = true;
			frame->ok = true;
			return;
		case HOST_WAL_KIND_BASE_ADVANCED:
			frame->ok = doltgres_entries_base_advanced(frame, body, body_len);
			return;
		case HOST_WAL_KIND_STORAGE_EMPTIED:
			if (!txn_record_StorageEmptied_decode(body, body_len, &emptied, &failed))
			{
				doltgres_entries_redo_raised(frame, &failed);
				return;
			}
			frame->ok = doltgres_entries_replayed(frame->caps, &emptied.storage, NULL, NULL,
												  doltgres_entries_doltgres_rmgr_redo_closure_7, &c, frame->error);
			return;
		case HOST_WAL_KIND_CHUNK:
			frame->ok = doltgres_entries_chunk_redone(frame, body, body_len);
			return;
		case HOST_WAL_KIND_DATABASE_CHANGE:
			frame->ok = doltgres_entries_database_change_redone(frame, body, body_len);
			return;
		case HOST_WAL_KIND_PAGE_LEFT:
			if (!txn_record_PageLeft_decode(body, body_len, &left, &failed))
			{
				doltgres_entries_redo_raised(frame, &failed);
				return;
			}
			c.record = &left;
			frame->ok = doltgres_entries_replayed(frame->caps, &left.storage, NULL, NULL,
												  doltgres_entries_doltgres_rmgr_redo_closure_8, &c, frame->error);
			return;
		case HOST_WAL_KIND_TIDS_FREED:
			frame->ok = doltgres_entries_tids_freed_redone(frame, body, body_len);
			return;
		case HOST_WAL_KIND_MAP_CARRIED:
			if (!txn_record_MapCarried_decode(body, body_len, &frame->carried, &failed))
			{
				doltgres_entries_redo_raised(frame, &failed);
				return;
			}
			frame->carried_live = true;
			c.end = pg_calls_record_end(frame->record);
			c.page.most = pg_calls_max_heap_tuples_per_page();
			c.record = &frame->carried;
			frame->ok = doltgres_entries_replayed(frame->caps, &frame->carried.storage, NULL, NULL,
												  doltgres_entries_doltgres_rmgr_redo_closure_11, &c, frame->error);
			return;
		case HOST_WAL_KIND_REPOSITORY_CREATED:
			frame->ok = doltgres_entries_store_redone(frame, store_databases_redo_created, false, body, body_len);
			return;
		case HOST_WAL_KIND_REPOSITORY_DROPPED:
			frame->ok = doltgres_entries_store_redone(frame, store_databases_redo_dropped, true, body, body_len);
			return;
		case HOST_WAL_KIND_REPOSITORY_COPIED:
			frame->ok = doltgres_entries_store_redone(frame, store_databases_redo_copied, false, body, body_len);
			return;
		case HOST_WAL_KIND_REPOSITORY_MOVED:
			frame->ok = doltgres_entries_store_redone(frame, store_databases_redo_moved, true, body, body_len);
			return;
#ifdef USE_INJECTION_POINTS
		case HOST_WAL_KIND_TEST:
			frame->ok = doltgres_entries_testing_redo_test(frame->caps, body, body_len, frame->error);
			return;
#endif
	}
	pg_calls_unknown_kind(info);
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_rmgr_redo replays one of the fork's WAL records
 * through the redo of the crate that owns its kind, in the startup process;
 * a kind no crate owns stops recovery, as Postgres' own resource managers
 * do.
 */
bool
doltgres_entries_doltgres_rmgr_redo(const PgCaps *caps, const PgWalRecord *record, PgPgError *error)
{
	doltgres_entries_doltgres_rmgr_redo_frame_t frame = {.caps = caps, .record = record, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_rmgr_redo_body, doltgres_entries_doltgres_rmgr_redo_pad);
	doltgres_entries_doltgres_rmgr_redo_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_rmgr_desc_frame_t is rmgr_desc's state across its
 * pad: the record decoded, each while live, and its description.
 */
typedef struct doltgres_entries_doltgres_rmgr_desc_frame_t
{
	const PgCaps *caps;
	PgDesc	   *buf;
	const PgWalRecord *record;
	PgPgError  *error;
	bool		ok;
	TxnRecordRowChange change;
	bool		change_live;
	TxnRecordBaseAdvanced advanced;
	bool		advanced_live;
	TxnRecordChunkWrite chunk;
	bool		chunk_live;
	TxnRecordDatabaseChange database_change;
	bool		database_change_live;
	TxnRecordTidsFreed freed;
	bool		freed_live;
	TxnRecordMapCarried carried;
	bool		carried_live;
	StdString text;
	bool		text_live;
} doltgres_entries_doltgres_rmgr_desc_frame_t;

/* doltgres_entries_doltgres_rmgr_desc_pad frees the description and the record decoded. */
static void
doltgres_entries_doltgres_rmgr_desc_pad(void *frame_)
{
	doltgres_entries_doltgres_rmgr_desc_frame_t *frame = frame_;

	if (frame->text_live)
		std_vec_free_Checked(&frame->text, 1, 1);
	if (frame->change_live)
		txn_record_RowChange_drop(&frame->change);
	if (frame->advanced_live)
		txn_record_BaseAdvanced_drop(&frame->advanced);
	if (frame->chunk_live)
		txn_record_ChunkWrite_drop(&frame->chunk);
	if (frame->database_change_live)
		txn_record_DatabaseChange_drop(&frame->database_change);
	if (frame->freed_live)
		txn_record_TidsFreed_drop(&frame->freed);
	if (frame->carried_live)
		txn_record_MapCarried_drop(&frame->carried);
	frame->text_live = frame->change_live = frame->advanced_live = frame->chunk_live = false;
	frame->database_change_live = frame->freed_live = frame->carried_live = false;
}

/* doltgres_entries_described formats a description into the frame's text. */
static void
doltgres_entries_described(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const StdFmtPiece *pieces,
						   size_t pieces_len, const StdFmtArg *args)
{
	StdFmtArguments arguments = {pieces, pieces_len, args};

	std_fmt_format_Checked(&frame->text, &arguments);
	frame->text_live = true;
}

/* doltgres_entries_tids counts the tids of runs, each a (u32, u16, u16, u64) whose count is its third. */
static uint64
doltgres_entries_tids(const StdVec *runs)
{
	uint64		tids = 0;

	for (size_t i = 0; i < runs->len; i++)
		tids += STD_VEC_AT(runs, std_TupleU32U16U16U64_t, i).f2;
	return tids;
}

/* doltgres_entries_rmgr_desc_row describes a row change record: a spent tid, or a change and its put's tid. */
static bool
doltgres_entries_rmgr_desc_row(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
							   size_t body_len, HostError *failed)
{
	StdFmtPiece spent_pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1),
		STD_FMT_LIT("/"), STD_FMT_ARG(2), STD_FMT_LIT("; tid ("), STD_FMT_ARG(3), STD_FMT_LIT(","), STD_FMT_ARG(4),
	STD_FMT_LIT(") spent")};
	StdFmtPiece change_pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1),
		STD_FMT_LIT("/"), STD_FMT_ARG(2), STD_FMT_LIT("; tid ("), STD_FMT_ARG(3), STD_FMT_LIT(","), STD_FMT_ARG(4),
	STD_FMT_LIT(")")};
	const HostStorage *s;
	TxnRecordSpent spent;
	const TxnMemtablePut *put;
	uint64		tid = 0;
	uint64		block;
	uint64		offset;
	bool		is;

	if (!txn_record_Spent_is(body, body_len, &is, failed))
		return false;
	if (is)
	{
		if (!txn_record_Spent_decode(body, body_len, &spent, failed))
			return false;
		s = &spent.storage;
		tid = spent.tuple.f0;
	}
	else
	{
		if (!txn_record_RowChange_decode(body, body_len, &frame->change, failed))
			return false;
		frame->change_live = true;
		s = &frame->change.storage;
		if (txn_record_RowChange_put(&frame->change, &put))
			tid = put->tuple.f0;
	}
	block = tid >> 16;
	offset = tid & 0xffff;
	{
		StdFmtArg	args[] = {{&s->tablespace, std_u32_Display_fmt}, {&s->database, std_u32_Display_fmt},
		{&s->relation, std_u32_Display_fmt}, {&block, std_u64_Display_fmt}, {&offset, std_u64_Display_fmt}};

		doltgres_entries_described(frame, is ? spent_pieces : change_pieces, lengthof(spent_pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_base describes a base's advance: its storage, fold point, largest xid, and packs. */
static bool
doltgres_entries_rmgr_desc_base(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
								size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"),
		STD_FMT_ARG(2), STD_FMT_LIT("; folded at "), STD_FMT_ARG(3), STD_FMT_LIT("/"), STD_FMT_ARG(4),
		STD_FMT_LIT("; max folded "), STD_FMT_ARG(5), STD_FMT_LIT("; "), STD_FMT_ARG(6),
	STD_FMT_LIT(" packs")};
	const TxnRecordBaseAdvanced *advanced = &frame->advanced;
	uint64		high;
	uint64		low;
	size_t		packs;

	if (!txn_record_BaseAdvanced_decode(body, body_len, &frame->advanced, failed))
		return false;
	frame->advanced_live = true;
	high = advanced->folded >> 32;
	low = advanced->folded & 0xffffffff;
	packs = advanced->packs.len;
	{
		StdFmtArg	args[] = {{&advanced->storage.tablespace, std_u32_Display_fmt},
		{&advanced->storage.database, std_u32_Display_fmt}, {&advanced->storage.relation, std_u32_Display_fmt},
		{&high, std_u64_UpperHex_fmt}, {&low, std_u64_UpperHex_fmt},
		{&advanced->max_folded, std_u64_Display_fmt}, {&packs, std_usize_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_storage describes a record by its storage alone, an emptied storage's. */
static void
doltgres_entries_rmgr_desc_storage(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const HostStorage *s)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"),
	STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&s->tablespace, std_u32_Display_fmt}, {&s->database, std_u32_Display_fmt},
	{&s->relation, std_u32_Display_fmt}};

	doltgres_entries_described(frame, pieces, lengthof(pieces), args);
}

/* doltgres_entries_rmgr_desc_chunk describes a chunk's write: its repository and its bytes. */
static bool
doltgres_entries_rmgr_desc_chunk(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
								 size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0), STD_FMT_LIT("; chunk of "), STD_FMT_ARG(1),
	STD_FMT_LIT(" bytes")};
	size_t		bytes;

	if (!txn_record_ChunkWrite_decode(body, body_len, &frame->chunk, failed))
		return false;
	frame->chunk_live = true;
	bytes = frame->chunk.bytes.len;
	{
		StdFmtArg	args[] = {{&frame->chunk.repository, std_u32_Display_fmt}, {&bytes, std_usize_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_identity describes a database's identity change: a put or a removal, and its key. */
static bool
doltgres_entries_rmgr_desc_identity(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
									size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0), STD_FMT_LIT("; identity "), STD_FMT_ARG(1),
	STD_FMT_LIT(" of a "), STD_FMT_ARG(2), STD_FMT_LIT("-byte key")};
	StdStr		what;
	size_t		key;

	if (!txn_record_DatabaseChange_decode(body, body_len, &frame->database_change, failed))
		return false;
	frame->database_change_live = true;
	what.ptr = frame->database_change.has_value ? "put" : "removal";
	what.len = strlen(what.ptr);
	key = frame->database_change.key.len;
	{
		StdFmtArg	args[] = {{&frame->database_change.repository, std_u32_Display_fmt},
		{&what, std_str_Display_fmt}, {&key, std_usize_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_left describes a left page: its storage and the block its tid moved to. */
static bool
doltgres_entries_rmgr_desc_left(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
								size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"),
	STD_FMT_ARG(2), STD_FMT_LIT("; to block "), STD_FMT_ARG(3)};
	TxnRecordPageLeft left;
	uint64		block;

	if (!txn_record_PageLeft_decode(body, body_len, &left, failed))
		return false;
	block = left.to >> 16;
	{
		StdFmtArg	args[] = {{&left.storage.tablespace, std_u32_Display_fmt},
		{&left.storage.database, std_u32_Display_fmt}, {&left.storage.relation, std_u32_Display_fmt},
		{&block, std_u64_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_freed describes freed tids: the storage, the horizon, and the freed and dead tids. */
static bool
doltgres_entries_rmgr_desc_freed(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
								 size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"),
		STD_FMT_ARG(2), STD_FMT_LIT("; horizon "), STD_FMT_ARG(3), STD_FMT_LIT("; "), STD_FMT_ARG(4),
	STD_FMT_LIT(" tids; "), STD_FMT_ARG(5), STD_FMT_LIT(" dead")};
	TxnInsertPage page = doltgres_entries_page();
	const HostStorage *s;
	uint64		tids;
	uint64		dead;

	if (!txn_record_TidsFreed_decode(body, body_len, page.most, page.bytes, &frame->freed, failed))
		return false;
	frame->freed_live = true;
	s = &frame->freed.storage;
	tids = doltgres_entries_tids(&frame->freed.runs);
	dead = doltgres_entries_tids(&frame->freed.dead);
	{
		StdFmtArg	args[] = {{&s->tablespace, std_u32_Display_fmt}, {&s->database, std_u32_Display_fmt},
		{&s->relation, std_u32_Display_fmt}, {&frame->freed.horizon, std_u64_Display_fmt},
		{&tids, std_u64_Display_fmt}, {&dead, std_u64_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_rmgr_desc_carried describes carried map versions: the storage, the map and key, and the versions. */
static bool
doltgres_entries_rmgr_desc_carried(doltgres_entries_doltgres_rmgr_desc_frame_t *frame, const uint8 *body,
								   size_t body_len, HostError *failed)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("rel "), STD_FMT_ARG(0), STD_FMT_LIT("/"), STD_FMT_ARG(1), STD_FMT_LIT("/"),
		STD_FMT_ARG(2), STD_FMT_LIT("; "), STD_FMT_ARG(3), STD_FMT_LIT(" key "), STD_FMT_ARG(4), STD_FMT_LIT("; "),
	STD_FMT_ARG(5), STD_FMT_LIT(" versions")};
	const HostStorage *s;
	StdStr		map;
	size_t		versions;

	if (!txn_record_MapCarried_decode(body, body_len, &frame->carried, failed))
		return false;
	frame->carried_live = true;
	s = &frame->carried.storage;
	map.ptr = frame->carried.map == TXN_MEMTABLE_MAP_MAP_FREE ? "free space" : "values";
	map.len = strlen(map.ptr);
	versions = frame->carried.versions.len;
	{
		StdFmtArg	args[] = {{&s->tablespace, std_u32_Display_fmt}, {&s->database, std_u32_Display_fmt},
		{&s->relation, std_u32_Display_fmt}, {&map, std_str_Display_fmt},
		{&frame->carried.key, std_u32_Display_fmt}, {&versions, std_usize_Display_fmt}};

		doltgres_entries_described(frame, pieces, lengthof(pieces), args);
	}
	return true;
}

/* doltgres_entries_doltgres_rmgr_desc_body describes the record by its kind, then appends the description. */
static void
doltgres_entries_doltgres_rmgr_desc_body(void *frame_)
{
	doltgres_entries_doltgres_rmgr_desc_frame_t *frame = frame_;
	size_t		body_len;
	const uint8 *body = pg_calls_record_data(frame->record, &body_len);
	HostWalKind kind;
	HostError	failed;
	bool		ok = true;

	if (host_WalKind_of(pg_calls_record_info(frame->record) >> 4, &kind))
	{
		switch (kind)
		{
			case HOST_WAL_KIND_ROW_CHANGE:
				ok = doltgres_entries_rmgr_desc_row(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_BASE_ADVANCED:
				ok = doltgres_entries_rmgr_desc_base(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_STORAGE_EMPTIED:
				{
					TxnRecordStorageEmptied emptied;

					ok = txn_record_StorageEmptied_decode(body, body_len, &emptied, &failed);
					if (ok)
						doltgres_entries_rmgr_desc_storage(frame, &emptied.storage);
					break;
				}
			case HOST_WAL_KIND_CHUNK:
				ok = doltgres_entries_rmgr_desc_chunk(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_DATABASE_CHANGE:
				ok = doltgres_entries_rmgr_desc_identity(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_PAGE_LEFT:
				ok = doltgres_entries_rmgr_desc_left(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_TIDS_FREED:
				ok = doltgres_entries_rmgr_desc_freed(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_MAP_CARRIED:
				ok = doltgres_entries_rmgr_desc_carried(frame, body, body_len, &failed);
				break;
			case HOST_WAL_KIND_REPOSITORY_CREATED:
				ok = store_databases_describe_created(body, body_len, &frame->text, &failed);
				frame->text_live = ok;
				break;
			case HOST_WAL_KIND_REPOSITORY_DROPPED:
				ok = store_databases_describe_dropped(body, body_len, &frame->text, &failed);
				frame->text_live = ok;
				break;
			case HOST_WAL_KIND_REPOSITORY_COPIED:
			case HOST_WAL_KIND_REPOSITORY_MOVED:
				ok = store_databases_describe_moved(body, body_len, &frame->text, &failed);
				frame->text_live = ok;
				break;
#ifdef USE_INJECTION_POINTS
			case HOST_WAL_KIND_TEST:
				doltgres_entries_testing_describe_test(body, body_len, &frame->text);
				frame->text_live = true;
				break;
#endif
			case HOST_WAL_KIND_CONFIRM:
				break;
		}
	}
	if (!ok)
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (frame->text_live &&
		!pg_calls_desc_append(pg_Caps_may_raise(frame->caps), frame->buf, frame->text.ptr, frame->text.len,
							  frame->error))
		return;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_rmgr_desc describes one of the fork's WAL
 * records, as the crate that owns its kind does, for pg_walinspect; a kind
 * no crate owns gets no description, and a record of the fork's own kinds
 * that doesn't decode raises its data corruption.
 */
bool
doltgres_entries_doltgres_rmgr_desc(const PgCaps *caps, PgDesc *buf, const PgWalRecord *record, PgPgError *error)
{
	doltgres_entries_doltgres_rmgr_desc_frame_t frame = {.caps = caps, .buf = buf, .record = record,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_rmgr_desc_body, doltgres_entries_doltgres_rmgr_desc_pad);
	doltgres_entries_doltgres_rmgr_desc_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_rmgr_identify returns the name of a record's kind, or nothing for a kind no crate owns. */
bool
doltgres_entries_doltgres_rmgr_identify(const PgCaps *caps, uint8 info, const char **result, PgPgError *error)
{
	HostWalKind kind;

	*result = host_WalKind_of(info >> 4, &kind) ? host_WalKind_name(kind) : NULL;
	return true;
}

/*
 * doltgres_entries_doltgres_table_am_routine gives a relation the fork stores
 * the fork's table access method routine, whatever access method it names
 * (design item 9), and answers nothing for any other relation, which keeps
 * the routine its handler returns.
 */
bool
doltgres_entries_doltgres_table_am_routine(const PgCatalogCaps *caps, const PgRel *relation,
										   PgTableRoutineOption *result, PgPgError *error)
{
	if (!doltgres_relation_stored(caps, relation, &result->has, error))
		return false;
	if (result->has)
		result->routine = pg_calls_table_routine();
	return true;
}

/*
 * doltgres_entries_doltgres_old_entries_invalidated is the relcache callback
 * of the removal of old entries (the btree text's section 5): it marks stale
 * each of the backend's removals of a relation an invalidation names, raising
 * nothing.
 */
bool
doltgres_entries_doltgres_old_entries_invalidated(const PgCleanup *caps, PgDatum arg, uint32 relid, PgPgError *error)
{
	doltgres_index_invalidated(relid);
	return true;
}

/* txn::state::held at (), which layouts.c defines and counted takes. */
#define TXN_STATE_TS Unit
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* txn::state::held at Option<Row>, which searches.c defines and serial takes. */
#define TXN_STATE_TS OptionRow
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, DoltgresScanRow *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* doltgres_entries_TupleU32OptionU32_t is the tuple (u32, Option<u32>): a storage's base's table, and its index if it has one. */
typedef struct doltgres_entries_TupleU32OptionU32_t
{
	uint32		f0;
	bool		has_f1;
	uint32		f1;
} doltgres_entries_TupleU32OptionU32_t;

/* txn::state::held at Option<(u32, Option<u32>)>, which committing takes. */
#define TXN_STATE_TS OptionTupleU32OptionU32
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, doltgres_entries_TupleU32OptionU32_t *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; doltgres_entries_TupleU32OptionU32_t *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_entries_committing_t is committing's lent calls' captures: the
 * reader, the transaction's top-level xid, a tree's written keys, and a
 * table's tree's order and keyless mark.
 */
typedef struct doltgres_entries_committing_t
{
	const HostSharedReader *reading;
	uint64		own;
	const StdVec *keys;
	const TreesKeyOrder *order;
	bool		keyless;
	bool		index;
} doltgres_entries_committing_t;

/*
 * doltgres_entries_committing_closure_0 is committing's lent call over a
 * value map: each written value's entry with the transaction's versions
 * against without, recorded for the commit to add.
 */
static bool
doltgres_entries_committing_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  HostError *error)
{
	const doltgres_entries_committing_t *c = captures;
	TreesStoreReader store;
	TxnBaseSizes adds = {0};
	TxnBaseSizes takes = {0};
	TxnBaseBase base;
	TreesStoreTree root = {0};
	bool		has_root = false;
	bool		some;

	if (!txn_state_base_record(io, held, &some, &base, error))
		return false;
	if (some)
	{
		has_root = base.has_root;
		root = base.root;
		txn_base_Base_drop(&base);
	}
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	for (size_t i = 0; i < c->keys->len; i++)
	{
		const StdVec *key = &STD_VEC_AT(c->keys, StdVec, i);
		TxnBaseSizes added;
		TxnBaseSizes taken;

		if (!txn_fold_committed_value_moves(io, TREES_STORE_READER_NODE_STORE(&store), held, has_root, root, c->own,
											key->ptr, key->len, &added, &taken, error))
			return false;
		adds = txn_base_Sizes_summed(adds, added);
		takes = txn_base_Sizes_summed(takes, taken);
	}
	txn_state_committing(held, adds, takes);
	return true;
}

/* doltgres_entries_committing_closure_1 is committing's based: the table and index its storage's base names. */
static bool
doltgres_entries_committing_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  bool *some, doltgres_entries_TupleU32OptionU32_t *result, HostError *error)
{
	TxnBaseBase base;

	if (!txn_state_base_record(io, held, some, &base, error))
		return false;
	if (*some)
	{
		*result = (doltgres_entries_TupleU32OptionU32_t)
		{
			base.table, base.has_index, base.index
		};
		txn_base_Base_drop(&base);
	}
	return true;
}

/* doltgres_entries_committing_closure_2 is committing's unknown: the storage's totals marked uncounted. */
static bool
doltgres_entries_committing_closure_2(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  HostError *error)
{
	txn_state_totals_unknown(host_shared_Held_record(held));
	return true;
}

/*
 * doltgres_entries_committing_closure_3 is committing's lent call over a
 * table's or an index's rows: each written key's fold over the base with the
 * transaction's versions against without, recorded for the commit to add.
 */
static bool
doltgres_entries_committing_closure_3(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									  HostError *error)
{
	const doltgres_entries_committing_t *c = captures;
	TreesStoreReader store;
	TxnBaseSizes adds = {0};
	TxnBaseSizes takes = {0};
	TxnBaseBase base;
	TreesStoreTree root;
	TreesStoreTree tuple_ids;
	bool		has_root;
	bool		has_tuple_ids;
	bool		some;

	if (!txn_state_base_record(io, held, &some, &base, error))
		return false;
	if (!some)
		return true;
	has_root = base.has_root;
	root = base.root;
	has_tuple_ids = base.has_tuple_ids;
	tuple_ids = base.tuple_ids;
	txn_base_Base_drop(&base);
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	for (size_t i = 0; i < c->keys->len; i++)
	{
		const StdVec *key = &STD_VEC_AT(c->keys, StdVec, i);
		TxnBaseSizes added;
		TxnBaseSizes taken;

		if (!txn_fold_committed_moves(io, TREES_STORE_READER_NODE_STORE(&store), held, has_root, root, has_tuple_ids,
									  tuple_ids, c->order, c->keyless, c->index, c->own, key->ptr, key->len, &added,
									  &taken, error))
			return false;
		adds = txn_base_Sizes_summed(adds, added);
		takes = txn_base_Sizes_summed(takes, taken);
	}
	txn_state_committing(held, adds, takes);
	return true;
}

/* doltgres_entries_TupleStorageOfVecVecU8_drop is a written tree's drop glue: its keys. */
static void
doltgres_entries_TupleStorageOfVecVecU8_drop(void *written)
{
	trees_VecVecU8_drop(&((txn_TupleStorageOfVecVecU8_t *) written)->f2);
}

/*
 * doltgres_entries_committing_frame_t is committing's state across its pad:
 * the written trees, the repository, its reader, and a table's relation, its
 * index's, and its tree's order, each while live.
 */
typedef struct doltgres_entries_committing_frame_t
{
	const PgCatalogCaps *caps;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	txn_TupleU64VecTupleStorageOfVecVecU8_t written;
	bool		written_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	PgRefRel	opened;
	bool		opened_live;
	PgRefRel	indexed;
	bool		indexed_live;
	TreesKeyOrder order;
	bool		order_live;

	/*
	 * snapshot is whether committing pushed the transaction's snapshot, for a
	 * comparator's folds.
	 */
	bool		snapshot;
} doltgres_entries_committing_frame_t;

/* doltgres_entries_committing_pad frees what committing holds. */
static void
doltgres_entries_committing_pad(void *frame_)
{
	doltgres_entries_committing_frame_t *frame = frame_;

	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->indexed_live)
		pg_RefRel_drop(&frame->indexed);
	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	if (frame->written_live)
		std_vec_drop_Checked(&frame->written.f1, sizeof(txn_TupleStorageOfVecVecU8_t),
							 _Alignof(txn_TupleStorageOfVecVecU8_t), doltgres_entries_TupleStorageOfVecVecU8_drop);
	frame->order_live = frame->indexed_live = frame->opened_live = frame->reading_live = frame->repo_live = false;
	frame->written_live = false;
}

/* doltgres_entries_committing_held runs a lent call on a held state by its storage, none when it isn't held. */
static bool
doltgres_entries_committing_held(doltgres_entries_committing_frame_t *frame, const HostStorage *storage,
								 bool (*call) (void *captures, HostIoIo *io, const RepoRepo *repo,
											   const HostSharedHeld *held, HostError *error),
								 void *captures)
{
	TxnStateFound found = {.tag = TXN_STATE_FOUND_HELD};
	HostError	failed;
	bool		some;

	if (!txn_state_held_to_end(&frame->io, storage, &some, &found.u.held, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (some && !txn_state_held_Unit(&frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
									 &frame->reading, found, call, captures, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_committing_rows records a table's or an index's written rows' moves of its totals. */
static bool
doltgres_entries_committing_rows(doltgres_entries_committing_frame_t *frame, const txn_TupleStorageOfVecVecU8_t *tree)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_entries_committing_t c = {.reading = &frame->reading, .own = frame->written.f0, .keys = &tree->f2};
	TxnStateFound found = {.tag = TXN_STATE_FOUND_HELD};
	HostError	failed;
	bool		some;
	bool		has_opened;
	bool		has_indexed = true;
	doltgres_entries_TupleU32OptionU32_t based;

	if (!txn_state_held_to_end(&frame->io, &tree->f0, &some, &found.u.held, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!some)
		return true;
	if (!txn_state_held_OptionTupleU32OptionU32(&frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
												&frame->reading, found, doltgres_entries_committing_closure_1, NULL,
												NULL, &some, &based, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!some)
		return true;
	if (!pg_RefRel_opened(raise, catalog, based.f0, &has_opened, &frame->opened, frame->error))
		return false;
	frame->opened_live = has_opened;
	if (based.has_f1)
	{
		if (!pg_RefRel_opened(raise, catalog, based.f1, &has_indexed, &frame->indexed, frame->error))
			return false;
		frame->indexed_live = has_indexed;
	}
	if (!has_opened || !has_indexed)
		return doltgres_entries_committing_held(frame, &tree->f0, doltgres_entries_committing_closure_2, NULL);
	if (!doltgres_entries_shape(frame->caps, &frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
								&frame->reading, pg_RefRel_rel(&frame->opened),
								based.has_f1 ? pg_RefRel_rel(&frame->indexed) : NULL, &frame->order, &c.keyless,
								frame->error))
		return false;
	frame->order_live = true;
	if (!frame->snapshot && !trees_KeyOrder_is_bytes(&frame->order))
	{
		if (!pg_calls_transaction_snapshot_push(raise, frame->error))
			return false;
		frame->snapshot = true;
	}
	c.order = &frame->order;
	c.index = based.has_f1;
	return doltgres_entries_committing_held(frame, &tree->f0, doltgres_entries_committing_closure_3, &c);
}

/* doltgres_entries_committing_body records each written tree's moves of its storage's totals. */
static void
doltgres_entries_committing_body(void *frame_)
{
	doltgres_entries_committing_frame_t *frame = frame_;
	HostError	failed;
	bool		some;

	txn_state_committing_cleared();
	frame->io = pghost_io_catalog(frame->caps);
	if (!txn_written_keys(&frame->io, &some, &frame->written, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!some)
	{
		frame->ok = doltgres_entries_raised(txn_state_held_totals_unknown(&frame->io, &failed), &failed,
											frame->error);
		return;
	}
	frame->written_live = true;
	if (frame->written.f1.len == 0)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	for (size_t i = 0; i < frame->written.f1.len; i++)
	{
		const txn_TupleStorageOfVecVecU8_t *tree = &STD_VEC_AT(&frame->written.f1, txn_TupleStorageOfVecVecU8_t, i);

		if (tree->f1 == TXN_WRITTEN_OF_VALUES)
		{
			doltgres_entries_committing_t c = {.reading = &frame->reading, .own = frame->written.f0,
			.keys = &tree->f2};

			if (!doltgres_entries_committing_held(frame, &tree->f0, doltgres_entries_committing_closure_0, &c))
				return;
			continue;
		}
		if (!doltgres_entries_committing_rows(frame, tree))
			return;
		if (frame->order_live)
			trees_KeyOrder_drop(&frame->order);
		if (frame->indexed_live)
			pg_RefRel_drop(&frame->indexed);
		if (frame->opened_live)
			pg_RefRel_drop(&frame->opened);
		frame->order_live = frame->indexed_live = frame->opened_live = false;
	}
	if (frame->snapshot)
		pg_calls_snapshot_pop();
	frame->ok = true;
}

/*
 * doltgres_entries_committing records how the backend's committing
 * transaction moves the totals of each storage whose rows or value map it
 * wrote, at its pre-commit, for the commit's critical section to add (design
 * item 5; bugs row 132): each written row key's fold over the storage's base
 * with the transaction's versions against without them, each fold a read of
 * the key's base entry and of the base pair of each tuple id it takes out,
 * and each written value's entry with them against without, a read of its
 * base entry. A transaction whose list overflowed marks every state it holds
 * as uncounted instead, and so does a storage whose base names a relation
 * that is gone, as a rewrite's new storage names the transient table it was
 * built in, so that its next reader counts it. A storage ordered by a
 * comparator, user code that may run a SQL function, folds under the
 * transaction's snapshot, pushed once for the rest of the pre-commit, as
 * Postgres pushes it for deferred triggers.
 */
static bool
doltgres_entries_committing(const PgCatalogCaps *caps, PgPgError *error)
{
	doltgres_entries_committing_frame_t frame = {.caps = caps, .error = error};

	std_landing(&frame, doltgres_entries_committing_body, doltgres_entries_committing_pad);
	doltgres_entries_committing_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_xact_ended is the fork's transaction callback:
 * before a transaction's commit or its prepare it re-keys the keyless tables
 * its DDL changed the layout of (layouts' rekeyed); before its commit, a
 * parallel worker's included, or its prepare it links the filter of the
 * process's pending spill, so no spill is left to a slot its exit clears; at
 * its pre-commit it then records how the commit moves its storages' totals,
 * and before its prepare it spills every tree the transaction wrote to, so a
 * prepared transaction, which no process publishes, holds no unspilled
 * versions (bugs row 144), and marks its totals uncounted, since the
 * prepared commit adds nothing; at its commit, a parallel worker's included,
 * or its prepare it ends the record of the states the transaction held,
 * which its resource owner drops only at an abort or an error, and ends its
 * list of the keys it wrote to; at its abort, a parallel worker's included,
 * which Postgres records before it calls this, it takes back the
 * transaction's versions by pruning those keys (2g's take-back). Each end
 * clears the moves of the totals. It ends the settle's list at a commit, a
 * prepare, or an abort (the rewrite text's section 6).
 */
bool
doltgres_entries_doltgres_xact_ended(const PgCatalogCaps *caps, int32 event, void *arg, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_CatalogCaps_may_raise(caps));
	HostError	failed;
	bool		end = event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PREPARE;
	bool		abort = event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT;

	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PARALLEL_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
	{
		if (event != XACT_EVENT_PARALLEL_PRE_COMMIT && !doltgres_layouts_rekeyed(caps, error))
			return false;
		if (!txn_spill_settle_pending(&io, &failed))
			return doltgres_entries_raised(false, &failed, error);
		if (event == XACT_EVENT_PRE_PREPARE)
		{
			bool		relieved;

			do
			{
				if (!txn_spill_relieved_own(&io, &relieved, &failed))
					return doltgres_entries_raised(false, &failed, error);
			} while (relieved);
			txn_state_committing_cleared();
			return doltgres_entries_raised(txn_state_held_totals_unknown(&io, &failed), &failed, error);
		}
		if (event == XACT_EVENT_PRE_COMMIT)
			return doltgres_entries_committing(caps, error);
	}
	else if (end)
	{
		if (!doltgres_index_ended(&io, &failed) || !txn_state_ended(&io, &failed) ||
			!txn_written_ended(&io, &failed))
			return doltgres_entries_raised(false, &failed, error);
		doltgres_settle_ended(&io, false);
	}
	else if (abort)
	{
		if (!txn_written_aborted(&io, &failed))
			return doltgres_entries_raised(false, &failed, error);
		doltgres_settle_ended(&io, true);
	}
	if (end || abort)
	{
		doltgres_entries_speculated_clear(&io);
		doltgres_layouts_ended(&io);
		txn_state_committing_cleared();
	}
	return true;
}

/* doltgres_entries_counted_closure_0 is counted's first lent call: whether the held state's totals are known. */
static bool
doltgres_entries_counted_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   bool *result, HostError *error)
{
	*result = txn_state_known(held);
	return true;
}

/*
 * doltgres_entries_counted_captures_t is counted's recount's captures: the
 * reader, the keys' order, and the keyless mark.
 */
typedef struct doltgres_entries_counted_captures_t
{
	const HostSharedReader *reading;
	const TreesKeyOrder *order;
	bool		keyless;
	TreesStoreReader store;
	const HostSharedHeld *held;
} doltgres_entries_counted_captures_t;

/* doltgres_entries_counted_closure_1_0 is counted's recount: the fold's count of the held state over the repository. */
static bool
doltgres_entries_counted_closure_1_0(void *captures, HostIoIo *io, bool *some, TxnBaseSizes *result,
									 HostError *error)
{
	doltgres_entries_counted_captures_t *c = captures;

	return txn_fold_recount(io, TREES_STORE_READER_NODE_STORE(&c->store), c->held, c->order, c->keyless, some, result,
							error);
}

/* doltgres_entries_counted_closure_1 is counted's second lent call: the held state's totals counted again. */
static bool
doltgres_entries_counted_closure_1(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   HostError *error)
{
	doltgres_entries_counted_captures_t *c = captures;
	TxnBaseSizes sizes;

	c->store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	c->held = held;
	return txn_state_recounted(io, held, doltgres_entries_counted_closure_1_0, c, NULL, &sizes, error);
}

/* doltgres_entries_counted_frame_t is counted's state across its pad: the keys' order, while live. */
typedef struct doltgres_entries_counted_frame_t
{
	TreesKeyOrder order;
	bool		order_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgRel *rel;
	const PgRel *table;
	bool		index;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_counted_frame_t;

/* doltgres_entries_counted_pad frees what counted holds. */
static void
doltgres_entries_counted_pad(void *frame_)
{
	doltgres_entries_counted_frame_t *frame = frame_;

	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
}

/* doltgres_entries_counted_found returns what held takes for the storage counted counts. */
static bool
doltgres_entries_counted_found(doltgres_entries_counted_frame_t *frame, TxnStateFound *found)
{
	if (frame->index)
		return doltgres_entries_index_making(frame->io, frame->rel, found, frame->error);
	return doltgres_entries_making(frame->caps, frame->io, frame->rel, found, frame->error);
}

/* doltgres_entries_counted_body counts the storage's totals again when they're unknown. */
static void
doltgres_entries_counted_body(void *frame_)
{
	doltgres_entries_counted_frame_t *frame = frame_;
	doltgres_entries_counted_captures_t captures = {.reading = frame->reading};
	TxnStateFound found;
	HostError	failed;
	bool		known;

	if (!doltgres_entries_counted_found(frame, &found))
		return;
	if (!txn_state_held_Bool(frame->io, frame->repo, frame->reading, found, doltgres_entries_counted_closure_0, NULL,
							 NULL, &known, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (known)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_shape(frame->caps, frame->io, frame->repo, frame->reading, frame->table,
								frame->index ? frame->rel : NULL, &frame->order, &captures.keyless, frame->error))
		return;
	frame->order_live = true;
	captures.order = &frame->order;
	if (!doltgres_entries_counted_found(frame, &found))
		return;
	if (!txn_state_held_Unit(frame->io, frame->repo, frame->reading, found, doltgres_entries_counted_closure_1,
							 &captures, NULL, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_counted counts a storage's totals again when they are
 * unknown, before a reader reports them, by its relation, its table's, and
 * whether it is an index's storage, whose table gives its keys' shape.
 */
bool
doltgres_entries_counted(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						 const HostSharedReader *reading, const PgRel *rel, const PgRel *table, bool index,
						 PgPgError *error)
{
	doltgres_entries_counted_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .rel = rel,
	.table = table, .index = index, .error = error};

	std_landing(&frame, doltgres_entries_counted_body, doltgres_entries_counted_pad);
	doltgres_entries_counted_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_handle returns the handle of the owned record a fork's scan reads through. */
HostOwnedHandle
doltgres_entries_handle(const PgScan *scan)
{
	DgOwned		owned = pg_calls_scan_record(scan);
	HostOwnedHandle handle = {owned.slot, owned.generation};

	return handle;
}

/* txn::state::held at (u64, Option<Base>), which scan_begin takes. */
#define TXN_STATE_TS TupleU64OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result_0, bool *some, TxnBaseBase *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS uint64 *result_0; bool *some; TxnBaseBase *result_1;
#define TXN_STATE_RESULT_INIT , result_0, some, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->some, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* doltgres_entries_doltgres_scan_begin_captures_t is scan_begin's lent call's captures: the relation and the read. */
typedef struct doltgres_entries_doltgres_scan_begin_captures_t
{
	const PgRel *relation;
	const HostSnapshot *read;
} doltgres_entries_doltgres_scan_begin_captures_t;

/*
 * doltgres_entries_doltgres_scan_begin_closure_0 is scan_begin's lent call:
 * the storage's block count and the base its read sees, or its newest when
 * it has no snapshot.
 */
static bool
doltgres_entries_doltgres_scan_begin_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
											   const HostSharedHeld *held, uint64 *result_0, bool *some,
											   TxnBaseBase *result_1, HostError *error)
{
	const doltgres_entries_doltgres_scan_begin_captures_t *c = captures;

	if (c->read != NULL ? !txn_state_readable(io, held, c->read, some, result_1, error) :
		!txn_state_newest(io, held, some, result_1, error))
		return false;
	*result_0 = doltgres_entries_blocks(c->relation, held);
	return true;
}

/*
 * doltgres_entries_doltgres_scan_begin_frame_t is scan_begin's state across
 * its pad: the repository, its reader, the base's key, its columns, keyed
 * layout, and order until the scan takes them, the state found until held
 * takes it, a parallel scan's split keys until its share takes them, and the
 * base read, each while live.
 */
typedef struct doltgres_entries_doltgres_scan_begin_frame_t
{
	const PgCatalogCaps *caps;
	const PgRel *relation;
	const PgSnapshot *snapshot;
	int32		nkeys;
	const PgScanKeys *key;
	const PgParallelScan *pscan;
	uint32		flags;
	PgScanDesc *result;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnBaseKey	key_recorded;
	bool		key_recorded_live;
	StdVec		columns;
	bool		columns_live;
	StdVec		keyed;
	bool		keyed_live;
	TreesKeyOrder order;
	bool		order_live;
	TxnStateFound found;
	bool		found_live;
	StdVec		splits;
	bool		splits_live;
	DoltgresScanShared shared;
	TxnBaseBase read_base;
	bool		read_base_live;
} doltgres_entries_doltgres_scan_begin_frame_t;

/* doltgres_entries_doltgres_scan_begin_pad frees what scan_begin holds. */
static void
doltgres_entries_doltgres_scan_begin_pad(void *frame_)
{
	doltgres_entries_doltgres_scan_begin_frame_t *frame = frame_;

	if (frame->read_base_live)
		txn_base_Base_drop(&frame->read_base);
	if (frame->splits_live)
		trees_VecVecU8_drop(&frame->splits);
	if (frame->found_live && frame->found.tag == TXN_STATE_FOUND_MADE)
		txn_base_Key_drop(&frame->found.u.made.f1.key);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->columns_live)
		doltgres_layout_VecKeyed_drop(&frame->columns);
	if (frame->key_recorded_live)
		txn_base_Key_drop(&frame->key_recorded);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->read_base_live = frame->splits_live = frame->found_live = frame->order_live = frame->keyed_live = false;
	frame->columns_live = frame->key_recorded_live = frame->reading_live = frame->repo_live = false;
}

/*
 * doltgres_entries_doltgres_scan_begin_parallel makes a parallel scan's
 * record over the base the leader shared: its trees, its split keys, its
 * epochs, and its count of entries and units.
 */
static bool
doltgres_entries_doltgres_scan_begin_parallel(doltgres_entries_doltgres_scan_begin_frame_t *frame,
											  const RepoRepo *repo, HostOwnedHandle *owned)
{
	PgScan	   *scan = pg_ScanDesc_scan(frame->result);
	DgParallelBase parts = pg_calls_parallel_base(scan);
	uint64		units = pg_calls_parallel_nblocks(scan);
	size_t		splits_len;
	const uint8 *splits = pg_calls_parallel_splits(scan, &splits_len);
	TreesStoreTree chain;
	bool		has_chain;
	TreesStoreReader store;
	HostError	failed;

	frame->shared.has_tree = doltgres_entries_root(&parts.table, &frame->shared.tree);
	frame->shared.has_tuple_ids = doltgres_entries_root(&parts.tuples, &frame->shared.tuple_ids);
	if (!doltgres_scan_split_keys(splits, splits_len, &frame->splits, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->splits_live = true;
	frame->shared.fresh_end = parts.fresh;
	frame->shared.entries = parts.entries;
	frame->shared.folded = parts.folded;
	frame->shared.units = units;
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	has_chain = doltgres_entries_root(&parts.chain, &chain);
	if (!doltgres_scan_Epochs_of(&frame->io, TREES_STORE_READER_NODE_STORE(&store), parts.epoch, has_chain, chain,
								 &frame->shared.epochs, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->shared.splits = frame->splits;
	frame->splits_live = frame->keyed_live = frame->order_live = false;
	if (!doltgres_scan_begin_parallel(&frame->io, &frame->shared, &frame->keyed, pg_calls_rel_id(frame->relation),
									  &frame->order, owned, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/*
 * doltgres_entries_doltgres_scan_begin_serial makes a serial scan's record
 * over the storage's base its snapshot sees, with the storage's block count.
 */
static bool
doltgres_entries_doltgres_scan_begin_serial(doltgres_entries_doltgres_scan_begin_frame_t *frame,
											const RepoRepo *repo, HostOwnedHandle *owned)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostSnapshot read;
	doltgres_entries_doltgres_scan_begin_captures_t c = {frame->relation};
	TreesStoreReader store;
	HostError	failed;
	uint64		blocks;
	bool		has_base;

	if (!doltgres_entries_making(frame->caps, &frame->io, frame->relation, &frame->found, frame->error))
		return false;
	frame->found_live = true;
	if (frame->snapshot != NULL)
	{
		if (!pghost_snapshot(raise, frame->snapshot, &read, frame->error))
			return false;
		c.read = &read;
	}
	frame->found_live = false;
	if (!txn_state_held_TupleU64OptionBase(&frame->io, repo, &frame->reading, frame->found,
										   doltgres_entries_doltgres_scan_begin_closure_0, &c, NULL, &blocks,
										   &has_base, &frame->read_base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->read_base_live = has_base;
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	frame->keyed_live = frame->order_live = false;
	if (!doltgres_scan_scan_begin(&frame->io, TREES_STORE_READER_NODE_STORE(&store), blocks,
								  has_base ? &frame->read_base : NULL, &frame->keyed,
								  pg_calls_rel_id(frame->relation), &frame->order, owned, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_doltgres_scan_begin_body makes the descriptor, then the scan's owned record. */
static void
doltgres_entries_doltgres_scan_begin_body(void *frame_)
{
	doltgres_entries_doltgres_scan_begin_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DgScanKeys	keys = pg_ScanKeys_with_count(frame->key, frame->nkeys);
	const RepoRepo *repo;
	TxnBaseBase base;
	HostOwnedHandle owned;
	DgOwned		recorded;
	HostError	failed;

	if (!pg_calls_scan_new(raise, catalog, frame->relation, frame->snapshot, keys, frame->pscan, frame->flags,
						   frame->result, frame->error))
		return;
	frame->io = pghost_io(raise);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	repo = std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo));
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_recorded(frame->caps, &frame->io, repo, &frame->reading, frame->relation, &base,
								   frame->error))
		return;
	frame->key_recorded = base.key;
	frame->key_recorded_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key_recorded, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	access_slot_keyed(frame->columns.ptr, frame->columns.len, &frame->keyed);
	frame->keyed_live = true;
	frame->columns_live = false;
	if (!doltgres_layout_key_order(raise, catalog, &frame->key_recorded, &frame->columns, &frame->order,
								   frame->error))
		return;
	frame->order_live = true;
	if (frame->pscan != NULL ? !doltgres_entries_doltgres_scan_begin_parallel(frame, repo, &owned) :
		!doltgres_entries_doltgres_scan_begin_serial(frame, repo, &owned))
		return;
	recorded.slot = owned.slot;
	recorded.generation = owned.generation;
	pg_calls_scan_set_record(pg_ScanDesc_scan(frame->result), recorded);
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_scan_begin is the fork's scan_begin: it makes the
 * scan's descriptor as heap's begin does, and its owned record over the
 * storage and its base, which the storage's state the backend's top-level
 * transaction holds names, with the storage's block count, the table's keyed
 * layout, and its tree's order; a parallel scan's record is over the base
 * the leader shared instead.
 */
bool
doltgres_entries_doltgres_scan_begin(const PgCatalogCaps *caps, const PgRel *relation, const PgSnapshot *snapshot,
									 int32 nkeys, const PgScanKeys *key, const PgParallelScan *pscan, uint32 flags,
									 PgScanDesc *result, PgPgError *error)
{
	doltgres_entries_doltgres_scan_begin_frame_t frame = {.caps = caps, .relation = relation, .snapshot = snapshot,
		.nkeys = nkeys, .key = key, .pscan = pscan, .flags = flags, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_doltgres_scan_begin_body, doltgres_entries_doltgres_scan_begin_pad);
	doltgres_entries_doltgres_scan_begin_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_given_frame_t is given's state across its pad: the
 * repository, its reader, and the row read, each while live.
 */
typedef struct doltgres_entries_given_frame_t
{
	const PgCatalogCaps *caps;
	PgScan	   *scan;
	int32		direction;
	PgSlot	   *slot;
	DoltgresEntriesReading reading_kind;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	DoltgresScanRow row;
	bool		row_live;
} doltgres_entries_given_frame_t;

/* doltgres_entries_given_pad frees the row, the reader, and the repository. */
static void
doltgres_entries_given_pad(void *frame_)
{
	doltgres_entries_given_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->row_live = frame->reading_live = frame->repo_live = false;
}

/* doltgres_entries_given_failed is given's refusal of a read it can't make, an internal error. */
static void
doltgres_entries_given_failed(doltgres_entries_given_frame_t *frame, const char *text, size_t len)
{
	StdString	message;

	std_string_from_Checked(&message, text, len);
	*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
}

/* doltgres_entries_given_body reads rows until one the scan's keys match, or none is left. */
static void
doltgres_entries_given_body(void *frame_)
{
	static const char undirected[] = "a scan was asked to read with no direction";
	static const char backward[] = "a parallel scan was asked to read backward";
	doltgres_entries_given_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	const RepoRepo *repo;
	DoltgresScanDirection way;
	HostOwnedHandle owned;
	HostError	failed;
	bool		parallel;
	bool		unchecked;

	if (frame->direction == ForwardScanDirection)
		way = DOLTGRES_SCAN_DIRECTION_FORWARD;
	else if (frame->direction == BackwardScanDirection)
		way = DOLTGRES_SCAN_DIRECTION_BACKWARD;
	else
	{
		doltgres_entries_given_failed(frame, undirected, sizeof(undirected) - 1);
		return;
	}
	owned = doltgres_entries_handle(frame->scan);
	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	repo = std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo));
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_scan_is_parallel(&frame->io, owned, &parallel, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (parallel && way == DOLTGRES_SCAN_DIRECTION_BACKWARD)
	{
		doltgres_entries_given_failed(frame, backward, sizeof(backward) - 1);
		return;
	}
	if (!doltgres_scan_unchecked(&frame->io, owned, &unchecked, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (unchecked && !doltgres_layouts_reading(frame->caps, &frame->io, owned, pg_calls_scan_relation(frame->scan),
											   frame->slot, frame->error))
		return;
	for (;;)
	{
		bool		has;
		bool		matched;

		if (!doltgres_entries_serial(frame->caps, &frame->io, repo, &frame->reading, frame->scan, way,
									 frame->reading_kind, parallel, &has, &frame->row, frame->error))
			return;
		if (!has)
		{
			bool		next;

			if (!parallel)
				break;
			if (!doltgres_entries_next_unit(raise, &frame->io, frame->scan, owned, frame->reading_kind, &next,
											frame->error))
				return;
			if (next)
				continue;
			break;
		}
		frame->row_live = true;
		if (!doltgres_entries_stored(raise, frame->slot, pg_calls_scan_relation(frame->scan), &frame->row,
									 frame->error) ||
			!pg_calls_scan_keys_match(raise, catalog, frame->scan, frame->slot, &matched, frame->error))
			return;
		if (matched)
		{
			if (!pg_calls_scan_counted(raise, frame->scan, frame->error))
				return;
			*frame->result = true;
			frame->ok = true;
			return;
		}
		doltgres_scan_Row_drop(&frame->row);
		frame->row_live = false;
	}
	pg_calls_slot_clear(frame->slot);
	*frame->result = false;
	frame->ok = true;
}

/*
 * doltgres_entries_given stores the scan's next row a reading gives in a
 * direction that the scan's keys match in the slot, with its copy's version,
 * table, and ctid, counts it as heap's does, and reports whether there was
 * one, clearing the slot when there was none. Its Io carries the catalog,
 * since a row's resolve compares through the table's key order. A parallel
 * scan reads one unit at a time, forward, taking the next from the shared
 * scan when one ends.
 */
static bool
doltgres_entries_given(const PgCatalogCaps *caps, PgScan *scan, int32 direction, PgSlot *slot,
					   DoltgresEntriesReading reading, bool *result, PgPgError *error)
{
	doltgres_entries_given_frame_t frame = {.caps = caps, .scan = scan, .direction = direction, .slot = slot,
	.reading_kind = reading, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_given_body, doltgres_entries_given_pad);
	doltgres_entries_given_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_doltgres_scan_getnextslot is the fork's scan_getnextslot:
 * it stores the scan's next row in a direction, as given stores it.
 */
bool
doltgres_entries_doltgres_scan_getnextslot(const PgCatalogCaps *caps, PgScan *scan, int32 direction, PgSlot *slot,
										   bool *result, PgPgError *error)
{
	return doltgres_entries_given(caps, scan, direction, slot, DOLTGRES_ENTRIES_READING_ALL, result, error);
}

/*
 * doltgres_entries_doltgres_scan_getnextslot_tidrange is the fork's
 * scan_getnextslot_tidrange: it stores the scan's next row of its tid range
 * in a direction, as given stores it.
 */
bool
doltgres_entries_doltgres_scan_getnextslot_tidrange(const PgCatalogCaps *caps, PgScan *scan, int32 direction,
													PgSlot *slot, bool *result, PgPgError *error)
{
	return doltgres_entries_given(caps, scan, direction, slot, DOLTGRES_ENTRIES_READING_RANGE, result, error);
}

/*
 * doltgres_entries_doltgres_scan_set_tidrange is the fork's
 * scan_set_tidrange: it sets the tids a scan's tid range reads give, the
 * first and the last, and starts the scan's record over.
 */
bool
doltgres_entries_doltgres_scan_set_tidrange(const PgCaps *caps, PgScan *scan, PgTid *mintid, PgTid *maxtid,
											PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	return doltgres_entries_raised(doltgres_scan_set_range(&io, doltgres_entries_handle(scan),
														   pg_calls_tid_value(mintid), pg_calls_tid_value(maxtid),
														   &failed),
								   &failed, error);
}

/*
 * doltgres_entries_scanning_frame_t is a scan entry's state across its pad:
 * the repository, its reader, a row read, and the rows of a sampled block,
 * each while live.
 */
typedef struct doltgres_entries_scanning_frame_t
{
	const PgCatalogCaps *caps;
	PgScan	   *scan;
	PgSlot	   *slot;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	DoltgresScanRow row;
	bool		row_live;
	StdVec		rows;
	bool		rows_live;
} doltgres_entries_scanning_frame_t;

/*
 * doltgres_entries_scanning_pad frees the rows, the row, the reader, and the
 * repository of a scan entry's frame, or of a frame that begins with one.
 */
static void
doltgres_entries_scanning_pad(void *frame_)
{
	doltgres_entries_scanning_frame_t *frame = frame_;

	if (frame->rows_live)
		std_vec_drop_Checked(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow),
							 doltgres_entries_Row_drop);
	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_Repo_drop);
	frame->rows_live = frame->row_live = frame->reading_live = frame->repo_live = false;
}

/* doltgres_entries_scanning_opened opens the repository and its reader for a scan entry, under the catalog's Io. */
static bool
doltgres_entries_scanning_opened(doltgres_entries_scanning_frame_t *frame)
{
	HostError	failed;

	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return false;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->reading_live = true;
	return true;
}

/* doltgres_entries_scanning_range reads the scan's tid range's next row forward into the frame's row. */
static bool
doltgres_entries_scanning_range(doltgres_entries_scanning_frame_t *frame, bool *has)
{
	if (!doltgres_entries_serial(frame->caps, &frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
								 &frame->reading, frame->scan, DOLTGRES_SCAN_DIRECTION_FORWARD,
								 DOLTGRES_ENTRIES_READING_RANGE, false, has, &frame->row, frame->error))
		return false;
	frame->row_live = *has;
	return true;
}

/* doltgres_entries_bitmap_t is bitmap_next_tuple's counters and its recheck flag. */
typedef struct doltgres_entries_bitmap_t
{
	PgIsNull   *recheck;
	PgCounter  *lossy_pages;
	PgCounter  *exact_pages;
} doltgres_entries_bitmap_t;

/*
 * doltgres_entries_bitmap_paged reads the bitmap's next page into the scan's
 * record, its recheck flag set and counted lossy or exact, answering whether
 * there was one.
 */
static bool
doltgres_entries_bitmap_paged(doltgres_entries_scanning_frame_t *frame, const doltgres_entries_bitmap_t *bitmap,
							  HostOwnedHandle owned, bool *found)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	DgBitmapPage page;
	HostError	failed;
	uint64		blocks;
	size_t		count;

	if (!doltgres_scan_blocks(&frame->io, owned, &blocks, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	if (!pg_calls_bitmap_next_page(raise, frame->scan, blocks > PG_UINT32_MAX ? PG_UINT32_MAX : (uint32) blocks,
								   &page, frame->error))
		return false;
	*found = page.found;
	if (!page.found)
		return true;
	pg_calls_flag_set(bitmap->recheck, page.recheck);
	pg_calls_count_page(page.lossy ? bitmap->lossy_pages : bitmap->exact_pages);
	count = page.count < 0 ? 0 : (size_t) page.count;
	if (!page.lossy)
		std_range_to_check(count, lengthof(page.offsets), RUST_FILE, 2588, 54);
	if (!doltgres_scan_bitmap_page(&frame->io, owned, page.block, !page.lossy, page.offsets, page.lossy ? 0 : count,
								   &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	return true;
}

/* doltgres_entries_doltgres_scan_bitmap_next_tuple_body steps the bitmap until a row its snapshot sees, or its end. */
static void
doltgres_entries_doltgres_scan_bitmap_next_tuple_body(doltgres_entries_scanning_frame_t *frame,
													  const doltgres_entries_bitmap_t *bitmap)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostOwnedHandle owned = doltgres_entries_handle(frame->scan);
	const PgRel *relation;
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_scanning_opened(frame))
		return;
	for (;;)
	{
		DoltgresScanBitmap step;
		bool		has;

		if (!doltgres_scan_bitmap_step(&frame->io, owned, &step, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (step.tag == DOLTGRES_SCAN_BITMAP_TID)
		{
			if (!doltgres_entries_fetched(frame->caps, pg_calls_scan_relation(frame->scan), step.tid,
										  pg_calls_scan_snapshot(frame->scan), &has, &frame->row, frame->error))
				return;
			frame->row_live = has;
		}
		else if (step.tag == DOLTGRES_SCAN_BITMAP_LOSSY)
		{
			if (!doltgres_entries_scanning_range(frame, &has))
				return;
			if (!has && !doltgres_scan_bitmap_ended(&frame->io, owned, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
		}
		else
		{
			bool		found;

			if (!doltgres_entries_bitmap_paged(frame, bitmap, owned, &found))
				return;
			if (!found)
			{
				pg_calls_slot_clear(frame->slot);
				*frame->result = false;
				frame->ok = true;
				return;
			}
			continue;
		}
		if (!has)
			continue;
		relation = pg_calls_scan_relation(frame->scan);
		if (!pg_calls_predicate_lock_tid(raise, relation, frame->row.stamp.tuple, pg_calls_scan_snapshot(frame->scan),
										 (uint32) frame->row.stamp.txn, frame->error) ||
			!doltgres_entries_relocked(frame->caps, relation, pg_calls_scan_snapshot(frame->scan),
									   frame->row.stamp.tuple, frame->error) ||
			!doltgres_entries_stored(raise, frame->slot, relation, &frame->row, frame->error) ||
			!pg_calls_scan_fetched(raise, frame->scan, frame->error))
			return;
		*frame->result = true;
		frame->ok = true;
		return;
	}
}

/* doltgres_entries_bitmap_frame_t is bitmap_next_tuple's frame: a scan entry's, with the bitmap's counters. */
typedef struct doltgres_entries_bitmap_frame_t
{
	doltgres_entries_scanning_frame_t scanning;
	doltgres_entries_bitmap_t bitmap;
} doltgres_entries_bitmap_frame_t;

/* doltgres_entries_bitmap_body runs bitmap_next_tuple's body over its frame. */
static void
doltgres_entries_bitmap_body(void *frame_)
{
	doltgres_entries_bitmap_frame_t *frame = frame_;

	doltgres_entries_doltgres_scan_bitmap_next_tuple_body(&frame->scanning, &frame->bitmap);
}

/*
 * doltgres_entries_doltgres_scan_bitmap_next_tuple is the fork's
 * scan_bitmap_next_tuple: the next row of the scan's TID bitmap's pages its
 * snapshot sees, an exact page's by each tid as fetched fetches it and a
 * lossy page's as a tid range read gives them, each with heap's tuple lock
 * and its fetch counted, reading the bitmap's next page when one ends, with
 * its recheck flag and a count of it as lossy or exact, as heap's does.
 */
bool
doltgres_entries_doltgres_scan_bitmap_next_tuple(const PgCatalogCaps *caps, PgScan *scan, PgSlot *slot,
												 PgIsNull *recheck, PgCounter *lossy_pages, PgCounter *exact_pages,
												 bool *result, PgPgError *error)
{
	doltgres_entries_bitmap_frame_t frame = {{.caps = caps, .scan = scan, .slot = slot, .result = result,
	.error = error}, {recheck, lossy_pages, exact_pages}};

	std_landing(&frame, doltgres_entries_bitmap_body, doltgres_entries_scanning_pad);
	doltgres_entries_scanning_pad(&frame);
	return frame.scanning.ok;
}

/* doltgres_entries_sample_next_block_t is sample_next_block's frame: a scan entry's, with the sampling state. */
typedef struct doltgres_entries_sample_next_block_t
{
	doltgres_entries_scanning_frame_t scanning;
	PgSampleState *scanstate;
} doltgres_entries_sample_next_block_t;

/* doltgres_entries_sample_next_block_body picks the block and reads its rows into the scan's record. */
static void
doltgres_entries_sample_next_block_body(void *frame_)
{
	doltgres_entries_sample_next_block_t *sampling = frame_;
	doltgres_entries_scanning_frame_t *frame = &sampling->scanning;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	HostOwnedHandle owned = doltgres_entries_handle(frame->scan);
	HostError	failed;
	uint64		counted;
	uint32		blocks;
	uint32		next;
	uint32		block = InvalidBlockNumber;
	uint64		first;
	bool		has;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_scan_blocks(&frame->io, owned, &counted, &failed) ||
		!doltgres_scan_sample_next(&frame->io, owned, &next, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	blocks = counted > PG_UINT32_MAX ? PG_UINT32_MAX : (uint32) counted;
	if (blocks != 0 && !pg_calls_sample_block(raise, catalog, sampling->scanstate, blocks, next, &block, frame->error))
		return;
	if (block == InvalidBlockNumber || block >= blocks)
	{
		if (!doltgres_scan_sample_block(&frame->io, owned, false, 0, NULL, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		*frame->result = false;
		frame->ok = true;
		return;
	}
	first = (uint64) block << 16;
	if (!doltgres_scan_set_range(&frame->io, owned, first | 1, first | 0xffff, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!doltgres_entries_scanning_opened(frame))
		return;
	std_vec_new(&frame->rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	frame->rows_live = true;
	for (;;)
	{
		if (!doltgres_entries_scanning_range(frame, &has))
			return;
		if (!has)
			break;
		std_vec_push_Checked(&frame->rows, &frame->row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
		frame->row_live = false;
	}
	frame->rows_live = false;
	if (!doltgres_scan_sample_block(&frame->io, owned, true, block, &frame->rows, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	*frame->result = true;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_scan_sample_next_block is the fork's
 * scan_sample_next_block: the sampling method's next block, or with none the
 * scan's next in order from the first, as heap's, and the block's rows the
 * scan's snapshot sees, read as its tid range; none past the last, or for a
 * storage with no blocks.
 */
bool
doltgres_entries_doltgres_scan_sample_next_block(const PgCatalogCaps *caps, PgScan *scan, PgSampleState *scanstate,
												 bool *result, PgPgError *error)
{
	doltgres_entries_sample_next_block_t frame = {{.caps = caps, .scan = scan, .result = result, .error = error},
	scanstate};

	std_landing(&frame, doltgres_entries_sample_next_block_body, doltgres_entries_scanning_pad);
	doltgres_entries_scanning_pad(&frame);
	return frame.scanning.ok;
}

/*
 * doltgres_entries_sample_next_tuple_body picks the block's next offset until
 * one whose row the scan's snapshot sees, or none.
 */
static void
doltgres_entries_sample_next_tuple_body(void *frame_)
{
	doltgres_entries_sample_next_block_t *sampling = frame_;
	doltgres_entries_scanning_frame_t *frame = &sampling->scanning;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	HostOwnedHandle owned = doltgres_entries_handle(frame->scan);
	HostError	failed;
	bool		has;
	uint32		block;
	uint16		most;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_scan_sample_page(&frame->io, owned, &has, &block, &most, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!has)
	{
		pg_calls_slot_clear(frame->slot);
		*frame->result = false;
		frame->ok = true;
		return;
	}
	for (;;)
	{
		uint32		picked;
		uint16		offset;

		if (!pg_calls_sample_tuple(raise, catalog, sampling->scanstate, block, most, &picked, frame->error))
			return;
		offset = picked > PG_UINT16_MAX ? 0 : (uint16) picked;
		if (offset == 0)
		{
			pg_calls_slot_clear(frame->slot);
			*frame->result = false;
			frame->ok = true;
			return;
		}
		if (!doltgres_scan_sample_take(&frame->io, owned, offset, &has, &frame->row, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (!has)
			continue;
		frame->row_live = true;
		if (!doltgres_entries_stored(raise, frame->slot, pg_calls_scan_relation(frame->scan), &frame->row,
									 frame->error) ||
			!pg_calls_scan_counted(raise, frame->scan, frame->error))
			return;
		*frame->result = true;
		frame->ok = true;
		return;
	}
}

/*
 * doltgres_entries_doltgres_scan_sample_next_tuple is the fork's
 * scan_sample_next_tuple: the row of the block's next offset the sampling
 * method picks that the scan's snapshot sees, counted as heap's, passing over
 * an offset no such row has; none once the method picks no more.
 */
bool
doltgres_entries_doltgres_scan_sample_next_tuple(const PgCatalogCaps *caps, PgScan *scan, PgSampleState *scanstate,
												 PgSlot *slot, bool *result, PgPgError *error)
{
	doltgres_entries_sample_next_block_t frame = {{.caps = caps, .scan = scan, .slot = slot, .result = result,
	.error = error}, scanstate};

	std_landing(&frame, doltgres_entries_sample_next_tuple_body, doltgres_entries_scanning_pad);
	doltgres_entries_scanning_pad(&frame);
	return frame.scanning.ok;
}

/*
 * doltgres_entries_doltgres_scan_analyze_next_block is the fork's
 * scan_analyze_next_block: the next block ANALYZE's block sampler picks,
 * taken from its read stream with no buffer read, read next as the scan's
 * tid range; none at the stream's end.
 */
bool
doltgres_entries_doltgres_scan_analyze_next_block(const PgCaps *caps, PgScan *scan, PgStream *stream, bool *result,
												  PgPgError *error)
{
	HostIoIo	io;
	HostError	failed;
	uint32		block;
	uint64		first;

	if (!pg_calls_stream_next_block(pg_Caps_may_raise(caps), stream, &block, error))
		return false;
	*result = block != InvalidBlockNumber;
	if (!*result)
		return true;
	first = (uint64) block << 16;
	io = pghost_io(pg_Caps_may_raise(caps));
	return doltgres_entries_raised(doltgres_scan_set_range(&io, doltgres_entries_handle(scan), first | 1,
														   first | 0xffff, &failed),
								   &failed, error);
}

/* DoltgresEntriesSampled is Sampled: how ANALYZE counts a row version, as heap's acquire_sample_rows counts a tuple. */
typedef enum DoltgresEntriesSampled
{
	DOLTGRES_ENTRIES_SAMPLED_LIVE,
	DOLTGRES_ENTRIES_SAMPLED_DEAD,
	DOLTGRES_ENTRIES_SAMPLED_PASSED,
} DoltgresEntriesSampled;

/*
 * doltgres_entries_sampled classes a row version for ANALYZE by heap's
 * acquire_sample_rows classes over its writer's outcome and its ender's, the
 * changer its stamp shows when that neither locked the row nor aborted: live
 * when its writer committed and nothing ended it, another transaction is
 * ending it, or the current transaction wrote it and nothing of its own
 * ended it; dead when its writer aborted, a committed transaction ended it,
 * or the current transaction ended it; and passed over when another
 * transaction is writing it.
 */
static bool
doltgres_entries_sampled(HostIoIo *io, const DoltgresScanRow *row, DoltgresEntriesSampled *result, HostError *error)
{
	HostStatus	writer;
	HostStatus	ender = HOST_STATUS_ABORTED;
	bool		has_ender = false;
	HostXid		xid;

	if (!txn_outcome_stamped(io, row->stamp.txn, &writer, error))
		return false;
	if (row->has_changer && !row->changer.locks)
	{
		if (!txn_outcome_stamped(io, row->changer.xid, &ender, error))
			return false;
		has_ender = ender != HOST_STATUS_ABORTED;
	}
	xid.f0 = has_ender ? row->changer.xid : row->stamp.txn;
	if (writer == HOST_STATUS_ABORTED)
		*result = DOLTGRES_ENTRIES_SAMPLED_DEAD;
	else if (writer == HOST_STATUS_IN_PROGRESS)
	{
		HostXid		wrote = {row->stamp.txn};

		if (!host_is_current(HOST_IO_IO_IO_DOORS(io), wrote))
			*result = DOLTGRES_ENTRIES_SAMPLED_PASSED;
		else if (!has_ender)
			*result = DOLTGRES_ENTRIES_SAMPLED_LIVE;
		else
			*result = host_is_current(HOST_IO_IO_IO_DOORS(io), xid) ? DOLTGRES_ENTRIES_SAMPLED_DEAD :
				DOLTGRES_ENTRIES_SAMPLED_LIVE;
	}
	else if (!has_ender)
		*result = DOLTGRES_ENTRIES_SAMPLED_LIVE;
	else if (ender == HOST_STATUS_IN_PROGRESS)
		*result = host_is_current(HOST_IO_IO_IO_DOORS(io), xid) ? DOLTGRES_ENTRIES_SAMPLED_DEAD :
			DOLTGRES_ENTRIES_SAMPLED_LIVE;
	else
		*result = DOLTGRES_ENTRIES_SAMPLED_DEAD;
	return true;
}

/* txn::state::held at u64, which entries_tail.c defines and analyze_next_tuple takes. */
#define TXN_STATE_TS U64
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , uint64 *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* doltgres_entries_spent_t is analyze_next_tuple's lent call's captures: the reader and the block's first tid. */
typedef struct doltgres_entries_spent_t
{
	const HostSharedReader *reading;
	uint64		first;
} doltgres_entries_spent_t;

/* doltgres_entries_doltgres_scan_analyze_next_tuple_closure_0 is analyze_next_tuple's lent call: the block's spent places. */
static bool
doltgres_entries_doltgres_scan_analyze_next_tuple_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															const HostSharedHeld *held, uint64 *result,
															HostError *error)
{
	const doltgres_entries_spent_t *c = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);
	TxnInsertPage page = doltgres_entries_page();

	return txn_vacuum_spent(io, TREES_STORE_READER_NODE_STORE(&store), held, (uint32) (c->first >> 16), &page,
							result, error);
}

/* doltgres_entries_analyze_next_tuple_t is analyze_next_tuple's frame: a scan entry's, with its counts. */
typedef struct doltgres_entries_analyze_next_tuple_t
{
	doltgres_entries_scanning_frame_t scanning;
	float8	   *liverows;
	float8	   *deadrows;
} doltgres_entries_analyze_next_tuple_t;

/* doltgres_entries_analyze_next_tuple_body counts the block's rows until a live one, then its spent places. */
static void
doltgres_entries_analyze_next_tuple_body(void *frame_)
{
	doltgres_entries_analyze_next_tuple_t *analyzing = frame_;
	doltgres_entries_scanning_frame_t *frame = &analyzing->scanning;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	bool		has;
	uint64		first;
	uint64		last;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_scanning_opened(frame))
		return;
	for (;;)
	{
		DoltgresEntriesSampled sampled;

		if (!doltgres_entries_scanning_range(frame, &has))
			return;
		if (!has)
			break;
		if (!doltgres_entries_sampled(&frame->io, &frame->row, &sampled, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		if (sampled == DOLTGRES_ENTRIES_SAMPLED_LIVE)
		{
			if (!doltgres_entries_stored(raise, frame->slot, pg_calls_scan_relation(frame->scan), &frame->row,
										 frame->error))
				return;
			*analyzing->liverows += 1.0;
			*frame->result = true;
			frame->ok = true;
			return;
		}
		if (sampled == DOLTGRES_ENTRIES_SAMPLED_DEAD)
			*analyzing->deadrows += 1.0;
		doltgres_scan_Row_drop(&frame->row);
		frame->row_live = false;
	}
	if (!doltgres_scan_range(&frame->io, doltgres_entries_handle(frame->scan), &has, &first, &last, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (has)
	{
		doltgres_entries_spent_t c = {&frame->reading, first};
		TxnStateFound found;
		uint64		spent;

		if (!doltgres_entries_making(frame->caps, &frame->io, pg_calls_scan_relation(frame->scan), &found,
									 frame->error))
			return;
		if (!txn_state_held_U64(&frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
								found, doltgres_entries_doltgres_scan_analyze_next_tuple_closure_0, &c, NULL, &spent,
								&failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		*analyzing->deadrows += (float8) spent;
	}
	pg_calls_slot_clear(frame->slot);
	*frame->result = false;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_scan_analyze_next_tuple is the fork's
 * scan_analyze_next_tuple: the block's next row heap's acquire_sample_rows
 * samples, counted live, with the rows before it that heap counts dead
 * counted dead, each read under SnapshotAny and classed by sampled; at the
 * block's end its spent places are counted dead too, an aborted insert's
 * tuple ids that VACUUM frees, as heap counts the aborted tuples until VACUUM
 * (bugs row 96).
 */
bool
doltgres_entries_doltgres_scan_analyze_next_tuple(const PgCatalogCaps *caps, PgScan *scan, float8 *liverows,
												  float8 *deadrows, PgSlot *slot, bool *result, PgPgError *error)
{
	doltgres_entries_analyze_next_tuple_t frame = {{.caps = caps, .scan = scan, .slot = slot, .result = result,
	.error = error}, liverows, deadrows};

	std_landing(&frame, doltgres_entries_analyze_next_tuple_body, doltgres_entries_scanning_pad);
	doltgres_entries_scanning_pad(&frame);
	return frame.scanning.ok;
}

/*
 * doltgres_entries_doltgres_scan_rescan is the fork's scan_rescan: it updates
 * the scan's flags and keys as heap's rescan does and starts the scan's
 * record over.
 */
bool
doltgres_entries_doltgres_scan_rescan(const PgCaps *caps, PgScan *scan, const PgScanKeys *key, bool set_params,
									  bool allow_strat, bool allow_sync, bool allow_pagemode, PgPgError *error)
{
	HostIoIo	io;
	HostError	failed;

	if (!pg_calls_scan_restart(pg_Caps_may_raise(caps), scan, key, set_params, allow_strat, allow_sync,
							   allow_pagemode, error))
		return false;
	io = pghost_io(pg_Caps_may_raise(caps));
	return doltgres_entries_raised(doltgres_scan_scan_rescan(&io, doltgres_entries_handle(scan), &failed), &failed,
								   error);
}

/*
 * doltgres_entries_doltgres_scan_end is the fork's scan_end: it ends the
 * scan's record and frees its descriptor as heap's end does.
 */
bool
doltgres_entries_doltgres_scan_end(const PgCaps *caps, PgScan *scan, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (!doltgres_scan_scan_end(&io, doltgres_entries_handle(scan), &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	pg_calls_scan_free(scan);
	return true;
}

/*
 * doltgres_entries_doltgres_tuple_fetch_row_version_frame_t is
 * tuple_fetch_row_version's state across its pad: the row fetched, while
 * live.
 */
typedef struct doltgres_entries_doltgres_tuple_fetch_row_version_frame_t
{
	const PgCatalogCaps *caps;
	const PgRel *relation;
	PgTid	   *tid;
	const PgSnapshot *snapshot;
	PgSlot	   *slot;
	bool	   *result;
	PgPgError  *error;
	bool		ok;
	DoltgresScanRow row;
	bool		row_live;
} doltgres_entries_doltgres_tuple_fetch_row_version_frame_t;

/* doltgres_entries_doltgres_tuple_fetch_row_version_pad frees the row fetched. */
static void
doltgres_entries_doltgres_tuple_fetch_row_version_pad(void *frame_)
{
	doltgres_entries_doltgres_tuple_fetch_row_version_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->row_live = false;
}

/* doltgres_entries_doltgres_tuple_fetch_row_version_body fetches the row, locks it for a serializable read, and stores it. */
static void
doltgres_entries_doltgres_tuple_fetch_row_version_body(void *frame_)
{
	doltgres_entries_doltgres_tuple_fetch_row_version_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostIoIo	io;
	HostError	failed;
	bool		has;

	if (!doltgres_entries_fetched(frame->caps, frame->relation, pg_calls_tid_value(frame->tid), frame->snapshot, &has,
								  &frame->row, frame->error))
		return;
	if (!has)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	frame->row_live = true;
	io = pghost_io(raise);
	if (!host_pause(HOST_SHARED_IO_PAUSES(&io), "doltgres-fetch-read", &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (!pg_calls_predicate_lock_tid(raise, frame->relation, frame->row.stamp.tuple, frame->snapshot,
									 (uint32) frame->row.stamp.txn, frame->error) ||
		!doltgres_entries_relocked(frame->caps, frame->relation, frame->snapshot, frame->row.stamp.tuple,
								   frame->error) ||
		!doltgres_entries_stored(raise, frame->slot, frame->relation, &frame->row, frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/*
 * doltgres_entries_doltgres_tuple_fetch_row_version is the fork's
 * tuple_fetch_row_version: it stores the row version a tid names that a
 * snapshot sees, as fetched reads it, in the slot with its version, table,
 * and ctid, and reports whether there was one.
 */
bool
doltgres_entries_doltgres_tuple_fetch_row_version(const PgCatalogCaps *caps, const PgRel *relation, PgTid *tid,
												  const PgSnapshot *snapshot, PgSlot *slot, bool *result,
												  PgPgError *error)
{
	doltgres_entries_doltgres_tuple_fetch_row_version_frame_t frame = {.caps = caps, .relation = relation,
	.tid = tid, .snapshot = snapshot, .slot = slot, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_doltgres_tuple_fetch_row_version_body,
				doltgres_entries_doltgres_tuple_fetch_row_version_pad);
	doltgres_entries_doltgres_tuple_fetch_row_version_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_serial_captures_t is serial's lent call's captures: the
 * serializable read's check, its conflicts, the snapshot, the reader, the
 * scan's handle and direction, and which rows it reads.
 */
typedef struct doltgres_entries_serial_captures_t
{
	bool		has_check;
	TxnVisibleSerial check;
	StdVec	   *conflicts;
	const HostSnapshot *read;
	const HostSharedReader *reading;
	HostOwnedHandle owned;
	DoltgresScanDirection way;
	DoltgresEntriesReading reading_kind;
	bool		parallel;
} doltgres_entries_serial_captures_t;

/* doltgres_entries_serial_closure_0 is serial's lent call: the scan's next row under the held state. */
static bool
doltgres_entries_serial_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								  bool *some, DoltgresScanRow *result, HostError *error)
{
	doltgres_entries_serial_captures_t *c = captures;
	DoltgresScanUnder under = {.state = host_shared_Held_record(held), .snapshot = c->read,
	.has_serial = c->has_check};
	DoltgresScanStores stores = {&DOLTGRES_SCAN_REPO_STORES_OPS, repo};

	under.serial.read = c->check;
	under.serial.conflicts = c->conflicts;
	if (c->reading_kind == DOLTGRES_ENTRIES_READING_RANGE)
	{
		TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, c->reading);

		return doltgres_scan_next_in_range(io, TREES_STORE_READER_NODE_STORE(&store), &under, c->owned, c->way, some,
										   result, error);
	}
	if (c->parallel)
	{
		DoltgresScanStep step;

		if (!doltgres_scan_next_parallel(io, &stores, &under, c->owned, &step, error))
			return false;
		*some = step.tag == DOLTGRES_SCAN_STEP_ROW;
		if (*some)
			*result = step.row;
		return true;
	}
	return doltgres_scan_next(io, &stores, &under, c->owned, c->way, some, result, error);
}

/*
 * doltgres_entries_serial_frame_t is serial's state across its pad: the read's
 * conflicts and the row read, each while live.
 */
typedef struct doltgres_entries_serial_frame_t
{
	StdVec		conflicts;
	bool		conflicts_live;
	bool		row_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const PgScan *scan;
	DoltgresScanDirection way;
	DoltgresEntriesReading reading_kind;
	bool		parallel;
	bool	   *has_result;
	DoltgresScanRow *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_serial_frame_t;

/* doltgres_entries_serial_pad frees what serial holds. */
static void
doltgres_entries_serial_pad(void *frame_)
{
	doltgres_entries_serial_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(frame->result);
	if (frame->conflicts_live)
		std_vec_free_Checked(&frame->conflicts, sizeof(uint64), _Alignof(uint64));
	frame->row_live = frame->conflicts_live = false;
}

/* doltgres_entries_serial_body reads the scan's next row under its snapshot, then checks its conflicts. */
static void
doltgres_entries_serial_body(void *frame_)
{
	doltgres_entries_serial_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgRel *relation = pg_calls_scan_relation(frame->scan);
	const PgSnapshot *snapshot = pg_calls_scan_snapshot(frame->scan);
	doltgres_entries_serial_captures_t captures = {.owned = doltgres_entries_handle(frame->scan), .way = frame->way,
	.reading_kind = frame->reading_kind, .parallel = frame->parallel, .reading = frame->reading};
	HostSnapshot read;
	TxnStateFound found;
	HostError	failed;

	if (snapshot == NULL)
	{
		StdString text;
		static const char message[] = "a scan was asked to read with no snapshot";

		std_string_from_Checked(&text, message, sizeof(message) - 1);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &text);
		return;
	}
	if (!pghost_snapshot(raise, snapshot, &read, frame->error) ||
		!doltgres_entries_serial_read(raise, relation, snapshot, &captures.has_check, &captures.check, frame->error))
		return;
	std_vec_new(&frame->conflicts, sizeof(uint64), _Alignof(uint64));
	frame->conflicts_live = true;
	captures.conflicts = &frame->conflicts;
	captures.read = &read;
	if (!doltgres_entries_making(frame->caps, frame->io, relation, &found, frame->error))
		return;
	if (!txn_state_held_OptionRow(frame->io, frame->repo, frame->reading, found, doltgres_entries_serial_closure_0,
								  &captures, NULL, frame->has_result, frame->result, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->row_live = *frame->has_result;
	frame->conflicts_live = false;
	if (!doltgres_entries_conflicts_out(raise, relation, snapshot, &frame->conflicts, frame->error))
		return;
	doltgres_entries_found_running(snapshot, &read, *frame->has_result ? frame->result : NULL);
	frame->row_live = false;
	frame->ok = true;
}

/*
 * doltgres_entries_serial reads a serial scan's next row in a direction, or
 * its tid range's, under the scan's snapshot, with its storage's state,
 * which the backend's top-level transaction holds, as found_running leaves
 * it.
 */
bool
doltgres_entries_serial(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *repo,
						const HostSharedReader *reading, const PgScan *scan, DoltgresScanDirection way,
						DoltgresEntriesReading reading_kind, bool parallel, bool *has_result, DoltgresScanRow *result,
						PgPgError *error)
{
	doltgres_entries_serial_frame_t frame = {.caps = caps, .io = io, .repo = repo, .reading = reading, .scan = scan,
		.way = way, .reading_kind = reading_kind, .parallel = parallel, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_serial_body, doltgres_entries_serial_pad);
	doltgres_entries_serial_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_found_running_closure_0 narrows an xid a Dirty read found
 * running to the 32 bits Postgres keeps, 0 for none.
 */
static uint32
doltgres_entries_found_running_closure_0(bool has, HostXid xid)
{
	return has ? (uint32) xid.f0 : 0;
}

/*
 * doltgres_entries_found_running stores what a read under a Dirty snapshot
 * found running of the row it gives (NULL for none) into the snapshot, as
 * HeapTupleSatisfiesDirty does, and leaves any other snapshot as it is.
 */
void
doltgres_entries_found_running(const PgSnapshot *snapshot, const HostSnapshot *read, const DoltgresScanRow *row)
{
	if (row == NULL || read->kind != HOST_SNAPSHOT_KIND_DIRTY)
		return;
	pg_calls_snapshot_set_dirty(snapshot, doltgres_entries_found_running_closure_0(row->dirty.has_xmin, row->dirty.xmin),
								doltgres_entries_found_running_closure_0(row->dirty.has_xmax, row->dirty.xmax),
								row->dirty.has_token ? row->dirty.token : 0);
}

/* doltgres_entries_unsnapshotted returns the error of a row version asked for with no snapshot. */
PgPgError
doltgres_entries_unsnapshotted(void)
{
	StdString text;
	static const char message[] = "a row version was asked for with no snapshot";

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return pg_PgError_fork((const uint8 *) "XX000", &text);
}

/*
 * doltgres_entries_fetched returns the row version with a tuple id of a
 * relation's storage that a snapshot (NULL for none, an error) sees, as rows
 * reads it, as found_running leaves the snapshot.
 */
bool
doltgres_entries_fetched(const PgCatalogCaps *caps, const PgRel *relation, uint64 tuple_id, const PgSnapshot *snapshot,
						 bool *has_result, DoltgresScanRow *result, PgPgError *error)
{
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tuple_id};
	HostSnapshot read;
	StdVec		rows;

	if (snapshot == NULL)
	{
		*error = doltgres_entries_unsnapshotted();
		return false;
	}
	if (!pghost_snapshot(pg_CatalogCaps_may_raise(caps), snapshot, &read, error) ||
		!doltgres_entries_rows(caps, relation, snapshot, &read, by, &rows, error))
		return false;
	*has_result = std_vec_pop(&rows, result, sizeof(DoltgresScanRow));
	std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	doltgres_entries_found_running(snapshot, &read, *has_result ? result : NULL);
	return true;
}

/*
 * doltgres_entries_relocked reads a row's key again after its predicate
 * lock, for a serializable read alone: a writer that pushed a version above
 * the row after the first read checked in before the lock was there, so the
 * second read finds that version and checks its conflict out, where heap
 * holds its buffer lock across the read and the lock. snapshot is NULL for
 * none.
 */
bool
doltgres_entries_relocked(const PgCatalogCaps *caps, const PgRel *relation, const PgSnapshot *snapshot, uint64 tuple_id,
						  PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	DoltgresEntriesBy by = {.tag = DOLTGRES_ENTRIES_BY_TID, .tuple_id = tuple_id};
	TxnVisibleSerial check;
	HostSnapshot read;
	StdVec		rows;
	bool		has;

	if (snapshot == NULL)
		return true;
	if (!doltgres_entries_serial_read(raise, relation, snapshot, &has, &check, error))
		return false;
	if (!has)
		return true;
	if (!pghost_snapshot(raise, snapshot, &read, error) ||
		!doltgres_entries_rows(caps, relation, snapshot, &read, by, &rows, error))
		return false;
	std_vec_drop_Checked(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_entries_Row_drop);
	return true;
}
#endif
