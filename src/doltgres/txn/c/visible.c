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
 * Which delta versions a snapshot sees: heapam_visibility.c's rule for each
 * snapshot type over a version, its writer as xmin and the writer of its
 * key's next newer version as xmax, each writer's outcome from outcome.c,
 * read with no fork lock held.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/visible.h"
#include "port/pg_bswap.h"
#ifdef DOLTGRES_TEST_TXN
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/slice.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/testing.h"
#include "doltgres/txn/state.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "visible.rs"

/* TXN_VISIBLE_VERSION is the size and alignment of a version. */
#define TXN_VISIBLE_VERSION sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion)

/* TxnVisibleWriter is a version's writer as the rules read it: its xid, its top-level one, and its command. */
typedef struct TxnVisibleWriter
{
	uint64		xid;
	uint64		top;
	uint32		command;
} TxnVisibleWriter;

/*
 * TxnVisibleAbove is a version that changes rows older than it: the tuple id
 * of the row it names, or none for a version that sets or locks its key and
 * so changes every older row of the key, whether it ends a row, which a lock
 * and an aborted writer's version don't, and whether it only locks.
 */
typedef struct TxnVisibleAbove
{
	bool		has_names;
	HostRow		names;
	bool		ends;
	bool		lock;
	TxnMemtableVersionVersion *version;
} TxnVisibleAbove;

/* txn_visible_Dirty_Default_default returns a Dirty read's findings before it finds anything running. */
TxnVisibleDirty
txn_visible_Dirty_Default_default(void)
{
	TxnVisibleDirty dirty = {0};

	return dirty;
}

/* txn_visible_Dirty_PartialEq_eq reports whether two Dirty findings are equal field by field. */
bool
txn_visible_Dirty_PartialEq_eq(const TxnVisibleDirty *self, const TxnVisibleDirty *other)
{
	return self->has_xmin == other->has_xmin && (!self->has_xmin || host_Xid_PartialEq_eq(&self->xmin, &other->xmin)) &&
		self->has_xmax == other->has_xmax && (!self->has_xmax || host_Xid_PartialEq_eq(&self->xmax, &other->xmax)) &&
		self->has_token == other->has_token && (!self->has_token || self->token == other->token);
}

/* txn_visible_Dirty_Debug_fmt is Dirty's derived Debug. */
bool
txn_visible_Dirty_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleDirty *self = value;
	StdOptionRef xmin = {self->has_xmin, {&self->xmin, host_Xid_Debug_fmt}};
	StdOptionRef xmax = {self->has_xmax, {&self->xmax, host_Xid_Debug_fmt}};
	StdOptionRef token = {self->has_token, {&self->token, std_u32_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Dirty", 5);

	std_DebugStruct_field(&s, "xmin", 4, &xmin, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "xmax", 4, &xmax, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "token", 5, &token, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_visible_contains reports whether a snapshot's array of narrowed xids holds one. */
static bool
txn_visible_contains(const uint32 *xids, size_t xids_len, uint32 xid)
{
	for (size_t i = 0; i < xids_len; i++)
		if (xids[i] == xid)
			return true;
	return false;
}

/*
 * txn_visible_in_snapshot reports whether a snapshot counts an xid as
 * running, XidInMVCCSnapshot's rule in snapmgr.c's order: below xmin not
 * running, at or past xmax running; else, outside recovery, a snapshot that
 * didn't overflow searches subxip and then xip, and one that overflowed
 * searches xip for the xid's top-level one unless that is below xmin; in
 * recovery, subxip holds every running xid, an overflowed snapshot's searched
 * for the top.
 */
bool
txn_visible_in_snapshot(const HostSnapshot *snapshot, uint64 xid, uint64 top)
{
	uint64		searched = xid;

	if (xid < snapshot->xmin.f0)
		return false;
	if (xid >= snapshot->xmax.f0)
		return true;
	if (snapshot->overflowed)
	{
		if (top < snapshot->xmin.f0)
			return false;
		searched = top;
	}
	if (snapshot->recovery)
		return txn_visible_contains(snapshot->subxip, snapshot->subxip_len, (uint32) searched);
	if (snapshot->overflowed)
		return txn_visible_contains(snapshot->xip, snapshot->xip_len, (uint32) searched);
	return txn_visible_contains(snapshot->subxip, snapshot->subxip_len, (uint32) xid) ||
		txn_visible_contains(snapshot->xip, snapshot->xip_len, (uint32) xid);
}

/* txn_visible_RowVersion_of returns a version as the row it writes, or false for a version that writes none. */
bool
txn_visible_RowVersion_of(TxnMemtableVersionVersion *version, TxnVisibleRowVersion *result)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	result->version = version;
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			result->put = op.u.put;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			result->put = op.u.add;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			result->put = op.u.add_row;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			result->put = op.u.update_row.f1;
			return true;
		default:
			return false;
	}
}

/* txn_visible_writer returns a version's writer. */
static TxnVisibleWriter
txn_visible_writer(TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
	TxnVisibleWriter result = {.xid = writer.xid, .top = writer.has_top ? writer.top : writer.xid,
	.command = writer.command};

	return result;
}

/*
 * txn_visible_names classifies what a version does to older rows: the row
 * it names, if any, and whether it only locks, or false for an add, which
 * changes none.
 */
static bool
txn_visible_names(TxnMemtableVersionVersion *version, bool *has_names, HostRow *names, bool *lock)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

	*has_names = false;
	*lock = false;
	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK:
			*lock = true;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			*has_names = true;
			*names = op.u.remove.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			*has_names = true;
			*names = op.u.update_row.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			*has_names = true;
			*names = op.u.delete_row.f0;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
			*has_names = true;
			*names = op.u.lock_row.f0;
			*lock = true;
			return true;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			return false;
	}
	pg_unreachable();
}

/* txn_visible_Seen_changes reports whether a version found so far changes the row with a tuple id. */
static bool
txn_visible_Seen_changes(const TxnVisibleAbove *above, HostRow tuple)
{
	return !above->has_names || host_Row_PartialEq_eq(&above->names, &tuple);
}

/*
 * txn_visible_Seen_ender returns the version that ended the row with a tuple
 * id: the nearest version newer than it that sets its key's value or names
 * it, isn't a lock, and whose writer didn't abort, or NULL for none.
 */
static TxnMemtableVersionVersion *
txn_visible_Seen_ender(const TxnVisibleSeen *self, HostRow tuple)
{
	for (size_t i = self->above.len; i > 0; i--)
	{
		const TxnVisibleAbove *above = &STD_VEC_AT(&self->above, TxnVisibleAbove, i - 1);

		if (txn_visible_Seen_changes(above, tuple) && above->ends)
			return above->version;
	}
	return NULL;
}

/*
 * txn_visible_Seen_changer returns the version whose writer the stamp of the
 * row with a tuple id shows as its xmax: the row's ender when it has one,
 * and otherwise the newest version newer than the row that sets or locks its
 * key or names it.
 */
static TxnMemtableVersionVersion *
txn_visible_Seen_changer(const TxnVisibleSeen *self, HostRow tuple)
{
	TxnMemtableVersionVersion *ender = txn_visible_Seen_ender(self, tuple);

	if (ender != NULL)
		return ender;
	for (size_t i = 0; i < self->above.len; i++)
	{
		const TxnVisibleAbove *above = &STD_VEC_AT(&self->above, TxnVisibleAbove, i);

		if (txn_visible_Seen_changes(above, tuple))
			return above->version;
	}
	return NULL;
}

/*
 * txn_visible_Seen_commander returns the version whose raw command the stamp
 * of the row with a tuple id shows as its cmin and cmax: the row's ender when
 * it has one, and otherwise the newest version newer than the row that
 * changes it and isn't a lock.
 */
static TxnMemtableVersionVersion *
txn_visible_Seen_commander(const TxnVisibleSeen *self, HostRow tuple)
{
	TxnMemtableVersionVersion *ender = txn_visible_Seen_ender(self, tuple);

	if (ender != NULL)
		return ender;
	for (size_t i = 0; i < self->above.len; i++)
	{
		const TxnVisibleAbove *above = &STD_VEC_AT(&self->above, TxnVisibleAbove, i);

		if (txn_visible_Seen_changes(above, tuple) && !above->lock)
			return above->version;
	}
	return NULL;
}

/*
 * txn_visible_Seen_base reports whether the snapshot sees the row with a
 * tuple id that the key holds in the base: it is seen unless its ender among
 * the key's versions ended it for the snapshot. It also returns the ender a
 * Dirty read found running, and the row's changer and commander.
 */
bool
txn_visible_Seen_base(const TxnVisibleSeen *self, HostIoIo *io, TxnVisibleRule rule, HostRow tuple,
					  bool *result_0, TxnVisibleDirty *result_1, TxnMemtableVersionVersion **result_2_0,
					  TxnMemtableVersionVersion **result_2_1, HostError *error)
{
	TxnMemtableVersionVersion *ender = txn_visible_Seen_ender(self, tuple);
	bool		ended;

	*result_1 = txn_visible_Dirty_Default_default();
	*result_0 = true;
	if (ender != NULL)
	{
		if (!rule.ops->ended(rule.self, io, ender, result_1, &ended, error))
			return false;
		*result_0 = !ended;
	}
	*result_2_0 = txn_visible_Seen_changer(self, tuple);
	*result_2_1 = txn_visible_Seen_commander(self, tuple);
	return true;
}

/*
 * txn_visible_conflict returns the writer a serializable read conflicts out
 * with over a row version it examined, by
 * HeapCheckForSerializableConflictOut's cases over HeapTupleSatisfiesVacuum's
 * answer against the read's xmin, its writer's xid, top-level xid, and
 * outcome given; a writer's top-level transaction stands for it, and the
 * read's own conflicts with nothing.
 */
static bool
txn_visible_conflict(HostIoIo *io, const TxnVisibleSerial *serial, uint64 xmin, uint64 xmin_top, HostStatus wrote,
					 TxnMemtableVersionVersion *ender, bool visible, bool *has_result, HostXid *result,
					 HostError *error)
{
	TxnVisibleWriter xmax = {0};
	HostStatus	ended = HOST_STATUS_ABORTED;
	uint64		xid = xmin;
	uint64		top = xmin_top;
	HostXid		conflicted;

	*has_result = false;
	if (ender != NULL)
	{
		xmax = txn_visible_writer(ender);
		if (!txn_outcome_outcome(io, ender, &ended, error))
			return false;
	}
	switch (wrote)
	{
		case HOST_STATUS_ABORTED:
			return true;
		case HOST_STATUS_IN_PROGRESS:
			if (ender != NULL && visible)
			{
				xid = xmax.xid;
				top = xmax.top;
			}
			break;
		case HOST_STATUS_COMMITTED:
			if (ender == NULL || ended == HOST_STATUS_ABORTED)
			{
				if (visible)
					return true;
			}
			else if (ended == HOST_STATUS_COMMITTED && xmax.xid < serial->xmin.f0)
				return true;
			else if (visible)
			{
				xid = xmax.xid;
				top = xmax.top;
			}
			break;
	}
	conflicted.f0 = top;
	if (xid < serial->xmin.f0 || top < serial->xmin.f0 ||
		(serial->has_own && host_Xid_PartialEq_eq(&serial->own, &conflicted)))
		return true;
	*has_result = true;
	result->f0 = top;
	return true;
}

/*
 * txn_visible_Seen_base_conflict returns the writer a serializable read
 * conflicts out with over the key's row with a tuple id in the base, written
 * by a stamp's xid, whether the read sees it or not, as conflict finds it
 * with the row's ender.
 */
bool
txn_visible_Seen_base_conflict(const TxnVisibleSeen *self, HostIoIo *io, const TxnVisibleSerial *serial,
							   HostRow tuple, uint64 writer, bool visible, bool *has_result, HostXid *result,
							   HostError *error)
{
	HostStatus	wrote;

	if (!txn_outcome_stamped(io, writer, &wrote, error))
		return false;
	return txn_visible_conflict(io, serial, writer, writer, wrote, txn_visible_Seen_ender(self, tuple), visible,
								has_result, result, error);
}

/* txn_visible_Seen_drop frees what a snapshot saw of a key. */
void
txn_visible_Seen_drop(TxnVisibleSeen *self)
{
	std_vec_free_Checked(&self->rows, sizeof(TxnVisibleSeenRow), _Alignof(TxnVisibleSeenRow));
	std_vec_free_Checked(&self->above, sizeof(TxnVisibleAbove), _Alignof(TxnVisibleAbove));
	std_vec_free_Checked(&self->conflicts, sizeof(txn_TupleRowXid_t), _Alignof(txn_TupleRowXid_t));
}

/*
 * txn_visible_vacuumed answers HeapTupleSatisfiesVacuum for a row version
 * against a removable horizon, from its writer's outcome and its ender, as
 * heap reads xmin and the raw xmax. A running writer's row is being deleted
 * only when the caller's own transaction ended it, and is being inserted
 * otherwise.
 */
bool
txn_visible_vacuumed(HostIoIo *io, HostStatus wrote, TxnMemtableVersionVersion *ender, uint64 horizon,
					 TxnVisibleVacuumed *result, HostError *error)
{
	uint64		xmax = 0;
	HostStatus	ended = HOST_STATUS_ABORTED;

	if (ender != NULL)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(ender);
		HostXid		asked;

		xmax = txn_memtable_version_Record_writer(&record).xid;
		if (!txn_outcome_outcome(io, ender, &ended, error))
			return false;
		asked.f0 = xmax;
		if (wrote == HOST_STATUS_IN_PROGRESS && ended != HOST_STATUS_ABORTED &&
			host_is_current(HOST_IO_IO_IO_DOORS(io), asked))
		{
			*result = TXN_VISIBLE_VACUUMED_DELETE_IN_PROGRESS;
			return true;
		}
	}
	switch (wrote)
	{
		case HOST_STATUS_ABORTED:
			*result = TXN_VISIBLE_VACUUMED_DEAD;
			break;
		case HOST_STATUS_IN_PROGRESS:
			*result = TXN_VISIBLE_VACUUMED_INSERT_IN_PROGRESS;
			break;
		case HOST_STATUS_COMMITTED:
			if (ender == NULL || ended == HOST_STATUS_ABORTED)
				*result = TXN_VISIBLE_VACUUMED_LIVE;
			else if (ended == HOST_STATUS_IN_PROGRESS)
				*result = TXN_VISIBLE_VACUUMED_DELETE_IN_PROGRESS;
			else if (xmax < horizon)
				*result = TXN_VISIBLE_VACUUMED_DEAD;
			else
				*result = TXN_VISIBLE_VACUUMED_RECENTLY_DEAD;
			break;
	}
	return true;
}

/*
 * txn_visible_unfolded_frame_t is unfolded's state across its pad: the
 * versions it takes, those from at on still its own, and the versions it
 * keeps, each while live.
 */
typedef struct txn_visible_unfolded_frame_t
{
	HostIoIo   *io;
	StdVec		versions;
	size_t		at;
	bool		versions_live;
	HostLsn		folded;
	StdVec		kept;
	bool		kept_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_visible_unfolded_frame_t;

/* txn_visible_unfolded_body keeps each version a base folded at the point didn't apply, dropping the others. */
static void
txn_visible_unfolded_body(void *frame_)
{
	txn_visible_unfolded_frame_t *frame = frame_;

	std_vec_new(&frame->kept, TXN_VISIBLE_VERSION);
	frame->kept_live = true;
	while (frame->at < frame->versions.len)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, frame->at);
		TxnMemtableVersionVersion dropped;
		bool		in;

		if (!txn_visible_in_base(frame->io, version, frame->folded, &in, frame->error))
			return;
		if (in)
		{
			dropped = *version;
			frame->at++;
			txn_memtable_version_Version_drop(&dropped);
			continue;
		}
		if (!std_vec_try_reserve(&frame->kept, 1, TXN_VISIBLE_VERSION))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		std_vec_push_Checked(&frame->kept, version, TXN_VISIBLE_VERSION);
		frame->at++;
	}
	*frame->result = frame->kept;
	frame->kept_live = false;
	frame->ok = true;
}

/* txn_visible_unfolded_pad frees what unfolded holds live: the versions it took and those it kept. */
static void
txn_visible_unfolded_pad(void *frame_)
{
	txn_visible_unfolded_frame_t *frame = frame_;

	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, frame->at);
	if (frame->kept_live)
		txn_memtable_version_versions_drop(&frame->kept, 0);
	frame->versions_live = frame->kept_live = false;
}

/* txn_visible_unfolded returns a key's versions, taking them, less each one a base folded at a point applied. */
bool
txn_visible_unfolded(HostIoIo *io, StdVec *versions, HostLsn folded, StdVec *result, HostError *error)
{
	txn_visible_unfolded_frame_t frame = {.io = io, .versions = *versions, .versions_live = true, .folded = folded,
	.result = result, .error = error};

	std_landing(&frame, txn_visible_unfolded_body, txn_visible_unfolded_pad);
	txn_visible_unfolded_pad(&frame);
	return frame.ok;
}

/*
 * txn_visible_versions lends a key's versions in a storage's rows, newest
 * first, as table's versions does, less each one a base folded at a point
 * applied, so a walk of them beside that base never meets a change twice.
 */
bool
txn_visible_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const uint8 *key,
					 size_t key_len, HostLsn folded, StdVec *result, HostError *error)
{
	StdVec		versions;

	if (!txn_memtable_table_versions(io, hold, state, key, key_len, &versions, error))
		return false;
	return txn_visible_unfolded(io, &versions, folded, result, error);
}

/* txn_visible_next_frame_t is next's state across its pad: the key it found, while live. */
typedef struct txn_visible_next_frame_t
{
	HostIoIo   *io;
	txn_TupleVecU8VecVersion_t found;
	bool		key_live;
	HostLsn		folded;
	txn_TupleVecU8VecVersion_t *result;
	HostError  *error;
	bool		ok;
} txn_visible_next_frame_t;

/* txn_visible_next_body drops the found key's versions a base folded at the point applied. */
static void
txn_visible_next_body(void *frame_)
{
	txn_visible_next_frame_t *frame = frame_;

	if (!txn_visible_unfolded(frame->io, &frame->found.f1, frame->folded, &frame->result->f1, frame->error))
		return;
	frame->result->f0 = frame->found.f0;
	frame->key_live = false;
	frame->ok = true;
}

/* txn_visible_next_pad frees the key next found, while live. */
static void
txn_visible_next_pad(void *frame_)
{
	txn_visible_next_frame_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->found.f0, 1, 1);
	frame->key_live = false;
}

/*
 * txn_visible_next lends the first key of a storage's rows past a place, the
 * way a scan goes, that has a version, as table's next does, with its
 * versions less each one a base folded at a point applied.
 */
bool
txn_visible_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
				 const uint8 *after, size_t after_len, TxnMemtableTableDirection way, HostLsn folded,
				 bool *has_result, txn_TupleVecU8VecVersion_t *result, HostError *error)
{
	txn_visible_next_frame_t frame = {.io = io, .folded = folded, .result = result, .error = error};

	if (!txn_memtable_table_next(io, hold, state, has_after, after, after_len, way, has_result, &frame.found, error))
		return false;
	if (!*has_result)
		return true;
	frame.key_live = true;
	std_landing(&frame, txn_visible_next_body, txn_visible_next_pad);
	txn_visible_next_pad(&frame);
	return frame.ok;
}

/*
 * txn_visible_in_base reports whether a base folded at a point applied a
 * version, by the predicate the fold chose it with; a base never folded
 * applied none.
 */
bool
txn_visible_in_base(HostIoIo *io, TxnMemtableVersionVersion *version, HostLsn folded, bool *result,
					HostError *error)
{
	TxnFoldApplied applied;

	*result = false;
	if (folded == 0)
		return true;
	if (!txn_fold_applied(io, version, folded, &applied, error))
		return false;
	*result = applied.tag == TXN_FOLD_APPLIED_YES;
	return true;
}

