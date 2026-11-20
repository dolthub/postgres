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
 * The row layouts DDL changes (doltgres' layouts.rs): each changed table's
 * transform chained in its statement, a keyless table's rows re-keyed in
 * place before its transaction commits, and a scan whose slot has a layout
 * this transaction's transforms moved its table past reading its rows in
 * that layout.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/heaptuple.h"
#include "doltgres/access/index.h"
#include "doltgres/access/slot.h"
#include "doltgres/access/transform.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/doltgres/layouts.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/doltgres/settle.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/state.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "layouts.rs"

/* The element layouts of layouts' vectors. */
#define DOLTGRES_LAYOUTS_BYTES 1, 1
#define DOLTGRES_LAYOUTS_BEFORE sizeof(DoltgresLayoutsBefore), _Alignof(DoltgresLayoutsBefore)
#define DOLTGRES_LAYOUTS_CHAINED sizeof(doltgres_TupleU32Chained_t), _Alignof(doltgres_TupleU32Chained_t)
#define DOLTGRES_LAYOUTS_REKEY sizeof(doltgres_TupleU32Rekey_t), _Alignof(doltgres_TupleU32Rekey_t)
#define DOLTGRES_LAYOUTS_DONE sizeof(doltgres_TupleU32Storage_t), _Alignof(doltgres_TupleU32Storage_t)
#define DOLTGRES_LAYOUTS_MISSING sizeof(access_OptionVecU8_t), _Alignof(access_OptionVecU8_t)
#define DOLTGRES_LAYOUTS_COLUMN sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn)

/*
 * DoltgresLayoutsBefore is Before: a relation's row layout before a
 * subcommand ran, its object id, its layout, and the tuple length its values
 * move out of line until a row is under, Postgres' toast_tuple_target.
 */
typedef struct DoltgresLayoutsBefore
{
	uint32		relation;
	AccessHeaptupleRowLayout layout;
	uint32		target;
} DoltgresLayoutsBefore;

/*
 * DoltgresLayoutsAltering is Altering: what the running subcommand found
 * before it ran, its command, its table's rewrite flags so far, and the
 * layouts, a Vec<Before>.
 */
typedef struct DoltgresLayoutsAltering
{
	uint32		command;
	int32		rewrite;
	StdVec		before;
} DoltgresLayoutsAltering;

/*
 * doltgres_TupleU64U32U64_t is a (u64, u32, u64): a transform's entry by its
 * top-level xid, command, and subtransaction's xid.
 */
typedef struct doltgres_TupleU64U32U64_t
{
	uint64		f0;
	uint32		f1;
	uint64		f2;
} doltgres_TupleU64U32U64_t;

/*
 * DoltgresLayoutsChained is Chained: a transform this transaction chained,
 * its table's storage, the transform's entry, and the table's layouts before
 * and after it.
 */
typedef struct DoltgresLayoutsChained
{
	HostStorage storage;
	doltgres_TupleU64U32U64_t entry;
	AccessHeaptupleRowLayout before;
	AccessHeaptupleRowLayout after;
} DoltgresLayoutsChained;

/*
 * DoltgresLayoutsRekey is Rekey: a keyless table whose rows this transaction
 * re-keys before it commits, and its storage.
 */
typedef struct DoltgresLayoutsRekey
{
	uint32		relation;
	HostStorage storage;
} DoltgresLayoutsRekey;

/* doltgres_TupleU32Chained_t is a (u32, Chained): a chained transform with the subtransaction that kept it. */
typedef struct doltgres_TupleU32Chained_t
{
	uint32		f0;
	DoltgresLayoutsChained f1;
} doltgres_TupleU32Chained_t;

/* doltgres_TupleU32Rekey_t is a (u32, Rekey): a re-key with the subtransaction that kept it. */
typedef struct doltgres_TupleU32Rekey_t
{
	uint32		f0;
	DoltgresLayoutsRekey f1;
} doltgres_TupleU32Rekey_t;

/* doltgres_TupleU32Storage_t is a (u32, Storage): a re-keyed table and its storage, as rekeyed lists those it did. */
typedef struct doltgres_TupleU32Storage_t
{
	uint32		f0;
	HostStorage f1;
} doltgres_TupleU32Storage_t;

/*
 * DoltgresLayoutsKept is Kept: what this transaction's DDL keeps until it
 * ends, the transforms it chained, oldest first, and the keyless tables it
 * re-keys, each with the subtransaction that kept it.
 */
typedef struct DoltgresLayoutsKept
{
	StdVec		chained;
	StdVec		rekeys;
} DoltgresLayoutsKept;

/* doltgres_layouts_Before_drop is Before's drop glue: its layout. */
static void
doltgres_layouts_Before_drop(void *before)
{
	access_heaptuple_RowLayout_drop(&((DoltgresLayoutsBefore *) before)->layout);
}

/* doltgres_layouts_Altering_Default_default is Altering's derived Default: no command, no flags, and no layouts. */
static void
doltgres_layouts_Altering_Default_default(DoltgresLayoutsAltering *self)
{
	self->command = 0;
	self->rewrite = 0;
	std_vec_new(&self->before, DOLTGRES_LAYOUTS_BEFORE);
}

/* doltgres_layouts_Altering_drop is Altering's drop glue: its layouts. */
static void
doltgres_layouts_Altering_drop(void *altering)
{
	std_vec_drop_Checked(&((DoltgresLayoutsAltering *) altering)->before, DOLTGRES_LAYOUTS_BEFORE,
						 doltgres_layouts_Before_drop);
}

/* doltgres_layouts_Chained_drop is Chained's drop glue: its layouts before and after. */
static void
doltgres_layouts_Chained_drop(DoltgresLayoutsChained *self)
{
	access_heaptuple_RowLayout_drop(&self->before);
	access_heaptuple_RowLayout_drop(&self->after);
}

/* doltgres_layouts_TupleU32Chained_drop is (u32, Chained)'s drop glue, an element of Kept's chained transforms. */
static void
doltgres_layouts_TupleU32Chained_drop(void *chained)
{
	doltgres_layouts_Chained_drop(&((doltgres_TupleU32Chained_t *) chained)->f1);
}

/* doltgres_layouts_Kept_Default_default is Kept's derived Default: nothing chained and nothing to re-key. */
static void
doltgres_layouts_Kept_Default_default(DoltgresLayoutsKept *self)
{
	std_vec_new(&self->chained, DOLTGRES_LAYOUTS_CHAINED);
	std_vec_new(&self->rekeys, DOLTGRES_LAYOUTS_REKEY);
}

/* doltgres_layouts_Kept_drop is Kept's drop glue: its chained transforms, then its re-keys. */
static void
doltgres_layouts_Kept_drop(void *kept)
{
	DoltgresLayoutsKept *self = kept;

	std_vec_drop_Checked(&self->chained, DOLTGRES_LAYOUTS_CHAINED, doltgres_layouts_TupleU32Chained_drop);
	std_vec_free_Checked(&self->rekeys, DOLTGRES_LAYOUTS_REKEY);
}

/* taken at Altering and IoMutex<Altering>, which layouts.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Altering
#define HOST_LOCK_TAKEN_T DoltgresLayoutsAltering
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_layouts_Altering_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_layouts_Altering_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_LAYOUTS_ALTERING_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Altering
#define HOST_LOCK_T DoltgresLayoutsAltering
#define HOST_LOCK_DEFAULT(place) doltgres_layouts_Altering_Default_default(place)
#include "doltgres/host/lock_template.h"

/* taken at Kept and IoMutex<Kept>, which layouts.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Kept
#define HOST_LOCK_TAKEN_T DoltgresLayoutsKept
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_layouts_Kept_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_layouts_Kept_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_LAYOUTS_KEPT_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Kept
#define HOST_LOCK_T DoltgresLayoutsKept
#define HOST_LOCK_DEFAULT(place) doltgres_layouts_Kept_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_layouts_IoMutexAltering_drop is IoMutex<Altering>'s drop glue: its value's. */
static void
doltgres_layouts_IoMutexAltering_drop(void *value)
{
	doltgres_layouts_Altering_drop(&((HostLockIoMutexAltering *) value)->value);
}

/* doltgres_layouts_IoMutexKept_drop is IoMutex<Kept>'s drop glue: its value's. */
static void
doltgres_layouts_IoMutexKept_drop(void *value)
{
	doltgres_layouts_Kept_drop(&((HostLockIoMutexKept *) value)->value);
}

/* DOLTGRES_LAYOUTS_ALTERING_TYPE and DOLTGRES_LAYOUTS_KEPT_TYPE are the two IoMutexes' sizes, Defaults, and drops. */
static const StdDefaultType DOLTGRES_LAYOUTS_ALTERING_TYPE = {sizeof(HostLockIoMutexAltering),
_Alignof(HostLockIoMutexAltering), host_lock_IoMutexAltering_Default_default, doltgres_layouts_IoMutexAltering_drop};
static const StdDefaultType DOLTGRES_LAYOUTS_KEPT_TYPE = {sizeof(HostLockIoMutexKept),
_Alignof(HostLockIoMutexKept), host_lock_IoMutexKept_Default_default, doltgres_layouts_IoMutexKept_drop};

/*
 * DOLTGRES_LAYOUTS_ALTERING is ALTERING: what the running subcommand found
 * before it ran, which a transaction's end clears.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexAltering process;
}			DOLTGRES_LAYOUTS_ALTERING = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DOLTGRES_LAYOUTS_KEPT is KEPT: what this transaction's DDL keeps, which its end clears. */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexKept process;
}			DOLTGRES_LAYOUTS_KEPT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DoltgresLayoutsOp is an operation on a backend record under its lock, with its captures. */
typedef void (*DoltgresLayoutsOp) (void *value, void *captures);

