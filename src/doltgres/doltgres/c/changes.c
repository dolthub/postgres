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
 * What a row's delete or update decides from its read of the row's key
 * (doltgres' changes.rs), as heap's heap_delete and heap_update decide it
 * from HeapTupleSatisfiesUpdate: go on, showing a combo command id for a row
 * its own transaction wrote; wait for a transaction that holds the row in a
 * mode the change's conflicts with; or fail with heap's TM_Result and the
 * failure data it fills.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/doltgres/changes.h"
#include "doltgres/std/num.h"
#include "doltgres/txn/visible.h"

/*
 * DOLTGRES_CHANGES_INVALID_COMMAND is Postgres' InvalidCommandId, the failure
 * data's command for a row another transaction changed.
 */
#define DOLTGRES_CHANGES_INVALID_COMMAND PG_UINT32_MAX

/*
 * doltgres_changes_invisible returns the failure of a row the change can't
 * see, which its entry raises as heap's does.
 */
static DoltgresChangesFailed
doltgres_changes_invisible(uint64 tuple)
{
	DoltgresChangesFailed failed = {DOLTGRES_CHANGES_TM_INVISIBLE, tuple, 0, DOLTGRES_CHANGES_INVALID_COMMAND};

	return failed;
}

/* doltgres_changes_writer returns a version's writer. */
static TxnMemtableVersionWriter
doltgres_changes_writer(const TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);

	return txn_memtable_version_Record_writer(&record);
}

/* doltgres_changes_lock returns the row lock a version holds while its transaction runs. */
static TxnMemtableLockMode
doltgres_changes_lock(const TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);

	return txn_memtable_version_Record_lock(&record);
}

/*
 * doltgres_changes_successor returns the tuple id of the row an ender's
 * update moved a row to, heap's t_ctid: a Put's or an UpdateRow's own row's,
 * the tuple id a successor's key ends with, or MOVED_PARTITIONS, and the
 * row's own tuple id for an ender that names none.
 */
uint64
doltgres_changes_successor(TxnMemtableVersionVersion *ender, uint64 tuple)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(ender);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	bool		has_named = false;
	TxnMemtableVersionSuccessorRef named;

	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			return op.u.put.tuple.f0;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			return op.u.update_row.f1.tuple.f0;
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			has_named = true;
			named = op.u.move;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			has_named = op.u.remove.has_f1;
			named = op.u.remove.f1;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			has_named = op.u.delete_row.has_f1;
			named = op.u.delete_row.f1;
			break;
		default:
			break;
	}
	if (!has_named)
		return tuple;
	if (named.tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE)
		return DOLTGRES_CHANGES_MOVED_PARTITIONS;
	if (named.u.key.len < 8)
		return tuple;
	return std_u64_from_be_bytes(named.u.key.ptr + named.u.key.len - 8);
}

/*
 * doltgres_changes_failed returns the failure of a row a committed
 * transaction's version ended: TM_Updated with the successor's tuple id for
 * one that names a successor, else TM_Deleted with the row's own.
 */
static DoltgresChangesFailed
doltgres_changes_failed(TxnMemtableVersionVersion *ender, uint64 tuple)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(ender);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	DoltgresChangesFailed failed = {.ctid = doltgres_changes_successor(ender, tuple),
	.xmax = (uint32) txn_memtable_version_Record_writer(&record).xid, .cmax = DOLTGRES_CHANGES_INVALID_COMMAND};
	bool		moved;

	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			moved = true;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			moved = op.u.remove.has_f1;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			moved = op.u.delete_row.has_f1;
			break;
		default:
			moved = false;
			break;
	}
	failed.result = moved ? DOLTGRES_CHANGES_TM_UPDATED : DOLTGRES_CHANGES_TM_DELETED;
	return failed;
}

/*
 * doltgres_changes_sees reports whether a snapshot sees the row with a tuple
 * id among its key's versions, or the base's row with it.
 */
static bool
doltgres_changes_sees(HostIoIo *io, const HostSnapshot *snapshot, TxnMemtableVersionVersion *versions,
					  size_t versions_len, uint64 tuple, bool based, bool *result, HostError *error)
{
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, snapshot};
	TxnVisibleSeen seen;
	HostRow		row = {tuple};
	bool		ok = true;

	if (!txn_visible_seen(io, rule, versions, versions_len, NULL, &seen, error))
		return false;
	*result = false;
	for (size_t i = 0; i < seen.rows.len && !*result; i++)
		*result = STD_VEC_AT(&seen.rows, TxnVisibleSeenRow, i).row.put.tuple.f0 == tuple;
	if (!*result && based)
	{
		TxnVisibleDirty dirty;
		TxnMemtableVersionVersion *changer;
		TxnMemtableVersionVersion *commander;

		ok = txn_visible_Seen_base(&seen, io, rule, row, result, &dirty, &changer, &commander, error);
	}
	txn_visible_Seen_drop(&seen);
	return ok;
}