/* txn_visible_missing returns the error of a committed transaction with no commit status entry. */
static HostError
txn_visible_missing(uint64 xid)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("committed transaction "), STD_FMT_ARG(0),
	STD_FMT_LIT(" has no commit status entry")};
	StdFmtArg	args[] = {{&xid, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/*
 * txn_visible_live_frame_t is live's state across its pad: the chain it
 * takes, those from at on still its own, and the live transforms it makes,
 * each while live.
 */
typedef struct txn_visible_live_frame_t
{
	HostIoIo   *io;
	StdVec		chain;
	size_t		at;
	bool		chain_live;
	StdVec		live;
	bool		live_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_visible_live_frame_t;

/* txn_visible_live_body moves each transform whose DDL didn't abort into the live ones, with its commit's end. */
static void
txn_visible_live_body(void *frame_)
{
	txn_visible_live_frame_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->live, frame->chain.len, sizeof(TxnVisibleLive), _Alignof(TxnVisibleLive));
	frame->live_live = true;
	while (frame->at < frame->chain.len)
	{
		TxnBasePending *pending = &STD_VEC_AT(&frame->chain, TxnBasePending, frame->at);
		TxnVisibleLive live = {.f0 = *pending};
		HostXid		sub = {pending->sub};
		HostXid		xid = {pending->xid};
		HostStatus	status;
		TxnStatusCommit commit;

		if (pending->has_end)
		{
			live.has_f1 = true;
			live.f1 = pending->end;
		}
		else
		{
			if (!txn_outcome_entry(frame->io, sub, &status, frame->error))
				return;
			if (status == HOST_STATUS_ABORTED)
			{
				frame->at++;
				txn_base_Pending_drop(&live.f0);
				continue;
			}
			if (status == HOST_STATUS_COMMITTED)
			{
				if (!txn_status_committed(frame->io, xid, &commit, frame->error))
					return;
				if (commit.tag == TXN_STATUS_COMMIT_MISSING)
				{
					*frame->error = txn_visible_missing(pending->xid);
					return;
				}
				live.has_f1 = commit.tag == TXN_STATUS_COMMIT_AT;
				if (live.has_f1)
					live.f1 = commit.u.at;
			}
		}
		std_vec_push_Checked(&frame->live, &live, sizeof(TxnVisibleLive), _Alignof(TxnVisibleLive));
		frame->at++;
	}
	*frame->result = frame->live;
	frame->live_live = false;
	frame->ok = true;
}

/* txn_visible_live_pad frees what live holds live: the chain it took and the live transforms it made. */
static void
txn_visible_live_pad(void *frame_)
{
	txn_visible_live_frame_t *frame = frame_;

	if (frame->chain_live)
	{
		for (size_t i = frame->at; i < frame->chain.len; i++)
			txn_base_Pending_drop(&STD_VEC_AT(&frame->chain, TxnBasePending, i));
		std_vec_free_Checked(&frame->chain, sizeof(TxnBasePending), _Alignof(TxnBasePending));
	}
	if (frame->live_live)
	{
		for (size_t i = 0; i < frame->live.len; i++)
			txn_base_Pending_drop(&STD_VEC_AT(&frame->live, TxnVisibleLive, i).f0);
		std_vec_free_Checked(&frame->live, sizeof(TxnVisibleLive), _Alignof(TxnVisibleLive));
	}
	frame->chain_live = frame->live_live = false;
}

/*
 * txn_visible_live returns the transforms of a chain, taking it, whose DDLs
 * didn't abort, oldest first, each with its commit's end from its top-level
 * xid's commit status entry, none while it runs, or from the entry itself
 * when a save wrote it: a rolled-back subtransaction's and an aborted or
 * crashed transaction's are passed over.
 */
bool
txn_visible_live(HostIoIo *io, StdVec *chain, StdVec *result, HostError *error)
{
	txn_visible_live_frame_t frame = {.io = io, .chain = *chain, .chain_live = true, .result = result,
	.error = error};

	std_landing(&frame, txn_visible_live_body, txn_visible_live_pad);
	txn_visible_live_pad(&frame);
	return frame.ok;
}

/*
 * txn_visible_epoch returns the epoch a delta version is stored at: its
 * storage's base epoch and one more for each live pending transform it was
 * written after, as epoch_after counts them for the version's writer and its
 * commit's end, with no lookup when none is pending.
 */
bool
txn_visible_epoch(HostIoIo *io, uint32 base_epoch, const TxnVisibleLive *chain, size_t chain_len,
				  TxnMemtableVersionVersion *version, uint32 *result, HostError *error)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionWriter writer;
	uint64		top;
	HostXid		asked;
	HostStatus	status;
	TxnStatusCommit commit;
	bool		has_end = false;
	uint64		end = 0;

	if (chain_len == 0)
	{
		*result = base_epoch;
		return true;
	}
	record = txn_memtable_version_Version_record(version);
	writer = txn_memtable_version_Record_writer(&record);
	top = writer.has_top ? writer.top : writer.xid;
	if (!txn_outcome_outcome(io, version, &status, error))
		return false;
	if (status == HOST_STATUS_COMMITTED && top < TXN_STATUS_FIRST_NORMAL)
		has_end = true;
	else if (status == HOST_STATUS_COMMITTED)
	{
		asked.f0 = top;
		if (!txn_status_committed(io, asked, &commit, error))
			return false;
		if (commit.tag == TXN_STATUS_COMMIT_MISSING)
		{
			*error = txn_visible_missing(top);
			return false;
		}
		has_end = commit.tag == TXN_STATUS_COMMIT_AT;
		if (has_end)
			end = commit.u.at;
	}
	*result = txn_visible_epoch_after(base_epoch, chain, chain_len, top, writer.command, has_end, end);
	return true;
}

/*
 * txn_visible_epoch_after returns the epoch of a row a writer wrote, by its
 * top-level xid and command, whose commit ends at an end, none for a writer
 * that hasn't committed: the base epoch and one more for each live pending
 * transform of its own transaction's earlier command, each other whose
 * commit ends before its writer's, and every one for a writer that hasn't
 * committed.
 */
uint32
txn_visible_epoch_after(uint32 base_epoch, const TxnVisibleLive *chain, size_t chain_len, uint64 top,
						uint32 command, bool has_end, uint64 end)
{
	size_t		count = 0;

	for (size_t i = 0; i < chain_len; i++)
	{
		const TxnVisibleLive *live = &chain[i];

		if (live->f0.xid == top)
			count += live->f0.command < command;
		else if (has_end)
			count += live->has_f1 && live->f1 < end;
		else
			count++;
	}
	return STD_ADD(u32, base_epoch, (uint32) count, 387, 5);
}

/* txn_visible_xid_order is u32's Ord over two narrowed xids, the order of a snapshot's sorted arrays. */
static StdOrdering
txn_visible_xid_order(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return x < y ? STD_ORDERING_LESS : (x > y ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* txn_visible_searched reports whether a sorted array of narrowed xids holds one, by a binary search. */
static bool
txn_visible_searched(const uint32 *xids, size_t xids_len, uint64 xid)
{
	uint32		narrow = (uint32) xid;
	size_t		at;

	return std_slice_binary_search(xids, xids_len, sizeof(uint32), &narrow, txn_visible_xid_order, &at);
}

/*
 * txn_visible_sees reports whether an MVCC snapshot sees a version's write:
 * the caller's own when its command is before the snapshot's, and any other
 * writer's when the snapshot doesn't count it running and it committed.
 */
static bool
txn_visible_sees(HostIoIo *io, const HostSnapshot *snapshot, TxnMemtableVersionVersion *version,
				 const TxnVisibleWriter *by, bool *result, HostError *error)
{
	HostXid		asked = {by->xid};
	HostStatus	status;

	if (host_is_current(HOST_IO_IO_IO_DOORS(io), asked))
		*result = by->command < snapshot->command.f0;
	else if (txn_visible_in_snapshot(snapshot, by->xid, by->top))
		*result = false;
	else
	{
		if (!txn_outcome_outcome(io, version, &status, error))
			return false;
		*result = status == HOST_STATUS_COMMITTED;
	}
	return true;
}

/*
 * txn_visible_ran reports whether a version's write has happened for a Self
 * or a Dirty snapshot: the caller's own has, a committed writer's has, and an
 * aborted one's hasn't; it answers none for a writer still running, which
 * the two kinds read differently.
 */
static bool
txn_visible_ran(HostIoIo *io, TxnMemtableVersionVersion *version, const TxnVisibleWriter *by,
				bool *has_result, bool *result, HostError *error)
{
	HostXid		asked = {by->xid};
	HostStatus	status;

	if (host_is_current(HOST_IO_IO_IO_DOORS(io), asked))
	{
		*has_result = true;
		*result = true;
		return true;
	}
	if (!txn_outcome_outcome(io, version, &status, error))
		return false;
	*has_result = status != HOST_STATUS_IN_PROGRESS;
	if (*has_result)
		*result = status == HOST_STATUS_COMMITTED;
	return true;
}

/*
 * txn_visible_historic_begun is the xmin half of
 * HeapTupleSatisfiesHistoricMVCC's rule, a logical decoding's read of a
 * relation it may read: the decoded transaction's own row from the command
 * after the one that inserted it, any other writer's when it committed below
 * the snapshot's xmin or is in xip, and never a row whose hint says aborted.
 */
static bool
txn_visible_historic_begun(HostIoIo *io, const HostSnapshot *snapshot, TxnMemtableVersionVersion *version,
						   const TxnVisibleWriter *xmin, bool *result, HostError *error)
{
	HostStatus	status;

	if (txn_memtable_version_Version_hint(version) == TXN_MEMTABLE_VERSION_HINT_ABORTED)
		*result = false;
	else if (txn_visible_searched(snapshot->subxip, snapshot->subxip_len, xmin->xid))
		*result = xmin->command < snapshot->command.f0;
	else if (xmin->xid < snapshot->xmin.f0)
	{
		if (!txn_outcome_outcome(io, version, &status, error))
			return false;
		*result = status == HOST_STATUS_COMMITTED;
	}
	else
		*result = xmin->xid < snapshot->xmax.f0 && txn_visible_searched(snapshot->xip, snapshot->xip_len, xmin->xid);
	return true;
}

/*
 * txn_visible_historic_ended is the xmax half of
 * HeapTupleSatisfiesHistoricMVCC's rule: the decoded transaction's own
 * delete ends a row from the command after it, any other ender when it
 * committed below the snapshot's xmin or is in xip, and never an ender whose
 * hint says aborted.
 */
static bool
txn_visible_historic_ended(HostIoIo *io, const HostSnapshot *snapshot, TxnMemtableVersionVersion *ender,
						   const TxnVisibleWriter *xmax, bool *result, HostError *error)
{
	HostStatus	status;

	if (txn_memtable_version_Version_hint(ender) == TXN_MEMTABLE_VERSION_HINT_ABORTED)
		*result = false;
	else if (txn_visible_searched(snapshot->subxip, snapshot->subxip_len, xmax->xid))
		*result = xmax->command < snapshot->command.f0;
	else if (xmax->xid < snapshot->xmin.f0)
	{
		if (!txn_outcome_outcome(io, ender, &status, error))
			return false;
		*result = status == HOST_STATUS_COMMITTED;
	}
	else
		*result = xmax->xid < snapshot->xmax.f0 && txn_visible_searched(snapshot->xip, snapshot->xip_len, xmax->xid);
	return true;
}

/*
 * txn_visible_begun reports whether a row version's writer began the row for
 * a snapshot, the xmin half of heap's rule for the snapshot's kind. A Dirty
 * read reports a running writer with a speculative insert's token, and a
 * killed speculative insert begins a row for Any alone.
 */
static bool
txn_visible_begun(HostIoIo *io, const HostSnapshot *snapshot, const TxnVisibleRowVersion *row,
				  TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	TxnVisibleWriter xmin = txn_visible_writer(row->version);
	HostStatus	status;
	bool		has_ran;

	if (txn_memtable_version_Version_killed(row->version) && snapshot->kind != HOST_SNAPSHOT_KIND_ANY)
	{
		*result = false;
		return true;
	}
	switch (snapshot->kind)
	{
		case HOST_SNAPSHOT_KIND_ANY:
			*result = true;
			return true;
		case HOST_SNAPSHOT_KIND_TOAST:
		case HOST_SNAPSHOT_KIND_NON_VACUUMABLE:
			if (!txn_outcome_outcome(io, row->version, &status, error))
				return false;
			*result = status != HOST_STATUS_ABORTED;
			return true;
		case HOST_SNAPSHOT_KIND_MVCC:
			return txn_visible_sees(io, snapshot, row->version, &xmin, result, error);
		case HOST_SNAPSHOT_KIND_CURRENT:
		case HOST_SNAPSHOT_KIND_DIRTY:
			if (!txn_visible_ran(io, row->version, &xmin, &has_ran, result, error))
				return false;
			if (!has_ran && snapshot->kind == HOST_SNAPSHOT_KIND_DIRTY)
			{
				dirty->has_xmin = true;
				dirty->xmin.f0 = xmin.xid;
				dirty->has_token = txn_memtable_version_Version_token(row->version, &dirty->token);
				*result = true;
			}
			else if (!has_ran)
				*result = false;
			return true;
		case HOST_SNAPSHOT_KIND_HISTORIC_MVCC:
			return txn_visible_historic_begun(io, snapshot, row->version, &xmin, result, error);
	}
	pg_unreachable();
}

/*
 * txn_visible_ended reports whether a row was ended for a snapshot by its
 * ender, the xmax half of heap's rule for the snapshot's kind. A Dirty read
 * reports a running ender, which is not yet an end, NonVacuumable's end is a
 * committed ender the snapshot's horizon has passed, and a killed
 * speculative insert ends nothing.
 */
static bool
txn_visible_ended(HostIoIo *io, const HostSnapshot *snapshot, TxnMemtableVersionVersion *ender,
				  TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	TxnVisibleWriter xmax = txn_visible_writer(ender);
	HostXid		asked = {xmax.xid};
	HostStatus	status;
	bool		has_ran;

	*result = false;
	if (txn_memtable_version_Version_killed(ender))
		return true;
	switch (snapshot->kind)
	{
		case HOST_SNAPSHOT_KIND_ANY:
		case HOST_SNAPSHOT_KIND_TOAST:
			return true;
		case HOST_SNAPSHOT_KIND_MVCC:
			return txn_visible_sees(io, snapshot, ender, &xmax, result, error);
		case HOST_SNAPSHOT_KIND_CURRENT:
		case HOST_SNAPSHOT_KIND_DIRTY:
			if (!txn_visible_ran(io, ender, &xmax, &has_ran, result, error))
				return false;
			if (!has_ran && snapshot->kind == HOST_SNAPSHOT_KIND_DIRTY)
			{
				dirty->has_xmax = true;
				dirty->xmax = asked;
			}
			if (!has_ran)
				*result = false;
			return true;
		case HOST_SNAPSHOT_KIND_NON_VACUUMABLE:
			if (!txn_outcome_outcome(io, ender, &status, error))
				return false;
			if (status != HOST_STATUS_COMMITTED)
				return true;
			return host_removable_in(HOST_IO_IO_IO_DOORS(io), snapshot, asked, result, error);
		case HOST_SNAPSHOT_KIND_HISTORIC_MVCC:
			return txn_visible_historic_ended(io, snapshot, ender, &xmax, result, error);
	}
	pg_unreachable();
}

/* txn_visible_Snapshot_Rule_begun is Snapshot's Rule's begun, heap's xmin half for the snapshot's kind. */
static bool
txn_visible_Snapshot_Rule_begun(const void *self, HostIoIo *io, const TxnVisibleRowVersion *row, TxnVisibleDirty *dirty,
								bool *result, HostError *error)
{
	return txn_visible_begun(io, self, row, dirty, result, error);
}

/* txn_visible_Snapshot_Rule_ended is Snapshot's Rule's ended, heap's xmax half for the snapshot's kind. */
static bool
txn_visible_Snapshot_Rule_ended(const void *self, HostIoIo *io, TxnMemtableVersionVersion *ender,
								TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	return txn_visible_ended(io, self, ender, dirty, result, error);
}

const TxnVisibleRuleOps TXN_VISIBLE_SNAPSHOT_RULE_OPS = {
	.begun = txn_visible_Snapshot_Rule_begun,
	.ended = txn_visible_Snapshot_Rule_ended,
};

/*
 * txn_visible_visible reports whether a read sees a row version with its
 * ender, NULL for none: the row's writer began it for the read and its ender
 * has not ended it. A Dirty read also returns what it found running, and
 * sees a row whose writer it found running with no look at its ender.
 */
static bool
txn_visible_visible(HostIoIo *io, TxnVisibleRule rule, const TxnVisibleRowVersion *row,
					TxnMemtableVersionVersion *ender, bool *result, TxnVisibleDirty *dirty, HostError *error)
{
	bool		begun;
	bool		ended;

	*dirty = txn_visible_Dirty_Default_default();
	if (!rule.ops->begun(rule.self, io, row, dirty, &begun, error))
		return false;
	*result = begun;
	if (begun && ender != NULL && !dirty->has_xmin)
	{
		if (!rule.ops->ended(rule.self, io, ender, dirty, &ended, error))
			return false;
		*result = !ended;
	}
	return true;
}

/* txn_visible_seen_frame_t is seen's state across its pad: what it has seen, while live. */
typedef struct txn_visible_seen_frame_t
{
	HostIoIo   *io;
	TxnVisibleRule rule;
	TxnMemtableVersionVersion *versions;
	size_t		versions_len;
	const TxnVisibleSerial *serial;
	TxnVisibleSeen *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} txn_visible_seen_frame_t;

/* txn_visible_seen_body walks the key's versions newest first, keeping the rows the read sees. */
static void
txn_visible_seen_body(void *frame_)
{
	txn_visible_seen_frame_t *frame = frame_;
	TxnVisibleSeen *seen = frame->result;

	std_vec_new(&seen->rows, sizeof(TxnVisibleSeenRow), _Alignof(TxnVisibleSeenRow));
	std_vec_new(&seen->above, sizeof(TxnVisibleAbove), _Alignof(TxnVisibleAbove));
	std_vec_new(&seen->conflicts, sizeof(txn_TupleRowXid_t), _Alignof(txn_TupleRowXid_t));
	frame->result_live = true;
	for (size_t i = 0; i < frame->versions_len; i++)
	{
		TxnMemtableVersionVersion *version = &frame->versions[i];
		TxnVisibleRowVersion row;
		TxnVisibleAbove above = {.version = version};
		HostStatus	status;

		if (txn_visible_RowVersion_of(version, &row))
		{
			HostRow		tuple = row.put.tuple;
			TxnMemtableVersionVersion *ender = txn_visible_Seen_ender(seen, tuple);
			TxnVisibleSeenRow seen_row = {.row = row};
			bool		visible;

			if (!txn_visible_visible(frame->io, frame->rule, &row, ender, &visible, &seen_row.dirty, frame->error))
				return;
			if (frame->serial != NULL)
			{
				TxnVisibleWriter wrote = txn_visible_writer(version);
				txn_TupleRowXid_t conflict = {.f0 = tuple};
				bool		has_conflict;

				if (!txn_outcome_outcome(frame->io, version, &status, frame->error) ||
					!txn_visible_conflict(frame->io, frame->serial, wrote.xid, wrote.top, status, ender, visible,
										  &has_conflict, &conflict.f1, frame->error))
					return;
				if (has_conflict)
					std_vec_push_Checked(&seen->conflicts, &conflict, sizeof(conflict), _Alignof(txn_TupleRowXid_t));
			}
			if (visible)
			{
				seen_row.changer = txn_visible_Seen_changer(seen, tuple);
				seen_row.commander = txn_visible_Seen_commander(seen, tuple);
				std_vec_push_Checked(&seen->rows, &seen_row, sizeof(seen_row), _Alignof(TxnVisibleSeenRow));
			}
		}
		if (!txn_visible_names(version, &above.has_names, &above.names, &above.lock))
			continue;
		if (!above.lock)
		{
			if (!txn_outcome_outcome(frame->io, version, &status, frame->error))
				return;
			above.ends = status != HOST_STATUS_ABORTED;
		}
		std_vec_push_Checked(&seen->above, &above, sizeof(above), _Alignof(TxnVisibleAbove));
	}
	frame->result_live = false;
	frame->ok = true;
}

/* txn_visible_seen_pad frees what seen has seen, while live. */
static void
txn_visible_seen_pad(void *frame_)
{
	txn_visible_seen_frame_t *frame = frame_;

	if (frame->result_live)
		txn_visible_Seen_drop(frame->result);
	frame->result_live = false;
}

/*
 * txn_visible_seen returns what a read sees of a storage's key from the key's
 * versions, newest first: every row version the rule sees, each with its
 * changer, and for a serializable read, NULL for any other, the conflicts it
 * found. A row's ender is the nearest version newer than it that isn't a
 * lock, whose writer didn't abort, and that sets the key's value or names the
 * row's tuple id.
 */
bool
txn_visible_seen(HostIoIo *io, TxnVisibleRule rule, TxnMemtableVersionVersion *versions, size_t versions_len,
				 const TxnVisibleSerial *serial, TxnVisibleSeen *result, HostError *error)
{
	txn_visible_seen_frame_t frame = {.io = io, .rule = rule, .versions = versions, .versions_len = versions_len,
	.serial = serial, .result = result, .error = error};

	std_landing(&frame, txn_visible_seen_body, txn_visible_seen_pad);
	txn_visible_seen_pad(&frame);
	return frame.ok;
}

/* txn_visible_decided returns a resolve's answer from the version that decided it: the row it writes, or free. */
static void
txn_visible_decided(bool written, const TxnVisibleRowVersion *row, TxnVisibleResolved *result)
{
	if (!written)
	{
		result->tag = TXN_VISIBLE_RESOLVED_FREE;
		return;
	}
	result->tag = TXN_VISIBLE_RESOLVED_EXISTS;
	std_slice_to_vec_Checked(&result->u.exists.f0, row->put.value.ptr, row->put.value.len, 1, 1);
	result->u.exists.has_f1 = true;
	result->u.exists.f1 = row->put.tuple;
}

/*
 * txn_visible_resolved answers an immediate key's resolve from its versions
 * newest first and the base's value at the key, NULL for none, as the
 * btree's _bt_check_unique reads the key with SnapshotDirty: the newest
 * version that writes or ends a row, passing over locks, killed speculative
 * inserts, and aborted writers' versions, decides it. A row whose writer
 * committed or is the caller's own transaction exists, and an end leaves the
 * key free; another transaction's running write or end is waited for, with
 * the tuple id of the row it writes or of the nearest row below it and a
 * speculative insert's token. With no such version, a base row exists and
 * its absence leaves the key free. A row that exists carries a copy of its
 * value, since it is used after the caller's hold is released.
 */
bool
txn_visible_resolved(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len, StdBytes based,
					 TxnVisibleResolved *result, HostError *error)
{
	for (size_t i = 0; i < versions_len; i++)
	{
		TxnMemtableVersionVersion *version = &versions[i];
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
		TxnVisibleRowVersion row;
		TxnVisibleWriter by;
		HostXid		asked;
		HostStatus	status;
		bool		written;
		size_t		at = 0;

		if (txn_memtable_version_Version_killed(version))
			continue;
		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
			continue;
		written = txn_visible_RowVersion_of(version, &row);
		by = txn_visible_writer(version);
		asked.f0 = by.xid;
		if (host_is_current(HOST_IO_IO_IO_DOORS(io), asked))
		{
			txn_visible_decided(written, &row, result);
			return true;
		}
		if (!txn_outcome_outcome(io, version, &status, error))
			return false;
		if (status == HOST_STATUS_ABORTED)
			continue;
		if (status == HOST_STATUS_COMMITTED)
		{
			txn_visible_decided(written, &row, result);
			return true;
		}
		result->tag = TXN_VISIBLE_RESOLVED_WAIT;
		result->u.wait.f0 = asked;
		result->u.wait.has_f1 = false;
		while (at < versions_len && !txn_memtable_version_Handle_PartialEq_eq(&versions[at].handle, &version->handle))
			at++;
		for (; at < versions_len && !result->u.wait.has_f1; at++)
			if (txn_visible_RowVersion_of(&versions[at], &row))
			{
				result->u.wait.has_f1 = true;
				result->u.wait.f1 = row.put.tuple;
			}
		result->u.wait.has_f2 = txn_memtable_version_Version_token(version, &result->u.wait.f2);
		return true;
	}
	if (based.ptr == NULL)
	{
		result->tag = TXN_VISIBLE_RESOLVED_FREE;
		return true;
	}
	result->tag = TXN_VISIBLE_RESOLVED_EXISTS;
	std_slice_to_vec_Checked(&result->u.exists.f0, based.ptr, based.len, 1, 1);
	result->u.exists.has_f1 = false;
	return true;
}

/* txn_visible_Resolved_PartialEq_eq reports whether two resolves found the same variant with the same values. */
bool
txn_visible_Resolved_PartialEq_eq(const TxnVisibleResolved *self, const TxnVisibleResolved *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case TXN_VISIBLE_RESOLVED_EXISTS:
			return trees_store_VecU8_PartialEq_eq(&self->u.exists.f0, &other->u.exists.f0) &&
				self->u.exists.has_f1 == other->u.exists.has_f1 &&
				(!self->u.exists.has_f1 || host_Row_PartialEq_eq(&self->u.exists.f1, &other->u.exists.f1));
		case TXN_VISIBLE_RESOLVED_WAIT:
			return host_Xid_PartialEq_eq(&self->u.wait.f0, &other->u.wait.f0) &&
				self->u.wait.has_f1 == other->u.wait.has_f1 &&
				(!self->u.wait.has_f1 || host_Row_PartialEq_eq(&self->u.wait.f1, &other->u.wait.f1)) &&
				self->u.wait.has_f2 == other->u.wait.has_f2 &&
				(!self->u.wait.has_f2 || self->u.wait.f2 == other->u.wait.f2);
		case TXN_VISIBLE_RESOLVED_FREE:
			return true;
	}
	pg_unreachable();
}

/* txn_visible_Resolved_Debug_fmt is Resolved's derived Debug. */
bool
txn_visible_Resolved_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleResolved *self = value;
	StdOptionRef row;
	StdOptionRef token;
	StdDebugTuple t;

	switch (self->tag)
	{
		case TXN_VISIBLE_RESOLVED_EXISTS:
			row.has = self->u.exists.has_f1;
			row.some.value = &self->u.exists.f1;
			row.some.fmt = host_Row_Debug_fmt;
			t = std_fmt_debug_tuple(f, "Exists", 6);
			std_DebugTuple_field(&t, &self->u.exists.f0, chunks_chunk_VecU8_Debug_fmt);
			std_DebugTuple_field(&t, &row, std_option_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case TXN_VISIBLE_RESOLVED_WAIT:
			row.has = self->u.wait.has_f1;
			row.some.value = &self->u.wait.f1;
			row.some.fmt = host_Row_Debug_fmt;
			token.has = self->u.wait.has_f2;
			token.some.value = &self->u.wait.f2;
			token.some.fmt = std_u32_Debug_fmt;
			t = std_fmt_debug_tuple(f, "Wait", 4);
			std_DebugTuple_field(&t, &self->u.wait.f0, host_Xid_Debug_fmt);
			std_DebugTuple_field(&t, &row, std_option_Debug_fmt);
			std_DebugTuple_field(&t, &token, std_option_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case TXN_VISIBLE_RESOLVED_FREE:
			return std_fmt_write_str(f, "Free", 4);
	}
	pg_unreachable();
}

/* txn_visible_Resolved_drop frees an existing row's value. */
void
txn_visible_Resolved_drop(TxnVisibleResolved *self)
{
	if (self->tag == TXN_VISIBLE_RESOLVED_EXISTS)
		std_vec_free_Checked(&self->u.exists.f0, 1, 1);
}

/* txn_visible_named returns the tuple id a successor's key ends with, the row an update moved its row to. */
static bool
txn_visible_named(const TxnMemtableVersionSuccessorRef *successor, HostRow *result)
{
	uint64		tid;

	if (successor->tag != TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY || successor->u.key.len < 8)
		return false;
	memcpy(&tid, successor->u.key.ptr + successor->u.key.len - 8, 8);
	result->f0 = pg_ntoh64(tid);
	return true;
}

/* txn_visible_moved_to reports whether a version ends a row by moving it to a successor that names a tuple id. */
static bool
txn_visible_moved_to(TxnMemtableVersionVersion *version, HostRow tuple, HostRow *ended)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	HostRow		named;

	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE && op.u.remove.has_f1 &&
		txn_visible_named(&op.u.remove.f1, &named) && host_Row_PartialEq_eq(&named, &tuple))
	{
		*ended = op.u.remove.f0;
		return true;
	}
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW && op.u.delete_row.has_f1 &&
		txn_visible_named(&op.u.delete_row.f1, &named) && host_Row_PartialEq_eq(&named, &tuple))
	{
		*ended = op.u.delete_row.f0;
		return true;
	}
	return false;
}

