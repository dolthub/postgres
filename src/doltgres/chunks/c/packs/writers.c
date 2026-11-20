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
 * The C port of chunks' packs/writers.rs: the holds of the pack numbers a
 * process's open writers write, each process's slot of the repository it
 * writes in and the lowest number it has open.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/io.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "writers.rs"

/* CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS is Writers' ProcSlots: each process's slot for its open pack writers. */
const HostSharedProcSlotsOps CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS = {
	.id = UINT64CONST(0x6368756e6b730003), .words = 2,
};

/*
 * CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX is Repository's word: the id of the
 * repository whose packs the process writes, or zero while it writes none.
 */
#define CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX 0

/* CHUNKS_PACKS_WRITERS_LOWEST_INDEX is Lowest's word: the lowest number among the packs the process has open. */
#define CHUNKS_PACKS_WRITERS_LOWEST_INDEX 1

/*
 * ChunksPacksWritersOpen is Open: the packs a process's writers were given
 * numbers for and haven't added or abandoned, and those they added while
 * still writing, each number with its tablespace, and the repository they are
 * of while there is one.
 */
typedef struct ChunksPacksWritersOpen
{
	uint64		repository;

	/*
	 * numbers is a Vec<(u64, u32)> in the numbers' order, which holds Open's
	 * BTreeMap<u64, u32>.
	 */
	StdVec		numbers;
} ChunksPacksWritersOpen;

/* chunks_packs_writers_Open_Default_default is Open's Default: no repository and no numbers. */
static void
chunks_packs_writers_Open_Default_default(ChunksPacksWritersOpen *result)
{
	result->repository = 0;
	std_vec_new(&result->numbers, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
}

/* chunks_packs_writers_Open_drop is Open's drop glue: its numbers'. */
static void
chunks_packs_writers_Open_drop(void *value)
{
	ChunksPacksWritersOpen *self = value;

	std_vec_free_Checked(&self->numbers, sizeof(std_TupleU64U32_t), _Alignof(std_TupleU64U32_t));
}

/*
 * chunks_packs_writers_Open_find reports whether Open's numbers hold a
 * number, with its place among them, or else the place it goes in their
 * order.
 */
static bool
chunks_packs_writers_Open_find(const ChunksPacksWritersOpen *self, uint64 number, size_t *at)
{
	size_t		low = 0;
	size_t		high = self->numbers.len;

	while (low < high)
	{
		size_t		middle = std_wrapping_add_usize(low, STD_SHR_FOLDED(usize, std_wrapping_sub_usize(high, low), 1));
		uint64		found = STD_VEC_AT(&self->numbers, std_TupleU64U32_t, middle).f0;

		if (found == number)
		{
			*at = middle;
			return true;
		}
		if (found < number)
			low = std_wrapping_add_usize(middle, 1);
		else
			high = middle;
	}
	*at = low;
	return false;
}

/* taken at Open and IoMutex<Open>, which writers.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Open
#define HOST_LOCK_TAKEN_T ChunksPacksWritersOpen
#define HOST_LOCK_TAKEN_DEFAULT(place) chunks_packs_writers_Open_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) chunks_packs_writers_Open_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_PACKS_WRITERS_OPEN_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Open
#define HOST_LOCK_T ChunksPacksWritersOpen
#define HOST_LOCK_DEFAULT(place) chunks_packs_writers_Open_Default_default(place)
#include "doltgres/host/lock_template.h"

/* chunks_packs_writers_IoMutexOpen_drop is IoMutex<Open>'s drop glue: its value's. */
static void
chunks_packs_writers_IoMutexOpen_drop(void *value)
{
	HostLockIoMutexOpen *self = value;

	chunks_packs_writers_Open_drop(&self->value);
}

/* CHUNKS_PACKS_WRITERS_OPEN_TYPE is IoMutex<Open>'s size, Default, and drop. */
static const StdDefaultType CHUNKS_PACKS_WRITERS_OPEN_TYPE = {sizeof(HostLockIoMutexOpen),
_Alignof(HostLockIoMutexOpen), host_lock_IoMutexOpen_Default_default, chunks_packs_writers_IoMutexOpen_drop};

/*
 * CHUNKS_PACKS_WRITERS_OPEN is OPEN: the process's open packs, one for every
 * Packs the process has, as its slot is; its BackendLocal header and the
 * process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexOpen process;
}			CHUNKS_PACKS_WRITERS_OPEN = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * chunks_packs_writers_space_of_frame_0_t is space_of's state across its pad:
 * the process's open packs and their guard, while live.
 */
typedef struct chunks_packs_writers_space_of_frame_0_t
{
	HostIoTakesShort io;
	uint64		repository;
	uint64		number;
	bool	   *some;
	uint32	   *result;
	HostLocalLocal open;
	bool		open_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} chunks_packs_writers_space_of_frame_0_t;

/* chunks_packs_writers_space_of_pad_0 is space_of's pad: the guard, then the process's open packs, while live. */
static void
chunks_packs_writers_space_of_pad_0(void *frame_)
{
	chunks_packs_writers_space_of_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->open_live)
	{
		frame->open_live = false;
		host_local_Local_drop(&frame->open, &CHUNKS_PACKS_WRITERS_OPEN_TYPE);
	}
}

