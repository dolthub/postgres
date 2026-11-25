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
 * The backend's list of the tables whose primary key a statement changed
 * (doltgres' settle.rs): a stack of utility statements' levels, each the
 * tables its statement's sites named with the subtransaction that named
 * each, the depth of that stack at each open subtransaction's start, the key
 * each transient table takes and its old table's primary key index, the
 * relations whose new relfilenode keeps their frozen xid, and each named
 * table's old key columns.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/doltgres/settle.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/unwind.h"

/* doltgres_TupleU32U32_t is a level's (u32, u32), a named table and its subtransaction, or a table's rebuilt index. */
typedef struct doltgres_TupleU32U32_t
{
	uint32		f0;
	uint32		f1;
} doltgres_TupleU32U32_t;

/* doltgres_TupleU32Usize_t is a mark's (u32, usize): a subtransaction and the levels open at its start. */
typedef struct doltgres_TupleU32Usize_t
{
	uint32		f0;
	size_t		f1;
} doltgres_TupleU32Usize_t;

/* doltgres_TupleU32Key_t is a transient table's (u32, Key): its object id and the key it takes. */
typedef struct doltgres_TupleU32Key_t
{
	uint32		f0;
	TxnBaseKey	f1;
} doltgres_TupleU32Key_t;

/* doltgres_TupleU32Columns_t is a pinned table's (u32, Columns): its object id and its old key columns. */
typedef struct doltgres_TupleU32Columns_t
{
	uint32		f0;
	StdVec		f1;
} doltgres_TupleU32Columns_t;

/*
 * DoltgresSettleSettling is Settling: the backend's list, its open levels,
 * innermost last, each a Vec<(u32, u32)>; its subtransactions' marks; its
 * transient tables' keys and rebuilt indexes; the relations a settle is
 * giving a new relfilenode; and each named table's old key columns.
 */
typedef struct DoltgresSettleSettling
{
	StdVec		levels;
	StdVec		marks;
	StdVec		transient;
	StdVec		rebuilt;
	StdVec		frozen;
	StdVec		pinned;
} DoltgresSettleSettling;

/* The element layouts of the list's vectors. */
#define DOLTGRES_SETTLE_LEVEL sizeof(StdVec), _Alignof(StdVec)
#define DOLTGRES_SETTLE_PAIR sizeof(doltgres_TupleU32U32_t), _Alignof(doltgres_TupleU32U32_t)
#define DOLTGRES_SETTLE_MARK sizeof(doltgres_TupleU32Usize_t), _Alignof(doltgres_TupleU32Usize_t)
#define DOLTGRES_SETTLE_TRANSIENT sizeof(doltgres_TupleU32Key_t), _Alignof(doltgres_TupleU32Key_t)
#define DOLTGRES_SETTLE_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_SETTLE_PINNED sizeof(doltgres_TupleU32Columns_t), _Alignof(doltgres_TupleU32Columns_t)
#define DOLTGRES_SETTLE_COLUMN sizeof(doltgres_TupleUsizeOrderKeyType_t), _Alignof(doltgres_TupleUsizeOrderKeyType_t)

/* doltgres_settle_Column_drop frees a key column's key type. */
static void
doltgres_settle_Column_drop(void *column)
{
	access_key_KeyType_drop(&((doltgres_TupleUsizeOrderKeyType_t *) column)->f2);
}

/* doltgres_settle_Columns_drop is Columns' drop glue: each column's key type, then the vector. */
void
doltgres_settle_Columns_drop(StdVec *columns)
{
	std_vec_drop_Checked(columns, DOLTGRES_SETTLE_COLUMN, doltgres_settle_Column_drop);
}

/* doltgres_settle_Level_drop frees a level held as a vector's element. */
static void
doltgres_settle_Level_drop(void *level)
{
	std_vec_free_Checked(level, DOLTGRES_SETTLE_PAIR);
}

/* doltgres_settle_Transient_drop frees a transient table's key. */
static void
doltgres_settle_Transient_drop(void *transient)
{
	txn_base_Key_drop(&((doltgres_TupleU32Key_t *) transient)->f1);
}

/* doltgres_settle_Pinned_drop frees a pinned table's old key columns. */
static void
doltgres_settle_Pinned_drop(void *pinned)
{
	doltgres_settle_Columns_drop(&((doltgres_TupleU32Columns_t *) pinned)->f1);
}

/* doltgres_settle_Settling_Default_default is Settling's derived Default: every list empty. */
static void
doltgres_settle_Settling_Default_default(DoltgresSettleSettling *self)
{
	std_vec_new(&self->levels, DOLTGRES_SETTLE_LEVEL);
	std_vec_new(&self->marks, DOLTGRES_SETTLE_MARK);
	std_vec_new(&self->transient, DOLTGRES_SETTLE_TRANSIENT);
	std_vec_new(&self->rebuilt, DOLTGRES_SETTLE_PAIR);
	std_vec_new(&self->frozen, DOLTGRES_SETTLE_U32);
	std_vec_new(&self->pinned, DOLTGRES_SETTLE_PINNED);
}

/* doltgres_settle_Settling_drop is Settling's drop glue: each list's. */
static void
doltgres_settle_Settling_drop(void *value)
{
	DoltgresSettleSettling *self = value;

	std_vec_drop_Checked(&self->levels, DOLTGRES_SETTLE_LEVEL, doltgres_settle_Level_drop);
	std_vec_free_Checked(&self->marks, DOLTGRES_SETTLE_MARK);
	std_vec_drop_Checked(&self->transient, DOLTGRES_SETTLE_TRANSIENT, doltgres_settle_Transient_drop);
	std_vec_free_Checked(&self->rebuilt, DOLTGRES_SETTLE_PAIR);
	std_vec_free_Checked(&self->frozen, DOLTGRES_SETTLE_U32);
	std_vec_drop_Checked(&self->pinned, DOLTGRES_SETTLE_PINNED, doltgres_settle_Pinned_drop);
}

/* taken at Settling and IoMutex<Settling>, which settle.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Settling
#define HOST_LOCK_TAKEN_T DoltgresSettleSettling
#define HOST_LOCK_TAKEN_DEFAULT(place) doltgres_settle_Settling_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) doltgres_settle_Settling_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_SETTLE_SETTLING_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Settling
#define HOST_LOCK_T DoltgresSettleSettling
#define HOST_LOCK_DEFAULT(place) doltgres_settle_Settling_Default_default(place)
#include "doltgres/host/lock_template.h"

/* doltgres_settle_IoMutexSettling_drop is IoMutex<Settling>'s drop glue: its value's. */
static void
doltgres_settle_IoMutexSettling_drop(void *value)
{
	doltgres_settle_Settling_drop(&((HostLockIoMutexSettling *) value)->value);
}

/* DOLTGRES_SETTLE_SETTLING_TYPE is IoMutex<Settling>'s size, Default, and drop. */
static const StdDefaultType DOLTGRES_SETTLE_SETTLING_TYPE = {sizeof(HostLockIoMutexSettling),
_Alignof(HostLockIoMutexSettling), host_lock_IoMutexSettling_Default_default, doltgres_settle_IoMutexSettling_drop};

/*
 * DOLTGRES_SETTLE_SETTLING is SETTLING: the backend's list, its BackendLocal
 * header and the process's value. Under its lock an operation allocates only
 * fallibly, growing a list only into room it reserved first, so nothing there
 * unwinds and the lock is never poisoned (bugs row 517).
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexSettling process;
}			DOLTGRES_SETTLE_SETTLING = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DoltgresSettleOp is one of the list's operations under its lock, with its captures. */
typedef void (*DoltgresSettleOp) (DoltgresSettleSettling *settling, void *captures);

/*
 * doltgres_settle_locked_frame_t is a locked operation's state across its
 * pad: the backend's list and its guard, each while live, and the
 * operation's cleanup on an unwind.
 */
typedef struct doltgres_settle_locked_frame_t
{
	HostIoIo   *io;
	DoltgresSettleOp op;
	void	   *captures;
	void		(*unwound) (void *captures);
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} doltgres_settle_locked_frame_t;

/* doltgres_settle_locked_body takes the backend's list and its lock, then runs the operation. */
static void
doltgres_settle_locked_body(void *frame_)
{
	doltgres_settle_locked_frame_t *frame = frame_;
	HostLockIoMutex *mutex;

	frame->local = host_local_BackendLocal_get(&DOLTGRES_SETTLE_SETTLING.local, &DOLTGRES_SETTLE_SETTLING_TYPE);
	frame->local_live = true;
	mutex = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local, &DOLTGRES_SETTLE_SETTLING_TYPE);
	frame->guard = host_lock_IoMutex_lock(mutex, &DOLTGRES_SETTLE_SETTLING_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	frame->op(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard), frame->captures);
}