/*
 * txn_visible_replaced returns the row a row-addressed update replaced with
 * a row: the row its UpdateRow names, or the row the end right under its Add
 * or AddRow names when that end names it as its successor.
 */
static bool
txn_visible_replaced(TxnMemtableVersionVersion *versions, size_t versions_len, size_t at, HostRow tuple,
					 HostRow *result)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;

	std_index_check(at, versions_len, RUST_FILE, 632, 39);
	record = txn_memtable_version_Version_record(&versions[at]);
	op = txn_memtable_version_Record_op(&record);
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW)
	{
		*result = op.u.update_row.f0;
		return true;
	}
	return at + 1 < versions_len && txn_visible_moved_to(&versions[at + 1], tuple, result);
}

/*
 * txn_visible_carried returns, by its first place and count, the lock-only
 * versions under a row that an update replacing their row carried to it, as
 * heap_update keeps the old tuple's lockers it doesn't conflict with: those
 * right under the row, past the end of the row it replaced, which names it,
 * a row-addressed one naming that row or the row itself.
 */
static void
txn_visible_carried(TxnMemtableVersionVersion *versions, size_t versions_len, size_t at, HostRow tuple,
					bool has_replaced, HostRow replaced, size_t *start, size_t *count)
{
	HostRow		ended;

	*start = at + 1;
	if (*start < versions_len && txn_visible_moved_to(&versions[*start], tuple, &ended))
		(*start)++;
	for (*count = 0; *start + *count < versions_len; (*count)++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&versions[*start + *count]);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK)
			continue;
		if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW ||
			(!host_Row_PartialEq_eq(&op.u.lock_row.f0, &tuple) &&
			 !(has_replaced && host_Row_PartialEq_eq(&op.u.lock_row.f0, &replaced))))
			break;
	}
}

/* txn_visible_among reports whether versions hold one, by its handle. */
static bool
txn_visible_among(TxnMemtableVersionVersion *versions, size_t versions_len,
				  TxnMemtableVersionVersion *version)
{
	for (size_t i = 0; i < versions_len; i++)
		if (txn_memtable_version_Handle_PartialEq_eq(&versions[i].handle, &version->handle))
			return true;
	return false;
}

/* TXN_VISIBLE_HOLDER is the size and alignment of a holder, a version's reference. */
#define TXN_VISIBLE_HOLDER sizeof(TxnMemtableVersionVersion *), _Alignof(TxnMemtableVersionVersion *)

/* txn_visible_updatable_frame_t is updatable's state across its pad: the holders it found, while live. */
typedef struct txn_visible_updatable_frame_t
{
	HostIoIo   *io;
	TxnMemtableVersionVersion *versions;
	size_t		versions_len;
	TxnVisibleChanged changed;
	uint32		command;
	StdVec		holders;
	bool		holders_live;
	TxnVisibleUpdatable *result;
	HostError  *error;
	bool		ok;
} txn_visible_updatable_frame_t;

/* txn_visible_updatable_answer sets updatable's answer with no holders. */
static void
txn_visible_updatable_answer(txn_visible_updatable_frame_t *frame, TxnVisibleUpdatableTag tag,
							 TxnMemtableVersionVersion *version)
{
	frame->result->tag = tag;
	if (tag == TXN_VISIBLE_UPDATABLE_SELF_MODIFIED)
		frame->result->u.self_modified = version;
	else if (tag == TXN_VISIBLE_UPDATABLE_UPDATED)
		frame->result->u.updated = version;
	else if (tag == TXN_VISIBLE_UPDATABLE_DELETED)
		frame->result->u.deleted = version;
	frame->ok = true;
}

/* txn_visible_being_modified sets updatable's answer that holders are changing the row, moving the holders into it. */
static void
txn_visible_being_modified(txn_visible_updatable_frame_t *frame, TxnMemtableVersionVersion *ended)
{
	frame->result->tag = TXN_VISIBLE_UPDATABLE_BEING_MODIFIED;
	frame->result->u.being_modified.holders = frame->holders;
	frame->result->u.being_modified.ended = ended;
	frame->holders_live = false;
	frame->ok = true;
}

/* txn_visible_updatable_body walks the row's carried lockers and then the versions above it, nearest first. */
static void
txn_visible_updatable_body(void *frame_)
{
	txn_visible_updatable_frame_t *frame = frame_;
	TxnMemtableVersionVersion *versions = frame->versions;
	size_t		newer = frame->versions_len;
	HostRow		tuple;
	size_t		carried_at = 0;
	size_t		carried_len = 0;
	bool		has_replaced = false;
	HostRow		replaced = {0};
	HostXid		asked;
	HostStatus	status;
	bool		own;

	if (frame->changed.tag == TXN_VISIBLE_CHANGED_DELTA)
	{
		const TxnVisibleRowVersion *row = &frame->changed.u.delta.row;
		TxnVisibleWriter by = txn_visible_writer(row->version);

		asked.f0 = by.xid;
		own = host_is_current(HOST_IO_IO_IO_DOORS(frame->io), asked);
		if (!txn_outcome_outcome(frame->io, row->version, &status, frame->error))
			return;
		if (status == HOST_STATUS_ABORTED || (!own && status == HOST_STATUS_IN_PROGRESS) ||
			(own && by.command >= frame->command))
		{
			txn_visible_updatable_answer(frame, TXN_VISIBLE_UPDATABLE_INVISIBLE, NULL);
			return;
		}
		newer = frame->changed.u.delta.at;
		tuple = row->put.tuple;
		has_replaced = txn_visible_replaced(versions, frame->versions_len, newer, tuple, &replaced);
		txn_visible_carried(versions, frame->versions_len, newer, tuple, has_replaced, replaced, &carried_at,
							&carried_len);
	}
	else
		tuple = frame->changed.u.base;
	std_vec_new(&frame->holders, TXN_VISIBLE_HOLDER);
	frame->holders_live = true;
	for (size_t k = 0; k < carried_len + newer; k++)
	{
		TxnMemtableVersionVersion *version = k < carried_len ? &versions[carried_at + k] :
			&versions[newer - 1 - (k - carried_len)];
		TxnVisibleWriter by;
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		bool		has_names;
		HostRow		names;
		bool		lock;

		if (!txn_visible_names(version, &has_names, &names, &lock))
			continue;
		if (has_names && !host_Row_PartialEq_eq(&names, &tuple) &&
			!(lock && has_replaced && host_Row_PartialEq_eq(&names, &replaced)) &&
			!txn_visible_among(&versions[carried_at], carried_len, version))
			continue;
		by = txn_visible_writer(version);
		asked.f0 = by.xid;
		own = host_is_current(HOST_IO_IO_IO_DOORS(frame->io), asked);
		if (!txn_outcome_outcome(frame->io, version, &status, frame->error))
			return;
		if (lock)
		{
			if (own || status == HOST_STATUS_IN_PROGRESS)
				std_vec_push_Checked(&frame->holders, &version, TXN_VISIBLE_HOLDER);
			continue;
		}
		if (status == HOST_STATUS_ABORTED)
			continue;
		if (own)
			txn_visible_updatable_answer(frame, by.command >= frame->command ? TXN_VISIBLE_UPDATABLE_SELF_MODIFIED :
										 TXN_VISIBLE_UPDATABLE_INVISIBLE, version);
		else if (status == HOST_STATUS_IN_PROGRESS)
		{
			std_vec_push_Checked(&frame->holders, &version, TXN_VISIBLE_HOLDER);
			txn_visible_being_modified(frame, NULL);
		}
		else if (frame->holders.len > 0)
			txn_visible_being_modified(frame, version);
		else
		{
			record = txn_memtable_version_Version_record(version);
			op = txn_memtable_version_Record_op(&record);
			if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT || op.tag == TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW ||
				op.tag == TXN_MEMTABLE_VERSION_OP_REF_MOVE ||
				(op.tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE && op.u.remove.has_f1) ||
				(op.tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW && op.u.delete_row.has_f1))
				txn_visible_updatable_answer(frame, TXN_VISIBLE_UPDATABLE_UPDATED, version);
			else
				txn_visible_updatable_answer(frame, TXN_VISIBLE_UPDATABLE_DELETED, version);
		}
		return;
	}
	if (frame->holders.len > 0)
		txn_visible_being_modified(frame, NULL);
	else
		txn_visible_updatable_answer(frame, TXN_VISIBLE_UPDATABLE_OK, NULL);
}

/* txn_visible_updatable_pad frees the holders updatable found, while live. */
static void
txn_visible_updatable_pad(void *frame_)
{
	txn_visible_updatable_frame_t *frame = frame_;

	if (frame->holders_live)
		std_vec_free_Checked(&frame->holders, TXN_VISIBLE_HOLDER);
	frame->holders_live = false;
}

/*
 * txn_visible_updatable answers whether the caller's command may change a
 * row, from its key's versions newest first, by heap's
 * HeapTupleSatisfiesUpdate cases: a row whose writer aborted, or another
 * transaction's running writer, or the caller's own at or past its command,
 * is invisible. Its ender is the nearest version above it that changes it
 * and whose writer didn't abort, and its lockers are the lock-only versions
 * between it and its ender, those an update carried to it from the row it
 * replaced, and those above it that name the row it replaced. The caller's
 * own ender at or past its command self-modified it and before it hid it;
 * another running ender is changing it, with the running lockers; a
 * committed ender updated it, by a successor, or deleted it, unless a locker
 * still runs; and with no ender, a running locker, the caller's own
 * included, is changing it.
 */
bool
txn_visible_updatable(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
					  TxnVisibleChanged changed, uint32 command, TxnVisibleUpdatable *result, HostError *error)
{
	txn_visible_updatable_frame_t frame = {.io = io, .versions = versions, .versions_len = versions_len,
	.changed = changed, .command = command, .result = result, .error = error};

	std_landing(&frame, txn_visible_updatable_body, txn_visible_updatable_pad);
	txn_visible_updatable_pad(&frame);
	return frame.ok;
}

/* txn_visible_Updatable_drop frees the holders of a row being changed. */
void
txn_visible_Updatable_drop(TxnVisibleUpdatable *self)
{
	if (self->tag == TXN_VISIBLE_UPDATABLE_BEING_MODIFIED)
		std_vec_free_Checked(&self->u.being_modified.holders, TXN_VISIBLE_HOLDER);
}

/* TXN_VISIBLE_HELD is the size and alignment of a lock-only version's writer and mode. */
#define TXN_VISIBLE_HELD sizeof(txn_TupleWriterLockMode_t), _Alignof(txn_TupleWriterLockMode_t)