/*
 * doltgres_changes_conflicts reports whether two row locks conflict,
 * Postgres' table of LockTupleModes: a key-share lock conflicts with an
 * update's only, a share lock with a no-key update's and an update's, a
 * no-key update's with every one but a key-share lock, and an update's with
 * every one.
 */
static bool
doltgres_changes_conflicts(TxnMemtableLockMode held, TxnMemtableLockMode wanted)
{
	TxnMemtableLockMode low = Min(held, wanted);
	TxnMemtableLockMode high = Max(held, wanted);

	switch (low)
	{
		case TXN_MEMTABLE_LOCK_MODE_KEY_SHARE:
			return high == TXN_MEMTABLE_LOCK_MODE_UPDATE;
		case TXN_MEMTABLE_LOCK_MODE_SHARE:
			return high == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE || high == TXN_MEMTABLE_LOCK_MODE_UPDATE;
		case TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE:
		case TXN_MEMTABLE_LOCK_MODE_UPDATE:
			return true;
	}
	return false;
}

/* doltgres_changes_stopped writes a Read that stops with a failure. */
static void
doltgres_changes_stopped(TxnChangeRead *result, DoltgresChangesStop *stopped, DoltgresChangesFailed failed)
{
	result->tag = TXN_CHANGE_READ_STOP;
	stopped->tag = DOLTGRES_CHANGES_STOP_FAILED;
	stopped->u.failed = failed;
}

/* doltgres_changes_waits writes a Read that stops to wait for a transaction. */
static void
doltgres_changes_waits(TxnChangeRead *result, DoltgresChangesStop *stopped, HostXid writer)
{
	result->tag = TXN_CHANGE_READ_STOP;
	stopped->tag = DOLTGRES_CHANGES_STOP_WAIT;
	stopped->u.wait = writer;
}

/*
 * doltgres_changes_ender reads which committed change ended the row a change
 * asks of, by updatable: none, a stop it writes, or the ender, which
 * being_modified's holders decide for a row running transactions hold.
 */
static bool
doltgres_changes_ender(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
					   const DoltgresChangesAsked *asked, TxnVisibleChanged changed, bool *stop,
					   TxnMemtableVersionVersion **ender, TxnChangeRead *result, DoltgresChangesStop *stopped,
					   HostError *error)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(io);
	TxnVisibleUpdatable updatable;

	*stop = false;
	*ender = NULL;
	if (!txn_visible_updatable(io, versions, versions_len, changed, asked->command, &updatable, error))
		return false;
	switch (updatable.tag)
	{
		case TXN_VISIBLE_UPDATABLE_OK:
			break;
		case TXN_VISIBLE_UPDATABLE_INVISIBLE:
			*stop = true;
			doltgres_changes_stopped(result, stopped, doltgres_changes_invisible(asked->tuple));
			break;
		case TXN_VISIBLE_UPDATABLE_SELF_MODIFIED:
			{
				TxnMemtableVersionVersion *modified = updatable.u.self_modified;
				TxnMemtableVersionWriter writer = doltgres_changes_writer(modified);
				DoltgresChangesFailed failed = {DOLTGRES_CHANGES_TM_SELF_MODIFIED,
					doltgres_changes_successor(modified, asked->tuple), (uint32) writer.xid,
				writer.command};

				*stop = true;
				doltgres_changes_stopped(result, stopped, failed);
				break;
			}
		case TXN_VISIBLE_UPDATABLE_BEING_MODIFIED:
			{
				const StdVec *holders = &updatable.u.being_modified.holders;

				for (size_t i = 0; asked->locks && i < holders->len && !*stop; i++)
				{
					TxnMemtableVersionVersion *holder = STD_VEC_AT(holders, TxnMemtableVersionVersion *, i);
					HostXid		by = {doltgres_changes_writer(holder).xid};

					if (host_is_current(doors, by) && doltgres_changes_lock(holder) >= asked->mode)
					{
						*stop = true;
						result->tag = TXN_CHANGE_READ_STOP;
						stopped->tag = DOLTGRES_CHANGES_STOP_HELD;
					}
				}
				for (size_t i = 0; i < holders->len && !*stop; i++)
				{
					TxnMemtableVersionVersion *holder = STD_VEC_AT(holders, TxnMemtableVersionVersion *, i);
					HostXid		by = {doltgres_changes_writer(holder).xid};

					if (!host_is_current(doors, by) &&
						doltgres_changes_conflicts(doltgres_changes_lock(holder), asked->mode))
					{
						*stop = true;
						doltgres_changes_waits(result, stopped, by);
					}
				}
				*ender = updatable.u.being_modified.ended;
				break;
			}
		case TXN_VISIBLE_UPDATABLE_UPDATED:
			*ender = updatable.u.updated;
			break;
		case TXN_VISIBLE_UPDATABLE_DELETED:
			*ender = updatable.u.deleted;
			break;
	}
	txn_visible_Updatable_drop(&updatable);
	return true;
}

