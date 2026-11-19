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
			if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), sub, &status, frame->error))
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

	*has_result = true;
	*result = true;
	if (host_is_current(HOST_IO_IO_IO_DOORS(io), asked))
		return true;
	if (!txn_outcome_outcome(io, version, &status, error))
		return false;
	*has_result = status != HOST_STATUS_IN_PROGRESS;
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