/* txn_visible_lockers_frame_t is lockers' state across its pad: the answer it asked and the holds it found. */
typedef struct txn_visible_lockers_frame_t
{
	HostIoIo   *io;
	TxnVisibleUpdatable updatable;
	bool		updatable_live;
	StdVec		held;
	bool		held_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_visible_lockers_frame_t;

/* txn_visible_lockers_body keeps each holder of a lock-only version another transaction wrote. */
static void
txn_visible_lockers_body(void *frame_)
{
	txn_visible_lockers_frame_t *frame = frame_;
	const StdVec *holders = &frame->updatable.u.being_modified.holders;

	std_vec_new(&frame->held, TXN_VISIBLE_HELD);
	frame->held_live = true;
	for (size_t i = 0; i < holders->len; i++)
	{
		TxnMemtableVersionVersion *holder = STD_VEC_AT(holders, TxnMemtableVersionVersion *, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(holder);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
		txn_TupleWriterLockMode_t held = {.f0 = txn_memtable_version_Record_writer(&record)};
		HostXid		asked = {held.f0.xid};

		if ((op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK && op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW) ||
			host_is_current(HOST_IO_IO_IO_DOORS(frame->io), asked))
			continue;
		if (!std_vec_try_reserve(&frame->held, 1, TXN_VISIBLE_HELD))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		held.f1 = txn_memtable_version_Record_lock(&record);
		std_vec_push_Checked(&frame->held, &held, TXN_VISIBLE_HELD);
	}
	*frame->result = frame->held;
	frame->held_live = false;
	frame->ok = true;
}

/* txn_visible_lockers_pad frees what lockers holds live: the answer's holders and the holds it found. */
static void
txn_visible_lockers_pad(void *frame_)
{
	txn_visible_lockers_frame_t *frame = frame_;

	if (frame->held_live)
		std_vec_free_Checked(&frame->held, TXN_VISIBLE_HELD);
	if (frame->updatable_live)
		txn_visible_Updatable_drop(&frame->updatable);
	frame->held_live = frame->updatable_live = false;
}

/*
 * txn_visible_lockers returns the transactions other than the caller's that
 * hold a row with a running lock-only version, each with its version's
 * writer and mode, from its key's versions newest first, as updatable finds
 * them for the row, in the delta or in the base.
 */
bool
txn_visible_lockers(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len, HostRow tuple,
					uint32 command, StdVec *result, HostError *error)
{
	txn_visible_lockers_frame_t frame = {.io = io, .result = result, .error = error};
	TxnVisibleChanged changed = {.tag = TXN_VISIBLE_CHANGED_BASE, .u.base = tuple};
	TxnVisibleRowVersion row;

	for (size_t at = 0; at < versions_len; at++)
		if (txn_visible_RowVersion_of(&versions[at], &row) && row.put.tuple.f0 == tuple.f0)
		{
			changed.tag = TXN_VISIBLE_CHANGED_DELTA;
			changed.u.delta.at = at;
			changed.u.delta.row = row;
			break;
		}
	if (!txn_visible_updatable(io, versions, versions_len, changed, command, &frame.updatable, error))
		return false;
	if (frame.updatable.tag != TXN_VISIBLE_UPDATABLE_BEING_MODIFIED)
	{
		std_vec_new(result, TXN_VISIBLE_HELD);
		return true;
	}
	frame.updatable_live = true;
	std_landing(&frame, txn_visible_lockers_body, txn_visible_lockers_pad);
	txn_visible_lockers_pad(&frame);
	return frame.ok;
}

/*
 * txn_visible_carries returns the locker an update of the row with a tuple id
 * carries to its new row, as heap_update carries the lock its old tuple's
 * xmax names: the newest running lock-only version updatable finds for the
 * row, the caller's own included, or else the row's own carried locker while
 * its transaction runs, from its key's versions newest first; false for none.
 */
bool
txn_visible_carries(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len, HostRow tuple,
					uint32 command, bool *has, uint64 *result, HostError *error)
{
	TxnVisibleChanged changed = {.tag = TXN_VISIBLE_CHANGED_BASE, .u.base = tuple};
	TxnVisibleRowVersion row;
	TxnVisibleUpdatable updatable;
	HostXid		asked;

	*has = false;
	for (size_t at = 0; at < versions_len; at++)
		if (txn_visible_RowVersion_of(&versions[at], &row) && row.put.tuple.f0 == tuple.f0)
		{
			changed.tag = TXN_VISIBLE_CHANGED_DELTA;
			changed.u.delta.at = at;
			changed.u.delta.row = row;
			break;
		}
	if (!txn_visible_updatable(io, versions, versions_len, changed, command, &updatable, error))
		return false;
	if (updatable.tag == TXN_VISIBLE_UPDATABLE_BEING_MODIFIED)
		for (size_t i = 0; i < updatable.u.being_modified.holders.len && !*has; i++)
		{
			TxnMemtableVersionVersion *holder = STD_VEC_AT(&updatable.u.being_modified.holders,
														   TxnMemtableVersionVersion *, i);
			TxnMemtableVersionRecord record = txn_memtable_version_Version_record(holder);
			TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

			*has = op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW;
			*result = txn_memtable_version_Record_writer(&record).xid;
		}
	txn_visible_Updatable_drop(&updatable);
	if (*has || changed.tag != TXN_VISIBLE_CHANGED_DELTA || !changed.u.delta.row.put.has_carried)
		return true;
	asked.f0 = changed.u.delta.row.put.carried;
	*result = asked.f0;
	*has = host_is_current(HOST_IO_IO_IO_DOORS(io), asked);
	return *has || host_in_progress(HOST_IO_IO_IO_DOORS(io), asked, has, error);
}

#ifdef DOLTGRES_TEST_TXN

/*
 * test_txn_visible_tests_a_running_writers_version_leaves_its_ran_answer_as_it_was:
 * a version whose writer is still running answers none for whether its
 * write ran and leaves its caller's answer as the caller had it (bugs row
 * 288).
 */
void
test_txn_visible_tests_a_running_writers_version_leaves_its_ran_answer_as_it_was(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_memtable_testing_created(io, 9652);
	HostXid		xid = simc_begin();
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(xid.f0), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnVisibleWriter by = {.xid = xid.f0, .top = xid.f0};
	TxnMemtableVersionHandle handle;
	HostDeltaHold hold;
	StdVec		versions;
	HostError	error;
	bool		has = true;
	bool		ran = true;
	TxnStatusRoomed unroomed = {0};

	if (!txn_memtable_table_alone(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, host_shared_Held_record(&held),
								  (const uint8 *) "k", 1, &change, false, unroomed, &handle, &error) ||
		!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_table_versions(io, &hold, host_shared_Held_record(&held), (const uint8 *) "k", 1, &versions,
									 &error) ||
		!txn_visible_ran(io, &STD_VEC_AT(&versions, TxnMemtableVersionVersion, 0), &by, &has, &ran, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !has && ran, "a running writer's version answers none and leaves its answer as it was");
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	txn_memtable_Op_drop(&op);
	simc_abort(xid);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_visible_tests_made returns a new storage's state, by its database, in a repository made for it. */
static HostSharedHeld
txn_visible_tests_made(HostIoIo *io, uint32 database)
{
	HostStorage storage = {.tablespace = HOST_PG_DEFAULT, .database = database, .relation = 16384};
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_create(io, database, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &failed,
								 chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &reading, &error) ||
		!txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held,
						 &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return held;
}

/* txn_visible_tests_by returns a writer of an xid under a top-level one, 0 for none, at a command. */
static TxnMemtableVersionWriter
txn_visible_tests_by(HostXid xid, uint64 top, uint32 command)
{
	TxnMemtableVersionWriter writer = {.xid = xid.f0, .has_top = top != 0, .top = top, .command = command};

	return writer;
}

/* txn_visible_tests_pushed pushes a version of a key by a writer doing an op. */
static void
txn_visible_tests_pushed(HostIoIo *io, HostSharedRecord state, const char *key, TxnMemtableVersionWriter writer,
						 const TxnMemtableOp *op)
{
	TxnMemtableVersionChange change = {.writer = writer, .op = op, .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
	.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionHandle handle;
	HostError	error;

	if (!txn_memtable_table_push(io, state, (const uint8 *) key, strlen(key), &change, &handle, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
}

/* txn_visible_tests_written pushes a row of a key by a writer with a tuple id, or with 0 the row's delete. */
static void
txn_visible_tests_written(HostIoIo *io, HostSharedRecord state, const char *key, TxnMemtableVersionWriter writer,
						  uint64 tuple)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_DELETE};

	if (tuple != 0)
	{
		op = txn_memtable_testing_put((const uint8 *) "row", 3);
		op.u.put.tuple.f0 = tuple;
	}
	txn_visible_tests_pushed(io, state, key, writer, &op);
	txn_memtable_Op_drop(&op);
}

/* txn_visible_tests_sees returns the tuple ids of the row versions of a key that a snapshot sees, newest first, in a Vec<u64>. */
static StdVec
txn_visible_tests_sees(HostIoIo *io, HostSharedRecord state, const char *key, const HostSnapshot *snapshot)
{
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, snapshot};
	HostDeltaHold hold;
	StdVec		versions;
	TxnVisibleSeen seen;
	StdVec		tuples;
	HostError	error;

	if (!host_delta_hold(io, &hold, &error) ||
		!txn_memtable_table_versions(io, &hold, state, (const uint8 *) key, strlen(key), &versions, &error) ||
		!txn_visible_seen(io, rule, versions.ptr, versions.len, NULL, &seen, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_new(&tuples, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < seen.rows.len; k++)
		std_vec_push_Checked(&tuples, &STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, k).row.put.tuple.f0, sizeof(uint64),
							 _Alignof(uint64));
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return tuples;
}

/* txn_visible_tests_sees_is asserts at its line that a snapshot sees exactly the tuple ids given of a key, newest first. */
static void
txn_visible_tests_sees_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key,
						  const HostSnapshot *snapshot, const uint64 *wanted, size_t wanted_len)
{
	StdVec		seen = txn_visible_tests_sees(io, state, key, snapshot);
	bool		same = seen.len == wanted_len;

	for (size_t k = 0; same && k < wanted_len; k++)
		same = STD_VEC_AT(&seen, uint64, k) == wanted[k];
	STD_ASSERT(line, 0, same, "a snapshot sees the key's row versions it should");
	std_vec_free_Checked(&seen, sizeof(uint64), _Alignof(uint64));
}

/*
 * S1's case (PRevPF 5's prevpf-258): a snapshot taken while X runs, after X's
 * subtransaction wrote a row, reads the subtransaction's row invisible once X
 * commits, whether the snapshot's subtransactions overflowed or not, and a
 * snapshot taken after the commit sees it.
 */
void
test_txn_visible_tests_a_subtransactions_row_stays_invisible_to_a_snapshot_taken_while_its_transaction_ran(void)
{
	static const char *const keys[2] = {"few", "many"};
	static const uint32 counts[2] = {1, 70};
	static const uint64 one[1] = {1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9340);

	for (size_t k = 0; k < lengthof(keys); k++)
	{
		HostXid		top = simc_begin();
		HostXid		first = simc_begin_under(top);
		SimcTaken	taken;
		SimcTaken	later;
		HostSnapshot lent;
		HostSnapshot after;

		for (uint32 sub = 1; sub < counts[k]; sub++)
			simc_begin_under(top);
		txn_visible_tests_written(io, host_shared_Held_record(&held), keys[k], txn_visible_tests_by(first, top.f0, 0),
								  1);
		taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
		lent = simc_Taken_lent(&taken);
		STD_ASSERT(__LINE__, 0, lent.overflowed == (counts[k] > 64), "taken.lent().overflowed == (subs > 64)");
		simc_commit(top);
		txn_visible_tests_sees_is(__LINE__, io, host_shared_Held_record(&held), keys[k], &lent, NULL, 0);
		later = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
		after = simc_Taken_lent(&later);
		txn_visible_tests_sees_is(__LINE__, io, host_shared_Held_record(&held), keys[k], &after, one, 1);
		simc_Taken_drop(&later);
		simc_Taken_drop(&taken);
	}
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * Heap's MVCC rule over a delete: a committed row stays visible to a snapshot
 * taken while its deleter ran, after the deleter commits, and a later
 * snapshot doesn't see the row; a recovery-shaped snapshot finds the running
 * deleter in subxip. An aborted update between the row and its delete is
 * passed over: the delete ends the row, and the update's row is seen by no
 * snapshot.
 */
void
test_txn_visible_tests_a_rows_delete_is_seen_only_by_a_snapshot_taken_after_its_commit(void)
{
	static const uint64 one[1] = {1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9341);
	HostXid		writer = simc_begin();
	HostXid		updater = simc_begin();
	HostXid		deleter;
	SimcTaken	before;
	SimcTaken	recovery;
	SimcTaken	later;
	HostSnapshot lent;

	txn_visible_tests_written(io, host_shared_Held_record(&held), "k", txn_visible_tests_by(writer, 0, 0), 1);
	simc_commit(writer);
	txn_visible_tests_written(io, host_shared_Held_record(&held), "k", txn_visible_tests_by(updater, 0, 0), 2);
	simc_abort(updater);
	deleter = simc_begin();
	txn_visible_tests_written(io, host_shared_Held_record(&held), "k", txn_visible_tests_by(deleter, 0, 0), 0);
	before = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	recovery = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, true);
	lent = simc_Taken_lent(&recovery);
	txn_visible_tests_sees_is(__LINE__, io, host_shared_Held_record(&held), "k", &lent, one, 1);
	simc_commit(deleter);
	lent = simc_Taken_lent(&before);
	txn_visible_tests_sees_is(__LINE__, io, host_shared_Held_record(&held), "k", &lent, one, 1);
	later = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	lent = simc_Taken_lent(&later);
	txn_visible_tests_sees_is(__LINE__, io, host_shared_Held_record(&held), "k", &lent, NULL, 0);
	simc_Taken_drop(&later);
	simc_Taken_drop(&recovery);
	simc_Taken_drop(&before);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* TxnVisibleTestsFound is a row version a read found: its tuple id and what a Dirty read found running. */
typedef struct TxnVisibleTestsFound
{
	uint64		tuple;
	TxnVisibleDirty dirty;
} TxnVisibleTestsFound;

/* TxnVisibleTestsRead is read's answer: the row versions a snapshot sees, newest first, and the base's rows it keeps. */
typedef struct TxnVisibleTestsRead
{
	/* rows is a Vec<TxnVisibleTestsFound>. */
	StdVec		rows;
	/* kept is a Vec<u64>. */
	StdVec		kept;
} TxnVisibleTestsRead;

/* TxnVisibleTestsChanged is a row's tuple id and its changer's xid, if it has one. */
typedef struct TxnVisibleTestsChanged
{
	uint64		tuple;
	bool		has_changer;
	HostXid		changer;
} TxnVisibleTestsChanged;

/* TxnVisibleTestsConflict is a conflict a serializable read found: its row's tuple id and its writer's xid. */
typedef struct TxnVisibleTestsConflict
{
	uint64		tuple;
	uint64		xid;
} TxnVisibleTestsConflict;

/* txn_visible_tests_u64s_Debug_fmt is a Vec<u64>'s Debug. */
static bool
txn_visible_tests_u64s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *u64s = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < u64s->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(u64s, uint64, k), std_u64_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_found_Debug_fmt is a found row's Debug, its tuple id and its Dirty. */
static bool
txn_visible_tests_found_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsFound *found = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&found->tuple, std_u64_Debug_fmt),
								  STD_ITEM(&found->dirty, txn_visible_Dirty_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_visible_tests_founds_Debug_fmt is a Vec of found rows' Debug. */
static bool
txn_visible_tests_founds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *founds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < founds->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(founds, TxnVisibleTestsFound, k), txn_visible_tests_found_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_read_Debug_fmt is a read's Debug, its rows and its kept base rows. */
static bool
txn_visible_tests_read_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsRead *read = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&read->rows, txn_visible_tests_founds_Debug_fmt),
								  STD_ITEM(&read->kept, txn_visible_tests_u64s_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_visible_tests_changer_Debug_fmt is an Option<Xid>'s Debug, a changer's xid. */
static bool
txn_visible_tests_changer_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsChanged *changed = value;
	StdOptionRef changer = {changed->has_changer, STD_ITEM(&changed->changer, host_Xid_Debug_fmt)};

	return std_option_Debug_fmt(&changer, f);
}

/* txn_visible_tests_changed_Debug_fmt is a changed row's Debug, its tuple id and its changer's xid. */
static bool
txn_visible_tests_changed_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsChanged *changed = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&changed->tuple, std_u64_Debug_fmt),
								  STD_ITEM(changed, txn_visible_tests_changer_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_visible_tests_changeds_Debug_fmt is a Vec of changed rows' Debug. */
static bool
txn_visible_tests_changeds_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *changeds = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < changeds->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(changeds, TxnVisibleTestsChanged, k),
							txn_visible_tests_changed_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_changers_Debug_fmt is a Vec of changers' Debug, each an Option<Xid>. */
static bool
txn_visible_tests_changers_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *changers = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < changers->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(changers, TxnVisibleTestsChanged, k),
							txn_visible_tests_changer_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_conflict_Debug_fmt is a conflict's Debug, its tuple id and its writer's xid. */
static bool
txn_visible_tests_conflict_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsConflict *conflict = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&conflict->tuple, std_u64_Debug_fmt),
								  STD_ITEM(&conflict->xid, std_u64_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_visible_tests_conflicts_Debug_fmt is a Vec of conflicts' Debug. */
static bool
txn_visible_tests_conflicts_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *conflicts = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < conflicts->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(conflicts, TxnVisibleTestsConflict, k),
							txn_visible_tests_conflict_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_key_failed is an assert_eq!'s failure naming a key's bytes, as its message `{key:?}` shows them. */
static void
txn_visible_tests_key_failed(const char *key, const void *left, StdFmtFn left_fmt, const void *right,
							 StdFmtFn right_fmt, uint32 line)
{
	StdBytes	bytes = {(const uint8 *) key, strlen(key)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&bytes, std_bytes_Debug_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};

	std_assert_failed(RUST_FILE, line, 13, "==", left, left_fmt, right, right_fmt, &arguments);
}

/* txn_visible_tests_put returns a row's put with a tuple id. */
static TxnMemtablePut
txn_visible_tests_put(uint64 tuple)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "row", 3);

	op.u.put.tuple.f0 = tuple;
	return op.u.put;
}

/* txn_visible_tests_pushing pushes a version of a key by a writer doing an op, taking the op. */
static void
txn_visible_tests_pushing(HostIoIo *io, HostSharedRecord state, const char *key, TxnMemtableVersionWriter writer,
						  TxnMemtableOp op)
{
	txn_visible_tests_pushed(io, state, key, writer, &op);
	txn_memtable_Op_drop(&op);
}

/* txn_visible_tests_versioned holds a key's versions under a hold of the delta, as table's versions reads them. */
static StdVec
txn_visible_tests_versioned(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const char *key)
{
	StdVec		versions;
	HostError	error;

	if (!txn_memtable_table_versions(io, hold, state, (const uint8 *) key, strlen(key), &versions, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return versions;
}

/* txn_visible_tests_hold returns a hold of the delta. */
static HostDeltaHold
txn_visible_tests_hold(HostIoIo *io)
{
	HostDeltaHold hold;
	HostError	error;

	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return hold;
}

/* txn_visible_tests_seen_of returns what a snapshot sees of versions, under a serializable read's bounds or none. */
static TxnVisibleSeen
txn_visible_tests_seen_of(HostIoIo *io, const HostSnapshot *snapshot, StdVec *versions, const TxnVisibleSerial *serial)
{
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, snapshot};
	TxnVisibleSeen seen;
	HostError	error;

	if (!txn_visible_seen(io, rule, versions->ptr, versions->len, serial, &seen, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return seen;
}

/* txn_visible_tests_xid returns the xid of a version's writer, or none. */
static TxnVisibleTestsChanged
txn_visible_tests_xid(uint64 tuple, const TxnMemtableVersionVersion *changer)
{
	TxnVisibleTestsChanged changed = {.tuple = tuple, .has_changer = changer != NULL};

	if (changer != NULL)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(changer);

		changed.changer.f0 = txn_memtable_version_Record_writer(&record).xid;
	}
	return changed;
}

/*
 * txn_visible_tests_read returns what a snapshot sees of a key: the tuple ids
 * of the row versions it sees, newest first, each with what a Dirty read
 * found running, and of the base's rows with the given tuple ids.
 */
static TxnVisibleTestsRead
txn_visible_tests_read(HostIoIo *io, HostSharedRecord state, const char *key, const HostSnapshot *snapshot,
					   const uint64 *base, size_t base_len)
{
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, snapshot};
	HostDeltaHold hold = txn_visible_tests_hold(io);
	StdVec		versions = txn_visible_tests_versioned(io, &hold, state, key);
	TxnVisibleSeen seen = txn_visible_tests_seen_of(io, snapshot, &versions, NULL);
	TxnVisibleTestsRead read;

	std_vec_new(&read.rows, sizeof(TxnVisibleTestsFound), _Alignof(TxnVisibleTestsFound));
	std_vec_new(&read.kept, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0; k < seen.rows.len; k++)
	{
		const TxnVisibleSeenRow *row = &STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, k);
		TxnVisibleTestsFound found = {row->row.put.tuple.f0, row->dirty};

		std_vec_push_Checked(&read.rows, &found, sizeof(found), _Alignof(TxnVisibleTestsFound));
	}
	for (size_t k = 0; k < base_len; k++)
	{
		bool		kept;
		TxnVisibleDirty dirty;
		TxnMemtableVersionVersion *changer;
		TxnMemtableVersionVersion *commander;
		HostRow		row = {base[k]};
		HostError	error;

		if (!txn_visible_Seen_base(&seen, io, rule, row, &kept, &dirty, &changer, &commander, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		if (kept)
			std_vec_push_Checked(&read.kept, &base[k], sizeof(uint64), _Alignof(uint64));
	}
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return read;
}

/* txn_visible_tests_read_is asserts at a line that a read found exactly the rows and kept base rows given, freeing it. */
static void
txn_visible_tests_read_is(uint32 line, TxnVisibleTestsRead *read, const TxnVisibleTestsFound *rows, size_t rows_len,
						  const uint64 *kept, size_t kept_len)
{
	TxnVisibleTestsRead wanted = {{(void *) rows, rows_len, rows_len}, {(void *) kept, kept_len, kept_len}};
	bool		same = read->rows.len == rows_len && read->kept.len == kept_len;

	for (size_t k = 0; same && k < rows_len; k++)
		same = STD_VEC_AT(&read->rows, TxnVisibleTestsFound, k).tuple == rows[k].tuple &&
			txn_visible_Dirty_PartialEq_eq(&STD_VEC_AT(&read->rows, TxnVisibleTestsFound, k).dirty, &rows[k].dirty);
	for (size_t k = 0; same && k < kept_len; k++)
		same = STD_VEC_AT(&read->kept, uint64, k) == kept[k];
	STD_ASSERT_EQ(line, 9, same, read, txn_visible_tests_read_Debug_fmt, &wanted, txn_visible_tests_read_Debug_fmt);
	std_vec_free_Checked(&read->rows, sizeof(TxnVisibleTestsFound), _Alignof(TxnVisibleTestsFound));
	std_vec_free_Checked(&read->kept, sizeof(uint64), _Alignof(uint64));
}

/* txn_visible_tests_dirty_is asserts at a line that a Dirty read of a key finds exactly the rows given. */
static void
txn_visible_tests_dirty_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key,
						   const TxnVisibleTestsFound *rows, size_t rows_len)
{
	SimcTaken	taken = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, false);
	HostSnapshot lent = simc_Taken_lent(&taken);
	TxnVisibleTestsRead read = txn_visible_tests_read(io, state, key, &lent, NULL, 0);

	txn_visible_tests_read_is(line, &read, rows, rows_len, NULL, 0);
	simc_Taken_drop(&taken);
}

/* txn_visible_tests_taken_sees_is asserts at a line that a taken snapshot sees exactly the tuple ids given, freeing it. */
static void
txn_visible_tests_taken_sees_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key, SimcTaken taken,
								const uint64 *wanted, size_t wanted_len)
{
	HostSnapshot lent = simc_Taken_lent(&taken);

	txn_visible_tests_sees_is(line, io, state, key, &lent, wanted, wanted_len);
	simc_Taken_drop(&taken);
}

/* txn_visible_tests_kind_seen_is asserts at a line whether a snapshot of a kind sees a row of a key, naming the key. */
static void
txn_visible_tests_kind_seen_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key,
							   HostSnapshotKind kind, bool wanted)
{
	SimcTaken	taken = simc_snapshot(kind, false);
	HostSnapshot lent = simc_Taken_lent(&taken);
	StdVec		seen = txn_visible_tests_sees(io, state, key, &lent);
	bool		got = seen.len != 0;

	if (got != wanted)
		txn_visible_tests_key_failed(key, &got, std_bool_Debug_fmt, &wanted, std_bool_Debug_fmt, line);
	std_vec_free_Checked(&seen, sizeof(uint64), _Alignof(uint64));
	simc_Taken_drop(&taken);
}