/* doltgres_settle_locked_pad releases what a locked operation holds: its guard, then the backend's list. */
static void
doltgres_settle_locked_pad(void *frame_)
{
	doltgres_settle_locked_frame_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, &DOLTGRES_SETTLE_SETTLING_TYPE);
	frame->guard_live = frame->local_live = false;
}

/* doltgres_settle_locked_unwound releases a locked operation's hold on an unwind, after the operation's cleanup. */
static void
doltgres_settle_locked_unwound(void *frame_)
{
	doltgres_settle_locked_frame_t *frame = frame_;

	if (frame->unwound != NULL)
		frame->unwound(frame->captures);
	doltgres_settle_locked_pad(frame);
}

/*
 * doltgres_settle_locked runs an operation on the backend's list under its
 * lock, as SETTLING.get().lock(io) holds it, releasing the guard and then
 * the list, and on an unwind running the operation's cleanup first.
 */
static void
doltgres_settle_locked(HostIoIo *io, DoltgresSettleOp op, void *captures, void (*unwound) (void *captures))
{
	doltgres_settle_locked_frame_t frame = {.io = io, .op = op, .captures = captures, .unwound = unwound};

	std_landing(&frame, doltgres_settle_locked_body, doltgres_settle_locked_unwound);
	doltgres_settle_locked_pad(&frame);
}

