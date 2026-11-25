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
 * The pooled records' doors over the fork's region (design item 14's S2):
 * every change of a record's slot is one compare-and-swap of its directory
 * word, from the word this process loaded to the word host's plain
 * functions give for it, and this process lists the handles it has, so its
 * exit gives up what its drops didn't. A process's slot of words is its own
 * record of the words' kind. The process also lists its exit works, which
 * its exit callback runs before anything here is given up.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/host/cache.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/shared/word.h"
#include "doltgres/pghost/delta.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_PGHOST
#include "doltgres/std/test.h"
#endif
#include "pg_calls.h"

#define RUST_FILE "pghost" STD_SEP "src" STD_SEP "shared.rs"

/* PghostSharedRoleTag is Role's variants. */
typedef enum PghostSharedRoleTag
{
	/* PGHOST_SHARED_ROLE_OWNER is the record's maker or claimant. */
	PGHOST_SHARED_ROLE_OWNER,

	/*
	 * PGHOST_SHARED_ROLE_HOLDER is a process that opened the record, with the
	 * number of its handles of it.
	 */
	PGHOST_SHARED_ROLE_HOLDER,

	/*
	 * PGHOST_SHARED_ROLE_RECLAIM is a reclaim looking at the record, with the
	 * word its take stored.
	 */
	PGHOST_SHARED_ROLE_RECLAIM,
} PghostSharedRoleTag;

/* PghostSharedRole is what this process is to a record it has a handle of. */
typedef struct PghostSharedRole
{
	PghostSharedRoleTag tag;
	union
	{
		uint32		holder;
		uint64		reclaim;
	}			u;
} PghostSharedRole;

/* pghost_shared_Role_PartialEq_eq is Role's derived PartialEq: the same variant with equal values. */
static bool
pghost_shared_Role_PartialEq_eq(const PghostSharedRole *self, const PghostSharedRole *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case PGHOST_SHARED_ROLE_OWNER:
			return true;
		case PGHOST_SHARED_ROLE_HOLDER:
			return self->u.holder == other->u.holder;
		case PGHOST_SHARED_ROLE_RECLAIM:
			return self->u.reclaim == other->u.reclaim;
	}
	pg_unreachable();
}

/* PghostSharedHandle is a record this process has a handle of: its slot in the directory and what the process is to it. */
typedef struct PghostSharedHandle
{
	uint32		slot;
	PghostSharedRole role;
} PghostSharedHandle;

/* PGHOST_SHARED_HANDLES_LOCK is HANDLES' RwLock, which guards PGHOST_SHARED_HANDLES. */
static StdRwLock PGHOST_SHARED_HANDLES_LOCK = STD_RW_LOCK_NEW;

/* PGHOST_SHARED_HANDLES are the records this process has handles of, which its exit gives up. */
static StdVec PGHOST_SHARED_HANDLES = STD_VEC_NEW;

/* PghostSharedWork is an exit work of this process: the type that names it, and its function. */
typedef struct PghostSharedWork
{
	const StdTypeId *f0;
	bool		(*f1) (HostIoIo *io, HostError *error);
} PghostSharedWork;

/* PGHOST_SHARED_WORKS_LOCK is WORKS' RwLock, which guards PGHOST_SHARED_WORKS. */
static StdRwLock PGHOST_SHARED_WORKS_LOCK = STD_RW_LOCK_NEW;

/* PGHOST_SHARED_WORKS are the exit works this process listed, the newest last. */
static StdVec PGHOST_SHARED_WORKS = STD_VEC_NEW;

/* PGHOST_SHARED_EXITING is whether this process's exit callback has begun, after which it lists no work. */
static StdAtomicBool PGHOST_SHARED_EXITING = STD_ATOMIC_BOOL_INIT(false);

/* PGHOST_SHARED_STORED is whether this process took its slot of a kind of words, so its exit has words to zero. */
static StdAtomicBool PGHOST_SHARED_STORED = STD_ATOMIC_BOOL_INIT(false);

/*
 * pghost_shared_own takes a list of this process's own, which no other
 * thread uses and no call holding it reenters, so the take never waits; a
 * poisoned list keeps what it holds.
 */
StdRwLockWriteGuard
pghost_shared_own(StdRwLock *list, void *data)
{
	StdRwLockWriteGuard guard;

	switch (std_rw_lock_try_write(list, data, &guard))
	{
		case STD_TRY_LOCK_OK:
		case STD_TRY_LOCK_POISONED:
			return guard;
		case STD_TRY_LOCK_WOULD_BLOCK:
			STD_PANIC(71, 42, "a process's own list was taken while it held it");
	}
	pg_unreachable();
}

/* pghost_shared_handles takes this process's list of handles. */
static StdRwLockWriteGuard
pghost_shared_handles(void)
{
	return pghost_shared_own(&PGHOST_SHARED_HANDLES_LOCK, &PGHOST_SHARED_HANDLES);
}

/* pghost_shared_internal returns the fork's internal error with a message; it takes the message. */
static HostError
pghost_shared_internal(StdString *message)
{
	return host_Error_fork((const uint8 *) "XX000", message);
}

/*
 * pghost_shared_limits returns the server's limits: its processes, and the
 * slots of the delta's page directory, one for each 1 KiB of delta_memory,
 * computed here alone for the directory and the pools.
 */
bool
pghost_shared_limits(const PgMayRaise *raise, HostSharedLimits *result, PgPgError *error)
{
	size_t		delta_pages = (size_t) host_delta_page_slots((uint64) pghost_delta_bytes());
	size_t		processes = (size_t) pg_calls_max_procs();
	size_t		relation_locks;

	if (!pg_calls_relation_locks(raise, &relation_locks, error))
		return false;
	*result = (HostSharedLimits)
	{
		processes, delta_pages, relation_locks
	};
	return true;
}

/* pghost_shared_kinds_frame_0_t is kinds' state across its pad: the kinds collected so far. */
typedef struct pghost_shared_kinds_frame_0_t
{
	const HostSharedKind *table;
	size_t		table_len;
	const HostSharedLimits *limits;
	StdVec	   *result;
	bool		result_live;
} pghost_shared_kinds_frame_0_t;