/*
 * test_txn_visible_tests_each_snapshot_kind_sees_the_versions_heaps_rule_sees:
 * the other kinds' rules: Any sees every row version; Toast every one but an
 * aborted writer's, an invalid one's among them; Self a running writer's row
 * not, and a committed one's; Dirty a running writer's row, with that writer
 * reported; a row FrozenTransactionId wrote is seen by MVCC; and
 * NonVacuumable sees a committed delete's victim while its horizon is at the
 * deleter, and not once the horizon passes the deleter.
 */
void
test_txn_visible_tests_each_snapshot_kind_sees_the_versions_heaps_rule_sees(void)
{
	static const char *const keys[] = {"run", "abort", "invalid"};
	static const bool any[] = {true, true, true};
	static const bool toast[] = {true, false, false};
	static const bool current[] = {false, false, false};
	static const uint64 one[] = {1};
	static const uint64 four[] = {4};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9342);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		running = simc_begin();
	HostXid		aborted = simc_begin();
	HostXid		deleter;
	HostXid		invalid = {0};
	HostXid		frozen = {2};
	HostXid		past;
	TxnVisibleTestsFound found = {1, txn_visible_Dirty_Default_default()};

	simc_abort(aborted);
	txn_visible_tests_written(io, state, "run", txn_visible_tests_by(running, 0, 0), 1);
	txn_visible_tests_written(io, state, "abort", txn_visible_tests_by(aborted, 0, 0), 2);
	txn_visible_tests_written(io, state, "invalid", txn_visible_tests_by(invalid, 0, 0), 3);
	txn_visible_tests_written(io, state, "frozen", txn_visible_tests_by(frozen, 0, 0), 4);
	for (size_t k = 0; k < lengthof(keys); k++)
	{
		txn_visible_tests_kind_seen_is(__LINE__, io, state, keys[k], HOST_SNAPSHOT_KIND_ANY, any[k]);
		txn_visible_tests_kind_seen_is(__LINE__, io, state, keys[k], HOST_SNAPSHOT_KIND_TOAST, toast[k]);
		txn_visible_tests_kind_seen_is(__LINE__, io, state, keys[k], HOST_SNAPSHOT_KIND_CURRENT, current[k]);
	}
	found.dirty.has_xmin = true;
	found.dirty.xmin = running;
	txn_visible_tests_dirty_is(__LINE__, io, state, "run", &found, 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "frozen", simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false), four,
									1);
	deleter = simc_begin();
	txn_visible_tests_written(io, state, "frozen", txn_visible_tests_by(deleter, 0, 0), 0);
	simc_commit(deleter);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "frozen", simc_non_vacuumable(deleter), four, 1);
	past.f0 = deleter.f0 + 1;
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "frozen", simc_non_vacuumable(past), NULL, 0);
	simc_commit(running);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "run", simc_snapshot(HOST_SNAPSHOT_KIND_CURRENT, false), one,
									1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_visible_tests_a_key_read_gives_every_row_version_the_snapshots_rule_sees:
 * a key's read gives every row version heap's rule sees, as a heap scan
 * returns each such tuple: under Dirty a running update's new row, its writer
 * reported, and its old row, its ender reported, where MVCC and Self see the
 * old row alone; a row one running transaction inserted and deleted is seen
 * under Dirty with its writer reported and its ender not, since heap's rule
 * returns at a running writer; once the update commits, Dirty and MVCC the
 * new row alone, NonVacuumable the old row beside it while its horizon is at
 * the updater, and Any both.
 */
void
test_txn_visible_tests_a_key_read_gives_every_row_version_the_snapshots_rule_sees(void)
{
	static const uint64 one[] = {1};
	static const uint64 two[] = {2};
	static const uint64 both[] = {2, 1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9344);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		writer = simc_begin();
	HostXid		updater;
	HostXid		past;
	TxnVisibleTestsFound running[2] = {{2, txn_visible_Dirty_Default_default()},
	{1, txn_visible_Dirty_Default_default()}};
	TxnVisibleTestsFound gone;
	TxnVisibleTestsFound committed = {2, txn_visible_Dirty_Default_default()};

	txn_visible_tests_written(io, state, "k", txn_visible_tests_by(writer, 0, 0), 1);
	simc_commit(writer);
	updater = simc_begin();
	txn_visible_tests_written(io, state, "k", txn_visible_tests_by(updater, 0, 0), 2);
	running[0].dirty.has_xmin = true;
	running[0].dirty.xmin = updater;
	running[1].dirty.has_xmax = true;
	running[1].dirty.xmax = updater;
	txn_visible_tests_dirty_is(__LINE__, io, state, "k", running, lengthof(running));
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false), one, 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_snapshot(HOST_SNAPSHOT_KIND_CURRENT, false), one,
									1);
	txn_visible_tests_written(io, state, "gone", txn_visible_tests_by(updater, 0, 0), 3);
	txn_visible_tests_written(io, state, "gone", txn_visible_tests_by(updater, 0, 1), 0);
	gone = (TxnVisibleTestsFound)
	{
		3, running[0].dirty
	};
	txn_visible_tests_dirty_is(__LINE__, io, state, "gone", &gone, 1);
	simc_commit(updater);
	txn_visible_tests_dirty_is(__LINE__, io, state, "k", &committed, 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false), two, 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_non_vacuumable(updater), both, 2);
	past.f0 = updater.f0 + 1;
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_non_vacuumable(past), two, 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "k", simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false), both, 2);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_visible_tests_a_row_named_by_its_tuple_id_is_ended_by_the_version_that_names_it:
 * the keys that hold more than one row. A counted key's copy is ended by the
 * removal that names its tuple id, in the delta and in the base, and by no
 * other; a row named by its tuple id is replaced by the update that names it,
 * beside its key's other row; a removal whose writer still runs has ended
 * nothing for an MVCC snapshot; and a version that sets the key's value ends
 * every older row of the key and the base's.
 */
void
test_txn_visible_tests_a_row_named_by_its_tuple_id_is_ended_by_the_version_that_names_it(void)
{
	static const uint64 counted_base[] = {10, 11, 12};
	static const uint64 rows_base[] = {10, 11};
	static const uint64 counted_kept[] = {11, 12};
	static const uint64 rows_kept[] = {11};
	static const uint64 after_kept[] = {12};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9345);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		writer = simc_begin();
	HostXid		remover;
	HostXid		setter;
	TxnMemtableVersionWriter by;
	TxnMemtableOp op;
	SimcTaken	mvcc;
	SimcTaken	after;
	HostSnapshot lent;
	TxnVisibleTestsRead read;
	TxnVisibleTestsFound counted[] = {{2, txn_visible_Dirty_Default_default()}};
	TxnVisibleTestsFound rows[] = {{3, txn_visible_Dirty_Default_default()}, {2, txn_visible_Dirty_Default_default()}};
	TxnVisibleTestsFound set[] = {{4, txn_visible_Dirty_Default_default()}};

	by = txn_visible_tests_by(writer, 0, 0);
	for (uint64 tuple = 1; tuple <= 2; tuple++)
	{
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_ADD, .u.add = txn_visible_tests_put(tuple)
		};
		txn_visible_tests_pushing(io, state, "counted", by, op);
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_ADD_ROW, .u.add_row = txn_visible_tests_put(tuple)
		};
		txn_visible_tests_pushing(io, state, "rows", by, op);
	}
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_REMOVE, .u.remove =
		{
			{
				1
			}
		}
	};
	txn_visible_tests_pushing(io, state, "counted", by, op);
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_REMOVE, .u.remove =
		{
			{
				10
			}
		}
	};
	txn_visible_tests_pushing(io, state, "counted", by, op);
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_UPDATE_ROW, .u.update_row =
		{
			{
				1
			}, txn_visible_tests_put(3)
		}
	};
	txn_visible_tests_pushing(io, state, "rows", by, op);
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_DELETE_ROW, .u.delete_row =
		{
			{
				10
			}
		}
	};
	txn_visible_tests_pushing(io, state, "rows", by, op);
	simc_commit(writer);
	remover = simc_begin();
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_REMOVE, .u.remove =
		{
			{
				11
			}
		}
	};
	txn_visible_tests_pushing(io, state, "counted", txn_visible_tests_by(remover, 0, 0), op);
	mvcc = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	lent = simc_Taken_lent(&mvcc);
	read = txn_visible_tests_read(io, state, "counted", &lent, counted_base, lengthof(counted_base));
	txn_visible_tests_read_is(__LINE__, &read, counted, lengthof(counted), counted_kept, lengthof(counted_kept));
	read = txn_visible_tests_read(io, state, "rows", &lent, rows_base, lengthof(rows_base));
	txn_visible_tests_read_is(__LINE__, &read, rows, lengthof(rows), rows_kept, lengthof(rows_kept));
	simc_commit(remover);
	setter = simc_begin();
	txn_visible_tests_written(io, state, "rows", txn_visible_tests_by(setter, 0, 0), 4);
	simc_commit(setter);
	after = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	lent = simc_Taken_lent(&after);
	read = txn_visible_tests_read(io, state, "rows", &lent, rows_base, lengthof(rows_base));
	txn_visible_tests_read_is(__LINE__, &read, set, lengthof(set), NULL, 0);
	read = txn_visible_tests_read(io, state, "counted", &lent, counted_base, lengthof(counted_base));
	txn_visible_tests_read_is(__LINE__, &read, counted, lengthof(counted), after_kept, lengthof(after_kept));
	simc_Taken_drop(&after);
	simc_Taken_drop(&mvcc);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* TxnVisibleTestsShown is a stamp case's read: each row's changer, and each named base row's. */
typedef struct TxnVisibleTestsShown
{
	/* rows is a Vec<TxnVisibleTestsChanged>. */
	StdVec		rows;
	/* based is a Vec<TxnVisibleTestsChanged>, whose tuple ids are the base's. */
	StdVec		based;
} TxnVisibleTestsShown;

/* txn_visible_tests_shown returns each row version an Any snapshot sees of a key with its changer, and each base row's. */
static TxnVisibleTestsShown
txn_visible_tests_shown(HostIoIo *io, HostSharedRecord state, const char *key, const HostSnapshot *any,
						const uint64 *base, size_t base_len)
{
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, any};
	HostDeltaHold hold = txn_visible_tests_hold(io);
	StdVec		versions = txn_visible_tests_versioned(io, &hold, state, key);
	TxnVisibleSeen seen = txn_visible_tests_seen_of(io, any, &versions, NULL);
	TxnVisibleTestsShown shown;

	std_vec_new(&shown.rows, sizeof(TxnVisibleTestsChanged), _Alignof(TxnVisibleTestsChanged));
	std_vec_new(&shown.based, sizeof(TxnVisibleTestsChanged), _Alignof(TxnVisibleTestsChanged));
	for (size_t k = 0; k < seen.rows.len; k++)
	{
		const TxnVisibleSeenRow *row = &STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, k);
		TxnVisibleTestsChanged changed = txn_visible_tests_xid(row->row.put.tuple.f0, row->changer);

		std_vec_push_Checked(&shown.rows, &changed, sizeof(changed), _Alignof(TxnVisibleTestsChanged));
	}
	for (size_t k = 0; k < base_len; k++)
	{
		bool		kept;
		TxnVisibleDirty dirty;
		TxnMemtableVersionVersion *changer;
		TxnMemtableVersionVersion *commander;
		TxnVisibleTestsChanged changed;
		HostRow		row = {base[k]};
		HostError	error;

		if (!txn_visible_Seen_base(&seen, io, rule, row, &kept, &dirty, &changer, &commander, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		changed = txn_visible_tests_xid(base[k], changer);
		std_vec_push_Checked(&shown.based, &changed, sizeof(changed), _Alignof(TxnVisibleTestsChanged));
	}
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return shown;
}

/* txn_visible_tests_changed_eq returns whether two Vecs of changed rows hold the same, comparing tuple ids only when rows is set. */
static bool
txn_visible_tests_changed_eq(const StdVec *got, const TxnVisibleTestsChanged *wanted, size_t wanted_len, bool rows)
{
	bool		same = got->len == wanted_len;

	for (size_t k = 0; same && k < wanted_len; k++)
	{
		const TxnVisibleTestsChanged *one = &STD_VEC_AT(got, TxnVisibleTestsChanged, k);

		same = (!rows || one->tuple == wanted[k].tuple) && one->has_changer == wanted[k].has_changer &&
			(!one->has_changer || one->changer.f0 == wanted[k].changer.f0);
	}
	return same;
}

/* txn_visible_tests_shown_is asserts at a line that a key's rows show the changers given, freeing what it read. */
static void
txn_visible_tests_shown_is(uint32 line, TxnVisibleTestsShown *shown, const TxnVisibleTestsChanged *wanted,
						   size_t wanted_len)
{
	StdVec		want = {(void *) wanted, wanted_len, wanted_len};

	STD_ASSERT_EQ(line, 9, txn_visible_tests_changed_eq(&shown->rows, wanted, wanted_len, true), &shown->rows,
				  txn_visible_tests_changeds_Debug_fmt, &want, txn_visible_tests_changeds_Debug_fmt);
	std_vec_free_Checked(&shown->rows, sizeof(TxnVisibleTestsChanged), _Alignof(TxnVisibleTestsChanged));
	std_vec_free_Checked(&shown->based, sizeof(TxnVisibleTestsChanged), _Alignof(TxnVisibleTestsChanged));
}

/* txn_visible_tests_changed returns a row's tuple id with its changer's xid, an xid of zero for none. */
static TxnVisibleTestsChanged
txn_visible_tests_changed(uint64 tuple, HostXid changer)
{
	TxnVisibleTestsChanged changed = {.tuple = tuple, .has_changer = changer.f0 != 0, .changer = changer};

	return changed;
}

/* txn_visible_tests_lock returns a lock of a key, or with a tuple id of a row it names, in update mode. */
static TxnMemtableOp
txn_visible_tests_lock(uint64 row)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_LOCK, .u.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE};

	if (row != 0)
		op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_LOCK_ROW, .u.lock_row =
		{
			{
				row
			}, TXN_MEMTABLE_LOCK_MODE_UPDATE
		}
	};
	return op;
}

/*
 * test_txn_visible_tests_a_rows_stamp_shows_the_last_change_above_it_whatever_its_outcome:
 * decision 4's stamp (bugs row 60): a row's changer is its ender, or with
 * none the newest version above it that sets or locks its key or names it,
 * whatever its outcome, as heap's raw xmax is the last change that set it. A
 * row with an aborted update above it shows the aborted writer; an aborted
 * update and then a running lock, the locker; a committed lock and then a
 * committed delete, the deleter, read under Any, which sees the deleted row;
 * a row whose committed update's new row was then locked shows the updater,
 * and the new row the locker; a row deleted and inserted again at its key
 * shows the deleter; a row named by its tuple id shows the version that names
 * it and not the one that names its key's other row; and a base row the
 * same.
 */
void
test_txn_visible_tests_a_rows_stamp_shows_the_last_change_above_it_whatever_its_outcome(void)
{
	static const uint64 base[] = {20, 21};
	static const HostXid none = {0};
	const TxnMemtableOp unnamed = {.tag = TXN_MEMTABLE_OP_DELETE_ROW, .u.delete_row = {{20}}};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9346);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		writer = simc_begin();
	HostXid		aborted = simc_begin();
	HostXid		locker = simc_begin();
	HostXid		deleter = simc_begin();
	SimcTaken	any;
	HostSnapshot lent;
	TxnVisibleTestsShown shown;
	TxnVisibleTestsChanged updated[2];
	TxnVisibleTestsChanged locked[2];
	TxnVisibleTestsChanged deleted[1];
	TxnVisibleTestsChanged relocked[2];
	TxnVisibleTestsChanged again[2];
	TxnVisibleTestsChanged rows[2];
	TxnVisibleTestsChanged based[2];
	StdVec		want = {based, 2, 2};

	txn_visible_tests_written(io, state, "updated", txn_visible_tests_by(writer, 0, 0), 1);
	txn_visible_tests_written(io, state, "updated", txn_visible_tests_by(aborted, 0, 0), 2);
	txn_visible_tests_written(io, state, "locked", txn_visible_tests_by(writer, 0, 0), 3);
	txn_visible_tests_written(io, state, "locked", txn_visible_tests_by(aborted, 0, 0), 4);
	txn_visible_tests_pushing(io, state, "locked", txn_visible_tests_by(locker, 0, 0), txn_visible_tests_lock(0));
	txn_visible_tests_written(io, state, "deleted", txn_visible_tests_by(writer, 0, 0), 5);
	txn_visible_tests_pushing(io, state, "deleted", txn_visible_tests_by(writer, 0, 1), txn_visible_tests_lock(0));
	txn_visible_tests_written(io, state, "deleted", txn_visible_tests_by(deleter, 0, 0), 0);
	for (uint64 tuple = 6; tuple <= 7; tuple++)
	{
		TxnMemtableOp added = {.tag = TXN_MEMTABLE_OP_ADD_ROW};

		added.u.add_row = txn_visible_tests_put(tuple);
		txn_visible_tests_pushing(io, state, "rows", txn_visible_tests_by(writer, 0, 0), added);
	}
	txn_visible_tests_written(io, state, "relocked", txn_visible_tests_by(writer, 0, 0), 8);
	txn_visible_tests_written(io, state, "relocked", txn_visible_tests_by(deleter, 0, 0), 9);
	txn_visible_tests_pushing(io, state, "relocked", txn_visible_tests_by(locker, 0, 0), txn_visible_tests_lock(0));
	txn_visible_tests_written(io, state, "again", txn_visible_tests_by(writer, 0, 0), 10);
	txn_visible_tests_written(io, state, "again", txn_visible_tests_by(deleter, 0, 0), 0);
	txn_visible_tests_written(io, state, "again", txn_visible_tests_by(locker, 0, 0), 11);
	txn_visible_tests_pushing(io, state, "rows", txn_visible_tests_by(locker, 0, 0), txn_visible_tests_lock(6));
	txn_visible_tests_pushing(io, state, "rows", txn_visible_tests_by(aborted, 0, 0), unnamed);
	simc_abort(aborted);
	simc_commit(writer);
	simc_commit(deleter);
	any = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	lent = simc_Taken_lent(&any);
	updated[0] = txn_visible_tests_changed(2, none);
	updated[1] = txn_visible_tests_changed(1, aborted);
	shown = txn_visible_tests_shown(io, state, "updated", &lent, NULL, 0);
	txn_visible_tests_shown_is(__LINE__, &shown, updated, lengthof(updated));
	locked[0] = txn_visible_tests_changed(4, locker);
	locked[1] = txn_visible_tests_changed(3, locker);
	shown = txn_visible_tests_shown(io, state, "locked", &lent, NULL, 0);
	txn_visible_tests_shown_is(__LINE__, &shown, locked, lengthof(locked));
	deleted[0] = txn_visible_tests_changed(5, deleter);
	shown = txn_visible_tests_shown(io, state, "deleted", &lent, NULL, 0);
	txn_visible_tests_shown_is(__LINE__, &shown, deleted, lengthof(deleted));
	relocked[0] = txn_visible_tests_changed(9, locker);
	relocked[1] = txn_visible_tests_changed(8, deleter);
	shown = txn_visible_tests_shown(io, state, "relocked", &lent, NULL, 0);
	txn_visible_tests_shown_is(__LINE__, &shown, relocked, lengthof(relocked));
	again[0] = txn_visible_tests_changed(11, none);
	again[1] = txn_visible_tests_changed(10, deleter);
	shown = txn_visible_tests_shown(io, state, "again", &lent, NULL, 0);
	txn_visible_tests_shown_is(__LINE__, &shown, again, lengthof(again));
	rows[0] = txn_visible_tests_changed(7, none);
	rows[1] = txn_visible_tests_changed(6, locker);
	based[0] = txn_visible_tests_changed(20, aborted);
	based[1] = txn_visible_tests_changed(21, none);
	shown = txn_visible_tests_shown(io, state, "rows", &lent, base, lengthof(base));
	STD_ASSERT_EQ(__LINE__, 9, txn_visible_tests_changed_eq(&shown.based, based, lengthof(based), false),
				  &shown.based, txn_visible_tests_changers_Debug_fmt, &want, txn_visible_tests_changers_Debug_fmt);
	txn_visible_tests_shown_is(__LINE__, &shown, rows, lengthof(rows));
	simc_Taken_drop(&any);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* TxnVisibleTestsCase is a row of the historic case: its key, its writer and command, its deleter and command, and whether it is seen. */