/* doltgres_settle_result_t is the captures of an operation that may fail: its arguments, its result, and its error. */
typedef struct doltgres_settle_result_t
{
	uint32		table;
	uint32		other;
	bool		flag;
	void	   *value;
	bool		ok;
	bool		some;
	HostError  *error;
} doltgres_settle_result_t;

/* doltgres_settle_reserve reserves room for one more element, answering false with an out-of-memory error. */
static bool
doltgres_settle_reserve(StdVec *v, size_t size, size_t align, doltgres_settle_result_t *result)
{
	if (std_vec_try_reserve(v, 1, size, align))
		return true;
	*result->error = host_Error_out_of_memory();
	return false;
}

/* doltgres_settle_opened_op opens a level. */
static void
doltgres_settle_opened_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	StdVec		level;

	if (!doltgres_settle_reserve(&settling->levels, DOLTGRES_SETTLE_LEVEL, result))
		return;
	std_vec_new(&level, DOLTGRES_SETTLE_PAIR);
	std_vec_push_Checked(&settling->levels, &level, DOLTGRES_SETTLE_LEVEL);
	result->ok = true;
}

/* doltgres_settle_opened opens a utility statement's level. */
bool
doltgres_settle_opened(HostIoIo *io, HostError *error)
{
	doltgres_settle_result_t result = {.error = error};

	doltgres_settle_locked(io, doltgres_settle_opened_op, &result, NULL);
	return result.ok;
}

/* doltgres_settle_named_op names a table in the innermost level once, opening one when none is. */
static void
doltgres_settle_named_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	StdVec	   *level;
	doltgres_TupleU32U32_t named = {result->table, result->other};

	if (settling->levels.len == 0)
	{
		StdVec		opened;

		if (!doltgres_settle_reserve(&settling->levels, DOLTGRES_SETTLE_LEVEL, result))
			return;
		std_vec_new(&opened, DOLTGRES_SETTLE_PAIR);
		std_vec_push_Checked(&settling->levels, &opened, DOLTGRES_SETTLE_LEVEL);
	}
	level = &STD_VEC_AT(&settling->levels, StdVec, settling->levels.len - 1);
	for (size_t i = 0; i < level->len; i++)
		if (STD_VEC_AT(level, doltgres_TupleU32U32_t, i).f0 == result->table)
		{
			result->ok = true;
			return;
		}
	if (!doltgres_settle_reserve(level, DOLTGRES_SETTLE_PAIR, result))
		return;
	std_vec_push_Checked(level, &named, DOLTGRES_SETTLE_PAIR);
	result->ok = true;
}

/*
 * doltgres_settle_named names a table in the innermost level, with the
 * subtransaction that names it, once; a drop made outside any utility
 * statement names it in a level of its own.
 */
bool
doltgres_settle_named(HostIoIo *io, uint32 table, uint32 subtransaction, HostError *error)
{
	doltgres_settle_result_t result = {.table = table, .other = subtransaction, .error = error};

	doltgres_settle_locked(io, doltgres_settle_named_op, &result, NULL);
	return result.ok;
}

/* doltgres_settle_closed_captures_t is closed's captures: the level it pops, and whether there was one. */
typedef struct doltgres_settle_closed_captures_t
{
	StdVec		level;
	bool		popped;
} doltgres_settle_closed_captures_t;

/* doltgres_settle_closed_op pops the innermost level, which allocates nothing under the lock. */
static void
doltgres_settle_closed_op(DoltgresSettleSettling *settling, void *captures_)
{
	doltgres_settle_closed_captures_t *captures = captures_;

	captures->popped = std_vec_pop(&settling->levels, &captures->level, sizeof(StdVec));
}

/*
 * doltgres_settle_closed closes the innermost level and returns its tables,
 * in the order they were named, listed after the lock is released (bugs row
 * 517); with no level open it returns none.
 */