/* pghost_shared_kinds_closure_0 is kinds' map: a kind of the table as the shared-memory crate takes it. */
static ShmemRecordsKind
pghost_shared_kinds_closure_0(const HostSharedLimits *limits, const HostSharedKind *kind)
{
	ShmemRecordsForm form = SHMEM_RECORDS_FORM_SINGLE;
	size_t		count = 0;
	size_t		run = 0;

	switch (kind->form.tag)
	{
		case HOST_SHARED_FORM_POOL:
			form = SHMEM_RECORDS_FORM_POOL;
			count = kind->form.u.pool(limits);
			run = 0;
			break;
		case HOST_SHARED_FORM_SINGLE:
			form = SHMEM_RECORDS_FORM_SINGLE;
			count = 1;
			run = kind->form.u.single.run(limits);
			break;
		case HOST_SHARED_FORM_AGED:
			form = SHMEM_RECORDS_FORM_AGED;
			count = kind->form.u.aged.count(limits);
			run = 0;
			break;
		case HOST_SHARED_FORM_PROCESS:
			form = kind->form.u.process.kept ? SHMEM_RECORDS_FORM_KEPT : SHMEM_RECORDS_FORM_PROCESS;
			count = limits->processes;
			run = 0;
			break;
	}
	return (ShmemRecordsKind)
	{
		kind->id, form, count, kind->locks, kind->flags,
			STD_ADD(usize, kind->atomics, run, 112, 26), kind->blobs, kind->blobs_len,
			kind->arrays, kind->arrays_len, kind->index
	};
}