typedef struct TxnVisibleTestsCase
{
	const char *key;
	int			writer;
	uint32		command;
	/* deleter is the deleter's place among the case's xids, or -1 for none. */
	int			deleter;
	uint32		deleted;
	bool		seen;
} TxnVisibleTestsCase;

/* txn_visible_tests_named_sees_is asserts at a line that a snapshot sees exactly the tuple ids given of a key, naming the key. */
static void
txn_visible_tests_named_sees_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key,
								const HostSnapshot *snapshot, const uint64 *wanted, size_t wanted_len)
{
	StdVec		seen = txn_visible_tests_sees(io, state, key, snapshot);
	StdVec		want = {(void *) wanted, wanted_len, wanted_len};
	bool		same = seen.len == wanted_len;

	for (size_t k = 0; same && k < wanted_len; k++)
		same = STD_VEC_AT(&seen, uint64, k) == wanted[k];
	if (!same)
	{
		StdStr		name = {key, strlen(key)};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_assert_failed(RUST_FILE, line, 13, "==", &seen, txn_visible_tests_u64s_Debug_fmt, &want,
						  txn_visible_tests_u64s_Debug_fmt, &arguments);
	}
	std_vec_free_Checked(&seen, sizeof(uint64), _Alignof(uint64));
}

/*
 * test_txn_visible_tests_a_historic_snapshot_sees_its_transactions_rows_by_their_commands:
 * Historic MVCC's rule, heap's HeapTupleSatisfiesHistoricMVCC, under a
 * snapshot whose bounds are two of the case's xids, whose xip holds one xid
 * that committed between them, and whose subxip holds the decoded
 * transaction. Another writer's row is seen when the writer committed below
 * xmin or is in xip, and not when it aborted, isn't in xip, or is at xmax; a
 * delete counts by the same rule. The decoded transaction's row is seen from
 * the command after the one its stamp names, and not at that command; its
 * delete hides a row from the command after the deleter's, and not before
 * (CommPF 7's ruling (S), Differences row 39).
 */
void
test_txn_visible_tests_a_historic_snapshot_sees_its_transactions_rows_by_their_commands(void)
{
	enum
	{
		OLD, ABORTED, LISTED, UNLISTED, OWN, LATER,
	};
	static const TxnVisibleTestsCase cases[] = {
		{"old", OLD, 0, -1, 0, true},
		{"aborted", ABORTED, 0, -1, 0, false},
		{"listed", LISTED, 0, -1, 0, true},
		{"unlisted", UNLISTED, 0, -1, 0, false},
		{"later", LATER, 0, -1, 0, false},
		{"old deleted by old", OLD, 0, OLD, 1, false},
		{"old deleted by aborted", OLD, 0, ABORTED, 0, true},
		{"old deleted by listed", OLD, 0, LISTED, 0, false},
		{"old deleted by unlisted", OLD, 0, UNLISTED, 0, true},
		{"old deleted by later", OLD, 0, LATER, 0, true},
		{"own before", OWN, 4, -1, 0, true},
		{"own at the command", OWN, 5, -1, 0, false},
		{"old deleted by own before", OLD, 0, OWN, 4, false},
		{"old deleted by own at the command", OLD, 0, OWN, 5, true},
		{"own deleted by own", OWN, 2, OWN, 3, false},
	};
	static const uint64 fifteen[] = {15};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9343);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		xids[6];
	SimcTaken	taken;
	SimcTaken	earlier;
	HostSnapshot lent;
	HostCommand at_five = {5};
	HostCommand at_three = {3};

	for (size_t k = 0; k < lengthof(xids); k++)
		xids[k] = simc_begin();
	simc_abort(xids[ABORTED]);
	simc_commit(xids[OLD]);
	simc_commit(xids[LISTED]);
	simc_commit(xids[UNLISTED]);
	simc_commit(xids[OWN]);
	simc_commit(xids[LATER]);
	for (size_t k = 0; k < lengthof(cases); k++)
	{
		txn_visible_tests_written(io, state, cases[k].key,
								  txn_visible_tests_by(xids[cases[k].writer], 0, cases[k].command), k + 1);
		if (cases[k].deleter >= 0)
			txn_visible_tests_written(io, state, cases[k].key,
									  txn_visible_tests_by(xids[cases[k].deleter], 0, cases[k].deleted), 0);
	}
	taken = simc_historic(xids[LISTED], xids[LATER], &xids[LISTED], 1, &xids[OWN], 1, at_five);
	lent = simc_Taken_lent(&taken);
	for (size_t k = 0; k < lengthof(cases); k++)
	{
		uint64		tuple = k + 1;

		txn_visible_tests_named_sees_is(__LINE__, io, state, cases[k].key, &lent, &tuple, cases[k].seen ? 1 : 0);
	}
	earlier = simc_historic(xids[LISTED], xids[LATER], &xids[LISTED], 1, &xids[OWN], 1, at_three);
	lent = simc_Taken_lent(&earlier);
	txn_visible_tests_sees_is(__LINE__, io, state, "own deleted by own", &lent, fifteen, 1);
	txn_visible_tests_sees_is(__LINE__, io, state, "own before", &lent, NULL, 0);
	simc_Taken_drop(&earlier);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_visible_tests_conflicts returns the writers a serializable read of a key conflicts out with, newest first, each with its row's tuple id. */
static StdVec
txn_visible_tests_conflicts(HostIoIo *io, HostSharedRecord state, const char *key, const HostSnapshot *snapshot,
							const TxnVisibleSerial *serial)
{
	HostDeltaHold hold = txn_visible_tests_hold(io);
	StdVec		versions = txn_visible_tests_versioned(io, &hold, state, key);
	TxnVisibleSeen seen = txn_visible_tests_seen_of(io, snapshot, &versions, serial);
	StdVec		conflicts;

	std_vec_new(&conflicts, sizeof(TxnVisibleTestsConflict), _Alignof(TxnVisibleTestsConflict));
	for (size_t k = 0; k < seen.conflicts.len; k++)
	{
		const txn_TupleRowXid_t *found = &STD_VEC_AT(&seen.conflicts, txn_TupleRowXid_t, k);
		TxnVisibleTestsConflict conflict = {found->f0.f0, found->f1.f0};

		std_vec_push_Checked(&conflicts, &conflict, sizeof(conflict), _Alignof(TxnVisibleTestsConflict));
	}
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return conflicts;
}

/* txn_visible_tests_conflicts_is asserts at a line that a serializable read of a key conflicts out with exactly the writers given. */
static void
txn_visible_tests_conflicts_is(uint32 line, HostIoIo *io, HostSharedRecord state, const char *key,
							   const SimcTaken *taken, const TxnVisibleSerial *serial,
							   const TxnVisibleTestsConflict *wanted, size_t wanted_len)
{
	HostSnapshot lent = simc_Taken_lent(taken);
	StdVec		got = txn_visible_tests_conflicts(io, state, key, &lent, serial);
	StdVec		want = {(void *) wanted, wanted_len, wanted_len};
	bool		same = got.len == wanted_len;

	for (size_t k = 0; same && k < wanted_len; k++)
		same = STD_VEC_AT(&got, TxnVisibleTestsConflict, k).tuple == wanted[k].tuple &&
			STD_VEC_AT(&got, TxnVisibleTestsConflict, k).xid == wanted[k].xid;
	STD_ASSERT_EQ(line, 9, same, &got, txn_visible_tests_conflicts_Debug_fmt, &want,
				  txn_visible_tests_conflicts_Debug_fmt);
	std_vec_free_Checked(&got, sizeof(TxnVisibleTestsConflict), _Alignof(TxnVisibleTestsConflict));
}

/* txn_visible_tests_base_conflict_is asserts at a line the writer a base row's serializable read conflicts out with, an xid of zero for none. */
static void
txn_visible_tests_base_conflict_is(uint32 line, HostIoIo *io, const TxnVisibleSeen *seen,
								   const TxnVisibleSerial *serial, bool visible, HostXid wanted)
{
	bool		has = false;
	HostXid		xid = {0};
	HostError	error;
	StdOptionRef got;
	StdOptionRef want = {wanted.f0 != 0, STD_ITEM(&wanted, host_Xid_Debug_fmt)};
	HostRow		nine = {9};

	if (!txn_visible_Seen_base_conflict(seen, io, serial, nine, 2, visible, &has, &xid, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	got = (StdOptionRef)
	{
		has, STD_ITEM(&xid, host_Xid_Debug_fmt)
	};
	STD_ASSERT_EQ(line, 9, has == (wanted.f0 != 0) && (!has || xid.f0 == wanted.f0), &got, std_option_Debug_fmt,
				  &want, std_option_Debug_fmt);
}

/*
 * test_txn_visible_tests_a_serializable_read_conflicts_out_with_the_writers_heaps_rule_names:
 * HeapCheckForSerializableConflictOut's cases over the delta: a running
 * writer's row the read doesn't see conflicts with its writer's top-level
 * transaction; a committed row the read sees conflicts with nothing, and its
 * running deleter's when the read sees the row it deletes; an aborted
 * writer's row and the read's own conflict with nothing; a row its deleter
 * committed below the read's xmin is dead and conflicts with nothing; and a
 * base row a running deleter ends conflicts with that deleter while the read
 * sees it.
 */
void
test_txn_visible_tests_a_serializable_read_conflicts_out_with_the_writers_heaps_rule_names(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9346);
	HostSharedRecord state = host_shared_Held_record(&held);
	const TxnVisibleSerial floor = {.xmin = {TXN_STATUS_FIRST_NORMAL}};
	TxnVisibleSerial own = floor;
	TxnVisibleSerial past = {0};
	TxnVisibleSerial between = {0};
	HostXid		top = simc_begin();
	HostXid		sub = simc_begin_under(top);
	HostXid		writer;
	HostXid		deleter;
	HostXid		aborted;
	HostXid		ender;
	HostXid		early;
	HostXid		late;
	SimcTaken	during;
	SimcTaken	before;
	SimcTaken	after;
	SimcTaken	now;
	HostSnapshot lent;
	HostDeltaHold hold;
	StdVec		versions;
	TxnVisibleSeen seen;
	TxnVisibleTestsConflict running;
	TxnVisibleTestsConflict deleting;
	HostXid		none = {0};

	txn_visible_tests_written(io, state, "running", txn_visible_tests_by(sub, top.f0, 0), 1);
	during = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	running = (TxnVisibleTestsConflict)
	{
		1, top.f0
	};
	txn_visible_tests_conflicts_is(__LINE__, io, state, "running", &during, &floor, &running, 1);
	own.has_own = true;
	own.own = top;
	txn_visible_tests_conflicts_is(__LINE__, io, state, "running", &during, &own, NULL, 0);

	writer = simc_begin();
	txn_visible_tests_written(io, state, "live", txn_visible_tests_by(writer, 0, 0), 2);
	simc_commit(writer);
	deleter = simc_begin();
	before = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	txn_visible_tests_conflicts_is(__LINE__, io, state, "live", &before, &floor, NULL, 0);
	txn_visible_tests_written(io, state, "live", txn_visible_tests_by(deleter, 0, 0), 0);
	deleting = (TxnVisibleTestsConflict)
	{
		2, deleter.f0
	};
	txn_visible_tests_conflicts_is(__LINE__, io, state, "live", &before, &floor, &deleting, 1);
	simc_commit(deleter);
	past.xmin.f0 = deleter.f0 + 1;
	txn_visible_tests_conflicts_is(__LINE__, io, state, "live", &before, &past, NULL, 0);

	aborted = simc_begin();
	txn_visible_tests_written(io, state, "aborted", txn_visible_tests_by(aborted, 0, 0), 3);
	simc_abort(aborted);
	after = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	txn_visible_tests_conflicts_is(__LINE__, io, state, "aborted", &after, &floor, NULL, 0);

	ender = simc_begin();
	txn_visible_tests_written(io, state, "based", txn_visible_tests_by(ender, 0, 0), 0);
	hold = txn_visible_tests_hold(io);
	versions = txn_visible_tests_versioned(io, &hold, state, "based");
	lent = simc_Taken_lent(&after);
	seen = txn_visible_tests_seen_of(io, &lent, &versions, &floor);
	txn_visible_tests_base_conflict_is(__LINE__, io, &seen, &floor, true, ender);
	txn_visible_tests_base_conflict_is(__LINE__, io, &seen, &floor, false, none);
	txn_visible_Seen_drop(&seen);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);

	early = simc_begin();
	late = simc_begin();
	txn_visible_tests_written(io, state, "dead", txn_visible_tests_by(late, 0, 0), 4);
	simc_commit(late);
	txn_visible_tests_written(io, state, "dead", txn_visible_tests_by(early, 0, 0), 0);
	simc_commit(early);
	between.xmin.f0 = early.f0 + 1;
	now = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	txn_visible_tests_conflicts_is(__LINE__, io, state, "dead", &now, &between, NULL, 0);
	simc_Taken_drop(&now);
	simc_Taken_drop(&after);
	simc_Taken_drop(&before);
	simc_Taken_drop(&during);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_visible_tests_a_transactions_own_row_is_seen_from_the_command_after_its_write:
 * heap's MVCC rule over the caller's own write: a row the transaction wrote
 * is seen by a snapshot whose command is past the write's, and not by one
 * taken at the write's own command.
 */
void
test_txn_visible_tests_a_transactions_own_row_is_seen_from_the_command_after_its_write(void)
{
	static const uint64 one[] = {1};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9347);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		sub;
	HostXid		top;
	HostCommand command;
	HostError	error;

	if (!host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &error) ||
		!host_current_command(HOST_IO_IO_IO_DOORS(io), true, &command, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_visible_tests_written(io, state, "own", txn_visible_tests_by(sub, top.f0, command.f0), 1);
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "own", simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false), NULL,
									0);
	simc_next_command();
	txn_visible_tests_taken_sees_is(__LINE__, io, state, "own", simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false), one, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* TxnVisibleTestsXids is a slice of xids, as an answer's text shows them. */
typedef struct TxnVisibleTestsXids
{
	const HostXid *ptr;
	size_t		len;
} TxnVisibleTestsXids;

/* txn_visible_tests_xids_Debug_fmt is a slice of xids' Debug. */
static bool
txn_visible_tests_xids_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnVisibleTestsXids *xids = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < xids->len; k++)
		std_DebugList_entry(&list, &xids->ptr[k], host_Xid_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_format returns a String of a template's pieces and arguments, as format! makes it. */
static StdString
txn_visible_tests_format(const StdFmtPiece *pieces, size_t pieces_len, const StdFmtArg *args)
{
	StdFmtArguments arguments = {pieces, pieces_len, args};
	StdString text;

	std_fmt_format_Checked(&text, &arguments);
	return text;
}

/* txn_visible_tests_answer_text returns an updatable answer's text as the case words it: its kind, the xids it names, and the one it is over. */
static StdString
txn_visible_tests_answer_text(TxnVisibleUpdatableTag tag, const HostXid *by, size_t by_len, const HostXid *over)
{
	TxnVisibleTestsXids holders = {by, by_len};

	switch (tag)
	{
		case TXN_VISIBLE_UPDATABLE_INVISIBLE:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("invisible")};

				return txn_visible_tests_format(pieces, lengthof(pieces), NULL);
			}
		case TXN_VISIBLE_UPDATABLE_SELF_MODIFIED:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("self-modified by "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{by, host_Xid_Debug_fmt}};

				return txn_visible_tests_format(pieces, lengthof(pieces), args);
			}
		case TXN_VISIBLE_UPDATABLE_BEING_MODIFIED:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("being modified by "), STD_FMT_ARG(0), STD_FMT_LIT(" over "),
				STD_FMT_ARG(1)};
				StdFmtArg	args[] = {{&holders, txn_visible_tests_xids_Debug_fmt}, {over, host_Xid_Debug_fmt}};

				return txn_visible_tests_format(pieces, over != NULL ? 4 : 2, args);
			}
		case TXN_VISIBLE_UPDATABLE_UPDATED:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("updated by "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{by, host_Xid_Debug_fmt}};

				return txn_visible_tests_format(pieces, lengthof(pieces), args);
			}
		case TXN_VISIBLE_UPDATABLE_DELETED:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("deleted by "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{by, host_Xid_Debug_fmt}};

				return txn_visible_tests_format(pieces, lengthof(pieces), args);
			}
		default:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("ok")};

				return txn_visible_tests_format(pieces, lengthof(pieces), NULL);
			}
	}
}

/* txn_visible_tests_writer_xid returns the xid of a version's writer. */
static HostXid
txn_visible_tests_writer_xid(const TxnMemtableVersionVersion *version)
{
	return txn_visible_tests_xid(0, version).changer;
}

/* txn_visible_tests_row_of returns a version's row, panicking as Option::unwrap does when it writes none. */
static TxnVisibleRowVersion
txn_visible_tests_row_of(TxnMemtableVersionVersion *version, uint32 line)
{
	TxnVisibleRowVersion row;

	if (!txn_visible_RowVersion_of(version, &row))
		std_panic(RUST_FILE, line, 0, "called `Option::unwrap()` on a `None` value",
				  sizeof("called `Option::unwrap()` on a `None` value") - 1);
	return row;
}