void
doltgres_settle_closed(HostIoIo *io, StdVec *result)
{
	doltgres_settle_closed_captures_t captures = {0};

	doltgres_settle_locked(io, doltgres_settle_closed_op, &captures, NULL);
	std_vec_new(result, DOLTGRES_SETTLE_U32);
	if (!captures.popped)
		return;
	if (!std_vec_try_reserve(result, captures.level.len, DOLTGRES_SETTLE_U32))
	{
		std_vec_free_Checked(&captures.level, DOLTGRES_SETTLE_PAIR);
		std_handle_alloc_error(DOLTGRES_SETTLE_U32);
	}
	for (size_t i = 0; i < captures.level.len; i++)
		std_vec_push_Checked(result, &STD_VEC_AT(&captures.level, doltgres_TupleU32U32_t, i).f0, DOLTGRES_SETTLE_U32);
	std_vec_free_Checked(&captures.level, DOLTGRES_SETTLE_PAIR);
}

/* doltgres_settle_pending_captures_t is pending's captures: the tables it takes, while live, and its error. */
typedef struct doltgres_settle_pending_captures_t
{
	StdVec		tables;
	bool		tables_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} doltgres_settle_pending_captures_t;

/*
 * doltgres_settle_pending_op drains every level's tables, outermost first
 * and each once, leaving every level open and empty; a level whose drain an
 * out-of-memory error stops is emptied, as its Drain's drop empties it.
 */
static void
doltgres_settle_pending_op(DoltgresSettleSettling *settling, void *captures_)
{
	doltgres_settle_pending_captures_t *captures = captures_;

	std_vec_new(&captures->tables, DOLTGRES_SETTLE_U32);
	captures->tables_live = true;
	for (size_t l = 0; l < settling->levels.len; l++)
	{
		StdVec	   *level = &STD_VEC_AT(&settling->levels, StdVec, l);

		for (size_t i = 0; i < level->len; i++)
		{
			uint32		table = STD_VEC_AT(level, doltgres_TupleU32U32_t, i).f0;
			bool		seen = false;

			for (size_t j = 0; j < captures->tables.len && !seen; j++)
				seen = STD_VEC_AT(&captures->tables, uint32, j) == table;
			if (seen)
				continue;
			if (!std_vec_try_reserve(&captures->tables, 1, DOLTGRES_SETTLE_U32))
			{
				level->len = 0;
				*captures->error = host_Error_out_of_memory();
				return;
			}
			std_vec_push_Checked(&captures->tables, &table, DOLTGRES_SETTLE_U32);
		}
		level->len = 0;
	}
	*captures->result = captures->tables;
	captures->tables_live = false;
	captures->ok = true;
}

/* doltgres_settle_pending_unwound frees the tables pending took, while live. */
static void
doltgres_settle_pending_unwound(void *captures_)
{
	doltgres_settle_pending_captures_t *captures = captures_;

	if (captures->tables_live)
		std_vec_free_Checked(&captures->tables, DOLTGRES_SETTLE_U32);
	captures->tables_live = false;
}

/*
 * doltgres_settle_pending takes every level's tables, outermost first and
 * each once, leaving every level open and empty, for the settle before a
 * commit or a prepare.
 */
bool
doltgres_settle_pending(HostIoIo *io, StdVec *result, HostError *error)
{
	doltgres_settle_pending_captures_t captures = {.result = result, .error = error};

	doltgres_settle_locked(io, doltgres_settle_pending_op, &captures, doltgres_settle_pending_unwound);
	if (!captures.ok)
		doltgres_settle_pending_unwound(&captures);
	return captures.ok;
}

/* doltgres_settle_ended_op empties the list, keeping its levels open at a commit and dropping them at an abort. */
static void
doltgres_settle_ended_op(DoltgresSettleSettling *settling, void *captures)
{
	bool		aborted = *(bool *) captures;

	if (aborted)
		std_vec_clear(&settling->levels, sizeof(StdVec), doltgres_settle_Level_drop);
	else
		for (size_t l = 0; l < settling->levels.len; l++)
			STD_VEC_AT(&settling->levels, StdVec, l).len = 0;
	settling->marks.len = 0;
	std_vec_clear(&settling->transient, sizeof(doltgres_TupleU32Key_t), doltgres_settle_Transient_drop);
	settling->rebuilt.len = 0;
	settling->frozen.len = 0;
	std_vec_clear(&settling->pinned, sizeof(doltgres_TupleU32Columns_t), doltgres_settle_Pinned_drop);
}

/*
 * doltgres_settle_ended empties the list at a transaction's commit or
 * prepare, keeping its levels open for the statements still running, and
 * drops every level and mark at its abort.
 */
void
doltgres_settle_ended(HostIoIo *io, bool aborted)
{
	doltgres_settle_locked(io, doltgres_settle_ended_op, &aborted, NULL);
}

/* doltgres_settle_sub_started_op marks a subtransaction's start with the number of levels open. */
static void
doltgres_settle_sub_started_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	doltgres_TupleU32Usize_t mark = {result->table, settling->levels.len};

	if (!doltgres_settle_reserve(&settling->marks, DOLTGRES_SETTLE_MARK, result))
		return;
	std_vec_push_Checked(&settling->marks, &mark, DOLTGRES_SETTLE_MARK);
	result->ok = true;
}