/*
 * doltgres_layouts_locked_frame_t is a locked operation's state across its
 * pad: the backend's record and its guard, each while live.
 */
typedef struct doltgres_layouts_locked_frame_t
{
	HostIoIo   *io;
	HostLocalBackendLocal *backend;
	const StdDefaultType *type;
	const HostLockType *lock_type;
	DoltgresLayoutsOp op;
	void	   *captures;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} doltgres_layouts_locked_frame_t;

/* doltgres_layouts_locked_body takes the backend's record and its lock, then runs the operation. */
static void
doltgres_layouts_locked_body(void *frame_)
{
	doltgres_layouts_locked_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(frame->backend, frame->type);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, frame->type);
	frame->guard = host_lock_IoMutex_lock(mutex, frame->lock_type, &frame->io->held);
	frame->guard_live = true;
	frame->op(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), frame->captures);
}

/* doltgres_layouts_locked_pad releases what a locked operation holds: its guard, then the backend's record. */
static void
doltgres_layouts_locked_pad(void *frame_)
{
	doltgres_layouts_locked_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, frame->type);
	frame->guard_live = frame->local_live = false;
}

/*
 * doltgres_layouts_altering_locked runs an operation on ALTERING under its
 * lock, as ALTERING.get().lock(io) holds it.
 */
static void
doltgres_layouts_altering_locked(HostIoIo *io, DoltgresLayoutsOp op, void *captures)
{
	doltgres_layouts_locked_frame_t frame = {.io = io, .backend = &DOLTGRES_LAYOUTS_ALTERING.local,
		.type = &DOLTGRES_LAYOUTS_ALTERING_TYPE, .lock_type = &DOLTGRES_LAYOUTS_ALTERING_LOCK_TYPE, .op = op,
	.captures = captures};

	std_landing(&frame, doltgres_layouts_locked_body, doltgres_layouts_locked_pad);
	doltgres_layouts_locked_pad(&frame);
}

/* doltgres_layouts_kept_locked runs an operation on KEPT under its lock, as KEPT.get().lock(io) holds it. */
static void
doltgres_layouts_kept_locked(HostIoIo *io, DoltgresLayoutsOp op, void *captures)
{
	doltgres_layouts_locked_frame_t frame = {.io = io, .backend = &DOLTGRES_LAYOUTS_KEPT.local,
		.type = &DOLTGRES_LAYOUTS_KEPT_TYPE, .lock_type = &DOLTGRES_LAYOUTS_KEPT_LOCK_TYPE, .op = op,
	.captures = captures};

	std_landing(&frame, doltgres_layouts_locked_body, doltgres_layouts_locked_pad);
	doltgres_layouts_locked_pad(&frame);
}

/*
 * doltgres_layouts_swap_altering puts an Altering in ALTERING's place, leaving
 * the one it held in the captures' place.
 */
static void
doltgres_layouts_swap_altering(void *value, void *captures)
{
	DoltgresLayoutsAltering held = *(DoltgresLayoutsAltering *) value;

	*(DoltgresLayoutsAltering *) value = *(DoltgresLayoutsAltering *) captures;
	*(DoltgresLayoutsAltering *) captures = held;
}

/* doltgres_layouts_swap_kept puts a Kept in KEPT's place, leaving the one it held in the captures' place. */
static void
doltgres_layouts_swap_kept(void *value, void *captures)
{
	DoltgresLayoutsKept held = *(DoltgresLayoutsKept *) value;

	*(DoltgresLayoutsKept *) value = *(DoltgresLayoutsKept *) captures;
	*(DoltgresLayoutsKept *) captures = held;
}


/*
 * doltgres_layouts_layout_key_frame_t is layout_key's state across its pad:
 * the repository's handle, the reader, and the recorded key, each while
 * live.
 */
typedef struct doltgres_layouts_layout_key_frame_t
{
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnBaseKey	key;
	bool		key_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_layout_key_frame_t;

/* doltgres_layouts_Repo_drop is Repo's drop glue, as an Arc's value takes it. */
static void
doltgres_layouts_Repo_drop(void *repo)
{
	repo_Repo_drop(repo);
}

/* doltgres_layouts_layout_key_pad frees what layout_key holds. */
static void
doltgres_layouts_layout_key_pad(void *frame_)
{
	doltgres_layouts_layout_key_frame_t *frame = frame_;

	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_layouts_Repo_drop);
	frame->key_live = frame->reading_live = frame->repo_live = false;
}

/* doltgres_layouts_layout_key_body reads the key the relation's storage records and the catalog's columns for it. */
static void
doltgres_layouts_layout_key_body(void *frame_)
{
	doltgres_layouts_layout_key_frame_t *frame = frame_;
	TxnBaseBase base;
	HostError	failed;

	if (!doltgres_entries_repository(frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_recorded(frame->caps, frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)),
								   &frame->reading, frame->rel, &base, frame->error))
		return;
	frame->key = base.key;
	frame->key_live = true;
	base.key.tag = TXN_BASE_KEY_NONE;
	txn_base_Base_drop(&base);
	frame->ok = doltgres_layout_key_columns(pg_CatalogCaps_may_raise(frame->caps), pg_CatalogCaps_catalog(frame->caps),
											&frame->key, frame->result, frame->error);
}

/*
 * doltgres_layouts_layout_key returns the columns of the key a relation's
 * storage records: the old key's as its settle pinned them while the
 * statement changes its primary key, since a drop may have taken a key
 * column's type, and otherwise the catalog's for the recorded key.
 */