/* chunks_packs_writers_space_of_body_0 is space_of's body: the number's tablespace under the open packs' lock. */
static void
chunks_packs_writers_space_of_body_0(void *frame_)
{
	chunks_packs_writers_space_of_frame_0_t *frame = frame_;
	HostIoHeld *held;
	ChunksPacksWritersOpen *open;
	size_t		at;

	frame->open = host_local_BackendLocal_get(&CHUNKS_PACKS_WRITERS_OPEN.local, &CHUNKS_PACKS_WRITERS_OPEN_TYPE);
	frame->open_live = true;
	held = frame->io.ops->io_doors.read_doors.deref_mut.deref_mut(frame->io.self);
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->open,
																						   &CHUNKS_PACKS_WRITERS_OPEN_TYPE),
										  &CHUNKS_PACKS_WRITERS_OPEN_LOCK_TYPE, held);
	frame->guard_live = true;
	open = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	*frame->some = chunks_packs_writers_Open_find(open, frame->number, &at) && open->repository == frame->repository;
	if (*frame->some)
		*frame->result = STD_VEC_AT(&open->numbers, std_TupleU64U32_t, at).f1;
	chunks_packs_writers_space_of_pad_0(frame);
}

/* chunks_packs_writers_space_of returns the tablespace of a number one of the process's writers in a repository holds. */
void
chunks_packs_writers_space_of(HostIoTakesShort io, uint64 repository, uint64 number, bool *some, uint32 *result)
{
	chunks_packs_writers_space_of_frame_0_t frame = {.io = io, .repository = repository, .number = number,
	.some = some, .result = result};

	std_landing(&frame, chunks_packs_writers_space_of_body_0, chunks_packs_writers_space_of_pad_0);
}

/* chunks_packs_writers_kept_from_frame_0_t is kept_from's state across its pad: the processes, while live. */
typedef struct chunks_packs_writers_kept_from_frame_0_t
{
	uint64		repository;
	uint64		reserved;
	uint64	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		processes;
	bool		processes_live;
} chunks_packs_writers_kept_from_frame_0_t;

/* chunks_packs_writers_kept_from_pad_0 is kept_from's pad: the processes, while live. */
static void
chunks_packs_writers_kept_from_pad_0(void *frame_)
{
	chunks_packs_writers_kept_from_frame_0_t *frame = frame_;

	if (frame->processes_live)
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
}

/*
 * chunks_packs_writers_kept_from_body_0 is kept_from's body: the lowest
 * number each process with the repository's slot has open, below one past
 * the numbers reserved.
 */
static void
chunks_packs_writers_kept_from_body_0(void *frame_)
{
	chunks_packs_writers_kept_from_frame_0_t *frame = frame_;
	uint64		from = STD_ADD(u64, frame->reserved, 1, 77, 20);
	HostError	host;

	frame->ok = false;
	if (!host_shared_processes(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, &frame->processes, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	frame->processes_live = true;
	for (size_t k = 0, k_len = frame->processes.len; k < k_len; k++)
	{
		uint32		process = STD_VEC_AT(&frame->processes, uint32, k);
		uint64		repository;
		uint64		lowest;

		if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX,
							  process, &repository, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_packs_writers_kept_from_pad_0(frame);
			return;
		}
		if (repository != frame->repository)
			continue;
		if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, process,
							  &lowest, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_packs_writers_kept_from_pad_0(frame);
			return;
		}
		if (lowest == 0)
			continue;
		if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX,
							  process, &repository, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_packs_writers_kept_from_pad_0(frame);
			return;
		}
		if (repository == frame->repository)
			from = std_min_u64(from, lowest);
	}
	chunks_packs_writers_kept_from_pad_0(frame);
	*frame->result = from;
	frame->ok = true;
}