/* doltgres_settle_sub_started marks a subtransaction's start with the number of levels open. */
bool
doltgres_settle_sub_started(HostIoIo *io, uint32 subtransaction, HostError *error)
{
	doltgres_settle_result_t result = {.table = subtransaction, .error = error};

	doltgres_settle_locked(io, doltgres_settle_sub_started_op, &result, NULL);
	return result.ok;
}

/*
 * doltgres_settle_sub_ended_op ends a subtransaction's mark: its abort
 * closes the levels opened since its start and takes out every table it or a
 * subtransaction under it named.
 */
static void
doltgres_settle_sub_ended_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	size_t		at = settling->marks.len;
	size_t		depth;

	while (at > 0 && STD_VEC_AT(&settling->marks, doltgres_TupleU32Usize_t, at - 1).f0 != result->table)
		at--;
	if (at == 0)
		return;
	at--;
	depth = STD_VEC_AT(&settling->marks, doltgres_TupleU32Usize_t, at).f1;
	settling->marks.len = at;
	if (!result->flag)
		return;
	std_vec_truncate(&settling->levels, depth, sizeof(StdVec), doltgres_settle_Level_drop);
	for (size_t l = 0; l < settling->levels.len; l++)
	{
		StdVec	   *level = &STD_VEC_AT(&settling->levels, StdVec, l);
		size_t		kept = 0;

		for (size_t i = 0; i < level->len; i++)
			if (STD_VEC_AT(level, doltgres_TupleU32U32_t, i).f1 < result->table)
				STD_VEC_AT(level, doltgres_TupleU32U32_t, kept++) = STD_VEC_AT(level, doltgres_TupleU32U32_t, i);
		level->len = kept;
	}
}

/*
 * doltgres_settle_sub_ended ends a subtransaction's mark: its commit leaves
 * its tables to its parent, and its abort closes the levels opened since its
 * start and takes out every table it or a subtransaction under it named.
 */
void
doltgres_settle_sub_ended(HostIoIo *io, uint32 subtransaction, bool aborted)
{
	doltgres_settle_result_t result = {.table = subtransaction, .flag = aborted};

	doltgres_settle_locked(io, doltgres_settle_sub_ended_op, &result, NULL);
}

/* doltgres_settle_transient_op records a transient table's key, taking it, and its old table's primary key index. */
static void
doltgres_settle_transient_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	doltgres_TupleU32Key_t transient = {result->table, *(TxnBaseKey *) result->value};
	doltgres_TupleU32U32_t rebuilt = {result->table, result->other};

	if (!doltgres_settle_reserve(&settling->transient, DOLTGRES_SETTLE_TRANSIENT, result) ||
		!doltgres_settle_reserve(&settling->rebuilt, DOLTGRES_SETTLE_PAIR, result))
		return;
	std_vec_push_Checked(&settling->transient, &transient, DOLTGRES_SETTLE_TRANSIENT);
	result->some = true;
	if (result->other != 0)
		std_vec_push_Checked(&settling->rebuilt, &rebuilt, DOLTGRES_SETTLE_PAIR);
	result->ok = true;
}

/* doltgres_settle_transient_unwound frees the key transient took, while it still holds it. */
static void
doltgres_settle_transient_unwound(void *captures)
{
	doltgres_settle_result_t *result = captures;

	if (!result->some)
		txn_base_Key_drop(result->value);
	result->some = true;
}

/*
 * doltgres_settle_transient records the key a transient table takes, taking
 * it, its old table's catalog key as make_new_heap made it, and the old
 * table's primary key index, zero for a keyless one.
 */
bool
doltgres_settle_transient(HostIoIo *io, uint32 table, TxnBaseKey *key, uint32 index, HostError *error)
{
	doltgres_settle_result_t result = {.table = table, .other = index, .value = key, .error = error};

	doltgres_settle_locked(io, doltgres_settle_transient_op, &result, doltgres_settle_transient_unwound);
	doltgres_settle_transient_unwound(&result);
	return result.ok;
}

/* doltgres_settle_rebuilt_op finds the primary key index of the table a transient table rewrites. */
static void
doltgres_settle_rebuilt_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;

	for (size_t i = 0; i < settling->rebuilt.len; i++)
		if (STD_VEC_AT(&settling->rebuilt, doltgres_TupleU32U32_t, i).f0 == result->table)
		{
			*(uint32 *) result->value = STD_VEC_AT(&settling->rebuilt, doltgres_TupleU32U32_t, i).f1;
			result->some = true;
			return;
		}
}

/*
 * doltgres_settle_rebuilt returns the primary key index of the table a
 * transient table rewrites, which the rewrite's index rebuild builds again,
 * or false for none.
 */
bool
doltgres_settle_rebuilt(HostIoIo *io, uint32 table, uint32 *result)
{
	doltgres_settle_result_t found = {.table = table, .value = result};

	doltgres_settle_locked(io, doltgres_settle_rebuilt_op, &found, NULL);
	return found.some;
}