static bool
doltgres_layouts_layout_key(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, StdVec *result,
							PgPgError *error)
{
	doltgres_layouts_layout_key_frame_t frame = {.caps = caps, .io = io, .rel = rel, .result = result,
	.error = error};

	if (doltgres_settle_held(io, pg_calls_rel_id(rel), result))
		return true;
	std_landing(&frame, doltgres_layouts_layout_key_body, doltgres_layouts_layout_key_pad);
	doltgres_layouts_layout_key_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_layouts_described_frame_t is described's state across its pad:
 * the key's columns and the slot made over the relation, each while live.
 */
typedef struct doltgres_layouts_described_frame_t
{
	StdVec		key;
	bool		key_live;
	PgOwnedSlot slot;
	bool		slot_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	StdVec	   *columns;
	StdVec	   *result_key;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_described_frame_t;

/* doltgres_layouts_described_pad frees what described holds. */
static void
doltgres_layouts_described_pad(void *frame_)
{
	doltgres_layouts_described_frame_t *frame = frame_;

	if (frame->slot_live)
		pg_OwnedSlot_drop(&frame->slot);
	if (frame->key_live)
		doltgres_layout_VecKeyed_drop(&frame->key);
	frame->slot_live = frame->key_live = false;
}

/*
 * doltgres_layouts_described_body reads the key's columns, then each
 * attribute's storage from a slot of the relation.
 */
static void
doltgres_layouts_described_body(void *frame_)
{
	doltgres_layouts_described_frame_t *frame = frame_;

	if (!doltgres_layouts_layout_key(frame->caps, frame->io, frame->rel, &frame->key, frame->error))
		return;
	frame->key_live = true;
	if (!pg_calls_make_slot(pg_CatalogCaps_may_raise(frame->caps), frame->rel, &frame->slot, frame->error))
		return;
	frame->slot_live = true;
	if (!access_slot_columns(pg_OwnedSlot_slot(&frame->slot), frame->columns, frame->error))
		return;
	*frame->result_key = frame->key;
	frame->key_live = false;
	frame->ok = true;
}

/*
 * doltgres_layouts_described returns how a relation's descriptor stores each
 * attribute and the columns of the key its storage records.
 */
static bool
doltgres_layouts_described(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, StdVec *columns, StdVec *key,
						   PgPgError *error)
{
	doltgres_layouts_described_frame_t frame = {.caps = caps, .io = io, .rel = rel, .columns = columns,
	.result_key = key, .error = error};

	std_landing(&frame, doltgres_layouts_described_body, doltgres_layouts_described_pad);
	doltgres_layouts_described_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_layouts_layout returns the layout a relation's rows take now, by
 * its descriptor and the key its storage records, and whether that key has
 * no columns.
 */
static bool
doltgres_layouts_layout(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, AccessHeaptupleRowLayout *result,
						bool *keyless, PgPgError *error)
{
	StdVec		columns;
	StdVec		key;

	if (!doltgres_layouts_described(caps, io, rel, &columns, &key, error))
		return false;
	*keyless = key.len == 0;
	access_heaptuple_RowLayout_new(&columns, &key, result);
	return true;
}

/* doltgres_layouts_alike_closure_0 is alike's missing: no column's value is missing, as none is read. */
static bool
doltgres_layouts_alike_closure_0(void *captures, size_t i, bool *has_result, StdVec *result, PgPgError *error)
{
	*has_result = false;
	return true;
}

/*
 * doltgres_layouts_alike reports whether rows of two layouts are stored alike,
 * whatever their dropped columns' types.
 */
static bool
doltgres_layouts_alike(const AccessHeaptupleRowLayout *a, const AccessHeaptupleRowLayout *b, bool *result,
					   PgPgError *error)
{
	AccessTransformTransform transform;
	bool		has;

	if (!access_heaptuple_RowLayout_transform_to(a, b, doltgres_layouts_alike_closure_0, NULL, 0, 0, &has,
												 &transform, error))
		return false;
	if (has)
		access_transform_Transform_drop(&transform);
	*result = !has;
	return true;
}

/* doltgres_layouts_subtransaction returns the current subtransaction's id. */
static uint32
doltgres_layouts_subtransaction(void)
{
	return (uint32) (pg_calls_transaction_level() & PG_UINT32_MAX);
}

/*
 * doltgres_layouts_without_frame_t is without's state across its pad: the
 * relation's columns and its key's, each while live.
 */
typedef struct doltgres_layouts_without_frame_t
{
	StdVec		columns;
	StdVec		key;
	bool		live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *rel;
	size_t		at;
	AccessHeaptupleRowLayout *result;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_without_frame_t;

/* doltgres_layouts_without_pad frees what without holds. */
static void
doltgres_layouts_without_pad(void *frame_)
{
	doltgres_layouts_without_frame_t *frame = frame_;

	if (frame->live)
	{
		std_vec_free_Checked(&frame->columns, DOLTGRES_LAYOUTS_COLUMN);
		doltgres_layout_VecKeyed_drop(&frame->key);
	}
	frame->live = false;
}

/* doltgres_layouts_without_body marks the attribute dropped in the relation's layout. */
static void
doltgres_layouts_without_body(void *frame_)
{
	doltgres_layouts_without_frame_t *frame = frame_;

	if (!doltgres_layouts_described(frame->caps, frame->io, frame->rel, &frame->columns, &frame->key, frame->error))
		return;
	frame->live = true;
	std_index_check(frame->at, frame->columns.len, RUST_FILE, 307, 5);
	STD_VEC_AT(&frame->columns, AccessHeaptupleColumn, frame->at).dropped = true;
	frame->live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &frame->key, frame->result);
	frame->ok = true;
}

/*
 * doltgres_layouts_without returns the layout a relation's rows take now with
 * one attribute, by its position, dropped, which the catalog doesn't show
 * until the command counter moves.
 */
static bool
doltgres_layouts_without(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *rel, size_t at,
						 AccessHeaptupleRowLayout *result, PgPgError *error)
{
	doltgres_layouts_without_frame_t frame = {.caps = caps, .io = io, .rel = rel, .at = at, .result = result,
	.error = error};

	std_landing(&frame, doltgres_layouts_without_body, doltgres_layouts_without_pad);
	doltgres_layouts_without_pad(&frame);
	return frame.ok;
}

/* doltgres_layouts_target returns the tuple length a relation's values move out of line until a row is under. */
static uint32
doltgres_layouts_target(const PgRel *rel)
{
	int32		target = pg_calls_rel_toast_target(rel);

	return target < 0 ? (uint32) 0 - (uint32) target : (uint32) target;
}

/* doltgres_layouts_Missing_drop is an Option<Vec<u8>>'s drop glue, an element of missing's answer. */
static void
doltgres_layouts_Missing_drop(void *missing)
{
	access_OptionVecU8_t *self = missing;

	if (self->has)
		std_vec_free_Checked(&self->bytes, DOLTGRES_LAYOUTS_BYTES);
}

/*
 * doltgres_layouts_missing_frame_t is missing's state across its pad: the
 * slot filled with the missing values and the values read so far, each
 * while live.
 */
typedef struct doltgres_layouts_missing_frame_t
{
	PgOwnedSlot filled;
	bool		filled_live;
	bool		values_live;
	const PgCatalogCaps *caps;
	const PgRel *rel;
	StdVec	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_missing_frame_t;

/* doltgres_layouts_missing_pad frees what missing holds. */
static void
doltgres_layouts_missing_pad(void *frame_)
{
	doltgres_layouts_missing_frame_t *frame = frame_;

	if (frame->values_live)
		std_vec_drop_Checked(frame->result, DOLTGRES_LAYOUTS_MISSING, doltgres_layouts_Missing_drop);
	if (frame->filled_live)
		pg_OwnedSlot_drop(&frame->filled);
	frame->values_live = frame->filled_live = false;
}

/* doltgres_layouts_missing_body reads each attribute's missing value from a slot that Postgres fills with them. */
static void
doltgres_layouts_missing_body(void *frame_)
{
	doltgres_layouts_missing_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	PgSlot	   *slot;
	int32		natts;

	if (!pg_calls_make_slot(raise, frame->rel, &frame->filled, frame->error))
		return;
	frame->filled_live = true;
	pg_calls_slot_missing(pg_OwnedSlot_slot(&frame->filled));
	slot = pg_OwnedSlot_slot(&frame->filled);
	std_vec_new(frame->result, DOLTGRES_LAYOUTS_MISSING);
	frame->values_live = true;
	natts = pg_calls_slot_natts(slot);
	for (int32 att = 0; att < natts; att++)
	{
		access_OptionVecU8_t value = {0};
		PgAttOption column;

		if (pg_calls_slot_value_null(slot, att))
		{
			std_vec_push_Checked(frame->result, &value, DOLTGRES_LAYOUTS_MISSING);
			continue;
		}
		column = pg_Att_of(pg_calls_slot_att(slot, att));
		if (!column.has)
		{
			StdString text;
			static const char message[] = "a slot's descriptor lost an attribute";

			std_string_from_Checked(&text, message, sizeof(message) - 1);
			*frame->error = pg_PgError_fork((const uint8 *) "XX000", &text);
			return;
		}
		value.has = true;
		if (pg_Att_by_value(column.att))
		{
			uint64		word = pg_calls_slot_value_word(slot, att);
			int16		length = pg_Att_length(column.att);
			size_t		len = (size_t) (length < 0 ? -(int32) length : length);
			uint8		bytes[8];

			for (int k = 0; k < 8; k++)
				bytes[k] = (uint8) (word >> (8 * k));
			std_range_to_check(len, sizeof(bytes), RUST_FILE, 332, 21);
			std_slice_to_vec_Checked(&value.bytes, bytes, len, DOLTGRES_LAYOUTS_BYTES);
		}
		else
		{
			const uint8 *bytes;
			size_t		len;

			if (!pg_calls_slot_value_bytes(raise, pg_CatalogCaps_catalog(frame->caps), slot, att, &bytes, &len,
										   frame->error))
				return;
			std_slice_to_vec_Checked(&value.bytes, bytes, len, DOLTGRES_LAYOUTS_BYTES);
		}
		std_vec_push_Checked(frame->result, &value, DOLTGRES_LAYOUTS_MISSING);
	}
	frame->values_live = false;
	frame->ok = true;
}

/*
 * doltgres_layouts_missing returns, for each attribute of a relation, the
 * stored bytes of the value a row lacking it reads for it, Postgres'
 * attmissingval, or none for NULL: a by-value word's bytes, low byte first,
 * as long as its type, or a stored value's bytes, as a row's forming stores
 * them.
 */
static bool
doltgres_layouts_missing(const PgCatalogCaps *caps, const PgRel *rel, StdVec *result, PgPgError *error)
{
	doltgres_layouts_missing_frame_t frame = {.caps = caps, .rel = rel, .result = result, .error = error};

	std_landing(&frame, doltgres_layouts_missing_body, doltgres_layouts_missing_pad);
	doltgres_layouts_missing_pad(&frame);
	return frame.ok;
}

/* txn::state::held at (), which chained takes. */
#define TXN_STATE_TS Unit
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS
#define TXN_STATE_RESULT_INIT
#define TXN_STATE_RESULT_ARGS(c)
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/*
 * doltgres_layouts_chained_captures_t is chained's lent call's captures: the
 * reader and the pending transform it moves in.
 */
typedef struct doltgres_layouts_chained_captures_t
{
	const HostSharedReader *reading;
	TxnBasePending pending;
} doltgres_layouts_chained_captures_t;

/*
 * doltgres_layouts_chained_closure_0 is chained's lent call: the pending
 * transform recorded in the held state's chain.
 */
static bool
doltgres_layouts_chained_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
								   HostError *error)
{
	doltgres_layouts_chained_captures_t *c = captures;

	return txn_fold_chained(io, repo, c->reading, held, &c->pending, error);
}

/*
 * doltgres_layouts_chained_closure_0_drop is chained's lent call's drop when
 * held never lends it: its pending transform.
 */
static void
doltgres_layouts_chained_closure_0_drop(void *captures)
{
	std_vec_free_Checked(&((doltgres_layouts_chained_captures_t *) captures)->pending.transform,
						 DOLTGRES_LAYOUTS_BYTES);
}

/*
 * doltgres_layouts_chained_frame_t is chained's state across its pad: the
 * pending transform until held takes it, the repository's handle, the
 * reader, and what held takes, each while live.
 */
typedef struct doltgres_layouts_chained_frame_t
{
	doltgres_layouts_chained_captures_t captures;
	bool		pending_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	TxnStateFound made;
	bool		made_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *table;
	uint32		command;
	const AccessTransformTransform *transform;
	doltgres_TupleU64U32U64_t *result;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_chained_frame_t;

/* doltgres_layouts_chained_pad frees what chained holds. */
static void
doltgres_layouts_chained_pad(void *frame_)
{
	doltgres_layouts_chained_frame_t *frame = frame_;

	if (frame->made_live && frame->made.tag == TXN_STATE_FOUND_MADE)
		txn_base_Key_drop(&frame->made.u.made.f1.key);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_layouts_Repo_drop);
	if (frame->pending_live)
		doltgres_layouts_chained_closure_0_drop(&frame->captures);
	frame->made_live = frame->reading_live = frame->repo_live = frame->pending_live = false;
}

/* doltgres_layouts_chained_body records the transform, written by the current subtransaction, in the table's chain. */
static void
doltgres_layouts_chained_body(void *frame_)
{
	doltgres_layouts_chained_frame_t *frame = frame_;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	HostError	failed;
	uint64		top;

	if (!doltgres_entries_current(frame->io, frame->command, &writer, &roomed, frame->error))
		return;
	top = writer.has_top ? writer.top : writer.xid;
	frame->captures.pending.xid = top;
	frame->captures.pending.command = frame->command;
	frame->captures.pending.sub = writer.xid;
	access_transform_Transform_encode(frame->transform, &frame->captures.pending.transform);
	frame->captures.pending.has_end = false;
	frame->pending_live = true;
	if (!doltgres_entries_repository(frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_making(frame->caps, frame->io, frame->table, &frame->made, frame->error))
		return;
	frame->made_live = true;
	frame->captures.reading = &frame->reading;
	frame->made_live = frame->pending_live = false;
	if (!txn_state_held_Unit(frame->io, std_arc_Deref_deref(frame->repo, _Alignof(RepoRepo)), &frame->reading,
							 frame->made, doltgres_layouts_chained_closure_0, &frame->captures,
							 doltgres_layouts_chained_closure_0_drop, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->result->f0 = top;
	frame->result->f1 = frame->command;
	frame->result->f2 = writer.xid;
	frame->ok = true;
}

/*
 * doltgres_layouts_chained records a table's transform in its storage's
 * chain (txn::fold::chained), written by the current subtransaction at a
 * command, and returns its entry by its top-level xid, command, and
 * subtransaction's xid.
 */
static bool
doltgres_layouts_chained(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *table, uint32 command,
						 const AccessTransformTransform *transform, doltgres_TupleU64U32U64_t *result,
						 PgPgError *error)
{
	doltgres_layouts_chained_frame_t frame = {.caps = caps, .io = io, .table = table, .command = command,
	.transform = transform, .result = result, .error = error};

	std_landing(&frame, doltgres_layouts_chained_body, doltgres_layouts_chained_pad);
	doltgres_layouts_chained_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_layouts_changed_push_t is changed's push under KEPT's lock: the
 * transform kept and the table to re-key, if any.
 */
typedef struct doltgres_layouts_changed_push_t
{
	doltgres_TupleU32Chained_t chained;
	bool		keyless;
	doltgres_TupleU32Rekey_t rekey;
} doltgres_layouts_changed_push_t;

/* doltgres_layouts_changed_push keeps a chained transform, and a keyless table for its re-key. */
static void
doltgres_layouts_changed_push(void *value, void *captures)
{
	DoltgresLayoutsKept *kept = value;
	doltgres_layouts_changed_push_t *push = captures;

	std_vec_push_Checked(&kept->chained, &push->chained, DOLTGRES_LAYOUTS_CHAINED);
	if (push->keyless)
		std_vec_push_Checked(&kept->rekeys, &push->rekey, DOLTGRES_LAYOUTS_REKEY);
}

/*
 * doltgres_layouts_changed_frame_t is changed's state across its pad: the
 * transform and the layouts before and after it, each while live.
 */
typedef struct doltgres_layouts_changed_frame_t
{
	AccessTransformTransform transform;
	bool		transform_live;
	AccessHeaptupleRowLayout before;
	AccessHeaptupleRowLayout after;
	bool		layouts_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	const PgRel *table;
	uint32		command;
	bool		keyless;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_changed_frame_t;

/* doltgres_layouts_changed_pad frees what changed holds. */
static void
doltgres_layouts_changed_pad(void *frame_)
{
	doltgres_layouts_changed_frame_t *frame = frame_;

	if (frame->layouts_live)
	{
		access_heaptuple_RowLayout_drop(&frame->after);
		access_heaptuple_RowLayout_drop(&frame->before);
	}
	if (frame->transform_live)
		access_transform_Transform_drop(&frame->transform);
	frame->layouts_live = frame->transform_live = false;
}

/* doltgres_layouts_changed_body chains the transform and keeps it, under KEPT's lock, with the layouts. */
static void
doltgres_layouts_changed_body(void *frame_)
{
	doltgres_layouts_changed_frame_t *frame = frame_;
	doltgres_layouts_changed_push_t push = {.keyless = frame->keyless};
	uint32		subtransaction;
	HostStorage storage;

	if (!doltgres_layouts_chained(frame->caps, frame->io, frame->table, frame->command, &frame->transform,
								  &push.chained.f1.entry, frame->error))
		return;
	subtransaction = doltgres_layouts_subtransaction();
	storage = doltgres_entries_storage(frame->table);
	push.chained.f0 = subtransaction;
	push.chained.f1.storage = storage;
	push.chained.f1.before = frame->before;
	push.chained.f1.after = frame->after;
	push.rekey.f0 = subtransaction;
	push.rekey.f1.relation = pg_calls_rel_id(frame->table);
	push.rekey.f1.storage = storage;
	frame->layouts_live = false;
	doltgres_layouts_kept_locked(frame->io, doltgres_layouts_changed_push, &push);
	frame->ok = true;
}

/*
 * doltgres_layouts_changed chains a table's transform at a command, keeps it
 * with the layouts before and after it, which it takes with the transform,
 * and keeps a keyless table for its re-key.
 */
static bool
doltgres_layouts_changed(const PgCatalogCaps *caps, HostIoIo *io, const PgRel *table, uint32 command,
						 AccessTransformTransform *transform, AccessHeaptupleRowLayout *before,
						 AccessHeaptupleRowLayout *after, bool keyless, PgPgError *error)
{
	doltgres_layouts_changed_frame_t frame = {.transform = *transform, .transform_live = true, .before = *before,
		.after = *after, .layouts_live = true, .caps = caps, .io = io, .table = table, .command = command,
	.keyless = keyless, .error = error};

	std_landing(&frame, doltgres_layouts_changed_body, doltgres_layouts_changed_pad);
	doltgres_layouts_changed_pad(&frame);
	return frame.ok;
}


/*
 * doltgres_layouts_altering_frame_t is altering's state across its pad: the
 * layouts found so far, the inheritor opened, and a layout read, each while
 * live.
 */
typedef struct doltgres_layouts_altering_frame_t
{
	DoltgresLayoutsAltering altering;
	bool		altering_live;
	PgRefRel	opened;
	bool		opened_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	AlterTableType subtype;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_altering_frame_t;

/* doltgres_layouts_altering_pad frees what altering holds. */
static void
doltgres_layouts_altering_pad(void *frame_)
{
	doltgres_layouts_altering_frame_t *frame = frame_;

	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	if (frame->altering_live)
		doltgres_layouts_Altering_drop(&frame->altering);
	frame->opened_live = frame->altering_live = false;
}

/* doltgres_layouts_altering_before keeps a relation's layout before the subcommand runs. */
static bool
doltgres_layouts_altering_before(doltgres_layouts_altering_frame_t *frame, const PgRel *rel)
{
	DoltgresLayoutsBefore before = {.relation = pg_calls_rel_id(rel)};
	bool		keyless;

	if (!doltgres_layouts_layout(frame->caps, &frame->io, rel, &before.layout, &keyless, frame->error))
		return false;
	before.target = doltgres_layouts_target(rel);
	std_vec_push_Checked(&frame->altering.before, &before, DOLTGRES_LAYOUTS_BEFORE);
	return true;
}

/* doltgres_layouts_altering_body finds the layouts before the subcommand runs and keeps them in ALTERING. */
static void
doltgres_layouts_altering_body(void *frame_)
{
	doltgres_layouts_altering_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	bool		stored;

	frame->io = pghost_io_catalog(frame->caps);
	if (!pg_calls_current_command(raise, false, &frame->altering.command, frame->error))
		return;
	std_vec_new(&frame->altering.before, DOLTGRES_LAYOUTS_BEFORE);
	frame->altering_live = true;
	if (!doltgres_relation_stored(frame->caps, frame->rel, &stored, frame->error))
		return;
	if (stored && !doltgres_layouts_altering_before(frame, frame->rel))
		return;
	if (frame->subtype == AT_AddColumn)
	{
		int32		next = 1;

		for (;;)
		{
			int32		at = next;
			uint32		child;
			bool		has;

			next = std_forward_i32_Checked(next, 1);
			if (!pg_calls_rel_inheritor(raise, catalog, pg_calls_rel_id(frame->rel), at, &child, frame->error))
				return;
			if (child == 0)
				break;
			if (!pg_RefRel_opened(raise, catalog, child, &has, &frame->opened, frame->error))
				return;
			if (!has)
				continue;
			frame->opened_live = true;
			if (!doltgres_relation_stored(frame->caps, pg_RefRel_rel(&frame->opened), &stored, frame->error))
				return;
			if (stored && !doltgres_layouts_altering_before(frame, pg_RefRel_rel(&frame->opened)))
				return;
			frame->opened_live = false;
			pg_RefRel_drop(&frame->opened);
		}
	}
	doltgres_layouts_altering_locked(&frame->io, doltgres_layouts_swap_altering, &frame->altering);
	frame->ok = true;
}

/*
 * doltgres_layouts_altering keeps the layouts of a relation the fork stores
 * before a subcommand of a type runs, with the command it runs at and its
 * table's rewrite flags so far, and of each inheritor the fork stores when
 * the subcommand is ADD COLUMN, whose recursion into inheritance children
 * and partitions runs inside the parent's ATExecCmd. A DROP COLUMN's layouts
 * are dropped's.
 */
bool
doltgres_layouts_altering(const PgCatalogCaps *caps, const PgRel *rel, AlterTableType subtype, int32 rewrite,
						  PgPgError *error)
{
	doltgres_layouts_altering_frame_t frame = {.altering = {.rewrite = rewrite}, .caps = caps, .rel = rel,
	.subtype = subtype, .error = error};

	std_landing(&frame, doltgres_layouts_altering_body, doltgres_layouts_altering_pad);
	doltgres_layouts_altering_pad(&frame);
	return frame.ok;
}

/* doltgres_layouts_altered_closure_0 is altered's missing: a column's missing value, taken from the relation's. */
static bool
doltgres_layouts_altered_closure_0(void *captures, size_t i, bool *has_result, StdVec *result, PgPgError *error)
{
	StdVec	   *lacking = captures;

	*has_result = false;
	if (i < lacking->len && STD_VEC_AT(lacking, access_OptionVecU8_t, i).has)
	{
		*has_result = true;
		*result = STD_VEC_AT(lacking, access_OptionVecU8_t, i).bytes;
		STD_VEC_AT(lacking, access_OptionVecU8_t, i).has = false;
	}
	return true;
}

/*
 * doltgres_layouts_altered_frame_t is altered's state across its pad: what
 * the subcommand found, from the next layout on, the layout taken from it,
 * the relation opened, its layout now, and its missing values, each while
 * live.
 */
typedef struct doltgres_layouts_altered_frame_t
{
	DoltgresLayoutsAltering altering;
	size_t		next;
	bool		altering_live;
	AccessHeaptupleRowLayout old;
	bool		old_live;
	PgRefRel	opened;
	bool		opened_live;
	AccessHeaptupleRowLayout new_;
	bool		new_live;
	StdVec		lacking;
	bool		lacking_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	const PgRel *rel;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_altered_frame_t;

/*
 * doltgres_layouts_altered_step frees what one layout's step holds: its
 * missing values, its layouts, and its relation.
 */
static void
doltgres_layouts_altered_step(doltgres_layouts_altered_frame_t *frame)
{
	if (frame->lacking_live)
		std_vec_drop_Checked(&frame->lacking, DOLTGRES_LAYOUTS_MISSING, doltgres_layouts_Missing_drop);
	if (frame->new_live)
		access_heaptuple_RowLayout_drop(&frame->new_);
	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	if (frame->old_live)
		access_heaptuple_RowLayout_drop(&frame->old);
	frame->lacking_live = frame->new_live = frame->opened_live = frame->old_live = false;
}

/* doltgres_layouts_altered_pad frees what altered holds. */
static void
doltgres_layouts_altered_pad(void *frame_)
{
	doltgres_layouts_altered_frame_t *frame = frame_;

	doltgres_layouts_altered_step(frame);
	if (frame->altering_live)
	{
		std_ptr_drop_in_place_slice((DoltgresLayoutsBefore *) frame->altering.before.ptr + frame->next,
									frame->altering.before.len - frame->next, sizeof(DoltgresLayoutsBefore),
									doltgres_layouts_Before_drop);
		std_vec_free_Checked(&frame->altering.before, DOLTGRES_LAYOUTS_BEFORE);
	}
	frame->altering_live = false;
}

/* doltgres_layouts_altered_body chains the transform of each kept relation whose layout the subcommand changed. */
static void
doltgres_layouts_altered_body(void *frame_)
{
	doltgres_layouts_altered_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);

	frame->io = pghost_io_catalog(frame->caps);
	doltgres_layouts_Altering_Default_default(&frame->altering);
	frame->altering_live = true;
	doltgres_layouts_altering_locked(&frame->io, doltgres_layouts_swap_altering, &frame->altering);
	while (frame->next < frame->altering.before.len)
	{
		DoltgresLayoutsBefore *before = &STD_VEC_AT(&frame->altering.before, DoltgresLayoutsBefore, frame->next);
		uint32		relation = before->relation;
		uint32		old_target = before->target;
		const PgRel *table = frame->rel;
		AccessTransformTransform transform;
		bool		has;
		bool		keyless;

		frame->old = before->layout;
		frame->old_live = true;
		frame->next++;
		if (relation == pg_calls_rel_id(frame->rel))
		{
			if (frame->altering.rewrite != 0)
			{
				doltgres_layouts_altered_step(frame);
				continue;
			}
		}
		else
		{
			if (!pg_RefRel_opened(raise, catalog, relation, &has, &frame->opened, frame->error))
				return;
			if (!has)
			{
				doltgres_layouts_altered_step(frame);
				continue;
			}
			frame->opened_live = true;
			table = pg_RefRel_rel(&frame->opened);
		}
		if (!doltgres_layouts_layout(frame->caps, &frame->io, table, &frame->new_, &keyless, frame->error))
			return;
		frame->new_live = true;
		if (!doltgres_layouts_missing(frame->caps, table, &frame->lacking, frame->error))
			return;
		frame->lacking_live = true;
		if (!access_heaptuple_RowLayout_transform_to(&frame->old, &frame->new_, doltgres_layouts_altered_closure_0,
													 &frame->lacking, old_target, doltgres_layouts_target(table),
													 &has, &transform, frame->error))
			return;
		if (has)
		{
			frame->old_live = frame->new_live = false;
			if (!doltgres_layouts_changed(frame->caps, &frame->io, table, frame->altering.command, &transform,
										  &frame->old, &frame->new_, keyless, frame->error))
				return;
		}
		doltgres_layouts_altered_step(frame);
	}
	frame->ok = true;
}

/*
 * doltgres_layouts_altered chains, at the command its subcommand ran at, the
 * transform of each relation altering kept whose layout the subcommand
 * changed, passing over the subcommand's own relation when its rewrite flags
 * say Postgres rewrites it in phase 3, as a type's change does, which flags
 * it before any subcommand runs; a later flag's subcommand changes no layout
 * a transform can't carry (a default's or an expression's rewrite), and the
 * rewrite reads its old layout (reading).
 */
bool
doltgres_layouts_altered(const PgCatalogCaps *caps, const PgRel *rel, PgPgError *error)
{
	doltgres_layouts_altered_frame_t frame = {.caps = caps, .rel = rel, .error = error};

	std_landing(&frame, doltgres_layouts_altered_body, doltgres_layouts_altered_pad);
	doltgres_layouts_altered_pad(&frame);
	return frame.ok;
}

/* doltgres_layouts_dropped_place_t is dropped's place under ALTERING's lock: the relation and the layout it now has. */
typedef struct doltgres_layouts_dropped_place_t
{
	uint32		relation;
	AccessHeaptupleRowLayout *layout;
	bool		placed;
} doltgres_layouts_dropped_place_t;

/* doltgres_layouts_dropped_closure_3 is dropped's find: whether a kept layout is the relation's. */
static bool
doltgres_layouts_dropped_closure_3(const DoltgresLayoutsBefore *before, uint32 relation)
{
	return before->relation == relation;
}

/* doltgres_layouts_dropped_place puts the relation's layout now in the place of the one the running subcommand kept. */
static void
doltgres_layouts_dropped_place(void *value, void *captures)
{
	DoltgresLayoutsAltering *altering = value;
	doltgres_layouts_dropped_place_t *place = captures;

	for (size_t i = 0; i < altering->before.len; i++)
	{
		DoltgresLayoutsBefore *before = &STD_VEC_AT(&altering->before, DoltgresLayoutsBefore, i);

		if (doltgres_layouts_dropped_closure_3(before, place->relation))
		{
			access_heaptuple_RowLayout_drop(&before->layout);
			before->layout = *place->layout;
			place->placed = true;
			return;
		}
	}
}

/* doltgres_layouts_dropped_closure_2 is dropped's missing: a dropped column's value is never read. */
static bool
doltgres_layouts_dropped_closure_2(void *captures, size_t i, bool *has_result, StdVec *result, PgPgError *error)
{
	*has_result = false;
	return true;
}

/*
 * doltgres_layouts_dropped_frame_t is dropped's state across its pad: the
 * table opened, and its layouts before and after the drop, each while live.
 */
typedef struct doltgres_layouts_dropped_frame_t
{
	PgRefRel	opened;
	bool		opened_live;
	AccessHeaptupleRowLayout old;
	bool		old_live;
	AccessHeaptupleRowLayout new_;
	bool		new_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	uint32		relation;
	int32		attnum;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_dropped_frame_t;

/* doltgres_layouts_dropped_pad frees what dropped holds. */
static void
doltgres_layouts_dropped_pad(void *frame_)
{
	doltgres_layouts_dropped_frame_t *frame = frame_;

	if (frame->new_live)
		access_heaptuple_RowLayout_drop(&frame->new_);
	if (frame->old_live)
		access_heaptuple_RowLayout_drop(&frame->old);
	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	frame->new_live = frame->old_live = frame->opened_live = false;
}

/* doltgres_layouts_dropped_body chains the drop's transform and gives the running subcommand the layout after it. */
static void
doltgres_layouts_dropped_body(void *frame_)
{
	doltgres_layouts_dropped_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	doltgres_layouts_dropped_place_t place;
	AccessTransformTransform transform;
	const PgRel *table;
	uint32		command;
	uint32		target;
	int32		natts;
	int32		at;
	bool		has;
	bool		stored;
	bool		keyless;

	if (!pg_RefRel_opened(raise, catalog, frame->relation, &has, &frame->opened, frame->error))
		return;
	if (!has)
	{
		frame->ok = true;
		return;
	}
	frame->opened_live = true;
	table = pg_RefRel_rel(&frame->opened);
	if (!doltgres_relation_stored(frame->caps, table, &stored, frame->error))
		return;
	if (!stored)
	{
		frame->ok = true;
		return;
	}
	frame->io = pghost_io_catalog(frame->caps);
	if (!pg_calls_current_command(raise, false, &command, frame->error))
		return;
	if (!doltgres_layouts_layout(frame->caps, &frame->io, table, &frame->old, &keyless, frame->error))
		return;
	frame->old_live = true;
	at = STD_SUB(i32, frame->attnum, 1, 152, 30);
	natts = pg_calls_rel_natts(table);
	if (at < 0 || (uint32) at >= (natts < 0 ? (uint32) 0 - (uint32) natts : (uint32) natts))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a dropped column's number "), STD_FMT_ARG(0), STD_FMT_LIT(" is out of range")};
		StdFmtArg	args[] = {{&frame->attnum, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString text;

		std_fmt_format_Checked(&text, &arguments);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &text);
		return;
	}
	if (!doltgres_layouts_without(frame->caps, &frame->io, table, (size_t) at, &frame->new_, frame->error))
		return;
	frame->new_live = true;
	target = doltgres_layouts_target(table);
	if (!access_heaptuple_RowLayout_transform_to(&frame->old, &frame->new_, doltgres_layouts_dropped_closure_2, NULL,
												 target, target, &has, &transform, frame->error))
		return;
	if (!has)
	{
		frame->ok = true;
		return;
	}
	frame->old_live = frame->new_live = false;
	if (!doltgres_layouts_changed(frame->caps, &frame->io, table, command, &transform, &frame->old, &frame->new_,
								  keyless, frame->error))
		return;
	if (!doltgres_layouts_without(frame->caps, &frame->io, table, (size_t) at, &frame->new_, frame->error))
		return;
	frame->new_live = true;
	place.relation = frame->relation;
	place.layout = &frame->new_;
	place.placed = false;
	doltgres_layouts_altering_locked(&frame->io, doltgres_layouts_dropped_place, &place);
	frame->new_live = !place.placed;
	frame->ok = true;
}

/*
 * doltgres_layouts_dropped chains the transform of a column's drop from a
 * table the fork stores, in doDeletion right after RemoveAttributeById and
 * before the command counter makes the drop visible: the layout after it is
 * the one before with that attribute dropped. A subcommand running on the
 * table now starts from that layout, so altered finds no change of its own.
 */
bool
doltgres_layouts_dropped(const PgCatalogCaps *caps, uint32 relation, int32 attnum, PgPgError *error)
{
	doltgres_layouts_dropped_frame_t frame = {.caps = caps, .relation = relation, .attnum = attnum, .error = error};

	std_landing(&frame, doltgres_layouts_dropped_body, doltgres_layouts_dropped_pad);
	doltgres_layouts_dropped_pad(&frame);
	return frame.ok;
}


/* doltgres_layouts_same_storage reports whether two storages are one, Storage's derived PartialEq. */
static bool
doltgres_layouts_same_storage(const HostStorage *a, const HostStorage *b)
{
	return a->tablespace == b->tablespace && a->database == b->database && a->relation == b->relation;
}

/* doltgres_layouts_reading_any_t is reading's first look under KEPT's lock: the storage, and whether it chained any. */
typedef struct doltgres_layouts_reading_any_t
{
	const HostStorage *storage;
	bool		any;
} doltgres_layouts_reading_any_t;

/* doltgres_layouts_reading_any finds whether this transaction chained any transform in the storage. */
static void
doltgres_layouts_reading_any(void *value, void *captures)
{
	DoltgresLayoutsKept *kept = value;
	doltgres_layouts_reading_any_t *look = captures;

	for (size_t i = 0; i < kept->chained.len && !look->any; i++)
		look->any = doltgres_layouts_same_storage(&STD_VEC_AT(&kept->chained, doltgres_TupleU32Chained_t, i).f1.storage,
												  look->storage);
}

/*
 * doltgres_layouts_reading_stop_t is reading's second look under KEPT's lock:
 * the storage and the slot's layout, whether the chain ends at that layout,
 * and otherwise the entry of the first transform chained after it.
 */
typedef struct doltgres_layouts_reading_stop_t
{
	const HostStorage *storage;
	const AccessHeaptupleRowLayout *read;
	bool		ends;
	bool		has_stop;
	doltgres_TupleU64U32U64_t stop;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_reading_stop_t;

/* doltgres_layouts_reading_stop finds, newest first, the storage's transform chained after the slot's layout. */
static void
doltgres_layouts_reading_stop(void *value, void *captures)
{
	DoltgresLayoutsKept *kept = value;
	doltgres_layouts_reading_stop_t *look = captures;
	bool		first = true;

	for (size_t i = kept->chained.len; i > 0; i--)
	{
		const DoltgresLayoutsChained *chained = &STD_VEC_AT(&kept->chained, doltgres_TupleU32Chained_t, i - 1).f1;
		bool		alike;

		if (!doltgres_layouts_same_storage(&chained->storage, look->storage))
			continue;
		if (first)
		{
			first = false;
			if (!doltgres_layouts_alike(&chained->after, look->read, &alike, look->error))
				return;
			if (alike)
			{
				look->ends = true;
				look->ok = true;
				return;
			}
		}
		if (!doltgres_layouts_alike(&chained->before, look->read, &alike, look->error))
			return;
		if (alike)
		{
			look->has_stop = true;
			look->stop = chained->entry;
			break;
		}
	}
	look->ok = true;
}

/*
 * doltgres_layouts_reading_frame_t is reading's state across its pad: the
 * slot's columns, then its layout, each while live.
 */
typedef struct doltgres_layouts_reading_frame_t
{
	StdVec		columns;
	bool		columns_live;
	AccessHeaptupleRowLayout read;
	bool		read_live;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	HostOwnedHandle scan;
	const PgRel *relation;
	const PgSlot *slot;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_reading_frame_t;

/* doltgres_layouts_reading_pad frees what reading holds. */
static void
doltgres_layouts_reading_pad(void *frame_)
{
	doltgres_layouts_reading_frame_t *frame = frame_;

	if (frame->read_live)
		access_heaptuple_RowLayout_drop(&frame->read);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, DOLTGRES_LAYOUTS_COLUMN);
	frame->read_live = frame->columns_live = false;
}

/* doltgres_layouts_reading_body stops the scan before the first transform chained after its slot's layout. */
static void
doltgres_layouts_reading_body(void *frame_)
{
	doltgres_layouts_reading_frame_t *frame = frame_;
	HostStorage storage = doltgres_entries_storage(frame->relation);
	doltgres_layouts_reading_any_t any = {.storage = &storage};
	doltgres_layouts_reading_stop_t stop = {.storage = &storage, .read = &frame->read, .error = frame->error};
	StdVec		key;
	HostError	failed;

	doltgres_layouts_kept_locked(frame->io, doltgres_layouts_reading_any, &any);
	if (!any.any)
	{
		frame->ok = true;
		return;
	}
	if (!access_slot_columns(frame->slot, &frame->columns, frame->error))
		return;
	frame->columns_live = true;
	if (!doltgres_layouts_layout_key(frame->caps, frame->io, frame->relation, &key, frame->error))
		return;
	frame->columns_live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &key, &frame->read);
	frame->read_live = true;
	doltgres_layouts_kept_locked(frame->io, doltgres_layouts_reading_stop, &stop);
	if (!stop.ok)
		return;
	if (stop.ends)
	{
		frame->ok = true;
		return;
	}
	if (!stop.has_stop)
	{
		StdString text;
		static const char message[] = "a slot's layout is none its table had in this transaction";

		std_string_from_Checked(&text, message, sizeof(message) - 1);
		*frame->error = pg_PgError_fork((const uint8 *) "XX000", &text);
		return;
	}
	if (!doltgres_scan_end_before(frame->io, frame->scan, stop.stop.f0, stop.stop.f1, stop.stop.f2, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_layouts_reading stops a scan of a relation's storage before the
 * first transform this transaction chained after the layout of the slot it
 * fills, when that isn't the layout the chain ends at: only a rewrite in
 * this transaction's ALTER TABLE fills such a slot, with the table's layout
 * from before the statement, and it reads the columns that statement
 * dropped, as heap's rewrite does from the old tuples. A keyless table's
 * rows are re-keyed only once its transaction commits, so the rows that
 * rewrite reads are still the ones the statement started with.
 */
bool
doltgres_layouts_reading(const PgCatalogCaps *caps, HostIoIo *io, HostOwnedHandle scan, const PgRel *relation,
						 const PgSlot *slot, PgPgError *error)
{
	doltgres_layouts_reading_frame_t frame = {.caps = caps, .io = io, .scan = scan, .relation = relation,
	.slot = slot, .error = error};

	std_landing(&frame, doltgres_layouts_reading_body, doltgres_layouts_reading_pad);
	doltgres_layouts_reading_pad(&frame);
	return frame.ok;
}

/* doltgres_layouts_take_rekeys takes KEPT's re-keys, leaving none, into the captures' place. */
static void
doltgres_layouts_take_rekeys(void *value, void *captures)
{
	DoltgresLayoutsKept *kept = value;

	*(StdVec *) captures = kept->rekeys;
	std_vec_new(&kept->rekeys, DOLTGRES_LAYOUTS_REKEY);
}

/*
 * doltgres_layouts_rekeyed_frame_t is rekeyed's state across its pad: the
 * re-keys taken, the tables done, and the table opened, each while live.
 */
typedef struct doltgres_layouts_rekeyed_frame_t
{
	StdVec		rekeys;
	bool		rekeys_live;
	StdVec		done;
	bool		done_live;
	PgRefRel	opened;
	bool		opened_live;
	const PgCatalogCaps *caps;
	HostIoIo	io;
	PgPgError  *error;
	bool		ok;
} doltgres_layouts_rekeyed_frame_t;

/* doltgres_layouts_rekeyed_pad frees what rekeyed holds. */
static void
doltgres_layouts_rekeyed_pad(void *frame_)
{
	doltgres_layouts_rekeyed_frame_t *frame = frame_;

	if (frame->opened_live)
		pg_RefRel_drop(&frame->opened);
	if (frame->done_live)
		std_vec_free_Checked(&frame->done, DOLTGRES_LAYOUTS_DONE);
	if (frame->rekeys_live)
		std_vec_free_Checked(&frame->rekeys, DOLTGRES_LAYOUTS_REKEY);
	frame->opened_live = frame->done_live = frame->rekeys_live = false;
}

/* doltgres_layouts_rekeyed_done reports whether rekeyed did a table in a storage already. */
static bool
doltgres_layouts_rekeyed_done(const StdVec *done, uint32 relation, const HostStorage *storage)
{
	for (size_t i = 0; i < done->len; i++)
	{
		const doltgres_TupleU32Storage_t *did = &STD_VEC_AT(done, doltgres_TupleU32Storage_t, i);

		if (did->f0 == relation && doltgres_layouts_same_storage(&did->f1, storage))
			return true;
	}
	return false;
}

/* doltgres_layouts_rekeyed_body re-keys each table taken once, when its storage is still the one it changed. */
static void
doltgres_layouts_rekeyed_body(void *frame_)
{
	doltgres_layouts_rekeyed_frame_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);

	frame->io = pghost_io_catalog(frame->caps);
	std_vec_new(&frame->rekeys, DOLTGRES_LAYOUTS_REKEY);
	frame->rekeys_live = true;
	doltgres_layouts_kept_locked(&frame->io, doltgres_layouts_take_rekeys, &frame->rekeys);
	std_vec_new(&frame->done, DOLTGRES_LAYOUTS_DONE);
	frame->done_live = true;
	for (size_t i = 0; i < frame->rekeys.len; i++)
	{
		const DoltgresLayoutsRekey *rekey = &STD_VEC_AT(&frame->rekeys, doltgres_TupleU32Rekey_t, i).f1;
		doltgres_TupleU32Storage_t did = {rekey->relation, rekey->storage};
		HostStorage storage;
		bool		has;

		if (doltgres_layouts_rekeyed_done(&frame->done, rekey->relation, &rekey->storage))
			continue;
		std_vec_push_Checked(&frame->done, &did, DOLTGRES_LAYOUTS_DONE);
		if (!pg_RefRel_opened(raise, catalog, rekey->relation, &has, &frame->opened, frame->error))
			return;
		if (!has)
			continue;
		frame->opened_live = true;
		storage = doltgres_entries_storage(pg_RefRel_rel(&frame->opened));
		if (doltgres_layouts_same_storage(&storage, &rekey->storage) &&
			!pg_calls_rekeyed_rows(raise, catalog, pg_RefRel_rel(&frame->opened), frame->error))
			return;
		frame->opened_live = false;
		pg_RefRel_drop(&frame->opened);
	}
	frame->ok = true;
}

/*
 * doltgres_layouts_rekeyed re-keys the rows of each keyless table this
 * transaction changed the layout of, as an update of each row with its index
 * entries, before the transaction commits or prepares; a table its
 * transaction since dropped or rewrote, whose storage is no longer the one
 * the change was chained in, has nothing to re-key.
 */
bool
doltgres_layouts_rekeyed(const PgCatalogCaps *caps, PgPgError *error)
{
	doltgres_layouts_rekeyed_frame_t frame = {.caps = caps, .error = error};

	std_landing(&frame, doltgres_layouts_rekeyed_body, doltgres_layouts_rekeyed_pad);
	doltgres_layouts_rekeyed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_layouts_sub_aborted_closure_0 is sub_aborted's retain of a chained
 * transform: whether an older subtransaction kept it.
 */
static bool
doltgres_layouts_sub_aborted_closure_0(void *captures, const void *chained)
{
	return ((const doltgres_TupleU32Chained_t *) chained)->f0 < *(const uint32 *) captures;
}

/*
 * doltgres_layouts_sub_aborted_closure_1 is sub_aborted's retain of a re-key:
 * whether an older subtransaction kept it.
 */
static bool
doltgres_layouts_sub_aborted_closure_1(void *captures, const void *rekey)
{
	return ((const doltgres_TupleU32Rekey_t *) rekey)->f0 < *(const uint32 *) captures;
}

/* doltgres_layouts_sub_aborted_retain takes back what the aborted subtransaction and its children kept. */
static void
doltgres_layouts_sub_aborted_retain(void *value, void *captures)
{
	DoltgresLayoutsKept *kept = value;

	std_vec_retain_Checked(&kept->chained, DOLTGRES_LAYOUTS_CHAINED, doltgres_layouts_TupleU32Chained_drop,
						   doltgres_layouts_sub_aborted_closure_0, captures);
	std_vec_retain_Checked(&kept->rekeys, DOLTGRES_LAYOUTS_REKEY, NULL, doltgres_layouts_sub_aborted_closure_1,
						   captures);
}

/*
 * doltgres_layouts_sub_aborted takes back what an aborted subtransaction and
 * its children kept, whose transforms the chain passes over and whose
 * catalog changes are undone.
 */
void
doltgres_layouts_sub_aborted(HostIoIo *io, uint32 subtransaction)
{
	doltgres_layouts_kept_locked(io, doltgres_layouts_sub_aborted_retain, &subtransaction);
}

/* doltgres_layouts_ended clears what a transaction's DDL kept. */
void
doltgres_layouts_ended(HostIoIo *io)
{
	DoltgresLayoutsAltering altering;
	DoltgresLayoutsKept kept;

	doltgres_layouts_Altering_Default_default(&altering);
	doltgres_layouts_altering_locked(io, doltgres_layouts_swap_altering, &altering);
	doltgres_layouts_Altering_drop(&altering);
	doltgres_layouts_Kept_Default_default(&kept);
	doltgres_layouts_kept_locked(io, doltgres_layouts_swap_kept, &kept);
	doltgres_layouts_Kept_drop(&kept);
}

#endif							/* FRONTEND */