/*
 * chunks_packs_writers_kept_from returns the number from which a collection
 * of the repository with an id keeps every pack, added to the table or not:
 * the lowest a process has open there, or one past the numbers reserved when
 * it read them, which it reads before the slots.
 */
bool
chunks_packs_writers_kept_from(uint64 repository, uint64 reserved, uint64 *result, ChunksErrorError *error)
{
	chunks_packs_writers_kept_from_frame_0_t frame = {.repository = repository, .reserved = reserved,
	.result = result, .error = error};

	std_landing(&frame, chunks_packs_writers_kept_from_body_0, chunks_packs_writers_kept_from_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_writers_covers reports whether the writers' word of a process,
 * one of those that took a slot, covers an unfinished pack with a number in
 * the repository with an id: the word names the repository, and its lowest
 * number is at or below the pack's.
 */
bool
chunks_packs_writers_covers(const uint32 *slots, size_t slots_len, uint32 process, uint64 repository, uint64 number,
							bool *result, ChunksErrorError *error)
{
	bool		found = false;
	uint64		lowest;
	uint64		named;
	HostError	host;

	for (size_t k = 0, k_len = slots_len; k < k_len; k++)
		if (slots[k] == process)
		{
			found = true;
			break;
		}
	if (!found)
	{
		*result = false;
		return true;
	}
	if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, process,
						  &lowest, &host) ||
		!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX, process,
						  &named, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	*result = named == repository && lowest != 0 && lowest <= number;
	return true;
}

/* chunks_packs_writers_writing_in_frame_0_t is writing_in's state across its pad: the slots' processes, while live. */
typedef struct chunks_packs_writers_writing_in_frame_0_t
{
	uint64		repository;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		processes;
	bool		processes_live;
} chunks_packs_writers_writing_in_frame_0_t;

/* chunks_packs_writers_writing_in_pad_0 is writing_in's pad: the slots' processes, while live. */
static void
chunks_packs_writers_writing_in_pad_0(void *frame_)
{
	chunks_packs_writers_writing_in_frame_0_t *frame = frame_;

	if (frame->processes_live)
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
}

/* chunks_packs_writers_writing_in_body_0 is writing_in's body: whether any slot names the repository with a number held. */
static void
chunks_packs_writers_writing_in_body_0(void *frame_)
{
	chunks_packs_writers_writing_in_frame_0_t *frame = frame_;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_writers_slots(&frame->processes, frame->error))
		return;
	frame->processes_live = true;
	*frame->result = false;
	for (size_t k = 0, k_len = frame->processes.len; k < k_len; k++)
	{
		uint32		process = STD_VEC_AT(&frame->processes, uint32, k);
		uint64		repository;
		uint64		lowest;

		if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX,
							  process, &repository, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_packs_writers_writing_in_pad_0(frame);
			return;
		}
		if (repository != frame->repository)
			continue;
		if (!host_shared_load(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, process,
							  &lowest, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			chunks_packs_writers_writing_in_pad_0(frame);
			return;
		}
		if (lowest != 0)
		{
			*frame->result = true;
			break;
		}
	}
	chunks_packs_writers_writing_in_pad_0(frame);
	frame->ok = true;
}

/* chunks_packs_writers_writing_in reports whether any process's writers hold a number of a repository. */
bool
chunks_packs_writers_writing_in(uint64 repository, bool *result, ChunksErrorError *error)
{
	chunks_packs_writers_writing_in_frame_0_t frame = {.repository = repository, .result = result, .error = error};

	std_landing(&frame, chunks_packs_writers_writing_in_body_0, chunks_packs_writers_writing_in_pad_0);
	return frame.ok;
}