/* doltgres_settle_transient_key_op takes the key a transient table takes out of the record. */
static void
doltgres_settle_transient_key_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;

	for (size_t i = 0; i < settling->transient.len; i++)
		if (STD_VEC_AT(&settling->transient, doltgres_TupleU32Key_t, i).f0 == result->table)
		{
			doltgres_TupleU32Key_t taken;

			std_vec_swap_remove(&settling->transient, i, &taken, DOLTGRES_SETTLE_TRANSIENT);
			*(TxnBaseKey *) result->value = taken.f1;
			result->some = true;
			return;
		}
}

/*
 * doltgres_settle_transient_key returns the key a transient table takes,
 * taking it out of the record, which its storage's make reads once, or false
 * for none.
 */
bool
doltgres_settle_transient_key(HostIoIo *io, uint32 table, TxnBaseKey *result)
{
	doltgres_settle_result_t found = {.table = table, .value = result};

	doltgres_settle_locked(io, doltgres_settle_transient_key_op, &found, NULL);
	return found.some;
}

/* doltgres_settle_keep_frozen_op records that a relation's next new relfilenode keeps its frozen xid. */
static void
doltgres_settle_keep_frozen_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;

	if (!doltgres_settle_reserve(&settling->frozen, DOLTGRES_SETTLE_U32, result))
		return;
	std_vec_push_Checked(&settling->frozen, &result->table, DOLTGRES_SETTLE_U32);
	result->ok = true;
}

/*
 * doltgres_settle_keep_frozen records that a relation's next new
 * relfilenode keeps its relfrozenxid and relminmxid, since the settle's
 * carried versions and stamps name xids back to them.
 */
bool
doltgres_settle_keep_frozen(HostIoIo *io, uint32 relation, HostError *error)
{
	doltgres_settle_result_t result = {.table = relation, .error = error};

	doltgres_settle_locked(io, doltgres_settle_keep_frozen_op, &result, NULL);
	return result.ok;
}

/* doltgres_settle_kept_frozen_op takes a relation's record of keeping its frozen xid, if any. */
static void
doltgres_settle_kept_frozen_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;

	for (size_t i = 0; i < settling->frozen.len; i++)
		if (STD_VEC_AT(&settling->frozen, uint32, i) == result->table)
		{
			uint32		taken;

			std_vec_swap_remove(&settling->frozen, i, &taken, DOLTGRES_SETTLE_U32);
			result->some = true;
			return;
		}
}

/*
 * doltgres_settle_kept_frozen reports whether a relation's new relfilenode
 * keeps its frozen xid, taking it out of the record, which its new file
 * reads once.
 */
bool
doltgres_settle_kept_frozen(HostIoIo *io, uint32 relation)
{
	doltgres_settle_result_t result = {.table = relation};

	doltgres_settle_locked(io, doltgres_settle_kept_frozen_op, &result, NULL);
	return result.some;
}

/* doltgres_settle_pinned_at returns where a table's old key columns are recorded, or the list's length for none. */
static size_t
doltgres_settle_pinned_at(const DoltgresSettleSettling *settling, uint32 table)
{
	size_t		at = 0;

	while (at < settling->pinned.len && STD_VEC_AT(&settling->pinned, doltgres_TupleU32Columns_t, at).f0 != table)
		at++;
	return at;
}

/* doltgres_settle_is_pinned_op reports whether a table's old key columns are recorded. */
static void
doltgres_settle_is_pinned_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;

	result->some = doltgres_settle_pinned_at(settling, result->table) < settling->pinned.len;
}

/*
 * doltgres_settle_is_pinned reports whether a table's old key columns are
 * recorded, so its naming reads them from the catalog only the first time it
 * is named in the transaction.
 */
bool
doltgres_settle_is_pinned(HostIoIo *io, uint32 table)
{
	doltgres_settle_result_t result = {.table = table};

	doltgres_settle_locked(io, doltgres_settle_is_pinned_op, &result, NULL);
	return result.some;
}

/* doltgres_settle_pin_op records a named table's old key columns, taking them, the first time it is named. */
static void
doltgres_settle_pin_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	doltgres_TupleU32Columns_t pinned = {result->table, *(StdVec *) result->value};

	if (doltgres_settle_pinned_at(settling, result->table) < settling->pinned.len)
	{
		result->ok = true;
		return;
	}
	if (!doltgres_settle_reserve(&settling->pinned, DOLTGRES_SETTLE_PINNED, result))
		return;
	std_vec_push_Checked(&settling->pinned, &pinned, DOLTGRES_SETTLE_PINNED);
	result->some = true;
	result->ok = true;
}