/*
 * doltgres_changes_decided answers a change's read of the key of the row it
 * asks for, the key's versions newest first, by visible's updatable: a row
 * the change may change goes on, unless the crosscheck snapshot doesn't see
 * it, which heap answers TM_Updated; a row running transactions hold keeps a
 * row lock its own transaction holds as strong, makes the change wait for
 * the first holder, other than its own transaction, whose row lock conflicts
 * with the change's, and with none answers as the committed ender under them
 * says; and every other answer fails as heap's does. A tuple id no version
 * holds is the newest base's row when that base holds it (based), and is
 * invisible otherwise. A change of a row its own transaction wrote shows a
 * combo command id, which a lock never takes. A lock that follows a running
 * update locks the row it made, waiting only when that update's row lock
 * conflicts with the lock's, and finds no row at a tid whose row another
 * transaction wrote.
 */
bool
doltgres_changes_decided(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
						 const DoltgresChangesAsked *asked, DoltgresChangesBased based, void *based_captures,
						 TxnChangeRead *result, DoltgresChangesStop *stopped, HostError *error)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(io);
	uint64		tuple = asked->tuple;
	bool		has_found = false;
	size_t		at = 0;
	TxnVisibleRowVersion row;
	TxnVisibleChanged changed;
	bool		stop;
	TxnMemtableVersionVersion *ender;

	for (at = 0; at < versions_len && !has_found; at++)
		has_found = txn_visible_RowVersion_of(&versions[at], &row) && row.put.tuple.f0 == tuple;
	at = has_found ? at - 1 : 0;
	if (asked->has_follow)
	{
		HostXid		writer = {asked->follow};
		bool		running;

		if (!has_found || doltgres_changes_writer(row.version).xid != asked->follow)
		{
			doltgres_changes_stopped(result, stopped, doltgres_changes_invisible(tuple));
			return true;
		}
		if (!host_is_current(doors, writer))
		{
			if (!host_in_progress(doors, writer, &running, error))
				return false;
			if (running)
			{
				if (doltgres_changes_conflicts(doltgres_changes_lock(row.version), asked->mode))
					doltgres_changes_waits(result, stopped, writer);
				else
				{
					result->tag = TXN_CHANGE_READ_GO;
					result->u.go.shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND;
					result->u.go.cmin = 0;
					result->u.go.has_row = false;
				}
				return true;
			}
		}
	}
	if (has_found)
	{
		changed.tag = TXN_VISIBLE_CHANGED_DELTA;
		changed.u.delta.at = at;
		changed.u.delta.row = row;
	}
	else
	{
		bool		in_base;

		if (!based(based_captures, io, &in_base, error))
			return false;
		if (!in_base)
		{
			doltgres_changes_stopped(result, stopped, doltgres_changes_invisible(tuple));
			return true;
		}
		changed.tag = TXN_VISIBLE_CHANGED_BASE;
		changed.u.base.f0 = tuple;
	}
	if (!doltgres_changes_ender(io, versions, versions_len, asked, changed, &stop, &ender, result, stopped, error))
		return false;
	if (stop)
		return true;
	if (ender != NULL)
	{
		doltgres_changes_stopped(result, stopped, doltgres_changes_failed(ender, tuple));
		return true;
	}
	if (asked->crosscheck != NULL)
	{
		bool		in_base;
		bool		sees;

		if (!based(based_captures, io, &in_base, error) ||
			!doltgres_changes_sees(io, asked->crosscheck, versions, versions_len, tuple, in_base, &sees, error))
			return false;
		if (!sees)
		{
			DoltgresChangesFailed failed = {DOLTGRES_CHANGES_TM_UPDATED, tuple, 0, DOLTGRES_CHANGES_INVALID_COMMAND};

			doltgres_changes_stopped(result, stopped, failed);
			return true;
		}
	}
	result->tag = TXN_CHANGE_READ_GO;
	result->u.go.shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND;
	result->u.go.cmin = 0;
	result->u.go.has_row = has_found;
	if (has_found)
	{
		TxnMemtableVersionWriter writer = doltgres_changes_writer(row.version);
		HostXid		xid = {writer.xid};

		result->u.go.row = row.version->handle;
		if (!asked->locks && host_is_current(doors, xid))
		{
			HostCommand cmin = {writer.command};
			HostCommand cmax = {asked->command};
			HostCommand combo;

			if (!host_combo_command(doors, xid, cmin, cmax, &combo, error))
				return false;
			result->u.go.shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO;
			result->u.go.shown.u.combo = combo.f0;
			result->u.go.cmin = writer.command;
		}
	}
	return true;
}