/* pghost_shared_kinds_body_0 is kinds' body: each kind of the table mapped and collected. */
static void
pghost_shared_kinds_body_0(void *frame_)
{
	pghost_shared_kinds_frame_0_t *frame = frame_;
	StdSizeHint hint = {frame->table_len, true, frame->table_len};

	std_vec_with_capacity_trusted(frame->result, hint, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
	frame->result_live = true;
	std_vec_extend_trusted_reserve(frame->result, hint, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
	for (size_t i = 0, i_len = frame->table_len; i < i_len; i++)
	{
		STD_VEC_AT(frame->result, ShmemRecordsKind, frame->result->len) =
			pghost_shared_kinds_closure_0(frame->limits, &frame->table[i]);
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->result_live = false;
}

/* pghost_shared_kinds_pad_0 is kinds' pad: the kinds collected so far, while they are live. */
static void
pghost_shared_kinds_pad_0(void *frame_)
{
	pghost_shared_kinds_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
}

/*
 * pghost_shared_kinds returns the server's table of kinds as the
 * shared-memory crate takes it, each pool with its count for the server's
 * processes, and each kind of words with a record for each process.
 */
bool
pghost_shared_kinds(const PgMayRaise *raise, const HostSharedKind *table, size_t table_len, StdVec *result,
					PgPgError *error)
{
	HostSharedLimits limits;
	pghost_shared_kinds_frame_0_t frame = {.table = table, .table_len = table_len, .limits = &limits,
	.result = result};

	if (!pghost_shared_limits(raise, &limits, error))
		return false;
	std_landing(&frame, pghost_shared_kinds_body_0, pghost_shared_kinds_pad_0);
	return true;
}

/* pghost_shared_place returns the place of a record: its kind's number among the region's kinds, then its number among the kind's records. */
static uint64
pghost_shared_place(size_t kind, uint32 index)
{
	return STD_SHL_FOLDED(u64, (uint64) kind, 32) | (uint64) index;
}

/* pghost_shared_placed returns a place's kind and its record's number among the kind's records. */
void
pghost_shared_placed(uint64 place, size_t *result_0, uint32 *result_1)
{
	*result_0 = (size_t) STD_SHR_FOLDED(u64, place, 32);
	*result_1 = (uint32) place;
}

/* PghostSharedPool is a pool in the region: its kind's number, its first slot in the directory, and its count of records. */
typedef struct PghostSharedPool
{
	size_t		kind;
	uint32		first;
	uint32		count;
} PghostSharedPool;

/* pghost_shared_kind returns the number of a shape's kind among the region's, refusing a shape the server's table doesn't list. */
static bool
pghost_shared_kind(uint64 id, size_t *result, HostError *error)
{

	if (shmem_records_kind(id, result))
		return true;
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("the server's table of kinds lists no shape "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&id, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pghost_shared_internal(&message);
	}
	return false;
}

/* pghost_shared_pool returns the pool of a shape, refusing a shape the server's table lists no pool of. */
static bool
pghost_shared_pool(uint64 id, PghostSharedPool *result, HostError *error)
{
	size_t		kind;
	std_TupleU32U32_t pool;

	if (!pghost_shared_kind(id, &kind, error))
		return false;
	if (!shmem_records_pool(kind, &pool))
	{
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("the server's table of kinds lists no pool of shape "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&id, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*error = pghost_shared_internal(&message);
		}
		return false;
	}
	*result = (PghostSharedPool)
	{
		kind, pool.f0, pool.f1
	};
	return true;
}

/*
 * pghost_shared_slot returns a location's slot in the directory, refusing
 * one outside a pool's slots, which only a forged location or another
 * kind's has.
 */
static bool
pghost_shared_slot(const PghostSharedPool *pool, uint64 location, uint32 *result, HostError *error)
{
	uint64		slot = host_shared_word_slot(location);

	if (slot < (uint64) pool->first ||
		slot >= STD_ADD(u64, (uint64) pool->first, (uint64) pool->count, 155, 48))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("location "), STD_FMT_ARG(0),
		STD_FMT_LIT(" names no record of its shape's pool")};
		StdFmtArg	args[] = {{&location, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pghost_shared_internal(&message);
		return false;
	}
	*result = (uint32) slot;
	return true;
}

/* pghost_shared_spot returns the slot in the directory of a pooled record at a place. */
static uint32
pghost_shared_spot(uint64 place)
{
	size_t		kind;
	uint32		index;
	std_TupleU32U32_t pool;
	uint32		first = 0;

	pghost_shared_placed(place, &kind, &index);
	if (shmem_records_pool(kind, &pool))
		first = pool.f0;
	return STD_ADD(u32, first, index, 164, 5);
}

/* pghost_shared_turn stores in a slot the word a transition gives for the word the slot holds, and returns it. */
static uint64
pghost_shared_turn(uint32 slot, uint64 (*next) (uint64))
{
	for (;;)
	{
		uint64		loaded = shmem_records_word(slot);
		uint64		word = next(loaded);

		if (shmem_records_swap(slot, loaded, word))
			return word;
	}
}

/* pghost_shared_reserve makes room in the process's list for one more handle, so that nothing after a slot's compare-and-swap can fail. */
static bool
pghost_shared_reserve(StdVec *handles, HostError *error)
{
	if (std_vec_try_reserve(handles, 1, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle)))
		return true;
	*error = host_Error_out_of_memory();
	return false;
}

/* pghost_shared_take_frame_0_t is take's state across its pad: the list's guard. */
typedef struct pghost_shared_take_frame_0_t
{
	uint32		slot;
	bool		(*like) (const PghostSharedRole *role);
	bool		has_result;
	PghostSharedRole result;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_take_frame_0_t;

/* pghost_shared_take_body_0 is take's body. */
static void
pghost_shared_take_body_0(void *frame_)
{
	pghost_shared_take_frame_0_t *frame = frame_;
	StdVec	   *handles;
	PghostSharedHandle removed;

	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	frame->has_result = false;
	for (size_t i = 0, i_len = handles->len; i < i_len; i++)
	{
		PghostSharedHandle *handle = &STD_VEC_AT(handles, PghostSharedHandle, i);

		if (handle->slot == frame->slot && frame->like(&handle->role))
		{
			std_vec_swap_remove(handles, i, &removed, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle));
			frame->result = removed.role;
			frame->has_result = true;
			break;
		}
	}
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_take_pad_0 is take's pad: the list's guard while it is live. */
static void
pghost_shared_take_pad_0(void *frame_)
{
	pghost_shared_take_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_take removes this process's handle of a slot in a role, reporting the role it had, or nothing when it has none. */
static bool
pghost_shared_take(uint32 slot, bool (*like) (const PghostSharedRole *role), PghostSharedRole *result)
{
	pghost_shared_take_frame_0_t frame = {.slot = slot, .like = like};

	std_landing(&frame, pghost_shared_take_body_0, pghost_shared_take_pad_0);
	if (frame.has_result)
		*result = frame.result;
	return frame.has_result;
}

/* pghost_shared_make_frame_0_t is make's state across its pad: its arguments, its result, and the list's guard. */
typedef struct pghost_shared_make_frame_0_t
{
	const HostMayRaise *raise;
	uint64		id;
	bool	   *has_result;
	uint64	   *result;
	HostError  *error;
	bool		done;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_make_frame_0_t;

/* pghost_shared_make_body_0 is make's body. */
static void
pghost_shared_make_body_0(void *frame_)
{
	pghost_shared_make_frame_0_t *frame = frame_;
	PghostSharedPool pool;
	StdVec	   *handles;
	bool		refused;
	PgPgError	error_;
	uint32		start;

	frame->done = false;
	if (!pghost_shared_pool(frame->id, &pool, frame->error))
		return;
	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	if (!pghost_shared_reserve(handles, frame->error))
		goto end;
	if (!pg_calls_injection_point_attached(pghost_pg_raise(frame->raise), "doltgres-shared-make-refused", &refused,
										   &error_))
	{
		*frame->error = pghost_held(&error_);
		goto end;
	}
	if (refused)
	{
		*frame->error = host_Error_out_of_memory();
		goto end;
	}
	if (pool.count == 0)
	{
		*frame->has_result = false;
		frame->done = true;
		goto end;
	}
	start = shmem_records_hand(pool.kind);
	for (uint32 step = 0, step_end = pool.count; step < step_end; step = std_forward_unchecked_u32_Checked(step, 1))
	{
		uint32		index = STD_REM(u32, STD_ADD(u32, start, step, 206, 22), pool.count, 206, 21);
		uint64		loaded = shmem_records_word(STD_ADD(u32, pool.first, index, 207, 36));
		uint64		owned;

		if (!host_shared_word_make(loaded, &owned))
			continue;
		if (!pg_calls_injection_point(pghost_pg_raise(frame->raise), "doltgres-shared-making", &error_))
		{
			*frame->error = pghost_held(&error_);
			goto end;
		}
		if (shmem_records_swap(STD_ADD(u32, pool.first, index, 210, 26), loaded, owned))
		{
			PghostSharedHandle handle = {STD_ADD(u32, pool.first, index, 212, 41), {PGHOST_SHARED_ROLE_OWNER}};

			shmem_records_clear(pool.kind, index);
			std_vec_push_Checked(handles, &handle, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle));
			*frame->has_result = true;
			*frame->result = pghost_shared_place(pool.kind, index);
			frame->done = true;
			goto end;
		}
	}
	*frame->has_result = false;
	frame->done = true;
end:
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_make_pad_0 is make's pad: the list's guard while it is live. */
static void
pghost_shared_make_pad_0(void *frame_)
{
	pghost_shared_make_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/*
 * pghost_shared_make takes a free slot of a shape's pool for a record this
 * process owns, sweeping the pool once from its hand, and answers nothing
 * when the sweep takes none. The fork's tests have it fail as an allocation
 * of its list entry does, and pause between a slot's load and its
 * compare-and-swap, through injection points.
 */
bool
pghost_shared_make(const HostMayRaise *raise, uint64 id, bool *has_result, uint64 *result, HostError *error)
{
	pghost_shared_make_frame_0_t frame = {.raise = raise, .id = id, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, pghost_shared_make_body_0, pghost_shared_make_pad_0);
	return frame.done;
}

/* pghost_shared_publish_closure_0 is take's test of publish's handle: the owner's role. */
static bool
pghost_shared_publish_closure_0(const PghostSharedRole *role)
{
	PghostSharedRole owner = {PGHOST_SHARED_ROLE_OWNER};

	return pghost_shared_Role_PartialEq_eq(role, &owner);
}

/* pghost_shared_publish publishes a record this process owns and returns its location. */
uint64
pghost_shared_publish(uint64 place)
{
	uint32		slot = pghost_shared_spot(place);
	PghostSharedRole role;

	(void) pghost_shared_take(slot, pghost_shared_publish_closure_0, &role);
	return host_shared_word_location(slot, pghost_shared_turn(slot, host_shared_word_publish));
}

/* pghost_shared_claim_frame_0_t is claim's state across its pad: its arguments, its result, and the list's guard. */
typedef struct pghost_shared_claim_frame_0_t
{
	uint64		id;
	uint64		location;
	bool	   *has_result;
	uint64	   *result;
	HostError  *error;
	bool		done;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_claim_frame_0_t;

/* pghost_shared_claim_body_0 is claim's body. */
static void
pghost_shared_claim_body_0(void *frame_)
{
	pghost_shared_claim_frame_0_t *frame = frame_;
	PghostSharedPool pool;
	uint32		slot;
	StdVec	   *handles;

	frame->done = false;
	if (!pghost_shared_pool(frame->id, &pool, frame->error))
		return;
	if (!pghost_shared_slot(&pool, frame->location, &slot, frame->error))
		return;
	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	if (pghost_shared_reserve(handles, frame->error))
	{
		for (;;)
		{
			uint64		loaded = shmem_records_word(slot);
			uint64		claimed;

			if (!host_shared_word_claim(loaded, frame->location, &claimed))
			{
				*frame->has_result = false;
				frame->done = true;
				break;
			}
			if (shmem_records_swap(slot, loaded, claimed))
			{
				PghostSharedHandle handle = {slot, {PGHOST_SHARED_ROLE_OWNER}};

				std_vec_push_Checked(handles, &handle, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle));
				*frame->has_result = true;
				*frame->result = pghost_shared_place(pool.kind, STD_SUB(u32, slot, pool.first, 238, 45));
				frame->done = true;
				break;
			}
		}
	}
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_claim_pad_0 is claim's pad: the list's guard while it is live. */
static void
pghost_shared_claim_pad_0(void *frame_)
{
	pghost_shared_claim_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/*
 * pghost_shared_claim takes the ownership of the published record of a
 * shape at a location, or answers nothing for a location that is stale or a
 * record that isn't published.
 */
bool
pghost_shared_claim(uint64 id, uint64 location, bool *has_result, uint64 *result, HostError *error)
{
	pghost_shared_claim_frame_0_t frame = {.id = id, .location = location, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, pghost_shared_claim_body_0, pghost_shared_claim_pad_0);
	return frame.done;
}

/* pghost_shared_free_closure_0 is take's test of free's handle: the owner's role. */
static bool
pghost_shared_free_closure_0(const PghostSharedRole *role)
{
	PghostSharedRole owner = {PGHOST_SHARED_ROLE_OWNER};

	return pghost_shared_Role_PartialEq_eq(role, &owner);
}

/* pghost_shared_free frees a record this process owns. */
void
pghost_shared_free(uint64 place)
{
	uint32		slot = pghost_shared_spot(place);
	PghostSharedRole role;

	(void) pghost_shared_take(slot, pghost_shared_free_closure_0, &role);
	(void) pghost_shared_turn(slot, host_shared_word_free);
}

/* pghost_shared_give_up_closure_0 is take's test of give_up's handle: the owner's role. */
static bool
pghost_shared_give_up_closure_0(const PghostSharedRole *role)
{
	PghostSharedRole owner = {PGHOST_SHARED_ROLE_OWNER};

	return pghost_shared_Role_PartialEq_eq(role, &owner);
}

/* pghost_shared_give_up gives up a record this process still owns, and does nothing for one it published or freed. */
void
pghost_shared_give_up(uint64 place)
{
	uint32		slot = pghost_shared_spot(place);
	PghostSharedRole role;

	if (pghost_shared_take(slot, pghost_shared_give_up_closure_0, &role))
		(void) pghost_shared_turn(slot, host_shared_word_give_up);
}

/* pghost_shared_open_frame_0_t is open's state across its pad: its arguments, its result, and the list's guard. */
typedef struct pghost_shared_open_frame_0_t
{
	const HostMayRaise *raise;
	uint64		id;
	uint64		location;
	bool	   *has_result;
	uint64	   *result;
	HostError  *error;
	bool		done;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_open_frame_0_t;

/* pghost_shared_open_body_0 is open's body. */
static void
pghost_shared_open_body_0(void *frame_)
{
	pghost_shared_open_frame_0_t *frame = frame_;
	PghostSharedPool pool;
	uint32		slot;
	uint64		place;
	StdVec	   *handles;
	PgPgError	error_;

	frame->done = false;
	if (!pghost_shared_pool(frame->id, &pool, frame->error))
		return;
	if (!pghost_shared_slot(&pool, frame->location, &slot, frame->error))
		return;
	place = pghost_shared_place(pool.kind, STD_SUB(u32, slot, pool.first, 264, 34));
	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	if (!pghost_shared_reserve(handles, frame->error))
		goto end;
	if (!pg_calls_injection_point(pghost_pg_raise(frame->raise), "doltgres-shared-opening", &error_))
	{
		*frame->error = pghost_held(&error_);
		goto end;
	}
	for (;;)
	{
		uint64		loaded = shmem_records_word(slot);
		uint64		opened;
		PghostSharedHandle *mine = NULL;

		if (!host_shared_word_open(loaded, frame->location, &opened))
		{
			*frame->has_result = false;
			frame->done = true;
			goto end;
		}
		for (size_t i = 0, i_len = handles->len; i < i_len; i++)
		{
			PghostSharedHandle *handle = &STD_VEC_AT(handles, PghostSharedHandle, i);

			if (handle->slot == slot && handle->role.tag == PGHOST_SHARED_ROLE_HOLDER)
			{
				mine = handle;
				break;
			}
		}
		if (mine != NULL)
		{
			mine->role.u.holder = STD_ADD(u32, mine->role.u.holder, 1, 273, 13);
			*frame->has_result = true;
			*frame->result = place;
			frame->done = true;
			goto end;
		}
		if (shmem_records_swap(slot, loaded, opened))
		{
			PghostSharedHandle handle = {slot, {PGHOST_SHARED_ROLE_HOLDER, {.holder = 1}}};

			std_vec_push_Checked(handles, &handle, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle));
			*frame->has_result = true;
			*frame->result = place;
			frame->done = true;
			goto end;
		}
	}
end:
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_open_pad_0 is open's pad: the list's guard while it is live. */
static void
pghost_shared_open_pad_0(void *frame_)
{
	pghost_shared_open_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/*
 * pghost_shared_open opens the published record of a shape at a location,
 * or answers nothing for a location that is stale or a record that isn't
 * published. A record this process already holds is counted in its own list
 * alone. The fork's tests pause it before its compare-and-swap through an
 * injection point.
 */
bool
pghost_shared_open(const HostMayRaise *raise, uint64 id, uint64 location, bool *has_result, uint64 *result,
				   HostError *error)
{
	pghost_shared_open_frame_0_t frame = {.raise = raise, .id = id, .location = location, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, pghost_shared_open_body_0, pghost_shared_open_pad_0);
	return frame.done;
}

/* pghost_shared_close_frame_0_t is close's state across its pad: the list's guard. */
typedef struct pghost_shared_close_frame_0_t
{
	uint64		place;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_close_frame_0_t;

/* pghost_shared_close_body_0 is close's body. */
static void
pghost_shared_close_body_0(void *frame_)
{
	pghost_shared_close_frame_0_t *frame = frame_;
	uint32		slot = pghost_shared_spot(frame->place);
	StdVec	   *handles;
	bool		found = false;
	size_t		at = 0;

	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	for (size_t i = 0, i_len = handles->len; i < i_len; i++)
	{
		PghostSharedHandle *handle = &STD_VEC_AT(handles, PghostSharedHandle, i);

		if (handle->slot == slot && handle->role.tag == PGHOST_SHARED_ROLE_HOLDER)
		{
			at = i;
			found = true;
			break;
		}
	}
	if (found)
	{
		PghostSharedRole *role;

		std_index_check(at, handles->len, RUST_FILE, 291, 39);
		role = &STD_VEC_AT(handles, PghostSharedHandle, at).role;
		if (role->tag == PGHOST_SHARED_ROLE_HOLDER)
		{
			role->u.holder = STD_SUB(u32, role->u.holder, 1, 292, 9);
			if (role->u.holder == 0)
			{
				PghostSharedHandle removed;

				std_vec_swap_remove(handles, at, &removed, sizeof(PghostSharedHandle),
									_Alignof(PghostSharedHandle));
				(void) pghost_shared_turn(slot, host_shared_word_close);
			}
		}
	}
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_close_pad_0 is close's pad: the list's guard while it is live. */
static void
pghost_shared_close_pad_0(void *frame_)
{
	pghost_shared_close_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_close closes one of this process's handles of a record it opened, which leaves the record's holders at the last. */
void
pghost_shared_close(uint64 place)
{
	pghost_shared_close_frame_0_t frame = {.place = place};

	std_landing(&frame, pghost_shared_close_body_0, pghost_shared_close_pad_0);
}

/* pghost_shared_reclaim_frame_0_t is reclaim's state across its pad: its arguments, its result, and the list's guard. */
typedef struct pghost_shared_reclaim_frame_0_t
{
	uint64		id;
	uint64		location;
	HostSharedLooked *result;
	HostError  *error;
	bool		done;
	StdRwLockWriteGuard handles;
	bool		handles_live;
} pghost_shared_reclaim_frame_0_t;

/* pghost_shared_reclaim_body_0 is reclaim's body. */
static void
pghost_shared_reclaim_body_0(void *frame_)
{
	pghost_shared_reclaim_frame_0_t *frame = frame_;
	PghostSharedPool pool;
	uint32		slot;
	StdVec	   *handles;

	frame->done = false;
	if (!pghost_shared_pool(frame->id, &pool, frame->error))
		return;
	if (!pghost_shared_slot(&pool, frame->location, &slot, frame->error))
		return;
	frame->handles = pghost_shared_handles();
	frame->handles_live = true;
	handles = frame->handles.data;
	if (pghost_shared_reserve(handles, frame->error))
	{
		for (;;)
		{
			uint64		loaded = shmem_records_word(slot);
			HostSharedWordLook look = host_shared_word_reclaim(loaded, frame->location);

			if (look.tag == HOST_SHARED_WORD_LOOK_TAKE)
			{
				if (shmem_records_swap(slot, loaded, look.u.take))
				{
					PghostSharedHandle handle = {slot, {PGHOST_SHARED_ROLE_RECLAIM, {.reclaim = look.u.take}}};

					std_vec_push_Checked(handles, &handle, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle));
					frame->result->tag = HOST_SHARED_LOOKED_MINE;
					frame->result->u.mine = pghost_shared_place(pool.kind, STD_SUB(u32, slot, pool.first, 313, 61));
					frame->done = true;
					break;
				}
				continue;
			}
			frame->result->tag = look.tag == HOST_SHARED_WORD_LOOK_THEIRS ? HOST_SHARED_LOOKED_THEIRS :
				HOST_SHARED_LOOKED_USED;
			frame->done = true;
			break;
		}
	}
	frame->handles_live = false;
	std_RwLockWriteGuard_drop(&frame->handles);
}

/* pghost_shared_reclaim_pad_0 is reclaim's pad: the list's guard while it is live. */
static void
pghost_shared_reclaim_pad_0(void *frame_)
{
	pghost_shared_reclaim_frame_0_t *frame = frame_;

	if (frame->handles_live)
		std_RwLockWriteGuard_drop(&frame->handles);
}

/*
 * pghost_shared_reclaim looks at the published record of a shape at a
 * location for a reclaim, taking it when no process holds it and no other
 * reclaim is looking at it.
 */
bool
pghost_shared_reclaim(uint64 id, uint64 location, HostSharedLooked *result, HostError *error)
{
	pghost_shared_reclaim_frame_0_t frame = {.id = id, .location = location, .result = result, .error = error};

	std_landing(&frame, pghost_shared_reclaim_body_0, pghost_shared_reclaim_pad_0);
	return frame.done;
}

/* pghost_shared_reclaimed_closure_0 is take's test of reclaimed's handle: a reclaim's role. */
static bool
pghost_shared_reclaimed_closure_0(const PghostSharedRole *role)
{
	return role->tag == PGHOST_SHARED_ROLE_RECLAIM;
}

/*
 * pghost_shared_reclaimed frees a record this process's reclaim took if its
 * slot still holds the word the take stored, which it does only if no
 * process opened the record since, and otherwise gives the record up.
 */
bool
pghost_shared_reclaimed(uint64 place)
{
	uint32		slot = pghost_shared_spot(place);
	PghostSharedRole role;
	uint64		taken;

	if (!pghost_shared_take(slot, pghost_shared_reclaimed_closure_0, &role) || role.tag != PGHOST_SHARED_ROLE_RECLAIM)
		return false;
	taken = role.u.reclaim;
	if (shmem_records_swap(slot, taken, host_shared_word_reclaimed(taken)))
		return true;
	(void) pghost_shared_turn(slot, host_shared_word_unreclaim);
	return false;
}

/* pghost_shared_unreclaim_closure_0 is take's test of unreclaim's handle: a reclaim's role. */
static bool
pghost_shared_unreclaim_closure_0(const PghostSharedRole *role)
{
	return role->tag == PGHOST_SHARED_ROLE_RECLAIM;
}

/* pghost_shared_unreclaim gives up a record this process's reclaim still has, and does nothing for one it freed or gave up. */
void
pghost_shared_unreclaim(uint64 place)
{
	uint32		slot = pghost_shared_spot(place);
	PghostSharedRole role;

	if (pghost_shared_take(slot, pghost_shared_unreclaim_closure_0, &role))
		(void) pghost_shared_turn(slot, host_shared_word_unreclaim);
}

/* pghost_shared_sweep_frame_0_t is sweep's state across its pad: its arguments and what it listed. */
typedef struct pghost_shared_sweep_frame_0_t
{
	const HostMayRaise *raise;
	uint64		id;
	size_t		atomic;
	StdVec	   *result;
	HostError  *error;
	bool		done;
	StdVec		listed;
	bool		listed_live;
} pghost_shared_sweep_frame_0_t;

/* pghost_shared_sweep_body_0 is sweep's body. */
static void
pghost_shared_sweep_body_0(void *frame_)
{
	pghost_shared_sweep_frame_0_t *frame = frame_;
	PghostSharedPool pool;
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};

	frame->done = false;
	if (!pghost_shared_pool(frame->id, &pool, frame->error))
		return;
	frame->listed = (StdVec) STD_VEC_NEW;
	frame->listed_live = true;
	for (uint32 index = 0, index_end = pool.count; index < index_end;
		 index = std_forward_unchecked_u32_Checked(index, 1))
	{
		uint64		first = shmem_records_word(STD_ADD(u32, pool.first, index, 349, 35));
		uint64		value;
		PgPgError	error_;

		if (!host_shared_word_published(first))
			continue;
		if (!pg_calls_injection_point(pghost_pg_raise(frame->raise), "doltgres-shared-sweeping", &error_))
		{
			*frame->error = pghost_held(&error_);
			frame->listed_live = false;
			std_vec_free_Checked(&frame->listed, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
			return;
		}
		value = shmem_records_atomic(pool.kind, index, frame->atomic, load);
		if (host_shared_word_swept(first, shmem_records_word(STD_ADD(u32, pool.first, index, 355, 45))))
		{
			std_TupleU64U64_t item;

			if (!std_vec_try_reserve(&frame->listed, 1, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t)))
			{
				*frame->error = host_Error_out_of_memory();
				frame->listed_live = false;
				std_vec_free_Checked(&frame->listed, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
				return;
			}
			item = (std_TupleU64U64_t)
			{
				host_shared_word_location(STD_ADD(u32, pool.first, index, 357, 41), first), value
			};
			std_vec_push_Checked(&frame->listed, &item, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
		}
	}
	frame->listed_live = false;
	*frame->result = frame->listed;
	frame->done = true;
}

/* pghost_shared_sweep_pad_0 is sweep's pad: what it listed while it is live. */
static void
pghost_shared_sweep_pad_0(void *frame_)
{
	pghost_shared_sweep_frame_0_t *frame = frame_;

	if (frame->listed_live)
		std_vec_free_Checked(&frame->listed, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
}

/*
 * pghost_shared_sweep lists the published records of a shape's pool: each
 * one's location and the value of one of its atomics, read with no handle
 * between two loads of the slot's word, for the slots those two words show
 * published at one generation. The fork's tests pause it between a slot's
 * first load and the read through an injection point.
 */
bool
pghost_shared_sweep(const HostMayRaise *raise, uint64 id, size_t atomic, StdVec *result, HostError *error)
{
	pghost_shared_sweep_frame_0_t frame = {.raise = raise, .id = id, .atomic = atomic, .result = result,
	.error = error};

	std_landing(&frame, pghost_shared_sweep_body_0, pghost_shared_sweep_pad_0);
	return frame.done;
}

/* pghost_shared_single returns the place of the server's one record of a shape, refusing a shape the server's table lists no single record of. */
bool
pghost_shared_single(uint64 id, uint64 *result, HostError *error)
{
	size_t		kind;

	if (!pghost_shared_kind(id, &kind, error))
		return false;
	if (shmem_records_form(kind) != SHMEM_RECORDS_FORM_SINGLE)
	{
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("the server's table of kinds lists no single record of shape "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&id, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*error = pghost_shared_internal(&message);
		}
		return false;
	}
	*result = pghost_shared_place(kind, 0);
	return true;
}

/*
 * pghost_shared_location returns the location of the record at a place: a
 * pooled record's, which this process has a handle of, so its slot keeps its
 * generation; an aged record's, its index and its generation as a page's
 * name is built, which a hold keeps; or zero for a single record, which has
 * no slot and is the one record of its shape.
 */
uint64
pghost_shared_location(uint64 place)
{
	size_t		kind;
	uint32		index;
	std_TupleU32U32_t pool;
	std_TupleU32U32_t aged;
	bool		has_pool;
	bool		has_aged;

	pghost_shared_placed(place, &kind, &index);
	has_pool = shmem_records_pool(kind, &pool);
	has_aged = shmem_records_aged(kind, &aged);
	if (has_pool)
	{
		uint32		slot = STD_ADD(u32, pool.f0, index, 379, 49);

		return host_shared_word_location(slot, shmem_records_word(STD_ADD(u32, pool.f0, index, 379, 78)));
	}
	if (has_aged)
		return host_delta_page_Page_value(host_delta_page_Page_of(index,
																  shmem_records_word(STD_ADD(u32, aged.f0, index, 380, 67))));
	return 0;
}

/* pghost_shared_placed_aged returns the place of the record at an index among the records of the aged kind at a number among the aged kinds. */
uint64
pghost_shared_placed_aged(uint32 aged, uint32 index)
{
	return pghost_shared_place(pghost_delta_kind_of(aged), index);
}

/* pghost_shared_live reports whether the pooled record at a location, which a slots array named, still has its slot. */
bool
pghost_shared_live(uint64 location)
{
	return host_shared_word_live(shmem_records_word((uint32) host_shared_word_slot(location)), location);
}

/* pghost_shared_gone reports whether the record of a shape that a location named no longer has its slot, refusing a location outside the shape's pool. */
bool
pghost_shared_gone(uint64 id, uint64 location, bool *result, HostError *error)
{
	PghostSharedPool pool;
	uint32		slot;

	if (!pghost_shared_pool(id, &pool, error))
		return false;
	if (!pghost_shared_slot(&pool, location, &slot, error))
		return false;
	*result = !host_shared_word_live(shmem_records_word(slot), location);
	return true;
}

/* pghost_shared_words returns the number of a kind of words among the region's kinds, refusing an id the server's table lists no words of. */
static bool
pghost_shared_words(uint64 id, size_t *result, HostError *error)
{
	size_t		kind;
	ShmemRecordsForm form;

	if (!pghost_shared_kind(id, &kind, error))
		return false;
	form = shmem_records_form(kind);
	if (!(form == SHMEM_RECORDS_FORM_PROCESS || form == SHMEM_RECORDS_FORM_KEPT))
	{
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("the server's table of kinds lists no words of id "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&id, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*error = pghost_shared_internal(&message);
		}
		return false;
	}
	*result = kind;
	return true;
}

/* pghost_shared_mine returns the place of this process's slot of a kind of words, which its exit then zeroes unless the kind is kept. */
bool
pghost_shared_mine(uint64 id, uint64 *result, HostError *error)
{
	size_t		kind;

	if (!pghost_shared_words(id, &kind, error))
		return false;
	*result = pghost_shared_place(kind, shmem_records_proc_number());
	std_AtomicBool_store_relaxed(&PGHOST_SHARED_STORED, true);
	return true;
}

/* pghost_shared_load loads a word of a process's slot of a kind of words, refusing a process number past the server's processes. */
bool
pghost_shared_load(uint64 id, uint32 proc, size_t word, uint64 *result, HostError *error)
{
	size_t		kind;
	PgPgError	error_;

	if (!pghost_shared_words(id, &kind, error))
		return false;
	if (!shmem_records_word_load(kind, proc, word, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_shared_place_of returns the place of a process's slot of a kind of words, refusing a process number past the server's processes. */
bool
pghost_shared_place_of(uint64 id, uint32 proc, uint64 *result, HostError *error)
{
	size_t		kind;

	if (!pghost_shared_words(id, &kind, error))
		return false;
	if (proc < shmem_records_count(kind))
	{
		*result = pghost_shared_place(kind, proc);
		return true;
	}
	{
		StdString	message;

		std_string_from_Checked(&message, "a process's number is past the server's processes",
								sizeof("a process's number is past the server's processes") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/* pghost_shared_processes returns the numbers of the processes whose slot of a kind of words can hold a word: every process's. */
bool
pghost_shared_processes(uint64 id, StdVec *result, HostError *error)
{
	size_t		kind;
	uint32		count;
	StdSizeHint hint;

	if (!pghost_shared_words(id, &kind, error))
		return false;
	count = shmem_records_count(kind);
	hint = (StdSizeHint)
	{
		(size_t) count, true, (size_t) count
	};
	std_vec_with_capacity_trusted(result, hint, sizeof(uint32), _Alignof(uint32));
	std_vec_extend_trusted_reserve(result, hint, sizeof(uint32), _Alignof(uint32));
	for (uint32 proc = 0, proc_end = count; proc < proc_end; proc = std_forward_unchecked_u32_Checked(proc, 1))
	{
		STD_VEC_AT(result, uint32, result->len) = proc;
		result->len = std_wrapping_add_usize(result->len, 1);
	}
	return true;
}

/* pghost_shared_at_exit_frame_0_t is at_exit's state across its pad: the works' guard. */
typedef struct pghost_shared_at_exit_frame_0_t
{
	const StdTypeId *id;
	bool		(*run) (HostIoIo *io, HostError *error);
	HostError  *error;
	bool		done;
	StdRwLockWriteGuard works;
	bool		works_live;
} pghost_shared_at_exit_frame_0_t;

/* pghost_shared_at_exit_body_0 is at_exit's body. */
static void
pghost_shared_at_exit_body_0(void *frame_)
{
	pghost_shared_at_exit_frame_0_t *frame = frame_;
	StdVec	   *works;
	bool		absent = true;

	frame->works = pghost_shared_own(&PGHOST_SHARED_WORKS_LOCK, &PGHOST_SHARED_WORKS);
	frame->works_live = true;
	works = frame->works.data;
	for (size_t i = 0, i_len = works->len; i < i_len; i++)
	{
		if (!(STD_VEC_AT(works, PghostSharedWork, i).f0 != frame->id))
		{
			absent = false;
			break;
		}
	}
	frame->done = true;
	if (absent)
	{
		if (!std_vec_try_reserve(works, 1, sizeof(PghostSharedWork), _Alignof(PghostSharedWork)))
		{
			*frame->error = host_Error_out_of_memory();
			frame->done = false;
		}
		else
		{
			PghostSharedWork work = {frame->id, frame->run};

			std_vec_push_Checked(works, &work, sizeof(PghostSharedWork), _Alignof(PghostSharedWork));
		}
	}
	frame->works_live = false;
	std_RwLockWriteGuard_drop(&frame->works);
}

/* pghost_shared_at_exit_pad_0 is at_exit's pad: the works' guard while it is live. */
static void
pghost_shared_at_exit_pad_0(void *frame_)
{
	pghost_shared_at_exit_frame_0_t *frame = frame_;

	if (frame->works_live)
		std_RwLockWriteGuard_drop(&frame->works);
}

/*
 * pghost_shared_at_exit lists an exit work of this process, once however
 * often it is asked, and refuses one asked after the process's exit callback
 * has begun, when nothing would run it.
 */
bool
pghost_shared_at_exit(const StdTypeId *id, bool (*run) (HostIoIo *io, HostError *error), HostError *error)
{
	pghost_shared_at_exit_frame_0_t frame = {.id = id, .run = run, .error = error};

	if (std_AtomicBool_load_relaxed(&PGHOST_SHARED_EXITING))
	{
		StdString	message;

		std_string_from_Checked(&message, "an exit work was registered after its process's exit began",
								sizeof("an exit work was registered after its process's exit began") - 1);
		*error = pghost_shared_internal(&message);
		return false;
	}
	std_landing(&frame, pghost_shared_at_exit_body_0, pghost_shared_at_exit_pad_0);
	return frame.done;
}

/*
 * pghost_shared_exit_step is one call of this process's exit callback that
 * may raise. It clears the flags the process holds, so a work starts with
 * none, and runs the newest exit work left, after registering the callback
 * again, so Postgres calls it for the next work whether this one returns or
 * raises; with no work left it gives up the node cache's pins.
 */
bool
pghost_shared_exit_step(const PgMayRaise *raise, HostError *error)
{
	StdRwLockWriteGuard works;
	PghostSharedWork newest;
	bool		has_newest;
	PgPgError	error_;
	HostIoIo	io;

	std_AtomicBool_store_relaxed(&PGHOST_SHARED_EXITING, true);
	shmem_records_clear_flags();
	works = pghost_shared_own(&PGHOST_SHARED_WORKS_LOCK, &PGHOST_SHARED_WORKS);
	has_newest = std_vec_pop(works.data, &newest, sizeof(PghostSharedWork));
	std_RwLockWriteGuard_drop(&works);
	if (!has_newest)
	{
		host_cache_release_all();
		return true;
	}
	if (!pg_calls_register_exit_again(raise, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	io = pghost_io(raise);
	return newest.f1(&io, error);
}

/* pghost_shared_exit_frame_0_t is exit's state across its pad: the for loop's IntoIter, while it is live. */
typedef struct pghost_shared_exit_frame_0_t
{
	StdVecIntoIter iter;
	bool		iter_live;
} pghost_shared_exit_frame_0_t;

/* pghost_shared_exit_body_0 is exit's body: HANDLES' taken handles each given up, closed, or unreclaimed, then the taken words zeroed. */
static void
pghost_shared_exit_body_0(void *frame_)
{
	pghost_shared_exit_frame_0_t *frame = frame_;
	StdRwLockWriteGuard handles = pghost_shared_handles();
	StdVec		left = *(StdVec *) handles.data;
	PghostSharedHandle handle;

	*(StdVec *) handles.data = (StdVec) STD_VEC_NEW;
	std_RwLockWriteGuard_drop(&handles);
	frame->iter = std_Vec_IntoIterator_into_iter(&left, sizeof(PghostSharedHandle), _Alignof(PghostSharedHandle),
												 NULL);
	frame->iter_live = true;
	while (std_VecIntoIter_Iterator_next(&frame->iter, &handle))
	{
		switch (handle.role.tag)
		{
			case PGHOST_SHARED_ROLE_OWNER:
				(void) pghost_shared_turn(handle.slot, host_shared_word_give_up);
				break;
			case PGHOST_SHARED_ROLE_HOLDER:
				(void) pghost_shared_turn(handle.slot, host_shared_word_close);
				break;
			case PGHOST_SHARED_ROLE_RECLAIM:
				(void) pghost_shared_turn(handle.slot, host_shared_word_unreclaim);
				break;
		}
	}
	frame->iter_live = false;
	std_VecIntoIter_drop(&frame->iter);
	if (std_AtomicBool_load_relaxed(&PGHOST_SHARED_STORED))
		shmem_records_zero_words();
}

/* pghost_shared_exit_pad_0 is exit's pad: the for loop's IntoIter's drop while it is live, its buffer freed. */
static void
pghost_shared_exit_pad_0(void *frame_)
{
	pghost_shared_exit_frame_0_t *frame = frame_;

	if (frame->iter_live)
		std_VecIntoIter_drop(&frame->iter);
}

/*
 * pghost_shared_exit gives up every record this process still has a handle
 * of and zeroes the words it took, from its exit callback, after its flags
 * are cleared, since a process ended by a FATAL or pg_terminate_backend
 * leaves without unwinding the frames whose drops would.
 */
void
pghost_shared_exit(void)
{
	pghost_shared_exit_frame_0_t frame = {0};

	std_landing(&frame, pghost_shared_exit_body_0, pghost_shared_exit_pad_0);
}

#ifdef USE_INJECTION_POINTS
/*
 * pghost_shared_held returns the number of handles this process holds of a
 * pool's records, which the fork's tests read to see a scan's hold released
 * (bugs row 358).
 */
bool
pghost_shared_held(uint64 id, uint64 *result, HostError *error)
{
	PghostSharedPool pool;
	StdRwLockWriteGuard guard;
	const StdVec *handles;

	if (!pghost_shared_pool(id, &pool, error))
		return false;
	*result = 0;
	guard = pghost_shared_handles();
	handles = guard.data;
	for (size_t i = 0; i < handles->len; i++)
	{
		const PghostSharedHandle *handle = &STD_VEC_AT(handles, PghostSharedHandle, i);

		if (handle->slot >= pool.first && handle->slot - pool.first < pool.count &&
			handle->role.tag == PGHOST_SHARED_ROLE_HOLDER)
			*result += handle->role.u.holder;
	}
	std_RwLockWriteGuard_drop(&guard);
	return true;
}
#endif

#ifdef DOLTGRES_TEST_PGHOST

/*
 * test_pghost_shared_tests_the_holders_field_holds_every_process: a
 * directory word counts more holders than Postgres can run processes, twice
 * over, since a process that looks at a record as a reclaim may open it as
 * well.
 */
void
test_pghost_shared_tests_the_holders_field_holds_every_process(void)
{
	uint64		processes = STD_ADD(u64, (uint64) MAX_BACKENDS, (uint64) DOLTGRES_AUXILIARY_PROCS, 497, 25);

	STD_ASSERT(498, 9, HOST_SHARED_WORD_HOLDERS_MAX >= STD_MUL(u64, 2, processes, 498, 38),
			   "word::HOLDERS_MAX >= 2 * processes");
}

/* test_pghost_shared_tests_a_place_is_a_kind_and_a_number: a place is a record's kind and its number among the kind's records. */
void
test_pghost_shared_tests_a_place_is_a_kind_and_a_number(void)
{
	{
		size_t		left_0;
		uint32		left_1;
		size_t		right_0 = 3;
		uint32		right_1 = 41;
		StdTupleRef left;
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_1, std_u32_Debug_fmt));

		pghost_shared_placed(pghost_shared_place(3, 41), &left_0, &left_1);
		left = STD_TUPLE(2, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_u32_Debug_fmt));
		STD_ASSERT_EQ(504, 9, left_0 == right_0 && left_1 == right_1, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		size_t		left_0;
		uint32		left_1;
		size_t		right_0 = 0;
		uint32		right_1 = PG_UINT32_MAX;
		StdTupleRef left;
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_1, std_u32_Debug_fmt));

		pghost_shared_placed(pghost_shared_place(0, PG_UINT32_MAX), &left_0, &left_1);
		left = STD_TUPLE(2, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_u32_Debug_fmt));
		STD_ASSERT_EQ(505, 9, left_0 == right_0 && left_1 == right_1, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_PGHOST */

#endif							/* FRONTEND */