/* chunks_packs_writers_slots returns the numbers of the processes that took a writers' slot. */
bool
chunks_packs_writers_slots(StdVec *result, ChunksErrorError *error)
{
	HostError	host;

	if (!host_shared_processes(&CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, result, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/* chunks_packs_writers_Writing_open_closure_0 is open's event: the slot it stored, with its lowest number. */
static void
chunks_packs_writers_Writing_open_closure_0(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_WRITER_STORED;
	result->u.writer_stored.lowest = *(const uint64 *) captures;
}

/*
 * chunks_packs_writers_Writing_open_frame_0_t is Writing::open's state across
 * its pad: its arguments and closures, the process's open packs and their
 * guard, and the hold, each while live.
 */
typedef struct chunks_packs_writers_Writing_open_frame_0_t
{
	HostIoIo   *io;
	uint64		repository;
	uint32		space;
	uint64		(*reserved) (void *captures, HostSharedOp op);
	void	   *reserved_captures;
	void		(*reserved_drop) (void *captures);
	bool		(*raise) (void *captures, HostIoIo *io, uint64 numbers, ChunksErrorError *error);
	void	   *raise_captures;
	void		(*raise_drop) (void *captures);
	ChunksPacksWritersWriting *result;
	ChunksErrorError *error;
	bool		ok;
	bool		closures_live;
	HostLocalLocal open;
	bool		open_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	ChunksPacksWritersWriting hold;
	bool		hold_live;
} chunks_packs_writers_Writing_open_frame_0_t;

/*
 * chunks_packs_writers_Writing_open_pad_0 is Writing::open's pad: the guard,
 * the hold, the process's open packs, and the closures, while live.
 */
static void
chunks_packs_writers_Writing_open_pad_0(void *frame_)
{
	chunks_packs_writers_Writing_open_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(&frame->hold);
	}
	if (frame->open_live)
	{
		frame->open_live = false;
		host_local_Local_drop(&frame->open, &CHUNKS_PACKS_WRITERS_OPEN_TYPE);
	}
	if (frame->closures_live)
	{
		frame->closures_live = false;
		if (frame->reserved_drop != NULL)
			frame->reserved_drop(frame->reserved_captures);
		if (frame->raise_drop != NULL)
			frame->raise_drop(frame->raise_captures);
	}
}

/*
 * chunks_packs_writers_Writing_open_lock_0 takes the guard of the process's
 * open packs and returns them.
 */
static ChunksPacksWritersOpen *
chunks_packs_writers_Writing_open_lock_0(chunks_packs_writers_Writing_open_frame_0_t *frame)
{
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->open,
																						   &CHUNKS_PACKS_WRITERS_OPEN_TYPE),
										  &CHUNKS_PACKS_WRITERS_OPEN_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/*
 * chunks_packs_writers_Writing_open_body_0 is Writing::open's body: the
 * slot's store when the process has no pack open, the pause after it, the
 * number's reservation, and its entry among the open packs.
 */
static void
chunks_packs_writers_Writing_open_body_0(void *frame_)
{
	chunks_packs_writers_Writing_open_frame_0_t *frame = frame_;
	const HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedMine slot;
	ChunksPacksWritersOpen *open;
	bool		stored;
	size_t		at;
	std_TupleU64U32_t entry;
	HostError	host;

	frame->ok = false;
	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(frame->io), &CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS, &slot, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_packs_writers_Writing_open_pad_0(frame);
		return;
	}
	frame->open = host_local_BackendLocal_get(&CHUNKS_PACKS_WRITERS_OPEN.local, &CHUNKS_PACKS_WRITERS_OPEN_TYPE);
	frame->open_live = true;
	open = chunks_packs_writers_Writing_open_lock_0(frame);
	if (open->numbers.len != 0 && open->repository != frame->repository)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a process writes packs of two repositories at once")};
		StdFmtArguments arguments = {pieces, 1, NULL};

		frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
		chunks_packs_writers_Writing_open_pad_0(frame);
		return;
	}
	stored = open->numbers.len == 0;
	if (stored)
	{
		uint64		lowest = STD_ADD(u64, chunks_packs_shared_given(frame->reserved(frame->reserved_captures, load)), 1,
									 153, 30);

		host_shared_Mine_store(&slot, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, lowest);
		host_shared_Mine_store(&slot, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX, frame->repository);
		chunks_trace_emit(frame->repository, chunks_packs_writers_Writing_open_closure_0, &lowest, NULL);
		open->repository = frame->repository;
	}
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->hold.number = 0;
	frame->hold.slot = slot;
	frame->hold_live = true;
	if (stored && !host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-chunks-writer-stored", &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_packs_writers_Writing_open_pad_0(frame);
		return;
	}
	for (;;)
	{
		uint64		numbers = frame->reserved(frame->reserved_captures, load);

		if (chunks_packs_shared_headroom(numbers) != 0)
		{
			HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE,
			.u.exchange = {numbers, chunks_packs_shared_taken(numbers)}};

			if (frame->reserved(frame->reserved_captures, exchange) == numbers)
			{
				frame->hold.number = STD_ADD(u64, chunks_packs_shared_given(numbers), 1, 168, 93);
				break;
			}
		}
		else if (!frame->raise(frame->raise_captures, frame->io, numbers, frame->error))
		{
			chunks_packs_writers_Writing_open_pad_0(frame);
			return;
		}
	}
	open = chunks_packs_writers_Writing_open_lock_0(frame);
	entry.f0 = frame->hold.number;
	entry.f1 = frame->space;
	if (chunks_packs_writers_Open_find(open, frame->hold.number, &at))
		STD_VEC_AT(&open->numbers, std_TupleU64U32_t, at) = entry;
	else
		std_vec_insert(&open->numbers, at, &entry, sizeof(entry), _Alignof(std_TupleU64U32_t), RUST_FILE, 173, 9);
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	*frame->result = frame->hold;
	frame->hold_live = false;
	frame->ok = true;
	chunks_packs_writers_Writing_open_pad_0(frame);
}