/* doltgres_settle_pin_unwound frees the columns pin took, while it still holds them. */
static void
doltgres_settle_pin_unwound(void *captures)
{
	doltgres_settle_result_t *result = captures;

	if (!result->some)
		doltgres_settle_Columns_drop(result->value);
	result->some = true;
}

/*
 * doltgres_settle_pin records a named table's old key columns, taking them,
 * the first time it is named in the transaction, which its settle reads its
 * storage by after a drop took a key column's type from the catalog.
 */
bool
doltgres_settle_pin(HostIoIo *io, uint32 table, StdVec *columns, HostError *error)
{
	doltgres_settle_result_t result = {.table = table, .value = columns, .error = error};

	doltgres_settle_locked(io, doltgres_settle_pin_op, &result, doltgres_settle_pin_unwound);
	doltgres_settle_pin_unwound(&result);
	return result.ok;
}

/*
 * doltgres_settle_held_captures_t is held's captures: the table, the copy it
 * makes, whether it found one, and whether an allocation of the copy failed.
 */
typedef struct doltgres_settle_held_captures_t
{
	uint32		table;
	StdVec	   *result;
	bool		some;
	bool		failed;
} doltgres_settle_held_captures_t;

/*
 * doltgres_settle_held_op copies a table's old key columns, each column's
 * key type cloned, with fallible allocations alone, so a failed one leaves no
 * copy and nothing unwinds under the lock (bugs row 517).
 */
static void
doltgres_settle_held_op(DoltgresSettleSettling *settling, void *captures_)
{
	doltgres_settle_held_captures_t *captures = captures_;
	size_t		at = doltgres_settle_pinned_at(settling, captures->table);
	const StdVec *columns;

	if (at == settling->pinned.len)
		return;
	columns = &STD_VEC_AT(&settling->pinned, doltgres_TupleU32Columns_t, at).f1;
	std_vec_new(captures->result, DOLTGRES_SETTLE_COLUMN);
	if (!std_vec_try_reserve(captures->result, columns->len, DOLTGRES_SETTLE_COLUMN))
	{
		captures->failed = true;
		return;
	}
	for (size_t i = 0; i < columns->len; i++)
	{
		const doltgres_TupleUsizeOrderKeyType_t *column = &STD_VEC_AT(columns, doltgres_TupleUsizeOrderKeyType_t, i);
		doltgres_TupleUsizeOrderKeyType_t copy = {column->f0, column->f1};

		if (!access_key_KeyType_try_clone(&column->f2, &copy.f2))
		{
			doltgres_settle_Columns_drop(captures->result);
			captures->failed = true;
			return;
		}
		std_vec_push_Checked(captures->result, &copy, DOLTGRES_SETTLE_COLUMN);
	}
	captures->some = true;
}

/*
 * doltgres_settle_held returns a copy of a table's old key columns as its
 * naming recorded them, leaving them for its settle, or false for none; it
 * copies them, since the settle takes the record later, and raises a failed
 * allocation of the copy once the lock is released.
 */
bool
doltgres_settle_held(HostIoIo *io, uint32 table, StdVec *result)
{
	doltgres_settle_held_captures_t captures = {.table = table, .result = result};

	doltgres_settle_locked(io, doltgres_settle_held_op, &captures, NULL);
	if (captures.failed)
		std_handle_alloc_error(DOLTGRES_SETTLE_COLUMN);
	return captures.some;
}

/* doltgres_settle_pinned_op takes a table's old key columns out of the record. */
static void
doltgres_settle_pinned_op(DoltgresSettleSettling *settling, void *captures)
{
	doltgres_settle_result_t *result = captures;
	size_t		at = doltgres_settle_pinned_at(settling, result->table);
	doltgres_TupleU32Columns_t taken;

	if (at == settling->pinned.len)
		return;
	std_vec_swap_remove(&settling->pinned, at, &taken, DOLTGRES_SETTLE_PINNED);
	*(StdVec *) result->value = taken.f1;
	result->some = true;
}

/*
 * doltgres_settle_pinned returns a table's old key columns as its naming
 * recorded them, taking them out, or false for none.
 */
bool
doltgres_settle_pinned(HostIoIo *io, uint32 table, StdVec *result)
{
	doltgres_settle_result_t found = {.table = table, .value = result};

	doltgres_settle_locked(io, doltgres_settle_pinned_op, &found, NULL);
	return found.some;
}

#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/pg/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "settle.rs"

/* DoltgresSettleTestsCall is a failed-allocation case's call: the backend's Io, a table, and what the call returned. */
typedef struct DoltgresSettleTestsCall
{
	HostIoIo   *io;
	uint32		table;
	StdVec		result;
	bool		some;
} DoltgresSettleTestsCall;

/* doltgres_settle_tests_close is the close case's call: the innermost level closed. */
static void
doltgres_settle_tests_close(void *captures)
{
	DoltgresSettleTestsCall *call = captures;

	doltgres_settle_closed(call->io, &call->result);
}