/* txn_visible_tests_updatable returns what updatable answers of a change of a row over versions, at command 1. */
static TxnVisibleUpdatable
txn_visible_tests_updatable(HostIoIo *io, StdVec *versions, TxnVisibleChanged changed)
{
	TxnVisibleUpdatable answer;
	HostError	error;

	if (!txn_visible_updatable(io, versions->ptr, versions->len, changed, 1, &answer, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return answer;
}

/*
 * txn_visible_tests_asked returns what updatable answers of a key's row as
 * the case words it: its oldest row version's unless based, the newest
 * version's when based with tuple id zero, and the base's row of the tuple
 * id when based with any other.
 */
static StdString
txn_visible_tests_asked(HostIoIo *io, HostSharedRecord state, const char *key, bool based, uint64 tuple)
{
	HostDeltaHold hold = txn_visible_tests_hold(io);
	StdVec		versions = txn_visible_tests_versioned(io, &hold, state, key);
	TxnMemtableVersionVersion *all = versions.ptr;
	TxnVisibleChanged changed = {.tag = TXN_VISIBLE_CHANGED_DELTA};
	TxnVisibleUpdatable answer;
	StdVec		by;
	HostXid		xid;
	HostXid		over;
	bool		has_over = false;
	StdString text;

	if (based && tuple == 0)
		changed.u.delta.row = txn_visible_tests_row_of(&all[0], __LINE__);
	else if (based)
	{
		changed.tag = TXN_VISIBLE_CHANGED_BASE;
		changed.u.base.f0 = tuple;
	}
	else
	{
		size_t		at = versions.len;
		bool		found = false;

		while (!found && at-- > 0)
			found = txn_visible_RowVersion_of(&all[at], &changed.u.delta.row);
		if (!found)
			std_panic(RUST_FILE, __LINE__, 0, "called `Option::unwrap()` on a `None` value",
					  sizeof("called `Option::unwrap()` on a `None` value") - 1);
		changed.u.delta.at = at;
	}
	answer = txn_visible_tests_updatable(io, &versions, changed);
	std_vec_new(&by, sizeof(HostXid), _Alignof(HostXid));
	switch (answer.tag)
	{
		case TXN_VISIBLE_UPDATABLE_SELF_MODIFIED:
			xid = txn_visible_tests_writer_xid(answer.u.self_modified);
			std_vec_push_Checked(&by, &xid, sizeof(xid), _Alignof(HostXid));
			break;
		case TXN_VISIBLE_UPDATABLE_BEING_MODIFIED:
			for (size_t k = 0; k < answer.u.being_modified.holders.len; k++)
			{
				xid = txn_visible_tests_writer_xid(STD_VEC_AT(&answer.u.being_modified.holders,
															  TxnMemtableVersionVersion *, k));
				std_vec_push_Checked(&by, &xid, sizeof(xid), _Alignof(HostXid));
			}
			has_over = answer.u.being_modified.ended != NULL;
			if (has_over)
				over = txn_visible_tests_writer_xid(answer.u.being_modified.ended);
			break;
		case TXN_VISIBLE_UPDATABLE_UPDATED:
			xid = txn_visible_tests_writer_xid(answer.u.updated);
			std_vec_push_Checked(&by, &xid, sizeof(xid), _Alignof(HostXid));
			break;
		case TXN_VISIBLE_UPDATABLE_DELETED:
			xid = txn_visible_tests_writer_xid(answer.u.deleted);
			std_vec_push_Checked(&by, &xid, sizeof(xid), _Alignof(HostXid));
			break;
		default:
			break;
	}
	text = txn_visible_tests_answer_text(answer.tag, by.ptr, by.len, has_over ? &over : NULL);

	std_vec_free_Checked(&by, sizeof(HostXid), _Alignof(HostXid));

	txn_visible_Updatable_drop(&answer);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	return text;
}

/* txn_visible_tests_asked_is asserts at a line that an answer's text is the one wanted, naming the key, freeing both. */
static void
txn_visible_tests_asked_is(uint32 line, StdString got, StdString wanted, const char *key)
{
	if (got.len != wanted.len || (got.len != 0 && memcmp(got.ptr, wanted.ptr, got.len) != 0))
	{
		StdStr		name = {key, key != NULL ? strlen(key) : 0};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&name, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_assert_failed(RUST_FILE, line, 13, "==", &got, std_String_Debug_fmt, &wanted, std_String_Debug_fmt,
						  key != NULL ? &arguments : NULL);
	}
	std_String_drop(&got);
	std_String_drop(&wanted);
}

/* The writers of the updatable case, by their places among its xids. */
enum
{
	TXN_VISIBLE_TESTS_COMMITTED,
	TXN_VISIBLE_TESTS_ABORTED,
	TXN_VISIBLE_TESTS_RUNNING,
	TXN_VISIBLE_TESTS_LOCKER,
	TXN_VISIBLE_TESTS_OWN,
};

/* TxnVisibleTestsWrite is a version the updatable case pushes: its writer's place, its command, and its op, of a tuple id or a lock's mode. */
typedef struct TxnVisibleTestsWrite
{
	int			who;
	uint32		command;
	TxnMemtableOpTag op;
	uint64		tuple;
	TxnMemtableLockMode mode;
} TxnVisibleTestsWrite;

/* TxnVisibleTestsKeyed is a key of the updatable case and its versions, oldest first. */
typedef struct TxnVisibleTestsKeyed
{
	const char *key;
	TxnVisibleTestsWrite writes[3];
	size_t		len;
} TxnVisibleTestsKeyed;

/* TxnVisibleTestsAnswer is an answer the updatable case wants of a key: its kind, the writer it names or -1, and the one it is over or -1. */
typedef struct TxnVisibleTestsAnswer
{
	const char *key;
	TxnVisibleUpdatableTag tag;
	int			by;
	int			over;
} TxnVisibleTestsAnswer;

/* txn_visible_tests_op returns a case's version's op. */
static TxnMemtableOp
txn_visible_tests_op(const TxnVisibleTestsWrite *write)
{
	TxnMemtableOp op = {.tag = write->op};

	switch (write->op)
	{
		case TXN_MEMTABLE_OP_PUT:
			op.u.put = txn_visible_tests_put(write->tuple);
			break;
		case TXN_MEMTABLE_OP_LOCK:
			op.u.lock = write->mode;
			break;
		case TXN_MEMTABLE_OP_MOVE:
			op.u.move.tag = TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE;
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			op.u.add_row = txn_visible_tests_put(write->tuple);
			break;
		case TXN_MEMTABLE_OP_DELETE_ROW:
			op.u.delete_row.f0.f0 = write->tuple;
			break;
		case TXN_MEMTABLE_OP_LOCK_ROW:
			op.u.lock_row.f0.f0 = write->tuple;
			op.u.lock_row.f1 = write->mode;
			break;
		default:
			break;
	}
	return op;
}

/* txn_visible_tests_wanted returns an answer's wanted text, by the case's xids. */
static StdString
txn_visible_tests_wanted(TxnVisibleUpdatableTag tag, const HostXid *xids, int by, int over)
{
	return txn_visible_tests_answer_text(tag, by >= 0 ? &xids[by] : NULL, by >= 0 ? 1 : 0,
										 over >= 0 ? &xids[over] : NULL);
}

/*
 * test_txn_visible_tests_a_rows_change_is_answered_as_heaps_satisfies_update_answers:
 * (4b)'s updatable (heap's HeapTupleSatisfiesUpdate): a row whose writer
 * aborted or another transaction runs is invisible, and the caller's own is
 * invisible at or past its command and changeable before it; a committed row
 * with nothing above it, or only an aborted change or a committed lock, is
 * changeable; another transaction's running change or lock is changing it,
 * and so is the caller's own lock, each by its version; a committed update or
 * move updated it and a committed delete deleted it, each by its version,
 * unless a locker below it still runs, which is changing it over that
 * version; the caller's own change at or past its command self-modified it
 * and before it hid it; a version naming another row of the key changes
 * nothing; a base row is asked the same way; and a row a no-key update wrote
 * over a running key-share locker's row is held by the lock it carried.
 */
void
test_txn_visible_tests_a_rows_change_is_answered_as_heaps_satisfies_update_answers(void)
{
	static const TxnVisibleTestsKeyed rows[] = {
		{"aborted", {{TXN_VISIBLE_TESTS_ABORTED, 0, TXN_MEMTABLE_OP_PUT, 1}}, 1},
		{"running", {{TXN_VISIBLE_TESTS_RUNNING, 0, TXN_MEMTABLE_OP_PUT, 2}}, 1},
		{"own late", {{TXN_VISIBLE_TESTS_OWN, 1, TXN_MEMTABLE_OP_PUT, 3}}, 1},
		{"own early", {{TXN_VISIBLE_TESTS_OWN, 0, TXN_MEMTABLE_OP_PUT, 4}}, 1},
		{"alone", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 5}}, 1},
		{"undone", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 6},
		{TXN_VISIBLE_TESTS_ABORTED, 0, TXN_MEMTABLE_OP_DELETE}}, 2},
		{"unlocked", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 7},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_LOCK, 0, TXN_MEMTABLE_LOCK_MODE_UPDATE}}, 2},
		{"changing", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 8},
		{TXN_VISIBLE_TESTS_RUNNING, 0, TXN_MEMTABLE_OP_PUT, 9}}, 2},
		{"locking", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 10},
		{TXN_VISIBLE_TESTS_LOCKER, 0, TXN_MEMTABLE_OP_LOCK, 0, TXN_MEMTABLE_LOCK_MODE_SHARE}}, 2},
		{"own lock", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 11},
		{TXN_VISIBLE_TESTS_OWN, 0, TXN_MEMTABLE_OP_LOCK, 0, TXN_MEMTABLE_LOCK_MODE_SHARE}}, 2},
		{"updated", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 12},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_PUT, 13}}, 2},
		{"deleted", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 14},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_DELETE}}, 2},
		{"moved", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 15},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_MOVE}}, 2},
		{"shared", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 16},
		{TXN_VISIBLE_TESTS_LOCKER, 0, TXN_MEMTABLE_OP_LOCK, 0, TXN_MEMTABLE_LOCK_MODE_KEY_SHARE},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_PUT, 17}}, 3},
		{"own late change", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 18},
		{TXN_VISIBLE_TESTS_OWN, 1, TXN_MEMTABLE_OP_DELETE}}, 2},
		{"own early change", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 19},
		{TXN_VISIBLE_TESTS_OWN, 0, TXN_MEMTABLE_OP_DELETE}}, 2},
		{"other row", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_ADD_ROW, 20},
		{TXN_VISIBLE_TESTS_RUNNING, 0, TXN_MEMTABLE_OP_DELETE_ROW, 21}}, 2},
		{"base", {{TXN_VISIBLE_TESTS_RUNNING, 0, TXN_MEMTABLE_OP_LOCK_ROW, 22, TXN_MEMTABLE_LOCK_MODE_UPDATE}}, 1},
		{"carried", {{TXN_VISIBLE_TESTS_COMMITTED, 0, TXN_MEMTABLE_OP_PUT, 24},
		{TXN_VISIBLE_TESTS_LOCKER, 0, TXN_MEMTABLE_OP_LOCK, 0, TXN_MEMTABLE_LOCK_MODE_KEY_SHARE},
		{TXN_VISIBLE_TESTS_COMMITTED, 1, TXN_MEMTABLE_OP_PUT, 25}}, 3},
	};
	static const TxnVisibleTestsAnswer answers[] = {
		{"aborted", TXN_VISIBLE_UPDATABLE_INVISIBLE, -1, -1},
		{"running", TXN_VISIBLE_UPDATABLE_INVISIBLE, -1, -1},
		{"own late", TXN_VISIBLE_UPDATABLE_INVISIBLE, -1, -1},
		{"own early", TXN_VISIBLE_UPDATABLE_OK, -1, -1},
		{"alone", TXN_VISIBLE_UPDATABLE_OK, -1, -1},
		{"undone", TXN_VISIBLE_UPDATABLE_OK, -1, -1},
		{"unlocked", TXN_VISIBLE_UPDATABLE_OK, -1, -1},
		{"changing", TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, TXN_VISIBLE_TESTS_RUNNING, -1},
		{"locking", TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, TXN_VISIBLE_TESTS_LOCKER, -1},
		{"own lock", TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, TXN_VISIBLE_TESTS_OWN, -1},
		{"updated", TXN_VISIBLE_UPDATABLE_UPDATED, TXN_VISIBLE_TESTS_COMMITTED, -1},
		{"deleted", TXN_VISIBLE_UPDATABLE_DELETED, TXN_VISIBLE_TESTS_COMMITTED, -1},
		{"moved", TXN_VISIBLE_UPDATABLE_UPDATED, TXN_VISIBLE_TESTS_COMMITTED, -1},
		{"shared", TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, TXN_VISIBLE_TESTS_LOCKER, TXN_VISIBLE_TESTS_COMMITTED},
		{"own late change", TXN_VISIBLE_UPDATABLE_SELF_MODIFIED, TXN_VISIBLE_TESTS_OWN, -1},
		{"own early change", TXN_VISIBLE_UPDATABLE_INVISIBLE, -1, -1},
		{"other row", TXN_VISIBLE_UPDATABLE_OK, -1, -1},
	};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9349);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		xids[5];
	HostXid		top;
	HostError	error;

	xids[TXN_VISIBLE_TESTS_COMMITTED] = simc_begin();
	xids[TXN_VISIBLE_TESTS_ABORTED] = simc_begin();
	xids[TXN_VISIBLE_TESTS_RUNNING] = simc_begin();
	xids[TXN_VISIBLE_TESTS_LOCKER] = simc_begin();
	if (!host_current_writer(HOST_IO_IO_IO_DOORS(io), &xids[TXN_VISIBLE_TESTS_OWN], &top, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (size_t k = 0; k < lengthof(rows); k++)
		for (size_t j = 0; j < rows[k].len; j++)
			txn_visible_tests_pushing(io, state, rows[k].key,
									  txn_visible_tests_by(xids[rows[k].writes[j].who], 0, rows[k].writes[j].command),
									  txn_visible_tests_op(&rows[k].writes[j]));
	simc_abort(xids[TXN_VISIBLE_TESTS_ABORTED]);
	simc_commit(xids[TXN_VISIBLE_TESTS_COMMITTED]);
	for (size_t k = 0; k < lengthof(answers); k++)
		txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, answers[k].key, false, 0),
								   txn_visible_tests_wanted(answers[k].tag, xids, answers[k].by, answers[k].over),
								   answers[k].key);
	txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, "base", true, 22),
							   txn_visible_tests_wanted(TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, xids,
														TXN_VISIBLE_TESTS_RUNNING, -1), NULL);
	txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, "base", true, 23),
							   txn_visible_tests_wanted(TXN_VISIBLE_UPDATABLE_OK, xids, -1, -1), NULL);
	txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, "carried", true, 0),
							   txn_visible_tests_wanted(TXN_VISIBLE_UPDATABLE_BEING_MODIFIED, xids,
														TXN_VISIBLE_TESTS_LOCKER, -1), NULL);
	simc_commit(xids[TXN_VISIBLE_TESTS_LOCKER]);
	txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, "shared", false, 0),
							   txn_visible_tests_wanted(TXN_VISIBLE_UPDATABLE_UPDATED, xids,
														TXN_VISIBLE_TESTS_COMMITTED, -1), NULL);
	txn_visible_tests_asked_is(__LINE__, txn_visible_tests_asked(io, state, "locking", false, 0),
							   txn_visible_tests_wanted(TXN_VISIBLE_UPDATABLE_OK, xids, -1, -1), NULL);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_visible_tests_resolved_is asserts at a line what an immediate key's resolve finds over versions, freeing the wanted answer. */
static void
txn_visible_tests_resolved_is(uint32 line, HostIoIo *io, StdVec *versions, TxnVisibleResolved *wanted)
{
	StdBytes	none = {NULL, 0};
	TxnVisibleResolved got;
	HostError	error;

	if (!txn_visible_resolved(io, versions->ptr, versions->len, none, &got, &error))
		STD_RESULT_EXPECT_FAILED(line, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT_EQ(line, 9, txn_visible_Resolved_PartialEq_eq(&got, wanted), &got, txn_visible_Resolved_Debug_fmt,
				  wanted, txn_visible_Resolved_Debug_fmt);
	txn_visible_Resolved_drop(&got);
	txn_visible_Resolved_drop(wanted);
}

/*
 * test_txn_visible_tests_a_killed_speculative_insert_is_aborted_for_every_reader_at_once:
 * (4b)'s speculative insert, CommPF 7's (b): a running transaction's insert
 * of a key, killed by its writer, is aborted for every reader at once, its
 * transaction still running: the resolve passes over it, finding the key free
 * with no wait, a Dirty read sees no row and reports nothing running, and its
 * outcome is aborted.
 */
void
test_txn_visible_tests_a_killed_speculative_insert_is_aborted_for_every_reader_at_once(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9350);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		running = simc_begin();
	HostDeltaHold hold;
	StdVec		versions;
	TxnVisibleResolved wait = {.tag = TXN_VISIBLE_RESOLVED_WAIT};
	TxnVisibleResolved free_key = {.tag = TXN_VISIBLE_RESOLVED_FREE};
	HostStatus	status;
	SimcTaken	dirty;
	HostSnapshot lent;
	TxnVisibleSeen seen;
	HostError	error;

	txn_visible_tests_written(io, state, "speculative", txn_visible_tests_by(running, 0, 0), 1);
	hold = txn_visible_tests_hold(io);
	versions = txn_visible_tests_versioned(io, &hold, state, "speculative");
	wait.u.wait.f0 = running;
	wait.u.wait.has_f1 = true;
	wait.u.wait.f1.f0 = 1;
	txn_visible_tests_resolved_is(__LINE__, io, &versions, &wait);
	txn_memtable_version_Version_kill(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, 0));
	txn_visible_tests_resolved_is(__LINE__, io, &versions, &free_key);
	if (!txn_outcome_outcome(io, &STD_VEC_AT(&versions, TxnMemtableVersionVersion, 0), &status, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, status == HOST_STATUS_ABORTED, "outcome(io, &versions[0]).unwrap() == Status::Aborted");
	dirty = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, false);
	lent = simc_Taken_lent(&dirty);
	seen = txn_visible_tests_seen_of(io, &lent, &versions, NULL);
	STD_ASSERT(__LINE__, 9, seen.rows.len == 0, "seen.rows.is_empty()");
	txn_visible_Seen_drop(&seen);
	simc_Taken_drop(&dirty);
	txn_memtable_version_versions_drop(&versions, 0);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_visible_tests_handles_Debug_fmt is a Vec of handles' Debug. */
static bool
txn_visible_tests_handles_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *handles = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < handles->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(handles, TxnMemtableVersionHandle, k),
							txn_memtable_version_Handle_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_strings_Debug_fmt is a Vec<String>'s Debug. */