/*
 * chunks_packs_writers_Writing_open gives a writer of a repository's packs in
 * a tablespace its number, the next after the highest given in the word
 * reserved loads and exchanges, while the word has headroom under the bound
 * the pack table's file holds; with none it has raise write a higher bound
 * first. It stores the process's slot before the number is reserved, when
 * the process has no pack open, and refuses a process that has packs of
 * another repository open.
 */
bool
chunks_packs_writers_Writing_open(HostIoIo *io, uint64 repository, uint32 space,
								  uint64 (*reserved) (void *captures, HostSharedOp op), void *reserved_captures,
								  void (*reserved_drop) (void *captures),
								  bool (*raise) (void *captures, HostIoIo *io, uint64 numbers,
												 ChunksErrorError *error),
								  void *raise_captures, void (*raise_drop) (void *captures),
								  ChunksPacksWritersWriting *result, ChunksErrorError *error)
{
	chunks_packs_writers_Writing_open_frame_0_t frame = {.io = io, .repository = repository, .space = space,
		.reserved = reserved, .reserved_captures = reserved_captures, .reserved_drop = reserved_drop,
		.raise = raise, .raise_captures = raise_captures, .raise_drop = raise_drop, .result = result,
	.error = error, .closures_live = true};

	std_landing(&frame, chunks_packs_writers_Writing_open_body_0, chunks_packs_writers_Writing_open_pad_0);
	return frame.ok;
}

/* chunks_packs_writers_Writing_number returns the pack's number. */
uint64
chunks_packs_writers_Writing_number(const ChunksPacksWritersWriting *self)
{
	return self->number;
}

/*
 * chunks_packs_writers_Writing_drop lets the pack's number go and stores the
 * process's slot, its lowest open number, or no repository and none once it
 * has none, with no I/O and nothing allocated.
 */
void
chunks_packs_writers_Writing_drop(ChunksPacksWritersWriting *self)
{
	HostLocalLocal local;
	HostIoInDrop in_drop;
	StdRwLockWriteGuard guard;

	if (!host_local_BackendLocal_existing(&CHUNKS_PACKS_WRITERS_OPEN.local, &local))
		return;
	in_drop = host_io_InDrop_new();
	if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																						&CHUNKS_PACKS_WRITERS_OPEN_TYPE),
									   &in_drop, &guard))
	{
		ChunksPacksWritersOpen *open = guard.data;
		size_t		at;

		if (chunks_packs_writers_Open_find(open, self->number, &at))
		{
			std_TupleU64U32_t removed;

			std_vec_remove(&open->numbers, at, &removed, sizeof(removed), _Alignof(std_TupleU64U32_t), RUST_FILE, 188,
						   13);
		}
		if (open->numbers.len != 0)
		{
			uint64		lowest = STD_VEC_AT(&open->numbers, std_TupleU64U32_t, 0).f0;
			ChunksTraceDropped dropped = {.tag = CHUNKS_TRACE_DROPPED_WRITER_STORED, .u.writer_stored = lowest};

			host_shared_Mine_store(&self->slot, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, lowest);
			chunks_trace_emit_in_drop(open->repository, dropped);
		}
		else
		{
			ChunksTraceDropped dropped = {.tag = CHUNKS_TRACE_DROPPED_WRITER_CLEARED};

			host_shared_Mine_store(&self->slot, CHUNKS_PACKS_WRITERS_REPOSITORY_INDEX, 0);
			host_shared_Mine_store(&self->slot, CHUNKS_PACKS_WRITERS_LOWEST_INDEX, 0);
			chunks_trace_emit_in_drop(open->repository, dropped);
		}
		std_RwLockWriteGuard_drop(&guard);
	}
	host_local_Local_drop(&local, &CHUNKS_PACKS_WRITERS_OPEN_TYPE);
}