/* doltgres_settle_tests_hold is the held case's call: a copy of the table's old key columns. */
static void
doltgres_settle_tests_hold(void *captures)
{
	DoltgresSettleTestsCall *call = captures;

	call->some = doltgres_settle_held(call->io, call->table, &call->result);
}

/* doltgres_settle_tests_failing runs a call with its nth allocation failing, answering whether it completed. */
static bool
doltgres_settle_tests_failing(void (*call) (void *captures), DoltgresSettleTestsCall *captures, uint32 nth)
{
	StdCaught	caught;
	bool		ok;

	pg_install();
	pg_fail_nth_allocation(nth);
	ok = std_catch_unwind(call, captures, &caught);
	pg_fail_allocations(false);
	if (!ok)
		std_Caught_drop_Checked(&caught);
	return ok;
}

/*
 * test_doltgres_settle_tests_a_failed_allocation_in_a_close_keeps_the_outer_level:
 * a close of an inner level whose allocations fail leaves the outer level's
 * named table to the next close, which takes the list's lock again (bugs row
 * 517). Before, the close allocated under the lock, its failure poisoned it,
 * and the next take rebuilt an empty list.
 */
void
test_doltgres_settle_tests_a_failed_allocation_in_a_close_keeps_the_outer_level(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	DoltgresSettleTestsCall inner = {.io = io};
	StdVec		outer;

	STD_ASSERT(__LINE__, 5, doltgres_settle_opened(io, &error) && doltgres_settle_named(io, 1, 0, &error) &&
			   doltgres_settle_opened(io, &error) && doltgres_settle_named(io, 2, 0, &error),
			   "the two levels open with a table each");
	STD_ASSERT(__LINE__, 5, !doltgres_settle_tests_failing(doltgres_settle_tests_close, &inner, 1),
			   "a close whose reserve fails raises");
	STD_ASSERT_EQ_U64(__LINE__, 5, pg_refused_size(), 4 * sizeof(uint32));
	doltgres_settle_closed(io, &outer);
	STD_ASSERT(__LINE__, 5, outer.len == 1 && STD_VEC_AT(&outer, uint32, 0) == 1,
			   "the outer level's table outlived a failed close of the inner level");
	std_vec_free_Checked(&outer, DOLTGRES_SETTLE_U32);
	doltgres_settle_ended(io, true);
	simc_TestIo_drop(&test_io);
}

/*
 * test_doltgres_settle_tests_a_failed_allocation_in_a_copy_of_old_key_columns_keeps_them:
 * a copy of a pinned table's old key columns, an enum's labels among them,
 * whose allocations fail leaves them pinned for the next copy, which takes
 * the list's lock again (bugs row 517). Before, the copy cloned under the
 * lock, its failure poisoned it, and the next take rebuilt a list with
 * nothing pinned.
 */
void
test_doltgres_settle_tests_a_failed_allocation_in_a_copy_of_old_key_columns_keeps_them(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdVec		columns;
	doltgres_TupleUsizeOrderKeyType_t column = {.f0 = 1, .f2 = access_key_KeyType_of(3500)};
	access_TupleU32F32_t labels[] = {{16400, 1}, {16401, 2}};
	DoltgresSettleTestsCall copy = {.io = io, .table = 9};
	DoltgresSettleTestsCall again = {.io = io, .table = 9};

	column.f2.inner.tag = ACCESS_KEY_INNER_ENUM;
	std_slice_to_vec_Checked(&column.f2.inner.u.labels, labels, lengthof(labels), sizeof(access_TupleU32F32_t),
							 _Alignof(access_TupleU32F32_t));
	std_vec_new(&columns, DOLTGRES_SETTLE_COLUMN);
	std_vec_push_Checked(&columns, &column, DOLTGRES_SETTLE_COLUMN);
	STD_ASSERT(__LINE__, 5, doltgres_settle_pin(io, 9, &columns, &error), "the table's old key columns are pinned");
	STD_ASSERT(__LINE__, 5, !doltgres_settle_tests_failing(doltgres_settle_tests_hold, &copy, 2),
			   "a copy whose labels' clone fails raises");
	STD_ASSERT_EQ_U64(__LINE__, 5, pg_refused_size(), 4 * sizeof(access_TupleU32F32_t));
	doltgres_settle_tests_hold(&again);
	STD_ASSERT(__LINE__, 5, again.some && again.result.len == 1 &&
			   STD_VEC_AT(&again.result, doltgres_TupleUsizeOrderKeyType_t, 0).f2.inner.u.labels.len == 2,
			   "the pinned columns outlived a failed copy");
	doltgres_settle_Columns_drop(&again.result);
	doltgres_settle_ended(io, true);
	simc_TestIo_drop(&test_io);
}
#endif							/* DOLTGRES_TEST_DOLTGRES */