static bool
txn_visible_tests_strings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *strings = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < strings->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(strings, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_visible_tests_strings_drop frees a Vec<String>. */
static void
txn_visible_tests_strings_drop(StdVec *strings)
{
	for (size_t k = 0; k < strings->len; k++)
		std_String_drop(&STD_VEC_AT(strings, StdString, k));
	std_vec_free_Checked(strings, sizeof(StdString), _Alignof(StdString));
}

/* txn_visible_tests_handle_push appends a version's handle, if there is a version. */
static void
txn_visible_tests_handle_push(StdVec *handles, const TxnMemtableVersionVersion *version)
{
	if (version != NULL)
		std_vec_push_Checked(handles, &version->handle, sizeof(version->handle), _Alignof(TxnMemtableVersionHandle));
}

/* txn_visible_tests_sighted returns a row's sight as answers words it: its dirty report, its changer's handle, and its commander's. */
static StdString
txn_visible_tests_sighted(bool base, bool kept, const TxnVisibleDirty *dirty, const TxnMemtableVersionVersion *changer,
						  const TxnMemtableVersionVersion *commander)
{
	TxnMemtableVersionHandle changed = {changer != NULL ? changer->handle.f0 : 0};
	TxnMemtableVersionHandle commanded = {commander != NULL ? commander->handle.f0 : 0};
	StdOptionRef changer_shown = {changer != NULL, STD_ITEM(&changed, txn_memtable_version_Handle_Debug_fmt)};
	StdOptionRef commander_shown = {commander != NULL, STD_ITEM(&commanded, txn_memtable_version_Handle_Debug_fmt)};
	StdFmtPiece row_pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
	StdFmtPiece base_pieces[] = {STD_FMT_LIT("base "), STD_FMT_ARG(3), STD_FMT_LIT(" "), STD_FMT_ARG(0),
	STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{dirty, txn_visible_Dirty_Debug_fmt}, {&changer_shown, std_option_Debug_fmt},
	{&commander_shown, std_option_Debug_fmt}, {&kept, std_bool_Display_fmt}};

	return base ? txn_visible_tests_format(base_pieces, lengthof(base_pieces), args) :
		txn_visible_tests_format(row_pieces, lengthof(row_pieces), args);
}

/*
 * txn_visible_tests_answers returns what seen under each of a case's
 * snapshots and updatable answer a tuple id over versions newest first, with
 * the row's place among them when they hold it: the row's sight, or the base
 * row's when they don't hold it, and updatable's shape, in a Vec<String>.
 */
static StdVec
txn_visible_tests_answers(HostIoIo *io, const HostSnapshot *snapshots, size_t snapshots_len, StdVec *versions,
						  bool has_row, size_t row, uint64 tuple)
{
	TxnVisibleChanged changed = {.tag = TXN_VISIBLE_CHANGED_BASE, .u.base = {tuple}};
	TxnVisibleUpdatable answer;
	StdVec		got;
	StdVec		handles;
	uint8		kind;
	StdString text;

	std_vec_new(&got, sizeof(StdString), _Alignof(StdString));
	for (size_t s = 0; s < snapshots_len; s++)
	{
		TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, &snapshots[s]};
		TxnVisibleSeen seen = txn_visible_tests_seen_of(io, &snapshots[s], versions, NULL);

		for (size_t k = 0; k < seen.rows.len; k++)
		{
			const TxnVisibleSeenRow *one = &STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, k);

			if (one->row.put.tuple.f0 != tuple)
				continue;
			text = txn_visible_tests_sighted(false, false, &one->dirty, one->changer, one->commander);

			std_vec_push_Checked(&got, &text, sizeof(text), _Alignof(StdString));
		}
		if (!has_row)
		{
			bool		kept;
			TxnVisibleDirty dirty;
			TxnMemtableVersionVersion *changer;
			TxnMemtableVersionVersion *commander;
			HostRow		based = {tuple};
			HostError	error;

			if (!txn_visible_Seen_base(&seen, io, rule, based, &kept, &dirty, &changer, &commander, &error))
				STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			text = txn_visible_tests_sighted(true, kept, &dirty, changer, commander);

			std_vec_push_Checked(&got, &text, sizeof(text), _Alignof(StdString));
		}
		txn_visible_Seen_drop(&seen);
	}
	if (has_row)
	{
		changed.tag = TXN_VISIBLE_CHANGED_DELTA;
		changed.u.delta.at = row;
		changed.u.delta.row = txn_visible_tests_row_of(&STD_VEC_AT(versions, TxnMemtableVersionVersion, row),
													   __LINE__);
	}
	answer = txn_visible_tests_updatable(io, versions, changed);
	std_vec_new(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	kind = (uint8) answer.tag;
	if (answer.tag == TXN_VISIBLE_UPDATABLE_SELF_MODIFIED)
		txn_visible_tests_handle_push(&handles, answer.u.self_modified);
	else if (answer.tag == TXN_VISIBLE_UPDATABLE_BEING_MODIFIED)
	{
		for (size_t k = 0; k < answer.u.being_modified.holders.len; k++)
			txn_visible_tests_handle_push(&handles, STD_VEC_AT(&answer.u.being_modified.holders,
															   TxnMemtableVersionVersion *, k));
		txn_visible_tests_handle_push(&handles, answer.u.being_modified.ended);
	}
	else if (answer.tag == TXN_VISIBLE_UPDATABLE_UPDATED)
		txn_visible_tests_handle_push(&handles, answer.u.updated);
	else if (answer.tag == TXN_VISIBLE_UPDATABLE_DELETED)
		txn_visible_tests_handle_push(&handles, answer.u.deleted);
	{
		StdTupleRef shape = STD_TUPLE(2, STD_ITEM(&kind, std_u8_Debug_fmt),
									  STD_ITEM(&handles, txn_visible_tests_handles_Debug_fmt));
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&shape, std_tuple_Debug_fmt}};

		text = txn_visible_tests_format(pieces, lengthof(pieces), args);
	}
	std_vec_push_Checked(&got, &text, sizeof(text), _Alignof(StdString));
	std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
	txn_visible_Updatable_drop(&answer);
	return got;
}

/* txn_visible_tests_strings_eq returns whether two Vec<String>s hold the same texts. */
static bool
txn_visible_tests_strings_eq(const StdVec *a, const StdVec *b)
{
	bool		same = a->len == b->len;

	for (size_t k = 0; same && k < a->len; k++)
	{
		const StdString *x = &STD_VEC_AT(a, StdString, k);
		const StdString *y = &STD_VEC_AT(b, StdString, k);

		same = x->len == y->len && (x->len == 0 || memcmp(x->ptr, y->ptr, x->len) == 0);
	}
	return same;
}

/* txn_visible_tests_whole_at returns the place of a tuple id's row among a key's whole chain, if it holds it. */
static bool
txn_visible_tests_whole_at(StdVec *whole, uint64 tuple, size_t *at)
{
	for (size_t k = 0; k < whole->len; k++)
	{
		TxnVisibleRowVersion row;

		if (txn_visible_RowVersion_of(&STD_VEC_AT(whole, TxnMemtableVersionVersion, k), &row) &&
			row.put.tuple.f0 == tuple)
		{
			*at = k;
			return true;
		}
	}
	return false;
}

/* txn_visible_tests_slice returns a row's slice of a key's versions, as row_versions gives it. */
static TxnMemtableSliceSlice
txn_visible_tests_slice(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, uint64 tuple)
{
	TxnMemtableSliceSlice slice;
	HostRow		tid = {tuple};
	HostError	error;

	if (!txn_memtable_slice_row_versions(io, hold, state, (const uint8 *) "k", 1, tid, 0, &slice, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return slice;
}

/* txn_visible_tests_next returns the next of a xorshift generator's numbers below n. */
static uint64
txn_visible_tests_next(uint64 *seed, uint64 n)
{
	*seed ^= *seed << 13;
	*seed ^= *seed >> 7;
	*seed ^= *seed << 17;
	return *seed % n;
}

/* txn_visible_tests_successor returns a delete's successor, the key "k" with a tuple id's eight big-endian bytes. */
static TxnMemtableSuccessor
txn_visible_tests_successor(uint64 tid)
{
	TxnMemtableSuccessor successor = {.tag = TXN_MEMTABLE_SUCCESSOR_KEY};
	uint8		key[9] = {'k'};

	for (int k = 0; k < 8; k++)
		key[1 + k] = (uint8) (tid >> (8 * (7 - k)));
	std_slice_to_vec_Checked(&successor.u.key, key, sizeof(key), 1, 1);
	return successor;
}

/* txn_visible_tests_random_push pushes one random version of the slice case's key, as its match chooses, by a writer. */
static void
txn_visible_tests_random_push(HostIoIo *io, HostSharedRecord state, TxnMemtableVersionWriter writer, uint64 *seed,
							  uint64 *tid)
{
	bool		named = *tid > 0;
	uint64		row = named ? 1 + txn_visible_tests_next(seed, *tid) : 0;
	uint64		choice = txn_visible_tests_next(seed, 8);
	TxnMemtableOp op = {0};

	if (choice == 0)
	{
		op.tag = TXN_MEMTABLE_OP_PUT;
		op.u.put = txn_visible_tests_put(++*tid);
	}
	else if (choice == 1)
		op.tag = TXN_MEMTABLE_OP_DELETE;
	else if (choice == 2)
	{
		op.tag = TXN_MEMTABLE_OP_LOCK;
		op.u.lock = TXN_MEMTABLE_LOCK_MODE_SHARE;
	}
	else if (choice == 3 && named)
	{
		op.tag = TXN_MEMTABLE_OP_DELETE_ROW;
		op.u.delete_row.f0.f0 = row;
	}
	else if (choice == 4 && named)
	{
		op.tag = TXN_MEMTABLE_OP_DELETE_ROW;
		op.u.delete_row.f0.f0 = row;
		op.u.delete_row.has_f1 = true;
		op.u.delete_row.f1 = txn_visible_tests_successor(++*tid);
		txn_visible_tests_pushing(io, state, "k", writer, op);
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_ADD_ROW, .u.add_row = txn_visible_tests_put(*tid)
		};
	}
	else if (choice == 5 && named)
	{
		op.tag = TXN_MEMTABLE_OP_UPDATE_ROW;
		op.u.update_row.f0.f0 = row;
		op.u.update_row.f1 = txn_visible_tests_put(++*tid);
	}
	else if (choice == 6 && named)
	{
		op.tag = TXN_MEMTABLE_OP_LOCK_ROW;
		op.u.lock_row.f0.f0 = row;
		op.u.lock_row.f1 = TXN_MEMTABLE_LOCK_MODE_KEY_SHARE;
	}
	else
	{
		op.tag = TXN_MEMTABLE_OP_ADD_ROW;
		op.u.add_row = txn_visible_tests_put(++*tid);
	}
	txn_visible_tests_pushing(io, state, "k", writer, op);
}

/* txn_visible_tests_walked_failed panics as the slice case's assert_eq! does, naming its case, tuple id, and walk. */
static void
txn_visible_tests_walked_failed(const StdVec *got, const StdVec *expected, uint32 case_number, uint64 tuple,
								uint32 walk, uint32 line)
{
	StdFmtPiece pieces[] = {STD_FMT_LIT("case "), STD_FMT_ARG(0), STD_FMT_LIT(", tuple "), STD_FMT_ARG(1),
	STD_FMT_LIT(", walk "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&case_number, std_u32_Display_fmt}, {&tuple, std_u64_Display_fmt},
	{&walk, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_assert_failed(RUST_FILE, line, 25, "==", got, txn_visible_tests_strings_Debug_fmt, expected,
					  txn_visible_tests_strings_Debug_fmt, &arguments);
}

/*
 * test_txn_visible_tests_a_rows_slice_answers_as_its_whole_chain: bugs row
 * 134: over random chains, seen and updatable answer every tuple id the same
 * over the slice row_versions gives as over the key's whole chain, at the
 * key's first walk, its second, and after more versions are pushed, with
 * writers committed, aborted, running, and the actor's own, its
 * subtransactions' among them, one rolled back between two walks.
 */
void
test_txn_visible_tests_a_rows_slice_answers_as_its_whole_chain(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		seed = UINT64CONST(0x2545f4914f6cdd1d);

	simc_end(false);
	for (uint32 case_number = 0; case_number < 60; case_number++)
	{
		HostSharedHeld held = txn_visible_tests_made(io, 9500 + case_number);
		HostSharedRecord state = host_shared_Held_record(&held);
		uint64		tid = 0;

		for (int round = 0; round < 3; round++)
		{
			uint64		pushes = 1 + txn_visible_tests_next(&seed, 12);
			SimcTaken	mvcc;
			SimcTaken	dirty;
			HostSnapshot snapshots[2];
			HostDeltaHold hold;
			StdVec		whole;

			for (uint64 push = 0; push < pushes; push++)
			{
				bool		own = txn_visible_tests_next(&seed, 5) == 0;
				bool		sub = txn_visible_tests_next(&seed, 7) == 0;
				TxnMemtableVersionWriter writer;
				uint64		ending;
				HostXid		begun;

				if (sub)
					simc_savepoint();
				if (own || sub)
				{
					HostXid		xid;
					HostXid		top;
					HostError	error;

					if (!host_current_writer(HOST_IO_IO_IO_DOORS(io), &xid, &top, &error))
						STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value",
												 &error, host_Error_Debug_fmt);
					writer = txn_visible_tests_by(xid, xid.f0 != top.f0 ? top.f0 : 0, 0);
				}
				else
					writer = txn_visible_tests_by(simc_begin(), 0, 0);
				txn_visible_tests_random_push(io, state, writer, &seed, &tid);
				ending = txn_visible_tests_next(&seed, 3);
				begun.f0 = writer.xid;
				if (sub && ending == 0)
					simc_rollback_to();
				else if (sub)
					simc_release();
				else if (!own && ending == 0)
					simc_commit(begun);
				else if (!own && ending == 1)
					simc_abort(begun);
			}
			mvcc = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
			dirty = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, false);
			snapshots[0] = simc_Taken_lent(&mvcc);
			snapshots[1] = simc_Taken_lent(&dirty);
			hold = txn_visible_tests_hold(io);
			whole = txn_visible_tests_versioned(io, &hold, state, "k");
			for (uint64 tuple = 1; tuple <= tid + 1; tuple++)
			{
				size_t		at = 0;
				bool		has_at = txn_visible_tests_whole_at(&whole, tuple, &at);
				StdVec		expected = txn_visible_tests_answers(io, snapshots, lengthof(snapshots), &whole, has_at,
																 at, tuple);

				for (uint32 walk = 0; walk < 2; walk++)
				{
					TxnMemtableSliceSlice slice = txn_visible_tests_slice(io, &hold, state, tuple);
					StdVec		got = txn_visible_tests_answers(io, snapshots, lengthof(snapshots), &slice.versions,
																slice.has_row, slice.row, tuple);

					if (!txn_visible_tests_strings_eq(&got, &expected))
						txn_visible_tests_walked_failed(&got, &expected, case_number, tuple, walk, __LINE__);
					txn_visible_tests_strings_drop(&got);
					txn_memtable_slice_Slice_drop(&slice);
				}
				txn_visible_tests_strings_drop(&expected);
			}
			txn_memtable_version_versions_drop(&whole, 0);
			host_delta_Hold_drop(&hold);
			simc_Taken_drop(&dirty);
			simc_Taken_drop(&mvcc);
		}
		host_shared_Held_drop(&held);
		simc_end(false);
	}
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_visible_tests_a_rows_slice_after_a_removal_under_its_head_answers_as_its_whole_chain:
 * bugs row 134 (orev-33 M1): a removal of a version under a key's head, as a
 * fold's drop or a prune takes one, moves the range's count, so a row's slice
 * walked before it is walked again after it and answers as the key's whole
 * chain does, the removed ender's bytes still on its page.
 */
void
test_txn_visible_tests_a_rows_slice_after_a_removal_under_its_head_answers_as_its_whole_chain(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	HostSharedRecord state;
	HostXid		writer;
	HostXid		ender;
	HostXid		locker;
	SimcTaken	mvcc;
	HostSnapshot snapshots[1];
	HostDeltaHold hold;
	StdVec		whole;
	StdVec		removed;
	TxnMemtableVersionHandle gone;
	TxnMemtableSliceSlice slice;
	StdVec		expected;
	StdVec		got;
	size_t		at = 0;
	bool		has_at = false;
	TxnMemtableOp lock = {.tag = TXN_MEMTABLE_OP_LOCK, .u.lock = TXN_MEMTABLE_LOCK_MODE_SHARE};
	HostError	error;

	simc_end(false);
	held = txn_visible_tests_made(io, 9700);
	state = host_shared_Held_record(&held);
	writer = simc_begin();
	ender = simc_begin();
	locker = simc_begin();
	txn_visible_tests_written(io, state, "k", txn_visible_tests_by(writer, 0, 0), 1);
	simc_commit(writer);
	txn_visible_tests_written(io, state, "k", txn_visible_tests_by(ender, 0, 0), 0);
	simc_commit(ender);
	txn_visible_tests_pushing(io, state, "k", txn_visible_tests_by(locker, 0, 0), lock);
	mvcc = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshots[0] = simc_Taken_lent(&mvcc);
	hold = txn_visible_tests_hold(io);
	for (int walk = 0; walk < 2; walk++)
	{
		slice = txn_visible_tests_slice(io, &hold, state, 1);
		txn_memtable_slice_Slice_drop(&slice);
	}
	whole = txn_visible_tests_versioned(io, &hold, state, "k");
	gone = STD_VEC_AT(&whole, TxnMemtableVersionVersion, 1).handle;
	txn_memtable_version_versions_drop(&whole, 0);
	if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, (const uint8 *) "k", 1, &gone, 1,
									&removed, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 9, removed.len == 1 && STD_VEC_AT(&removed, bool, 0),
			   "table::removed::<table::Rows>(io, state, b\"k\", &[gone]).unwrap() == [true]");
	std_vec_free_Checked(&removed, sizeof(bool), _Alignof(bool));
	whole = txn_visible_tests_versioned(io, &hold, state, "k");
	for (size_t k = 0; !has_at && k < whole.len; k++)
	{
		TxnVisibleRowVersion row;

		has_at = txn_visible_RowVersion_of(&STD_VEC_AT(&whole, TxnMemtableVersionVersion, k), &row);
		at = k;
	}
	expected = txn_visible_tests_answers(io, snapshots, lengthof(snapshots), &whole, has_at, at, 1);
	slice = txn_visible_tests_slice(io, &hold, state, 1);
	got = txn_visible_tests_answers(io, snapshots, lengthof(snapshots), &slice.versions, slice.has_row, slice.row, 1);
	STD_ASSERT_EQ(__LINE__, 9, txn_visible_tests_strings_eq(&got, &expected), &got, txn_visible_tests_strings_Debug_fmt,
				  &expected, txn_visible_tests_strings_Debug_fmt);
	txn_visible_tests_strings_drop(&got);
	txn_visible_tests_strings_drop(&expected);
	txn_memtable_slice_Slice_drop(&slice);
	txn_memtable_version_versions_drop(&whole, 0);
	host_delta_Hold_drop(&hold);
	simc_Taken_drop(&mvcc);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_visible_tests_a_versions_epoch_counts_the_transforms_written_before_it:
 * STORAGE_DESIGN §II.10's epochs (PLAN's base field 4): a version is at its
 * base's epoch and one more for each pending transform it was written after,
 * one whose commit ends before its writer's, its own transaction's of an
 * earlier command, and every one for a running writer.
 */
void
test_txn_visible_tests_a_versions_epoch_counts_the_transforms_written_before_it(void)
{
	static const char *const keys[] = {"early", "before", "after", "late", "running"};
	static const uint32 wanted[] = {3, 3, 4, 5, 5};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held = txn_visible_tests_made(io, 9351);
	HostSharedRecord state = host_shared_Held_record(&held);
	HostXid		early = simc_begin();
	HostXid		ddl = simc_begin();
	HostXid		later_ddl = simc_begin();
	HostXid		late = simc_begin();
	HostXid		running = simc_begin();
	TxnMemtableVersionWriter writers[5];
	HostXid		ended[4];
	HostLsn		ends[] = {100, 200, 250, 300};
	TxnVisibleLive chain[2] = {0};
	HostDeltaHold hold;
	uint32		epochs[5];
	StdTupleRef got;
	StdTupleRef want;

	writers[0] = txn_visible_tests_by(early, 0, 0);
	writers[1] = txn_visible_tests_by(ddl, 0, 1);
	writers[2] = txn_visible_tests_by(ddl, 0, 3);
	writers[3] = txn_visible_tests_by(late, 0, 0);
	writers[4] = txn_visible_tests_by(running, 0, 0);
	for (size_t k = 0; k < lengthof(keys); k++)
		txn_visible_tests_written(io, state, keys[k], writers[k], k + 1);
	ended[0] = early;
	ended[1] = ddl;
	ended[2] = later_ddl;
	ended[3] = late;
	for (size_t k = 0; k < lengthof(ended); k++)
	{
		TxnStatusOutcome outcome = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = ends[k]};
		TxnStatusRoomed roomed;
		bool		took;
		HostError	error;

		if (!txn_status_take(io, ended[k], outcome, &roomed, &took, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		simc_commit(ended[k]);
	}
	chain[0].f0 = (TxnBasePending)
	{
		.xid = ddl.f0, .command = 2, .sub = ddl.f0, .transform = STD_VEC_NEW
	};
	chain[0].has_f1 = true;
	chain[0].f1 = 200;
	chain[1].f0 = (TxnBasePending)
	{
		.xid = later_ddl.f0, .sub = later_ddl.f0, .transform = STD_VEC_NEW
	};
	chain[1].has_f1 = true;
	chain[1].f1 = 250;
	hold = txn_visible_tests_hold(io);
	for (size_t k = 0; k < lengthof(keys); k++)
	{
		StdVec		versions = txn_visible_tests_versioned(io, &hold, state, keys[k]);
		HostError	error;

		if (!txn_visible_epoch(io, 3, chain, lengthof(chain), &STD_VEC_AT(&versions, TxnMemtableVersionVersion, 0),
							   &epochs[k], &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		txn_memtable_version_versions_drop(&versions, 0);
	}
	got = STD_TUPLE(5, STD_ITEM(&epochs[0], std_u32_Debug_fmt), STD_ITEM(&epochs[1], std_u32_Debug_fmt),
					STD_ITEM(&epochs[2], std_u32_Debug_fmt), STD_ITEM(&epochs[3], std_u32_Debug_fmt),
					STD_ITEM(&epochs[4], std_u32_Debug_fmt));
	want = STD_TUPLE(5, STD_ITEM(&wanted[0], std_u32_Debug_fmt), STD_ITEM(&wanted[1], std_u32_Debug_fmt),
					 STD_ITEM(&wanted[2], std_u32_Debug_fmt), STD_ITEM(&wanted[3], std_u32_Debug_fmt),
					 STD_ITEM(&wanted[4], std_u32_Debug_fmt));
	STD_ASSERT_EQ(__LINE__, 9, memcmp(epochs, wanted, sizeof(epochs)) == 0, &got, std_tuple_Debug_fmt, &want,
				  std_tuple_Debug_fmt);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
