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
 * The C port of chunks' packs/backend.rs: a backend's hold of a repository's
 * packs, its handle on the repository's pack record and, beside it, its
 * view, which it brings up to date before each read, lookup, and change.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/merge.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/marking.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "backend.rs"

/* CHUNKS_PACKS_BACKEND_COPY_BLOCK is how many bytes a repository's copy reads and writes at once. */
#define CHUNKS_PACKS_BACKEND_COPY_BLOCK (UINT64CONST(1) << 20)

/* taken at View and LongMutex<View>, which backend.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS View
#define HOST_LOCK_TAKEN_T ChunksPacksViewView
#define HOST_LOCK_TAKEN_DEFAULT(place) chunks_packs_view_View_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) chunks_packs_view_View_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_PACKS_BACKEND_VIEW_LOCK_TYPE
#define HOST_LOCK_LONG_MUTEX
#define HOST_LOCK_TS View
#define HOST_LOCK_T ChunksPacksViewView
#include "doltgres/host/lock_template.h"

/* ChunksPacksBackendViewMutex is the backend's view under its LongMutex, in the box Packs holds. */
struct ChunksPacksBackendViewMutex
{
	HostLockLongMutexView mutex;
};

/* CHUNKS_PACKS_BACKEND_VIEW_AT is where Packs' LongMutex is declared, which a take that finds it held names. */
static const StdLocation CHUNKS_PACKS_BACKEND_VIEW_AT = {RUST_FILE, 179, 20};

/* ChunksPacksBackendOpen is OPEN's value, the Vec<u64> of the records this process holds through its Packs. */
typedef struct ChunksPacksBackendOpen
{
	StdVec		locations;
} ChunksPacksBackendOpen;

/* chunks_packs_backend_Open_Default_default is an empty list of locations. */
static void
chunks_packs_backend_Open_Default_default(ChunksPacksBackendOpen *result)
{
	std_vec_new(&result->locations, sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_backend_Open_drop is the list's drop glue. */
static void
chunks_packs_backend_Open_drop(ChunksPacksBackendOpen *self)
{
	std_vec_free_Checked(&self->locations, sizeof(uint64), _Alignof(uint64));
}

/* taken at Open and IoMutex<Open>, which backend.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Open
#define HOST_LOCK_TAKEN_T ChunksPacksBackendOpen
#define HOST_LOCK_TAKEN_DEFAULT(place) chunks_packs_backend_Open_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) chunks_packs_backend_Open_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_PACKS_BACKEND_OPEN_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Open
#define HOST_LOCK_T ChunksPacksBackendOpen
#define HOST_LOCK_DEFAULT(place) chunks_packs_backend_Open_Default_default(place)
#include "doltgres/host/lock_template.h"

/* chunks_packs_backend_IoMutexOpen_drop is IoMutex<Open>'s drop glue: its value's. */
static void
chunks_packs_backend_IoMutexOpen_drop(void *value)
{
	chunks_packs_backend_Open_drop(&((HostLockIoMutexOpen *) value)->value);
}

/* CHUNKS_PACKS_BACKEND_OPEN_TYPE is IoMutex<Open>'s size, Default, and drop. */
static const StdDefaultType CHUNKS_PACKS_BACKEND_OPEN_TYPE = {sizeof(HostLockIoMutexOpen),
_Alignof(HostLockIoMutexOpen), host_lock_IoMutexOpen_Default_default, chunks_packs_backend_IoMutexOpen_drop};

/*
 * CHUNKS_PACKS_BACKEND_OPEN is OPEN: the locations of the pack records this
 * process holds through its Packs, one for each, which its exit reads to
 * append the adds they defer; its BackendLocal header and the process's
 * value, which follows it. Under its lock a section grows the list only into
 * room it reserved first, so nothing there unwinds and the lock is never
 * poisoned (bugs row 517).
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexOpen process;
}			CHUNKS_PACKS_BACKEND_OPEN = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* chunks_packs_backend_open_lock takes OPEN's lock through a local of it and returns the list it guards. */
static ChunksPacksBackendOpen *
chunks_packs_backend_open_lock(HostLocalLocal *open, HostIoHeld *held, HostLockLockedRwLockWriteGuard *guard)
{
	*guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(open,
																					 &CHUNKS_PACKS_BACKEND_OPEN_TYPE),
									&CHUNKS_PACKS_BACKEND_OPEN_LOCK_TYPE, held);
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(guard);
}

/*
 * chunks_packs_backend_held_frame_0_t is held's state across its pad: the
 * view made, until the box holds it.
 */
typedef struct chunks_packs_backend_held_frame_0_t
{
	HostSharedHeld *held;
	uint64		location;
	ChunksPacksBackendPacks *result;
	ChunksPacksViewView view;
	bool		view_live;
} chunks_packs_backend_held_frame_0_t;

/* chunks_packs_backend_held_pad_0 is held's pad: the view, while no box holds it. */
static void
chunks_packs_backend_held_pad_0(void *frame_)
{
	chunks_packs_backend_held_frame_0_t *frame = frame_;

	if (frame->view_live)
	{
		frame->view_live = false;
		chunks_packs_view_View_drop(&frame->view);
	}
}

/* chunks_packs_backend_held_body_0 is held's body: the view made, then boxed under its LongMutex. */
static void
chunks_packs_backend_held_body_0(void *frame_)
{
	chunks_packs_backend_held_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	ChunksPacksBackendPacks *result = frame->result;

	result->repository = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX);
	result->own = (uint32) chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_OWN_INDEX);
	chunks_packs_view_View_new((uint32) result->repository, result->own, &frame->view);
	frame->view_live = true;
	result->view = std_alloc(sizeof(ChunksPacksBackendViewMutex), _Alignof(ChunksPacksBackendViewMutex));
	frame->view_live = false;
	host_lock_LongMutexView_new(&result->view->mutex, frame->view, &CHUNKS_PACKS_BACKEND_VIEW_AT);
	result->held = *frame->held;
	result->location = frame->location;
}

/*
 * chunks_packs_backend_held returns the packs over a pack record, off the
 * process's list of those its exit closes. It takes the record, which the
 * packs hold once it returns.
 */
static void
chunks_packs_backend_held(HostSharedHeld *held, uint64 location, ChunksPacksBackendPacks *result)
{
	chunks_packs_backend_held_frame_0_t frame = {.held = held, .location = location, .result = result};

	std_landing(&frame, chunks_packs_backend_held_body_0, chunks_packs_backend_held_pad_0);
}

/*
 * chunks_packs_backend_Closing_frame_0_t is the exit work's state across its
 * pad: OPEN's local and guard, the locations taken, the record opened, the
 * packs over it, and its reader.
 */
typedef struct chunks_packs_backend_Closing_frame_0_t
{
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	HostLocalLocal open;
	bool		open_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	StdVec		locations;
	bool		locations_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} chunks_packs_backend_Closing_frame_0_t;

/* chunks_packs_backend_Closing_pad_0 is the exit work's pad: what it holds, newest first. */
static void
chunks_packs_backend_Closing_pad_0(void *frame_)
{
	chunks_packs_backend_Closing_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->locations_live)
	{
		frame->locations_live = false;
		std_vec_free_Checked(&frame->locations, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->open_live)
	{
		frame->open_live = false;
		host_local_Local_drop(&frame->open, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	}
}

/* chunks_packs_backend_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
chunks_packs_backend_u64_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* The exit work's sort of its locations: sort_unstable at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT chunks_packs_backend_u64_lt
#include "doltgres/std/sort_template.h"

/* chunks_packs_backend_Address_lt is the fn item Address's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
chunks_packs_backend_Address_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return chunks_address_Address_PartialOrd_lt(a, b);
}

/* with_prefix's sort of the addresses it found: sort_unstable at Address. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS Address
#define STD_SORT_T ChunksAddressAddress
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT chunks_packs_backend_Address_lt
#include "doltgres/std/sort_template.h"

/*
 * chunks_packs_backend_Closing_body_0 is the exit work's body: each pack
 * record the process still holds through its Packs opened again, once, and
 * its deferred adds appended.
 */
static void
chunks_packs_backend_Closing_body_0(void *frame_)
{
	chunks_packs_backend_Closing_frame_0_t *frame = frame_;
	ChunksPacksBackendOpen *open;
	size_t		kept = 0;

	frame->ok = false;
	frame->open = host_local_BackendLocal_get(&CHUNKS_PACKS_BACKEND_OPEN.local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	frame->open_live = true;
	open = chunks_packs_backend_open_lock(&frame->open, &frame->io->held, &frame->guard);
	frame->guard_live = true;
	frame->locations = open->locations;
	frame->locations_live = true;
	std_vec_new(&open->locations, sizeof(uint64), _Alignof(uint64));
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->open_live = false;
	host_local_Local_drop(&frame->open, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	std_slice_sort_unstable_U64(frame->locations.ptr, frame->locations.len);
	for (size_t k = 0, k_len = frame->locations.len; k < k_len; k++)
	{
		uint64		location = STD_VEC_AT(&frame->locations, uint64, k);

		if (kept == 0 || STD_VEC_AT(&frame->locations, uint64, kept - 1) != location)
			STD_VEC_AT(&frame->locations, uint64, kept++) = location;
	}
	frame->locations.len = kept;
	for (size_t k = 0, k_len = frame->locations.len; k < k_len; k++)
	{
		uint64		location = STD_VEC_AT(&frame->locations, uint64, k);
		bool		has_held;
		HostSharedHeld held;
		ChunksErrorError failed;

		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location,
							  &has_held, &held, frame->error))
			goto done;
		if (!has_held)
			continue;
		chunks_packs_backend_held(&held, location, &frame->packs);
		frame->packs_live = true;
		if (!host_shared_read(frame->io, &frame->reading, frame->error))
			goto done;
		frame->reading_live = true;
		if (!chunks_packs_backend_Packs_flush(&frame->packs, HOST_IO_IO_TAKES_SHORT(frame->io), &frame->reading,
											  &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	frame->ok = true;
done:
	chunks_packs_backend_Closing_pad_0(frame);
}

/*
 * chunks_packs_backend_Closing_Exit_run is Closing's Exit: a process's work
 * at its exit for the packs it still holds, which appends the adds each
 * defers to the list files, as their close would have.
 */
static bool
chunks_packs_backend_Closing_Exit_run(HostIoIo *io, HostError *error)
{
	chunks_packs_backend_Closing_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, chunks_packs_backend_Closing_body_0, chunks_packs_backend_Closing_pad_0);
	return frame.ok;
}

/* CHUNKS_PACKS_BACKEND_CLOSING_TYPE_ID is Closing's identity, by which its exit work is registered once. */
static const StdTypeId CHUNKS_PACKS_BACKEND_CLOSING_TYPE_ID = {"chunks::packs::backend::Closing", 0, 1, NULL};

/* CHUNKS_PACKS_BACKEND_CLOSING_EXIT_OPS is Closing's Exit. */
static const HostExitOps CHUNKS_PACKS_BACKEND_CLOSING_EXIT_OPS = {chunks_packs_backend_Closing_Exit_run};

/*
 * chunks_packs_backend_Packs_over_frame_0_t is over's state across its pad:
 * the record it takes, OPEN's local, and its guard.
 */
typedef struct chunks_packs_backend_Packs_over_frame_0_t
{
	HostIoIo   *io;
	HostSharedHeld *held;
	uint64		location;
	ChunksPacksBackendPacks *result;
	ChunksErrorError *error;
	bool		ok;
	bool		held_live;
	HostLocalLocal open;
	bool		open_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
} chunks_packs_backend_Packs_over_frame_0_t;

/* chunks_packs_backend_Packs_over_pad_0 is over's pad: the guard, the local, then the record. */
static void
chunks_packs_backend_Packs_over_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_over_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->open_live)
	{
		frame->open_live = false;
		host_local_Local_drop(&frame->open, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(frame->held);
	}
}

/* chunks_packs_backend_Packs_over_body_0 is over's body: the exit work listed, the location noted, then the packs. */
static void
chunks_packs_backend_Packs_over_body_0(void *frame_)
{
	chunks_packs_backend_Packs_over_frame_0_t *frame = frame_;
	HostError	host;
	ChunksPacksBackendOpen *list;

	frame->ok = false;
	if (!host_at_exit(&CHUNKS_PACKS_BACKEND_CLOSING_TYPE_ID, &CHUNKS_PACKS_BACKEND_CLOSING_EXIT_OPS, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_packs_backend_Packs_over_pad_0(frame);
		return;
	}
	frame->open = host_local_BackendLocal_get(&CHUNKS_PACKS_BACKEND_OPEN.local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	frame->open_live = true;
	list = chunks_packs_backend_open_lock(&frame->open, &frame->io->held, &frame->guard);
	frame->guard_live = true;
	if (!std_vec_try_reserve(&list->locations, 1, sizeof(frame->location), _Alignof(uint64)))
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
		host = host_Error_out_of_memory();
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_packs_backend_Packs_over_pad_0(frame);
		return;
	}
	std_vec_push_Checked(&list->locations, &frame->location, sizeof(frame->location), _Alignof(uint64));
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->open_live = false;
	host_local_Local_drop(&frame->open, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	frame->held_live = false;
	chunks_packs_backend_held(frame->held, frame->location, frame->result);
	frame->ok = true;
}

/*
 * chunks_packs_backend_Packs_over returns a backend's packs over a pack
 * record it opened, on the process's list of those its exit closes. It takes
 * the record, which it drops on an error.
 */
bool
chunks_packs_backend_Packs_over(HostIoIo *io, HostSharedHeld *held, uint64 location, ChunksPacksBackendPacks *result,
								ChunksErrorError *error)
{
	chunks_packs_backend_Packs_over_frame_0_t frame = {.io = io, .held = held, .location = location,
	.result = result, .error = error, .held_live = true};

	std_landing(&frame, chunks_packs_backend_Packs_over_body_0, chunks_packs_backend_Packs_over_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_drop takes the packs off the process's list of
 * those its exit closes, then lets the record and the view go.
 */
void
chunks_packs_backend_Packs_drop(ChunksPacksBackendPacks *self)
{
	HostLocalLocal local;

	if (host_local_BackendLocal_existing(&CHUNKS_PACKS_BACKEND_OPEN.local, &local))
	{
		HostIoInDrop in_drop = host_io_InDrop_new();
		StdRwLockWriteGuard guard;

		if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																							&CHUNKS_PACKS_BACKEND_OPEN_TYPE),
										   &in_drop, &guard))
		{
			ChunksPacksBackendOpen *open = guard.data;

			for (size_t k = 0, k_len = open->locations.len; k < k_len; k++)
				if (STD_VEC_AT(&open->locations, uint64, k) == self->location)
				{
					uint64		removed;

					std_vec_swap_remove(&open->locations, k, &removed, sizeof(removed), _Alignof(uint64));
					break;
				}
			std_RwLockWriteGuard_drop(&guard);
		}
		host_local_Local_drop(&local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	}
	host_shared_Held_drop(&self->held);
	chunks_packs_view_View_drop(&self->view->mutex.value);
	std_free(self->view, sizeof(ChunksPacksBackendViewMutex), _Alignof(ChunksPacksBackendViewMutex));
}

static bool chunks_packs_backend_copied_dir(HostIoIoDoors io, const char *dir, size_t dir_len, const StdVec *skipped,
											const HostSharedPauses *pauses, const char *into,
											size_t into_len, bool created, ChunksErrorError *error);

/* chunks_packs_backend_view_lock_frame_0_t is view_lock's state across its pad: the packs, and the view while it lends it. */
typedef struct chunks_packs_backend_view_lock_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	HostLockLending view;
	bool		view_live;
} chunks_packs_backend_view_lock_frame_0_t;

/* chunks_packs_backend_view_lock_pad_0 is view_lock's pad: the view, which an unwind gives back poisoned. */
static void
chunks_packs_backend_view_lock_pad_0(void *frame_)
{
	chunks_packs_backend_view_lock_frame_0_t *frame = frame_;

	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/*
 * chunks_packs_backend_view_lock_body_0 is view_lock's body: the view taken,
 * and made again for the repository when a panic's poisoning rebuilt it to
 * View's default, which names no directory (B5; bugs row 499).
 */
static void
chunks_packs_backend_view_lock_body_0(void *frame_)
{
	chunks_packs_backend_view_lock_frame_0_t *frame = frame_;
	ChunksPacksViewView *value;

	frame->view = host_lock_LongMutex_lock(&frame->self->view->mutex.header, &CHUNKS_PACKS_BACKEND_VIEW_LOCK_TYPE,
										   frame->io);
	frame->view_live = true;
	value = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	if (value->repository != 0)
		return;
	chunks_packs_view_View_drop(value);
	chunks_packs_view_View_Default_default(value);
	chunks_packs_view_View_new((uint32) frame->self->repository, frame->self->own, value);
}

/*
 * chunks_packs_backend_Packs_view_lock takes the backend's view under its
 * lock, for the modules of packs that change it and every read of it, made
 * again for the repository when a panic's poisoning left View's default.
 */
HostLockLending
chunks_packs_backend_Packs_view_lock(const ChunksPacksBackendPacks *self, HostIoTakesShort io)
{
	chunks_packs_backend_view_lock_frame_0_t frame = {.self = self, .io = io};

	std_landing(&frame, chunks_packs_backend_view_lock_body_0, chunks_packs_backend_view_lock_pad_0);
	return frame.view;
}

/* chunks_packs_backend_Packs_viewed_frame_0_t is viewed's state across its pad: the view's guard. */
typedef struct chunks_packs_backend_Packs_viewed_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	chunks_packs_backend_Read read;
	void	   *captures;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
} chunks_packs_backend_Packs_viewed_frame_0_t;

/* chunks_packs_backend_Packs_viewed_pad_0 is viewed's pad: the view's guard. */
static void
chunks_packs_backend_Packs_viewed_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_viewed_frame_0_t *frame = frame_;

	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/* chunks_packs_backend_Packs_viewed_body_0 is viewed's body: the roots settled, the view refreshed, then the read. */
static void
chunks_packs_backend_Packs_viewed_body_0(void *frame_)
{
	chunks_packs_backend_Packs_viewed_frame_0_t *frame = frame_;
	ChunksPacksViewView *view;

	frame->ok = false;
	if (!chunks_packs_roots_Packs_settle_roots(frame->self, frame->io, frame->error))
		return;
	frame->view = chunks_packs_backend_Packs_view_lock(frame->self, HOST_IO_IO_TAKES_SHORT(frame->io));
	frame->view_live = true;
	view = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	frame->ok = chunks_packs_view_View_refresh(view, HOST_IO_IO_TAKES_SHORT(frame->io),
											   host_shared_Held_record(&frame->self->held),
											   frame->error) &&
		frame->read(frame->captures, view, frame->io, frame->error);
	chunks_packs_backend_Packs_viewed_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_viewed brings the backend's view up to date and
 * runs a read over it, for a caller that has published its reader's epoch,
 * which keeps the files its view opens from being removed under it.
 */
bool
chunks_packs_backend_Packs_viewed(const ChunksPacksBackendPacks *self, HostIoIo *io, chunks_packs_backend_Read read,
								  void *captures, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_viewed_frame_0_t frame = {.self = self, .io = io, .read = read, .captures = captures,
	.error = error};

	std_landing(&frame, chunks_packs_backend_Packs_viewed_body_0, chunks_packs_backend_Packs_viewed_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_changed_frame_0_t is changed's state across its
 * pad: the view's guard and the change under way.
 */
typedef struct chunks_packs_backend_Packs_changed_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	chunks_packs_backend_Change change;
	void	   *captures;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
	ChunksPacksListsChange under;
	bool		under_live;
} chunks_packs_backend_Packs_changed_frame_0_t;

/* chunks_packs_backend_Packs_changed_pad_0 is changed's pad: the change, then the view's guard. */
static void
chunks_packs_backend_Packs_changed_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_changed_frame_0_t *frame = frame_;

	if (frame->under_live)
	{
		frame->under_live = false;
		chunks_packs_lists_Change_drop(&frame->under);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/* chunks_packs_backend_Packs_changed_body_0 is changed's body: the change run under the lists lock and installed. */
static void
chunks_packs_backend_Packs_changed_body_0(void *frame_)
{
	chunks_packs_backend_Packs_changed_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(&frame->self->held);
	HostSharedLongGuard long_;
	HostError	host;

	frame->ok = false;
	frame->view = chunks_packs_backend_Packs_view_lock(frame->self, frame->io);
	frame->view_live = true;
	if (!host_shared_Record_lock_long(record, host_lock_Lending_io(&frame->view), CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX,
									  &long_, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	if (!chunks_packs_lists_Change_begin(host_lock_Lending_DerefMut_deref_mut(&frame->view), record, &long_,
										 &frame->under, frame->error))
		goto done;
	frame->under_live = true;
	if (!frame->change(frame->captures, &frame->under, frame->error))
		goto done;
	frame->under_live = false;
	frame->ok = chunks_packs_lists_Change_install(&frame->under, frame->error);
done:
	chunks_packs_backend_Packs_changed_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_changed runs a change to the list files under
 * the lists lock and installs it, and then writes whole any list file the
 * change left past its slack.
 */
bool
chunks_packs_backend_Packs_changed(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
								   const HostSharedReader *reading, chunks_packs_backend_Change change, void *captures,
								   ChunksErrorError *error)
{
	chunks_packs_backend_Packs_changed_frame_0_t frame = {.self = self, .io = io, .change = change,
	.captures = captures, .error = error};

	std_landing(&frame, chunks_packs_backend_Packs_changed_body_0, chunks_packs_backend_Packs_changed_pad_0);
	return frame.ok && chunks_packs_backend_Packs_compact(self, io, reading, false, error);
}

/*
 * chunks_packs_backend_Packs_compact_frame_0_t is compact's state across its
 * pad, and remove_space's: the view's guard, the list files' flag, and the
 * rewrite under way.
 */
typedef struct chunks_packs_backend_Packs_compact_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	bool		all;
	bool		removing;
	uint32		space;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
	ChunksPacksListsChange rewrite;
	bool		rewrite_live;
} chunks_packs_backend_Packs_compact_frame_0_t;

/* chunks_packs_backend_Packs_compact_pad_0 is compact's pad: the rewrite, the flag, then the view's guard. */
static void
chunks_packs_backend_Packs_compact_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_compact_frame_0_t *frame = frame_;

	if (frame->rewrite_live)
	{
		frame->rewrite_live = false;
		chunks_packs_lists_Change_drop(&frame->rewrite);
	}
	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/* chunks_packs_backend_Packs_compact_body_0 is compact's body, the list files due a whole write written whole, and remove_space's, its tablespace taken out of them. */
static void
chunks_packs_backend_Packs_compact_body_0(void *frame_)
{
	chunks_packs_backend_Packs_compact_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(&frame->self->held);
	ChunksPacksViewView *view;
	HostSharedFlagIo flag_io;
	HostError	host;

	frame->ok = false;
	frame->view = chunks_packs_backend_Packs_view_lock(frame->self, frame->io);
	frame->view_live = true;
	view = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	if (!frame->removing && !frame->all && !chunks_packs_lists_past_slack(view))
	{
		frame->ok = true;
		goto done;
	}
	if (!host_shared_Record_flag(record, host_lock_Lending_io(&frame->view), CHUNKS_PACKS_SHARED_REWRITING_INDEX,
								 &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->rewriting_live = true;
	flag_io = host_shared_FlagGuard_io(&frame->rewriting);
	if (!chunks_packs_lists_Rewrite_begin(view, record, &flag_io, &frame->rewrite, frame->error))
		goto done;
	frame->rewrite_live = true;
	if (frame->removing ? !chunks_packs_lists_Rewrite_remove_space(&frame->rewrite, frame->space, frame->error) :
		!chunks_packs_lists_Rewrite_compact(&frame->rewrite, frame->all, frame->error))
		goto done;
	frame->rewrite_live = false;
	frame->ok = chunks_packs_lists_Change_install(&frame->rewrite, frame->error);
done:
	chunks_packs_backend_Packs_compact_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_compact writes list files whole under the list
 * files' flag: all of them, or those past their slack, taking the flag only
 * when one is.
 */
bool
chunks_packs_backend_Packs_compact(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
								   const HostSharedReader *reading, bool all, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_compact_frame_0_t frame = {.self = self, .io = io, .all = all, .error = error};

	(void) reading;
	std_landing(&frame, chunks_packs_backend_Packs_compact_body_0, chunks_packs_backend_Packs_compact_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_remove_space takes a tablespace out of the
 * repository's list files under their flag once none of its packs is listed,
 * refusing while one is (bugs row 99).
 */
bool
chunks_packs_backend_Packs_remove_space(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
										const HostSharedReader *reading, uint32 space, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_compact_frame_0_t frame = {.self = self, .io = io, .removing = true, .space = space,
	.error = error};

	(void) reading;
	std_landing(&frame, chunks_packs_backend_Packs_compact_body_0, chunks_packs_backend_Packs_compact_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_flush_frame_0_t is flush's state across its
 * pad: the view's guard, and whether the view defers no add.
 */
typedef struct chunks_packs_backend_Packs_flush_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	ChunksErrorError *error;
	bool		ok;
	bool		none;
	HostLockLending view;
	bool		view_live;
} chunks_packs_backend_Packs_flush_frame_0_t;

/* chunks_packs_backend_Packs_flush_pad_0 is flush's pad: the view's guard. */
static void
chunks_packs_backend_Packs_flush_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_flush_frame_0_t *frame = frame_;

	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/* chunks_packs_backend_Packs_flush_body_0 is flush's body: the view refreshed, then whether it defers no add. */
static void
chunks_packs_backend_Packs_flush_body_0(void *frame_)
{
	chunks_packs_backend_Packs_flush_frame_0_t *frame = frame_;
	ChunksPacksViewView *value;

	frame->view = chunks_packs_backend_Packs_view_lock(frame->self, frame->io);
	frame->view_live = true;
	value = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	frame->ok = chunks_packs_view_View_refresh(value, frame->io, host_shared_Held_record(&frame->self->held),
											   frame->error);
	frame->none = value->deferred.len == 0;
	chunks_packs_backend_Packs_flush_pad_0(frame);
}

/* chunks_packs_backend_flushed is flush's change: every deferred add appended. */
static bool
chunks_packs_backend_flushed(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	(void) captures;
	return chunks_packs_lists_Change_flush(change, error);
}

/*
 * chunks_packs_backend_Packs_flush appends the records of the deferred adds
 * to the list files, as a fold does before the record that names the packs
 * its base rests on, so a crash's recovery finds them listed.
 */
bool
chunks_packs_backend_Packs_flush(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
								 const HostSharedReader *reading, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_flush_frame_0_t frame = {.self = self, .io = io, .error = error};

	std_landing(&frame, chunks_packs_backend_Packs_flush_body_0, chunks_packs_backend_Packs_flush_pad_0);
	if (!frame.ok)
		return false;
	return frame.none || chunks_packs_backend_Packs_changed(self, io, reading, chunks_packs_backend_flushed, NULL,
															error);
}

/*
 * chunks_packs_backend_Packs_close ends the backend's hold of the
 * repository's packs, appending the records of the deferred adds to the list
 * files first. It takes the packs.
 */
bool
chunks_packs_backend_Packs_close(ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								 ChunksErrorError *error)
{
	bool		ok = chunks_packs_backend_Packs_flush(self, HOST_IO_IO_TAKES_SHORT(io), reading, error);

	chunks_packs_backend_Packs_drop(self);
	return ok;
}

/*
 * chunks_packs_backend_copied_file_frame_0_t is copied_file's state across
 * its pad: its two files and the bytes of a block.
 */
typedef struct chunks_packs_backend_copied_file_frame_0_t
{
	HostIoIoDoors io;
	const char *from_path;
	size_t		from_path_len;
	const char *to_path;
	size_t		to_path_len;
	ChunksErrorError *error;
	bool		ok;
	HostFile	from;
	bool		from_live;
	HostFile	to;
	bool		to_live;
	StdVec		bytes;
} chunks_packs_backend_copied_file_frame_0_t;

/* chunks_packs_backend_copied_file_pad_0 is copied_file's pad: the bytes, then the files. */
static void
chunks_packs_backend_copied_file_pad_0(void *frame_)
{
	chunks_packs_backend_copied_file_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->bytes, 1, 1);
	std_vec_new(&frame->bytes, 1, 1);
	if (frame->to_live)
	{
		frame->to_live = false;
		host_File_drop(&frame->to);
	}
	if (frame->from_live)
	{
		frame->from_live = false;
		host_File_drop(&frame->from);
	}
}

/* chunks_packs_backend_copied_file_body_0 is copied_file's body. */
static void
chunks_packs_backend_copied_file_body_0(void *frame_)
{
	chunks_packs_backend_copied_file_frame_0_t *frame = frame_;
	HostIoIoDoors io = frame->io;
	HostError	host;
	uint64		len;
	uint64		offset = 0;

	frame->ok = false;
	if (!host_open_to_read(HOST_IO_IO_DOORS_READ_DOORS(io), frame->from_path, frame->from_path_len, &frame->from,
						   &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->from_live = true;
	if (!chunks_file_create(io, frame->to_path, frame->to_path_len, &frame->to, frame->error))
		goto done;
	frame->to_live = true;
	if (!host_size(HOST_IO_IO_DOORS_READ_DOORS(io), &frame->from, &len, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	while (offset < len)
	{
		size_t		block = (size_t) std_min_u64(len - offset, CHUNKS_PACKS_BACKEND_COPY_BLOCK);
		bool		written;

		if (!chunks_file_read_at(HOST_IO_IO_DOORS_READ_DOORS(io), &frame->from, offset, block, &frame->bytes,
								 frame->error))
			goto done;
		written = chunks_file_write_at(io, &frame->to, frame->to_path, frame->to_path_len, offset, frame->bytes.ptr,
									   frame->bytes.len, frame->error);
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_new(&frame->bytes, 1, 1);
		if (!written)
			goto done;
		offset = STD_ADD(u64, offset, (uint64) block, 78, 9);
	}
	frame->ok = chunks_file_sync(io, &frame->to, frame->error);
done:
	chunks_packs_backend_copied_file_pad_0(frame);
}

/* chunks_packs_backend_copied_file copies a file whole into a new file, synced once its bytes are written. */
bool
chunks_packs_backend_copied_file(HostIoIoDoors io, const char *from_path, size_t from_path_len, const char *to_path,
								 size_t to_path_len, ChunksErrorError *error)
{
	chunks_packs_backend_copied_file_frame_0_t frame = {.io = io, .from_path = from_path,
	.from_path_len = from_path_len, .to_path = to_path, .to_path_len = to_path_len, .error = error};

	std_vec_new(&frame.bytes, 1, 1);
	std_landing(&frame, chunks_packs_backend_copied_file_body_0, chunks_packs_backend_copied_file_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_left_out reports whether a creation's copy leaves an
 * entry of a repository's directory out: the manifest's previous generation,
 * the checkpoint's runs and spills, which the new repository's own runs
 * replace, the backups' generations, which are the template's, and a
 * creation's mark (bugs row 404).
 */
static bool
chunks_packs_backend_left_out(const StdString *name)
{
	static const char *const names[] = {CHUNKS_PACKS_MANIFEST ".prev", CHUNKS_PACKS_DELTA, CHUNKS_PACKS_BACKUP,
	CHUNKS_PACKS_DROPPED_CREATING};

	for (size_t k = 0; k < lengthof(names); k++)
		if (name->len == strlen(names[k]) && memcmp(name->ptr, names[k], name->len) == 0)
			return true;
	return false;
}

/*
 * chunks_packs_backend_named reports whether a list of names holds a name.
 */
static bool
chunks_packs_backend_named(const StdVec *names, const StdString *name)
{
	for (size_t k = 0, k_len = names->len; k < k_len; k++)
	{
		const StdString *held = &STD_VEC_AT(names, StdString, k);

		if (held->len == name->len && memcmp(held->ptr, name->ptr, name->len) == 0)
			return true;
	}
	return false;
}

/*
 * chunks_packs_backend_queued_names returns the names of the files the
 * removal queue holds for a repository in a space, which a whole copy of that
 * space's directory leaves out, since nothing would list them in the copy
 * (bugs row 451).
 */
static bool
chunks_packs_backend_queued_names(HostIoIo *io, uint32 repository, uint32 space, StdVec *result,
								  ChunksErrorError *error)
{
	StdVec		entries;

	if (!chunks_packs_removal_queued(io, repository, &entries, error))
		return false;
	for (size_t k = 0, k_len = entries.len; k < k_len; k++)
	{
		const ChunksPacksSingleRemoval *entry = &STD_VEC_AT(&entries, ChunksPacksSingleRemoval, k);
		StdString	name;

		if (entry->space != space)
			continue;
		name = chunks_packs_removal_file_name(entry);
		std_vec_push_Checked(result, &name, sizeof(name), _Alignof(StdString));
	}
	std_vec_free_Checked(&entries, sizeof(ChunksPacksSingleRemoval), _Alignof(ChunksPacksSingleRemoval));
	return true;
}

/*
 * chunks_packs_backend_copied_dir_frame_0_t is copied_dir's state across its
 * pad: the names listed and the paths of the entry it copies.
 */
typedef struct chunks_packs_backend_copied_dir_frame_0_t
{
	HostIoIoDoors io;
	const char *dir;
	size_t		dir_len;
	const StdVec *skipped;
	const HostSharedPauses *pauses;
	const char *into;
	size_t		into_len;
	bool		created;
	ChunksErrorError *error;
	bool		ok;
	StdVec		names;
	StdPathBuf	from_path;
	StdPathBuf	to_path;
} chunks_packs_backend_copied_dir_frame_0_t;

/* chunks_packs_backend_copied_dir_pad_0 is copied_dir's pad: the paths, then the names. */
static void
chunks_packs_backend_copied_dir_pad_0(void *frame_)
{
	chunks_packs_backend_copied_dir_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->to_path.bytes, 1, 1);
	std_PathBuf_new(&frame->to_path);
	std_vec_free_Checked(&frame->from_path.bytes, 1, 1);
	std_PathBuf_new(&frame->from_path);
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
}

/*
 * chunks_packs_backend_copied_dir_body_0 is copied_dir's body: each entry
 * copied, a directory made and copied the same way, then the directory
 * copied into synced.
 */
static void
chunks_packs_backend_copied_dir_body_0(void *frame_)
{
	chunks_packs_backend_copied_dir_frame_0_t *frame = frame_;
	HostError	host;

	frame->ok = false;
	if (!chunks_file_file_names(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->dir, frame->dir_len, &frame->names,
								frame->error))
		goto done;
	if (frame->pauses != NULL && !host_pause(*frame->pauses, "doltgres-packs-copy-listed", &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		bool		there;
		bool		copied;

		if ((frame->created && chunks_packs_backend_left_out(name)) ||
			(frame->skipped != NULL && chunks_packs_backend_named(frame->skipped, name)))
			continue;
		std_vec_free_Checked(&frame->from_path.bytes, 1, 1);
		std_PathBuf_new(&frame->from_path);
		std_path_join(frame->dir, frame->dir_len, name->ptr, name->len, &frame->from_path);
		std_vec_free_Checked(&frame->to_path.bytes, 1, 1);
		std_PathBuf_new(&frame->to_path);
		std_path_join(frame->into, frame->into_len, name->ptr, name->len, &frame->to_path);
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->from_path.bytes.ptr,
								   frame->from_path.bytes.len, &there, frame->error))
			goto done;
		if (there)
			copied = chunks_file_create_dir(frame->io, frame->to_path.bytes.ptr, frame->to_path.bytes.len,
											frame->error) &&
				chunks_packs_backend_copied_dir(frame->io, frame->from_path.bytes.ptr, frame->from_path.bytes.len,
												NULL, NULL, frame->to_path.bytes.ptr, frame->to_path.bytes.len,
												false, frame->error);
		else
			copied = chunks_packs_backend_copied_file(frame->io, frame->from_path.bytes.ptr,
													  frame->from_path.bytes.len, frame->to_path.bytes.ptr,
													  frame->to_path.bytes.len, frame->error);
		if (!copied)
			goto done;
	}
	frame->ok = chunks_file_sync_dir(frame->io, frame->into, frame->into_len, frame->error);
done:
	chunks_packs_backend_copied_dir_pad_0(frame);
}

/*
 * chunks_packs_backend_copied_dir copies each entry of a directory whole
 * into another directory, made already, as Postgres' copydir copies a
 * database's, and then syncs the directory it copied into; it leaves out the
 * entries of the directory itself that the skipped names list, pausing at
 * the pauses given once it has listed them, and a creation's copy leaves out
 * the entries left_out names.
 */
static bool
chunks_packs_backend_copied_dir(HostIoIoDoors io, const char *dir, size_t dir_len, const StdVec *skipped,
								const HostSharedPauses *pauses, const char *into, size_t into_len, bool created,
								ChunksErrorError *error)
{
	chunks_packs_backend_copied_dir_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len, .skipped = skipped,
	.pauses = pauses, .into = into, .into_len = into_len, .created = created, .error = error};

	std_vec_new(&frame.names, sizeof(StdString), _Alignof(StdString));
	std_PathBuf_new(&frame.from_path);
	std_PathBuf_new(&frame.to_path);
	std_landing(&frame, chunks_packs_backend_copied_dir_body_0, chunks_packs_backend_copied_dir_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_copied_frame_0_t is copied's state across its
 * pad: the repository's own directory and the files' flag.
 */
typedef struct chunks_packs_backend_Packs_copied_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	const char *into;
	size_t		into_len;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	StdVec		skipped;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
} chunks_packs_backend_Packs_copied_frame_0_t;

/* chunks_packs_backend_Packs_copied_pad_0 is copied's pad: the flag, the names it skips, then the directory. */
static void
chunks_packs_backend_Packs_copied_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_copied_frame_0_t *frame = frame_;

	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	std_vec_drop_Checked(&frame->skipped, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->skipped, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_backend_Packs_copied_body_0 is copied's body: what the record owes written, then the copy. */
static void
chunks_packs_backend_Packs_copied_body_0(void *frame_)
{
	chunks_packs_backend_Packs_copied_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedFlagIo flag_io;
	HostSharedPauses pauses;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_backend_Packs_unreserve(self, frame->io, frame->reading, frame->error) ||
		!chunks_packs_backend_Packs_even_manifest(self, frame->io, frame->error) ||
		!chunks_packs_backend_Packs_flush(self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading, frame->error) ||
		!chunks_packs_open_made_dir(frame->io, frame->into, frame->into_len, frame->error))
		goto done;
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-packs-copy-made", &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->dir = host_tablespace_dir((uint32) self->repository, self->own);
	if (!host_shared_Record_flag(host_shared_Held_record(&self->held), HOST_IO_IO_TAKES_SHORT(frame->io),
								 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->rewriting_live = true;
	if (!chunks_packs_backend_queued_names(frame->io, (uint32) self->repository, self->own, &frame->skipped,
										   frame->error))
		goto done;
	flag_io = host_shared_FlagGuard_io(&frame->rewriting);
	pauses = HOST_SHARED_FLAG_IO_PAUSES(&flag_io);
	frame->ok = chunks_packs_backend_copied_dir(HOST_SHARED_FLAG_IO_IO_DOORS(&flag_io), frame->dir.bytes.ptr,
												frame->dir.bytes.len, &frame->skipped, &pauses, frame->into,
												frame->into_len, false, frame->error);
done:
	chunks_packs_backend_Packs_copied_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_copied writes what the record owes the
 * repository's own files and then copies its own directory whole into
 * another directory, made first, under the files' flag every whole write of
 * a kept file holds and the removal pass tries, leaving out the files the
 * removal queue holds, read under the flag before the directory is listed.
 */
bool
chunks_packs_backend_Packs_copied(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								  const char *into, size_t into_len, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_copied_frame_0_t frame = {.self = self, .io = io, .reading = reading, .into = into,
	.into_len = into_len, .error = error};

	std_PathBuf_new(&frame.dir);
	std_vec_new(&frame.skipped, sizeof(StdString), _Alignof(StdString));
	std_landing(&frame, chunks_packs_backend_Packs_copied_body_0, chunks_packs_backend_Packs_copied_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_created_frame_0_t is created's state across its
 * pad: the repository's directories, the one it copies into, and the files'
 * flag.
 */
typedef struct chunks_packs_backend_Packs_created_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	uint32		database;
	uint32		own;
	ChunksErrorError *error;
	bool		ok;
	StdVec		dirs;
	StdPathBuf	into;
	StdVec		skipped;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
} chunks_packs_backend_Packs_created_frame_0_t;

/* chunks_packs_backend_Packs_created_pad_0 is created's pad: the flag, the names it skips, the directory it copies into, then the directories. */
static void
chunks_packs_backend_Packs_created_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_created_frame_0_t *frame = frame_;

	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	std_vec_drop_Checked(&frame->skipped, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->skipped, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&frame->into.bytes, 1, 1);
	std_PathBuf_new(&frame->into);
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&frame->dirs, chunks_TupleU32PathBuf_t, k).f1.bytes, 1, 1);
	std_vec_free_Checked(&frame->dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	std_vec_new(&frame->dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
}

/* chunks_packs_backend_Packs_created_body_0 is created's body: what the record owes written, then each directory's copy. */
static void
chunks_packs_backend_Packs_created_body_0(void *frame_)
{
	chunks_packs_backend_Packs_created_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedFlagIo flag_io;
	HostSharedPauses pauses;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_backend_Packs_unreserve(self, frame->io, frame->reading, frame->error) ||
		!chunks_packs_backend_Packs_even_manifest(self, frame->io, frame->error) ||
		!chunks_packs_backend_Packs_flush(self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading, frame->error))
		goto done;
	if (!host_shared_Record_flag(host_shared_Held_record(&self->held), HOST_IO_IO_TAKES_SHORT(frame->io),
								 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->rewriting_live = true;
	if (!chunks_packs_open_spaced_directories(HOST_IO_IO_READ_DOORS(frame->io), (uint32) self->repository, self->own,
											  &frame->dirs, frame->error))
		goto done;
	flag_io = host_shared_FlagGuard_io(&frame->rewriting);
	pauses = HOST_SHARED_FLAG_IO_PAUSES(&flag_io);
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
	{
		const chunks_TupleU32PathBuf_t *dir = &STD_VEC_AT(&frame->dirs, chunks_TupleU32PathBuf_t, k);

		std_vec_free_Checked(&frame->into.bytes, 1, 1);
		frame->into = host_tablespace_dir(frame->database,
										  dir->f0 == CHUNKS_PACKS_DEFAULT_SPACE ? frame->own : dir->f0);
		std_vec_drop_Checked(&frame->skipped, sizeof(StdString), _Alignof(StdString), std_String_drop);
		std_vec_new(&frame->skipped, sizeof(StdString), _Alignof(StdString));
		if (!chunks_packs_open_made_dir(frame->io, frame->into.bytes.ptr, frame->into.bytes.len, frame->error) ||
			!chunks_packs_backend_queued_names(frame->io, (uint32) self->repository,
											   dir->f0 == CHUNKS_PACKS_DEFAULT_SPACE ? self->own : dir->f0,
											   &frame->skipped, frame->error) ||
			!chunks_packs_backend_copied_dir(HOST_SHARED_FLAG_IO_IO_DOORS(&flag_io), dir->f1.bytes.ptr,
											 dir->f1.bytes.len, &frame->skipped, &pauses, frame->into.bytes.ptr,
											 frame->into.bytes.len, true, frame->error))
			goto done;
	}
	frame->ok = true;
done:
	chunks_packs_backend_Packs_created_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_created writes what the record owes the
 * repository's files and then copies each of its directories into a new
 * database's directory in the same tablespace, its own into the new
 * database's own in the tablespace given, under the files' flag every whole
 * write of a kept file holds and the removal pass tries, leaving out what a
 * creation's copy leaves out (bugs row 404) and the files the removal queue
 * holds, read under the flag before each directory is listed (bugs row 451).
 */
bool
chunks_packs_backend_Packs_created(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   uint32 database, uint32 own, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_created_frame_0_t frame = {.self = self, .io = io, .reading = reading,
	.database = database, .own = own, .error = error};

	std_vec_new(&frame.dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	std_PathBuf_new(&frame.into);
	std_vec_new(&frame.skipped, sizeof(StdString), _Alignof(StdString));
	std_landing(&frame, chunks_packs_backend_Packs_created_body_0, chunks_packs_backend_Packs_created_pad_0);
	return frame.ok;
}

/* chunks_packs_backend_Packs_location returns the pack record's location, which no other repository's has while the server runs. */
uint64
chunks_packs_backend_Packs_location(const ChunksPacksBackendPacks *self)
{
	return self->location;
}

/* chunks_packs_backend_with_prefix_t is with_prefix's read's captures: the prefix and the addresses found. */
typedef struct chunks_packs_backend_with_prefix_t
{
	const char *prefix;
	size_t		prefix_len;
	StdVec	   *found;
} chunks_packs_backend_with_prefix_t;

/* chunks_packs_backend_with_prefix_frame_0_t is with_prefix's read's state across its pad: its lists. */
typedef struct chunks_packs_backend_with_prefix_frame_0_t
{
	chunks_packs_backend_with_prefix_t *with;
	ChunksPacksViewView *view;
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	StdVec		listed;
	StdVec		spaces;
	StdVec		entries;
	StdVec		found;
} chunks_packs_backend_with_prefix_frame_0_t;

/* chunks_packs_backend_with_prefix_pad_0 is with_prefix's read's pad: its lists. */
static void
chunks_packs_backend_with_prefix_pad_0(void *frame_)
{
	chunks_packs_backend_with_prefix_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_new(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
}

/* chunks_packs_backend_with_prefix_body_0 is with_prefix's read's body: each tablespace's index asked once. */
static void
chunks_packs_backend_with_prefix_body_0(void *frame_)
{
	chunks_packs_backend_with_prefix_frame_0_t *frame = frame_;
	size_t		kept = 0;

	frame->ok = false;
	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_view_View_listed(frame->view, &frame->listed);
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
	{
		uint32		space = STD_VEC_AT(&frame->listed, ChunksPacksTableEntry, k).f1;
		size_t		at = 0;

		while (at < frame->spaces.len && STD_VEC_AT(&frame->spaces, uint32, at) < space)
			at++;
		if (at == frame->spaces.len || STD_VEC_AT(&frame->spaces, uint32, at) != space)
			std_vec_insert(&frame->spaces, at, &space, sizeof(space), _Alignof(uint32), RUST_FILE, 274, 82);
	}
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
	{
		const ChunksIndexAddressIndexLocator *index = chunks_packs_view_View_index(frame->view,
																				   STD_VEC_AT(&frame->spaces, uint32,
																							  k));

		if (index == NULL)
			continue;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
		std_vec_new(&frame->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		if (!chunks_index_AddressIndexLocator_with_prefix(index, frame->io, frame->with->prefix,
														  frame->with->prefix_len, &frame->entries, frame->error))
			goto done;
		for (size_t n = 0, n_len = frame->entries.len; n < n_len; n++)
		{
			const chunks_TupleAddressLocator_t *entry = &STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, n);

			if (chunks_packs_view_View_pack(frame->view, entry->f1.pack) != NULL)
				std_vec_push_Checked(&frame->found, &entry->f0, sizeof(entry->f0), _Alignof(ChunksAddressAddress));
		}
	}
	std_slice_sort_unstable_Address(frame->found.ptr, frame->found.len);
	for (size_t k = 0, k_len = frame->found.len; k < k_len; k++)
	{
		const ChunksAddressAddress *address = &STD_VEC_AT(&frame->found, ChunksAddressAddress, k);

		if (kept == 0 || !chunks_address_Address_PartialEq_eq(&STD_VEC_AT(&frame->found, ChunksAddressAddress, kept - 1),
															  address))
			STD_VEC_AT(&frame->found, ChunksAddressAddress, kept++) = *address;
	}
	frame->found.len = kept;
	*frame->with->found = frame->found;
	std_vec_new(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->ok = true;
done:
	chunks_packs_backend_with_prefix_pad_0(frame);
}

/* chunks_packs_backend_with_prefix is with_prefix's read: each tablespace's index asked, the listed packs' entries kept. */
static bool
chunks_packs_backend_with_prefix(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_with_prefix_frame_0_t frame = {.with = captures, .view = view, .io = io, .error = error};

	std_vec_new(&frame.listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame.spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame.entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_new(&frame.found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	std_landing(&frame, chunks_packs_backend_with_prefix_body_0, chunks_packs_backend_with_prefix_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_with_prefix returns the addresses of the listed
 * packs' chunks whose text starts with the prefix, in address order.
 */
bool
chunks_packs_backend_Packs_with_prefix(const ChunksPacksBackendPacks *self, HostIoIo *io,
									   const HostSharedReader *reading, const char *prefix, size_t prefix_len,
									   StdVec *result, ChunksErrorError *error)
{
	chunks_packs_backend_with_prefix_t with = {prefix, prefix_len, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_with_prefix, &with, error);
}

/* chunks_packs_backend_listing_t is a read's state across its pad over the view's listing: the listing and the answer. */
typedef struct chunks_packs_backend_listing_t
{
	ChunksPacksViewView *view;
	void	   *result;
	void		(*answer) (const StdVec *listed, StdVec *result);
	size_t		size;
	size_t		align;
	StdVec		listed;
	StdVec		made;
} chunks_packs_backend_listing_t;

/* chunks_packs_backend_listing_pad_0 is a listing read's pad: the answer, then the listing. */
static void
chunks_packs_backend_listing_pad_0(void *frame_)
{
	chunks_packs_backend_listing_t *frame = frame_;

	std_vec_free_Checked(&frame->made, frame->size, frame->align);
	std_vec_new(&frame->made, frame->size, frame->align);
	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
}

/* chunks_packs_backend_listing_body_0 is a listing read's body: the listing made, then its answer. */
static void
chunks_packs_backend_listing_body_0(void *frame_)
{
	chunks_packs_backend_listing_t *frame = frame_;

	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_view_View_listed(frame->view, &frame->listed);
	frame->answer(&frame->listed, &frame->made);
	*(StdVec *) frame->result = frame->made;
	std_vec_new(&frame->made, frame->size, frame->align);
	chunks_packs_backend_listing_pad_0(frame);
}

/* chunks_packs_backend_listing answers a read from the view's listing, a list of elements of a size and an alignment. */
static void
chunks_packs_backend_listing(ChunksPacksViewView *view, void (*answer) (const StdVec *listed, StdVec *result),
							 size_t size, size_t align, StdVec *result)
{
	chunks_packs_backend_listing_t frame = {.view = view, .result = result, .answer = answer, .size = size,
	.align = align};

	std_vec_new(&frame.listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame.made, size, align);
	std_landing(&frame, chunks_packs_backend_listing_body_0, chunks_packs_backend_listing_pad_0);
}

/* chunks_packs_backend_numbers_of answers numbers: the listed packs' numbers, in order. */
static void
chunks_packs_backend_numbers_of(const StdVec *listed, StdVec *result)
{
	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
		std_vec_push_Checked(result, &STD_VEC_AT(listed, ChunksPacksTableEntry, k).f0, sizeof(uint64),
							 _Alignof(uint64));
}

/* chunks_packs_backend_numbers is numbers' read: the listed packs' numbers. */
static bool
chunks_packs_backend_numbers(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	(void) io;
	(void) error;
	chunks_packs_backend_listing(view, chunks_packs_backend_numbers_of, sizeof(uint64), _Alignof(uint64), captures);
	return true;
}

/* chunks_packs_backend_Packs_numbers returns the numbers of the packs listed, in order. */
bool
chunks_packs_backend_Packs_numbers(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   StdVec *result, ChunksErrorError *error)
{
	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_numbers, result, error);
}

/* chunks_packs_backend_spaces_of answers spaces: each listed pack's number with its tablespace, in order. */
static void
chunks_packs_backend_spaces_of(const StdVec *listed, StdVec *result)
{
	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *entry = &STD_VEC_AT(listed, ChunksPacksTableEntry, k);
		std_TupleU64U32_t pair = {entry->f0, entry->f1};

		std_vec_push_Checked(result, &pair, sizeof(pair), _Alignof(std_TupleU64U32_t));
	}
}

/* chunks_packs_backend_spaces is spaces' read: each listed pack's number with its tablespace. */
static bool
chunks_packs_backend_spaces(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	(void) io;
	(void) error;
	chunks_packs_backend_listing(view, chunks_packs_backend_spaces_of, sizeof(std_TupleU64U32_t),
								 _Alignof(std_TupleU64U32_t), captures);
	return true;
}

/*
 * chunks_packs_backend_Packs_spaces returns each listed pack's number with
 * the tablespace it is in, DEFAULT_SPACE for the repository's own directory,
 * in order.
 */
bool
chunks_packs_backend_Packs_spaces(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								  StdVec *result, ChunksErrorError *error)
{
	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_spaces, result, error);
}

/* chunks_packs_backend_newest_vector is newest_vector's read: the newest vector generation the table names. */
static bool
chunks_packs_backend_newest_vector(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	const ChunksPacksTable *table = chunks_packs_view_View_table(view);
	uint64		newest = 0;

	(void) io;
	(void) error;
	for (size_t k = 0, k_len = table->packs.len; k < k_len; k++)
	{
		const ChunksPacksListed *listed = &STD_VEC_AT(&table->packs, chunks_TupleU64Listed_t, k).f1;

		if (listed->has_dead)
			newest = std_max_u64(newest, listed->dead);
	}
	*(uint64 *) captures = newest;
	return true;
}

/*
 * chunks_packs_backend_Packs_newest_vector returns the newest generation of
 * a deletion vector the pack table names, 0 when it names none.
 */
bool
chunks_packs_backend_Packs_newest_vector(const ChunksPacksBackendPacks *self, HostIoIo *io,
										 const HostSharedReader *reading, uint64 *result, ChunksErrorError *error)
{
	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_newest_vector, result, error);
}

/* chunks_packs_backend_holds_t is holds' read's captures: the ids asked about and the answer. */
typedef struct chunks_packs_backend_holds_t
{
	const ChunksPackPackId *ids;
	size_t		ids_len;
	bool	   *result;
} chunks_packs_backend_holds_t;

/* chunks_packs_backend_ids_of answers holds' listing: the listed packs' ids. */
static void
chunks_packs_backend_ids_of(const StdVec *listed, StdVec *result)
{
	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
		std_vec_push_Checked(result, &STD_VEC_AT(listed, ChunksPacksTableEntry, k).f2, sizeof(ChunksPackPackId),
							 _Alignof(ChunksPackPackId));
}

/* chunks_packs_backend_holds is holds' read: whether the listed packs have each id. */
static bool
chunks_packs_backend_holds(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_holds_t *holds = captures;
	StdVec		ids;

	(void) io;
	(void) error;
	chunks_packs_backend_listing(view, chunks_packs_backend_ids_of, sizeof(ChunksPackPackId),
								 _Alignof(ChunksPackPackId), &ids);
	*holds->result = true;
	for (size_t k = 0; k < holds->ids_len && *holds->result; k++)
	{
		bool		found = false;

		for (size_t n = 0, n_len = ids.len; n < n_len && !found; n++)
			found = chunks_pack_PackId_PartialEq_eq(&STD_VEC_AT(&ids, ChunksPackPackId, n), &holds->ids[k]);
		*holds->result = found;
	}
	std_vec_free_Checked(&ids, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	return true;
}

/* chunks_packs_backend_Packs_holds reports whether the repository lists a pack of each id. */
bool
chunks_packs_backend_Packs_holds(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								 const ChunksPackPackId *ids, size_t ids_len, bool *result, ChunksErrorError *error)
{
	chunks_packs_backend_holds_t holds = {ids, ids_len, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_holds, &holds, error);
}

/* chunks_packs_backend_pack_reader_t is pack_reader's read's captures: the number and the answer. */
typedef struct chunks_packs_backend_pack_reader_t
{
	uint64		number;
	bool	   *has_result;
	StdArcInner **result;
} chunks_packs_backend_pack_reader_t;

/* chunks_packs_backend_pack_reader is pack_reader's read: the open pack of a number, shared. */
static bool
chunks_packs_backend_pack_reader(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_pack_reader_t *pack = captures;
	StdArcInner *const *reader = chunks_packs_view_View_pack(view, pack->number);

	(void) io;
	(void) error;
	*pack->has_result = reader != NULL;
	if (reader != NULL)
		*pack->result = std_arc_clone(*reader);
	return true;
}

/*
 * chunks_packs_backend_Packs_pack_reader returns the open pack of a number,
 * if one is listed, shared with the view that holds it open.
 */
bool
chunks_packs_backend_Packs_pack_reader(const ChunksPacksBackendPacks *self, HostIoIo *io,
									   const HostSharedReader *reading, uint64 number, bool *has_result,
									   StdArcInner **result, ChunksErrorError *error)
{
	chunks_packs_backend_pack_reader_t pack = {number, has_result, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_pack_reader, &pack, error);
}

/* chunks_packs_backend_space_dir_t is writer's read's captures: the tablespace and where its directory goes. */
typedef struct chunks_packs_backend_space_dir_t
{
	uint32		space;
	StdPathBuf *dir;
} chunks_packs_backend_space_dir_t;

/* chunks_packs_backend_space_dir is writer's read: the directory of the repository's packs in a tablespace. */
static bool
chunks_packs_backend_space_dir(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_space_dir_t *asked = captures;

	(void) io;
	(void) error;
	chunks_packs_view_View_space_dir(view, asked->space, asked->dir);
	return true;
}

/* chunks_packs_backend_reserved_atomic is writer's reserved: an op on the record's Reserved. */
static uint64
chunks_packs_backend_reserved_atomic(void *captures, HostSharedOp op)
{
	return host_shared_Record_atomic(host_shared_Held_record(captures), CHUNKS_PACKS_SHARED_RESERVED_INDEX, op);
}

/* chunks_packs_backend_raise_t is writer's raise's captures: the packs and the reader. */
typedef struct chunks_packs_backend_raise_t
{
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
} chunks_packs_backend_raise_t;

/* chunks_packs_backend_reserve is writer's raise's change: the bound raised past the one read. */
static bool
chunks_packs_backend_reserve(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	return chunks_packs_lists_Change_reserve(change, *(const uint64 *) captures, error);
}

/* chunks_packs_backend_raise is writer's raise: a change that raises the bound of the writers' numbers. */
static bool
chunks_packs_backend_raise(void *captures, HostIoIo *io, uint64 numbers, ChunksErrorError *error)
{
	chunks_packs_backend_raise_t *raise = captures;

	return chunks_packs_backend_Packs_changed(raise->packs, HOST_IO_IO_TAKES_SHORT(io), raise->reading,
											  chunks_packs_backend_reserve, &numbers, error);
}

/* chunks_packs_backend_pack_opened is PackOpened's event, of a pack's number. */
static void
chunks_packs_backend_pack_opened(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_PACK_OPENED;
	result->u.pack_opened.pack = *(const uint64 *) captures;
}

/*
 * chunks_packs_backend_Packs_writer_frame_0_t is writer's state across its
 * pad: the tablespace's directory and the hold of the pack's number, until
 * the writer owns it.
 */
typedef struct chunks_packs_backend_Packs_writer_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	uint32		space;
	ChunksPackPackWriterInDir *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	ChunksPacksWritersWriting hold;
	bool		hold_live;
} chunks_packs_backend_Packs_writer_frame_0_t;

/* chunks_packs_backend_Packs_writer_pad_0 is writer's pad: the hold, then the directory. */
static void
chunks_packs_backend_Packs_writer_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_writer_frame_0_t *frame = frame_;

	if (frame->hold_live)
	{
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(&frame->hold);
	}
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_backend_Packs_writer_body_0 is writer's body: a number held, the directory made, then the writer. */
static void
chunks_packs_backend_Packs_writer_body_0(void *frame_)
{
	chunks_packs_backend_Packs_writer_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	chunks_packs_backend_space_dir_t asked = {frame->space, &frame->dir};
	chunks_packs_backend_raise_t raise = {self, frame->reading};
	uint64		number;
	bool		made;

	frame->ok = false;
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	if (!chunks_packs_backend_Packs_viewed(self, frame->io, chunks_packs_backend_space_dir, &asked, frame->error))
	{
		std_PathBuf_new(&frame->dir);
		goto done;
	}
	if (!chunks_packs_writers_Writing_open(frame->io, self->repository, frame->space,
										   chunks_packs_backend_reserved_atomic, (void *) &self->held, NULL,
										   chunks_packs_backend_raise, &raise, NULL, &frame->hold, frame->error))
		goto done;
	frame->hold_live = true;
	made = frame->space == CHUNKS_PACKS_DEFAULT_SPACE ?
		chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len,
							   frame->error) :
		chunks_packs_open_made_dir(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, frame->error);
	number = chunks_packs_writers_Writing_number(&frame->hold);
	if (!made || !chunks_pack_PackWriterInDir_create(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, number,
													 frame->result, frame->error))
		goto done;
	frame->result->repo = self->repository;
	chunks_trace_emit(self->repository, chunks_packs_backend_pack_opened, &number, NULL);
	frame->hold_live = false;
	chunks_pack_PackWriterInDir_owned(frame->result, &frame->hold);
	frame->ok = true;
done:
	chunks_packs_backend_Packs_writer_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_writer starts a pack in a tablespace with a
 * number of its own, past every number given so far, which the pack takes in
 * the table when it is added.
 */
bool
chunks_packs_backend_Packs_writer(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								  uint32 space, ChunksPackPackWriterInDir *result, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_writer_frame_0_t frame = {.self = self, .io = io, .reading = reading, .space = space,
	.result = result, .error = error};

	std_PathBuf_new(&frame.dir);
	std_landing(&frame, chunks_packs_backend_Packs_writer_body_0, chunks_packs_backend_Packs_writer_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_kept_from returns the number from which a
 * collection of the repository keeps every pack, added to the table or not.
 */
bool
chunks_packs_backend_Packs_kept_from(const ChunksPacksBackendPacks *self, uint64 *result, ChunksErrorError *error)
{
	HostSharedRecord record = host_shared_Held_record(&self->held);

	return chunks_packs_writers_kept_from(chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX),
										  chunks_packs_shared_given(chunks_packs_shared_load(record,
																							 CHUNKS_PACKS_SHARED_RESERVED_INDEX)),
										  result, error);
}

/* chunks_packs_backend_Packs_reserved returns the highest number a pack writer was given so far. */
uint64
chunks_packs_backend_Packs_reserved(const ChunksPacksBackendPacks *self)
{
	return chunks_packs_shared_given(chunks_packs_shared_load(host_shared_Held_record(&self->held),
															  CHUNKS_PACKS_SHARED_RESERVED_INDEX));
}

/* chunks_packs_backend_unreserved is unreserve's change. */
static bool
chunks_packs_backend_unreserved(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	(void) captures;
	return chunks_packs_lists_Change_unreserve(change, error);
}

/*
 * chunks_packs_backend_Packs_unreserve sets the bound of the writers'
 * numbers back to the highest number given, in the table's file too.
 */
bool
chunks_packs_backend_Packs_unreserve(const ChunksPacksBackendPacks *self, HostIoIo *io,
									 const HostSharedReader *reading, ChunksErrorError *error)
{
	return chunks_packs_backend_Packs_changed(self, HOST_IO_IO_TAKES_SHORT(io), reading,
											  chunks_packs_backend_unreserved, NULL, error);
}

/*
 * chunks_packs_backend_Packs_writing reports whether one of the process's
 * pack writers in the repository holds a number.
 */
bool
chunks_packs_backend_Packs_writing(const ChunksPacksBackendPacks *self, HostIoIo *io, uint64 number)
{
	bool		some;
	uint32		space;

	chunks_packs_writers_space_of(HOST_IO_IO_TAKES_SHORT(io), self->repository, number, &some, &space);
	return some;
}

/* chunks_packs_backend_listed_t is listed's change's captures: the pack's entry and whether its add is deferred. */
typedef struct chunks_packs_backend_listed_t
{
	ChunksPacksTableEntry entry;
	bool		defer;
} chunks_packs_backend_listed_t;

/* chunks_packs_backend_added is listed's change: the pack added. */
static bool
chunks_packs_backend_added(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	chunks_packs_backend_listed_t *listed = captures;

	return chunks_packs_lists_Change_add(change, &listed->entry, listed->defer, error);
}

/*
 * chunks_packs_backend_Packs_listed_frame_0_t is listed's state across its
 * pad: the pack it takes and the hold of its number, until it returns.
 */
typedef struct chunks_packs_backend_Packs_listed_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	const HostSharedReader *reading;
	ChunksPackPack *pack;
	bool		defer;
	ChunksPacksWritersWriting *result;
	ChunksErrorError *error;
	bool		ok;
	bool		hold_live;
	bool		pack_live;
} chunks_packs_backend_Packs_listed_frame_0_t;

/* chunks_packs_backend_Packs_listed_pad_0 is listed's pad: the hold, then the pack. */
static void
chunks_packs_backend_Packs_listed_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_listed_frame_0_t *frame = frame_;

	if (frame->hold_live)
	{
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(frame->result);
	}
	if (frame->pack_live)
	{
		frame->pack_live = false;
		chunks_pack_Pack_drop(frame->pack);
	}
}

/* chunks_packs_backend_Packs_listed_body_0 is listed's body: the pack's number's hold taken, then the pack added. */
static void
chunks_packs_backend_Packs_listed_body_0(void *frame_)
{
	chunks_packs_backend_Packs_listed_frame_0_t *frame = frame_;
	ChunksPackPack *pack = frame->pack;
	bool		some;
	uint32		space;
	chunks_packs_backend_listed_t listed;

	frame->ok = false;
	chunks_packs_writers_space_of(frame->io, frame->self->repository, pack->number, &some, &space);
	if (!some || !pack->has_owner)
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack number "), STD_FMT_ARG(0), STD_FMT_LIT(" has no writer")};
		StdFmtArg	args[] = {{&pack->number, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		goto done;
	}
	*frame->result = pack->owner;
	pack->has_owner = false;
	frame->hold_live = true;
	listed.entry.f0 = pack->number;
	listed.entry.f1 = space;
	listed.entry.f2 = pack->id;
	listed.defer = frame->defer;
	if (!chunks_packs_backend_Packs_changed(frame->self, frame->io, frame->reading, chunks_packs_backend_added,
											&listed, frame->error))
		goto done;
	frame->hold_live = false;
	frame->ok = true;
done:
	chunks_packs_backend_Packs_listed_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_listed lists a finished pack in the tablespace
 * its writer was started in, and returns the hold of its number, refusing a
 * pack no writer of the process in the repository holds the number of. It
 * takes the pack.
 */
static bool
chunks_packs_backend_Packs_listed(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
								  const HostSharedReader *reading, ChunksPackPack *pack, bool defer,
								  ChunksPacksWritersWriting *result, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_listed_frame_0_t frame = {.self = self, .io = io, .reading = reading, .pack = pack,
	.defer = defer, .result = result, .error = error, .pack_live = true};

	std_landing(&frame, chunks_packs_backend_Packs_listed_body_0, chunks_packs_backend_Packs_listed_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_add lists a finished pack one of the process's
 * writers in the repository wrote, at the number the writer was given, and
 * appends its records to the list files.
 */
bool
chunks_packs_backend_Packs_add(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
							   ChunksPackPack *pack, ChunksErrorError *error)
{
	ChunksPacksWritersWriting hold;

	if (!chunks_packs_backend_Packs_listed(self, HOST_IO_IO_TAKES_SHORT(io), reading, pack, false, &hold, error))
		return false;
	chunks_packs_writers_Writing_drop(&hold);
	return true;
}

/* chunks_packs_backend_Packs_add_deferred lists a finished pack as add does, leaving its records out of the list files. */
bool
chunks_packs_backend_Packs_add_deferred(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
										const HostSharedReader *reading, ChunksPackPack *pack, ChunksErrorError *error)
{
	ChunksPacksWritersWriting hold;

	if (!chunks_packs_backend_Packs_listed(self, io, reading, pack, true, &hold, error))
		return false;
	chunks_packs_writers_Writing_drop(&hold);
	return true;
}

/*
 * chunks_packs_backend_Packs_add_still_writing lists a finished pack as
 * add_deferred does, for a writer that goes on into another pack, and
 * returns the hold that keeps its number among those being written.
 */
bool
chunks_packs_backend_Packs_add_still_writing(const ChunksPacksBackendPacks *self, HostIoIo *io,
											 const HostSharedReader *reading, ChunksPackPack *pack,
											 ChunksPacksWritersWriting *result, ChunksErrorError *error)
{
	return chunks_packs_backend_Packs_listed(self, HOST_IO_IO_TAKES_SHORT(io), reading, pack, true, result, error);
}

/* chunks_packs_backend_forward_t is forward's change's captures: the moves and the forwarding file's line. */
typedef struct chunks_packs_backend_forward_t
{
	const chunks_packs_backend_Packs_forward_moves_t *moves;
	size_t		moves_len;
	const StdString *line;
} chunks_packs_backend_forward_t;

/* chunks_packs_backend_forward_event is Forwarded's event, of the moves. */
static void
chunks_packs_backend_forward_event(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_backend_forward_t *forward = captures;

	result->tag = CHUNKS_TRACE_EVENT_FORWARDED;
	std_slice_to_vec_Checked(&result->u.forwarded.moves, forward->moves, forward->moves_len,
							 sizeof(chunks_packs_backend_Packs_forward_moves_t),
							 _Alignof(chunks_packs_backend_Packs_forward_moves_t));
}

/* chunks_packs_backend_forward_change is forward's change: the moves noted and the forwards' run's line appended. */
static bool
chunks_packs_backend_forward_change(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	const chunks_packs_backend_forward_t *forward = captures;
	StdVec		record;
	bool		ok;

	chunks_packs_lists_Change_note(change, chunks_packs_backend_forward_event, captures);
	chunks_packs_Runs_added(forward->line->ptr, forward->line->len, &record);
	ok = chunks_packs_lists_Change_change_forwarding(change, record.ptr, record.len, error);
	std_vec_free_Checked(&record, 1, 1);
	return ok;
}

/* chunks_packs_backend_Packs_forward_frame_0_t is forward's state across its pad: the places, the view while it lends it, the run's name, path, and entries, the packs, and the line. */
typedef struct chunks_packs_backend_Packs_forward_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	const chunks_packs_backend_Packs_forward_moves_t *moves;
	size_t		moves_len;
	ChunksPacksRetireForwarded *result;
	ChunksErrorError *error;
	bool		ok;
	/* froms is a BTreeSet<(u64, u64)> of the old places, a vector in order. */
	StdVec		froms;
	bool		froms_live;
	HostLockLending view;
	bool		view_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		entries;
	bool		entries_live;
	/* from is a BTreeSet<u64> of the packs moved from, a vector in order. */
	StdVec		from;
	bool		from_live;
	StdString	line;
	bool		line_live;
} chunks_packs_backend_Packs_forward_frame_0_t;

/* chunks_packs_backend_Packs_forward_pad_0 is forward's pad: what it holds, the latest first. */
static void
chunks_packs_backend_Packs_forward_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_forward_frame_0_t *frame = frame_;

	if (frame->line_live)
	{
		frame->line_live = false;
		std_vec_free_Checked(&frame->line, 1, 1);
	}
	if (frame->from_live)
	{
		frame->from_live = false;
		std_vec_free_Checked(&frame->from, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
	if (frame->froms_live)
	{
		frame->froms_live = false;
		std_vec_free_Checked(&frame->froms, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/*
 * chunks_packs_backend_Packs_forward_body_0 is forward's body: the moves'
 * old places collected, and with any moves their forwards written as one
 * synced run whose line the forwarding file then appends.
 */
static void
chunks_packs_backend_Packs_forward_body_0(void *frame_)
{
	chunks_packs_backend_Packs_forward_frame_0_t *frame = frame_;
	chunks_packs_backend_forward_t forward = {frame->moves, frame->moves_len, &frame->line};
	ChunksIndexRunLocator run;

	frame->ok = false;
	std_vec_new(&frame->froms, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->froms_live = true;
	for (size_t k = 0; k < frame->moves_len; k++)
	{
		std_TupleU64U64_t place = {frame->moves[k].f0.pack, chunks_trailer_Locator_offset(&frame->moves[k].f0)};

		chunks_packs_retire_froms_insert(&frame->froms, &place, RUST_FILE, 448, 100);
	}
	if (frame->moves_len != 0)
	{
		ChunksPackPackId id = chunks_pack_PackId_new();
		const ChunksPacksViewView *value;

		frame->view = chunks_packs_backend_Packs_view_lock(frame->self, HOST_IO_IO_TAKES_SHORT(frame->io));
		frame->view_live = true;
		value = host_lock_Lending_DerefMut_deref_mut(&frame->view);
		frame->name = chunks_pack_PackId_file_name(&id, "fwd", 3);
		frame->name_live = true;
		std_path_join(value->dir.bytes.ptr, value->dir.bytes.len, frame->name.ptr, frame->name.len, &frame->path);
		frame->path_live = true;
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
		chunks_packs_forward_entries((const chunks_TupleLocatorLocator_t *) frame->moves, frame->moves_len,
									 &frame->entries);
		frame->entries_live = true;
		if (!chunks_index_written_run(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len,
									  frame->entries.ptr, frame->entries.len, &run, frame->error))
			goto done;
		chunks_index_RunLocator_drop(&run);
		std_vec_new(&frame->from, sizeof(uint64), _Alignof(uint64));
		frame->from_live = true;
		for (size_t k = 0, k_len = frame->froms.len; k < k_len; k++)
		{
			uint64		pack = STD_VEC_AT(&frame->froms, std_TupleU64U64_t, k).f0;

			if (frame->from.len == 0 || STD_VEC_AT(&frame->from, uint64, frame->from.len - 1) != pack)
				std_vec_push_Checked(&frame->from, &pack, sizeof(pack), _Alignof(uint64));
		}
		std_string_from_Checked(&frame->line, frame->name.ptr, frame->name.len);
		frame->line_live = true;
		std_string_push_str_Checked(&frame->line, ":", 1);
		for (size_t k = 0, k_len = frame->from.len; k < k_len; k++)
		{
			StdString	number;
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&STD_VEC_AT(&frame->from, uint64, k), std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			if (k > 0)
				std_string_push_str_Checked(&frame->line, ",", 1);
			std_fmt_format_Checked(&number, &arguments);
			std_string_push_str_Checked(&frame->line, number.ptr, number.len);
			std_vec_free_Checked(&number, 1, 1);
		}
		if (!chunks_packs_backend_Packs_changed(frame->self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading,
												chunks_packs_backend_forward_change, &forward, frame->error))
			goto done;
	}
	chunks_packs_retire_Forwarded_of(frame->self, &frame->froms, frame->result);
	frame->froms_live = false;
	frame->ok = true;
done:
	chunks_packs_backend_Packs_forward_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_forward records durably where a collection
 * copied chunks of packs it is about to retire, each from its old place to
 * its copy's, and returns the proof of it a retirement takes: it writes the
 * forwards as one run, synced, and then appends the run's line to the
 * forwarding file. The forwards count for nothing while their packs are
 * listed.
 */
bool
chunks_packs_backend_Packs_forward(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   const chunks_packs_backend_Packs_forward_moves_t *moves, size_t moves_len,
								   ChunksPacksRetireForwarded *result, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_forward_frame_0_t frame = {.self = self, .io = io, .reading = reading, .moves = moves,
	.moves_len = moves_len, .result = result, .error = error};

	std_landing(&frame, chunks_packs_backend_Packs_forward_body_0, chunks_packs_backend_Packs_forward_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_compact_lists appends every deferred add's
 * records and writes whole every list file that holds a record past its
 * whole, as a checkpoint does.
 */
bool
chunks_packs_backend_Packs_compact_lists(const ChunksPacksBackendPacks *self, HostIoIo *io,
										 const HostSharedReader *reading, ChunksErrorError *error)
{
	return chunks_packs_backend_Packs_compact(self, HOST_IO_IO_TAKES_SHORT(io), reading, true, error);
}

/*
 * chunks_packs_backend_Packs_rotations returns the count of the whole writes
 * of the repository's files, odd while one is under way.
 */
uint64
chunks_packs_backend_Packs_rotations(const ChunksPacksBackendPacks *self)
{
	return chunks_packs_shared_load(host_shared_Held_record(&self->held), CHUNKS_PACKS_SHARED_ROTATIONS_INDEX);
}

/*
 * chunks_packs_backend_Packs_kept_path makes a path where one of the
 * repository's kept files is, in its own directory; the path, which its
 * caller owns from before the call, holds a whole PathBuf at every step.
 */
static void
chunks_packs_backend_Packs_kept_path(const ChunksPacksBackendPacks *self, ChunksPacksBackendKept kept,
									 StdPathBuf *result)
{
	Assert(kept == CHUNKS_PACKS_BACKEND_KEPT_MANIFEST);
	std_vec_free_Checked(&result->bytes, 1, 1);
	std_PathBuf_new(result);
	*result = host_tablespace_dir((uint32) self->repository, self->own);
	std_PathBuf_push(result, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST));
}

/*
 * chunks_packs_backend_Packs_rewrite_kept is write_kept under the
 * repository's files' flag, which its caller holds: the mark that the
 * manifest's generations may differ set before the renames, and cleared
 * after a write that replaced the same bytes.
 */
static bool
chunks_packs_backend_Packs_rewrite_kept(const ChunksPacksBackendPacks *self, HostSharedFlagIo *io, const char *path,
										size_t path_len, const uint8 *bytes, size_t bytes_len,
										ChunksErrorError *error)
{
	HostSharedRecord record = host_shared_Held_record(&self->held);
	bool		same;
	bool		written;

	chunks_packs_shared_set(record, CHUNKS_PACKS_SHARED_DIFFERS_INDEX, 1);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 1);
	written = chunks_file_write_kept(HOST_SHARED_FLAG_IO_IO_DOORS(io), path, path_len, bytes, bytes_len, &same, error);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 1);
	if (!written)
		return false;
	chunks_packs_shared_set(record, CHUNKS_PACKS_SHARED_DIFFERS_INDEX, !same);
	return true;
}

/*
 * chunks_packs_backend_kept_frame_0_t is the state across their pad of the
 * kept files' writes and reads: the files' flag and the file's path.
 */
typedef struct chunks_packs_backend_kept_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	HostIoTakesShort takes;
	ChunksPacksBackendKept kept;
	const uint8 *bytes;
	size_t		bytes_len;
	uint64		rotations;
	bool		previous;
	bool	   *has_result;
	StdVec	   *result_0;
	bool	   *has_result_1;
	ChunksFileFallback *result_1;
	bool	   *wrote;
	ChunksErrorError *error;
	bool		ok;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
	StdPathBuf	path;
	StdVec		read;
} chunks_packs_backend_kept_frame_0_t;

/* chunks_packs_backend_kept_pad_0 is the kept files' pad: the flag, then the path and the bytes read. */
static void
chunks_packs_backend_kept_pad_0(void *frame_)
{
	chunks_packs_backend_kept_frame_0_t *frame = frame_;

	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->read, 1, 1);
	std_vec_new(&frame->read, 1, 1);
}

/* chunks_packs_backend_kept_flag takes the repository's files' flag into the frame. */
static bool
chunks_packs_backend_kept_flag(chunks_packs_backend_kept_frame_0_t *frame, HostSharedFlagIo *flag_io)
{
	HostError	host;

	if (!host_shared_Record_flag(host_shared_Held_record(&frame->self->held), frame->takes,
								 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	frame->rewriting_live = true;
	*flag_io = host_shared_FlagGuard_io(&frame->rewriting);
	return true;
}

/* chunks_packs_backend_kept_landed runs a body over a kept file's frame, its path and read bytes empty to start. */
static bool
chunks_packs_backend_kept_landed(chunks_packs_backend_kept_frame_0_t *frame, void (*body) (void *frame))
{
	std_PathBuf_new(&frame->path);
	std_vec_new(&frame->read, 1, 1);
	std_landing(frame, body, chunks_packs_backend_kept_pad_0);
	return frame->ok;
}

/* chunks_packs_backend_Packs_write_kept_body_0 is write_kept's body: the flag taken, then the whole write. */
static void
chunks_packs_backend_Packs_write_kept_body_0(void *frame_)
{
	chunks_packs_backend_kept_frame_0_t *frame = frame_;
	HostSharedFlagIo flag_io;

	frame->ok = false;
	if (chunks_packs_backend_kept_flag(frame, &flag_io))
	{
		chunks_packs_backend_Packs_kept_path(frame->self, frame->kept, &frame->path);
		frame->ok = chunks_packs_backend_Packs_rewrite_kept(frame->self, &flag_io, frame->path.bytes.ptr,
															frame->path.bytes.len, frame->bytes, frame->bytes_len,
															frame->error);
	}
	chunks_packs_backend_kept_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_write_kept writes one of the repository's kept
 * files whole and durably, keeping the file it replaces as its previous
 * generation, under the repository's files' flag.
 */
bool
chunks_packs_backend_Packs_write_kept(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksPacksBackendKept kept,
									  const uint8 *bytes, size_t bytes_len, ChunksErrorError *error)
{
	chunks_packs_backend_kept_frame_0_t frame = {.self = self, .io = io, .takes = HOST_IO_IO_TAKES_SHORT(io),
	.kept = kept, .bytes = bytes, .bytes_len = bytes_len, .error = error};

	return chunks_packs_backend_kept_landed(&frame, chunks_packs_backend_Packs_write_kept_body_0);
}

/* chunks_packs_backend_Packs_write_kept_if_body_0 is write_kept_if's body: the write, when no other came between. */
static void
chunks_packs_backend_Packs_write_kept_if_body_0(void *frame_)
{
	chunks_packs_backend_kept_frame_0_t *frame = frame_;
	HostSharedFlagIo flag_io;

	frame->ok = false;
	if (chunks_packs_backend_kept_flag(frame, &flag_io))
	{
		*frame->wrote = chunks_packs_shared_load(host_shared_Held_record(&frame->self->held),
												 CHUNKS_PACKS_SHARED_ROTATIONS_INDEX) == frame->rotations;
		frame->ok = true;
		if (*frame->wrote)
		{
			chunks_packs_backend_Packs_kept_path(frame->self, frame->kept, &frame->path);
			frame->ok = chunks_packs_backend_Packs_rewrite_kept(frame->self, &flag_io, frame->path.bytes.ptr,
																frame->path.bytes.len, frame->bytes,
																frame->bytes_len, frame->error);
		}
	}
	chunks_packs_backend_kept_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_write_kept_if writes one of the repository's
 * kept files as write_kept does only if no file of the repository was
 * written whole since the count of rotations was rotations, answering
 * whether it wrote.
 */
bool
chunks_packs_backend_Packs_write_kept_if(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
										 ChunksPacksBackendKept kept, const uint8 *bytes, size_t bytes_len,
										 uint64 rotations, bool *result, ChunksErrorError *error)
{
	chunks_packs_backend_kept_frame_0_t frame = {.self = self, .takes = io, .kept = kept, .bytes = bytes,
	.bytes_len = bytes_len, .rotations = rotations, .wrote = result, .error = error};

	return chunks_packs_backend_kept_landed(&frame, chunks_packs_backend_Packs_write_kept_if_body_0);
}

/*
 * chunks_packs_backend_Packs_even_manifest_body_0 is even_manifest's body:
 * the newest manifest read and written once more under one take of the
 * files' flag.
 */
static void
chunks_packs_backend_Packs_even_manifest_body_0(void *frame_)
{
	chunks_packs_backend_kept_frame_0_t *frame = frame_;
	HostSharedFlagIo flag_io;
	bool		has_bytes;
	bool		has_fallback;
	ChunksFileFallback fallback;

	frame->ok = false;
	if (!chunks_packs_backend_kept_flag(frame, &flag_io))
		goto done;
	chunks_packs_backend_Packs_kept_path(frame->self, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &frame->path);
	std_vec_free_Checked(&frame->read, 1, 1);
	if (!chunks_file_read_kept(HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_FLAG_IO_IO_DOORS(&flag_io)),
							   frame->path.bytes.ptr, frame->path.bytes.len, &has_bytes, &frame->read, &has_fallback,
							   &fallback, frame->error))
	{
		std_vec_new(&frame->read, 1, 1);
		goto done;
	}
	if (!has_bytes)
		std_vec_new(&frame->read, 1, 1);
	if (!has_bytes || has_fallback)
	{
		StdString	message;
		StdStr		shown = {frame->path.bytes.ptr, frame->path.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" is not whole, which the startup process settles")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		goto done;
	}
	frame->ok = chunks_packs_backend_Packs_rewrite_kept(frame->self, &flag_io, frame->path.bytes.ptr,
														frame->path.bytes.len, frame->read.ptr, frame->read.len,
														frame->error);
done:
	chunks_packs_backend_kept_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_even_manifest writes the newest manifest once
 * more when the record's mark says its generations may differ, reading and
 * writing under one take of the repository's files' flag.
 */
bool
chunks_packs_backend_Packs_even_manifest(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_kept_frame_0_t frame = {.self = self, .takes = HOST_IO_IO_TAKES_SHORT(io), .error = error};

	if (chunks_packs_shared_load(host_shared_Held_record(&self->held), CHUNKS_PACKS_SHARED_DIFFERS_INDEX) == 0)
		return true;
	return chunks_packs_backend_kept_landed(&frame, chunks_packs_backend_Packs_even_manifest_body_0);
}

/*
 * chunks_packs_backend_kept_read is a read of a kept file's generations: its
 * newest, falling back to its previous, or its previous alone.
 */
static bool
chunks_packs_backend_kept_read(bool previous, HostIoReadDoors io, const char *path, size_t path_len,
							   bool *has_result, StdVec *result_0, bool *has_result_1, ChunksFileFallback *result_1,
							   ChunksErrorError *error)
{
	if (previous)
		return chunks_file_read_previous(io, path, path_len, has_result, result_0, error);
	return chunks_file_read_kept(io, path, path_len, has_result, result_0, has_result_1, result_1, error);
}

/*
 * chunks_packs_backend_Packs_kept_body_0 is kept's body: the read under no
 * lock while the count of rotations is even and unmoved, else again under
 * the files' flag.
 */
static void
chunks_packs_backend_Packs_kept_body_0(void *frame_)
{
	chunks_packs_backend_kept_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(&frame->self->held);
	uint64		mark = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX);
	HostSharedFlagIo flag_io;

	frame->ok = false;
	chunks_packs_backend_Packs_kept_path(frame->self, frame->kept, &frame->path);
	*frame->has_result_1 = false;
	if (STD_REM_FOLDED(u64, mark, 2) == 0)
	{
		ChunksErrorError failed;

		if (chunks_packs_backend_kept_read(frame->previous, HOST_IO_IO_READ_DOORS(frame->io), frame->path.bytes.ptr,
										   frame->path.bytes.len, frame->has_result, frame->result_0,
										   frame->has_result_1, frame->result_1, &failed))
		{
			if (chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX) == mark)
			{
				frame->ok = true;
				goto done;
			}
			if (*frame->has_result)
				std_vec_free_Checked(frame->result_0, 1, 1);
			*frame->has_result_1 = false;
		}
		else
		{
			ChunksErrorOwn own;

			if (!chunks_error_Error_own(&failed, &own, frame->error))
				goto done;
			chunks_error_Own_drop(&own);
		}
	}
#ifdef DOLTGRES_TEST_CHUNKS
	chunks_packs_view_count_reread();
#endif
	if (chunks_packs_backend_kept_flag(frame, &flag_io))
		frame->ok = chunks_packs_backend_kept_read(frame->previous,
												   HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_FLAG_IO_IO_DOORS(&flag_io)),
												   frame->path.bytes.ptr, frame->path.bytes.len, frame->has_result,
												   frame->result_0, frame->has_result_1, frame->result_1,
												   frame->error);
done:
	chunks_packs_backend_kept_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_kept runs a read of one of the repository's
 * kept files with nothing held, and again under the repository's files' flag
 * when the count of rotations was odd or moved, so that it never reads a
 * file's generations part way through their renames.
 */
static bool
chunks_packs_backend_Packs_kept(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksPacksBackendKept kept,
								bool previous, bool *has_result, StdVec *result_0, bool *has_result_1,
								ChunksFileFallback *result_1, ChunksErrorError *error)
{
	chunks_packs_backend_kept_frame_0_t frame = {.self = self, .io = io, .takes = HOST_IO_IO_TAKES_SHORT(io),
		.kept = kept, .previous = previous, .has_result = has_result, .result_0 = result_0,
	.has_result_1 = has_result_1, .result_1 = result_1, .error = error};

	return chunks_packs_backend_kept_landed(&frame, chunks_packs_backend_Packs_kept_body_0);
}

/*
 * chunks_packs_backend_Packs_read_kept returns the bytes of one of the
 * repository's kept files, or of its previous generation when the newest is
 * missing or damaged, with why, or none when neither exists.
 */
bool
chunks_packs_backend_Packs_read_kept(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksPacksBackendKept kept,
									 bool *has_result, StdVec *result_0, bool *has_result_1,
									 ChunksFileFallback *result_1, ChunksErrorError *error)
{
	return chunks_packs_backend_Packs_kept(self, io, kept, false, has_result, result_0, has_result_1, result_1, error);
}

/*
 * chunks_packs_backend_Packs_read_previous returns the bytes of the previous
 * generation of one of the repository's kept files, if it has one that
 * reads.
 */
bool
chunks_packs_backend_Packs_read_previous(const ChunksPacksBackendPacks *self, HostIoIo *io,
										 ChunksPacksBackendKept kept, bool *has_result, StdVec *result,
										 ChunksErrorError *error)
{
	bool		unused;
	ChunksFileFallback none;

	return chunks_packs_backend_Packs_kept(self, io, kept, true, has_result, result, &unused, &none, error);
}

/* chunks_packs_backend_found_t is find's and find_in's read's captures: the address, the tablespace, and the answer. */
typedef struct chunks_packs_backend_found_t
{
	const ChunksAddressAddress *address;
	bool		in_space;
	uint32		space;
	bool	   *has_result;
	ChunksTrailerLocator *result;
} chunks_packs_backend_found_t;

/* chunks_packs_backend_found is find's and find_in's read: where the view finds the chunk. */
static bool
chunks_packs_backend_found(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_found_t *found = captures;

	if (found->in_space)
		return chunks_packs_reads_View_find_in(view, HOST_IO_IO_READ_DOORS(io), found->address, found->space,
											   found->has_result, found->result, error);
	return chunks_packs_reads_View_find(view, HOST_IO_IO_READ_DOORS(io), found->address, found->has_result,
										found->result, error);
}

/*
 * chunks_packs_backend_Packs_find returns where the chunk with the address
 * is, from the address index of the repository's own packs first and then
 * each tablespace's, answering a copy a full collection found dead only when
 * no tablespace holds another.
 */
bool
chunks_packs_backend_Packs_find(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								const ChunksAddressAddress *address, bool *has_result, ChunksTrailerLocator *result,
								ChunksErrorError *error)
{
	chunks_packs_backend_found_t found = {address, false, 0, has_result, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_found, &found, error);
}

/*
 * chunks_packs_backend_Packs_find_in returns where a tablespace's packs hold
 * the chunk with the address, from that tablespace's address index.
 */
bool
chunks_packs_backend_Packs_find_in(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   const ChunksAddressAddress *address, uint32 space, bool *has_result,
								   ChunksTrailerLocator *result, ChunksErrorError *error)
{
	chunks_packs_backend_found_t found = {address, true, space, has_result, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_found, &found, error);
}

/* chunks_packs_backend_records_t is records' read's captures: the address and the answer. */
typedef struct chunks_packs_backend_records_t
{
	const ChunksAddressAddress *address;
	StdVec	   *result;
} chunks_packs_backend_records_t;

/* chunks_packs_backend_records is records' read: every record the listed packs hold of the chunk. */
static bool
chunks_packs_backend_records(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_records_t *records = captures;

	return chunks_packs_reads_View_records(view, HOST_IO_IO_READ_DOORS(io), records->address, records->result, error);
}

/*
 * chunks_packs_backend_Packs_records returns every record the listed packs
 * hold of the chunk with the address, the live ones, or those a full
 * collection found dead when none is live.
 */
bool
chunks_packs_backend_Packs_records(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   const ChunksAddressAddress *address, StdVec *result, ChunksErrorError *error)
{
	chunks_packs_backend_records_t records = {address, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_records, &records, error);
}

/* chunks_packs_backend_read_t is read's read's captures: the address, the locator, and the chunk. */
typedef struct chunks_packs_backend_read_t
{
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
	ChunksChunkChunk *result;
} chunks_packs_backend_read_t;

/* chunks_packs_backend_read is read's read: the chunk's stored bytes. */
static bool
chunks_packs_backend_read(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_read_t *read = captures;

	return chunks_packs_reads_View_read(view, HOST_IO_IO_READ_DOORS(io), read->address, read->locator, read->result,
										error);
}

/*
 * chunks_packs_backend_Packs_read returns the stored bytes of the chunk with
 * the address, from the locator when it is right, through the forwards when
 * a collection retired its pack, and through the address index otherwise.
 */
bool
chunks_packs_backend_Packs_read(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								const ChunksAddressAddress *address, ChunksTrailerLocator locator,
								ChunksChunkChunk *result, ChunksErrorError *error)
{
	chunks_packs_backend_read_t read = {address, locator, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_read, &read, error);
}

/* chunks_packs_backend_forwarded_t is forwarded's read's captures: the locator and the answer. */
typedef struct chunks_packs_backend_forwarded_t
{
	ChunksTrailerLocator locator;
	bool	   *has_result;
	ChunksTrailerLocator *result;
} chunks_packs_backend_forwarded_t;

/* chunks_packs_backend_forwarded is forwarded's read: where the forwards lead from a place no listed pack holds. */
static bool
chunks_packs_backend_forwarded(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_forwarded_t *forwarded = captures;
	ChunksTrailerLocator end;

	*forwarded->has_result = false;
	if (chunks_packs_view_View_pack(view, forwarded->locator.pack) != NULL)
		return true;
	if (!chunks_packs_followed(HOST_IO_IO_READ_DOORS(io), chunks_packs_view_View_forwards(view),
							   chunks_packs_view_View_table(view), forwarded->locator, &end, error))
		return false;
	if (!chunks_trailer_Locator_PartialEq_eq(&end, &forwarded->locator))
	{
		*forwarded->has_result = true;
		*forwarded->result = end;
	}
	return true;
}

/*
 * chunks_packs_backend_Packs_forwarded returns where collections copied the
 * chunk a locator names, when its pack is no longer listed and a forward
 * leads on from its place.
 */
bool
chunks_packs_backend_Packs_forwarded(const ChunksPacksBackendPacks *self, HostIoIo *io,
									 const HostSharedReader *reading, ChunksTrailerLocator locator, bool *has_result,
									 ChunksTrailerLocator *result, ChunksErrorError *error)
{
	chunks_packs_backend_forwarded_t forwarded = {locator, has_result, result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_forwarded, &forwarded, error);
}

/*
 * chunks_packs_backend_current_frame_0_t is current's read's state across
 * its pad: the locators it makes.
 */
typedef struct chunks_packs_backend_current_frame_0_t
{
	ChunksPacksViewView *view;
	HostIoIo   *io;
	const ChunksTrailerLocator *locators;
	size_t		locators_len;
	bool	   *has_result;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		current;
} chunks_packs_backend_current_frame_0_t;

/* chunks_packs_backend_current_pad_0 is current's read's pad: the locators made. */
static void
chunks_packs_backend_current_pad_0(void *frame_)
{
	chunks_packs_backend_current_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->current, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_vec_new(&frame->current, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
}

/*
 * chunks_packs_backend_current_body_0 is current's read's body: each
 * locator into a pack no longer listed replaced by where its forwards lead,
 * answered only when one was.
 */
static void
chunks_packs_backend_current_body_0(void *frame_)
{
	chunks_packs_backend_current_frame_0_t *frame = frame_;
	bool		same = true;

	frame->ok = false;
	std_vec_with_capacity_Checked(&frame->current, frame->locators_len, sizeof(ChunksTrailerLocator),
								  _Alignof(ChunksTrailerLocator));
	for (size_t k = 0; k < frame->locators_len; k++)
	{
		ChunksTrailerLocator locator = frame->locators[k];
		ChunksTrailerLocator led = locator;

		if (chunks_trailer_Locator_is_known(&locator) && chunks_packs_view_View_pack(frame->view, locator.pack) == NULL &&
			!chunks_packs_followed(HOST_IO_IO_READ_DOORS(frame->io), chunks_packs_view_View_forwards(frame->view),
								   chunks_packs_view_View_table(frame->view), locator, &led, frame->error))
			goto done;
		same = same && chunks_trailer_Locator_PartialEq_eq(&led, &locator);
		std_vec_push_Checked(&frame->current, &led, sizeof(led), _Alignof(ChunksTrailerLocator));
	}
	*frame->has_result = !same;
	if (!same)
	{
		*frame->result = frame->current;
		std_vec_new(&frame->current, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	}
	frame->ok = true;
done:
	chunks_packs_backend_current_pad_0(frame);
}

/* chunks_packs_backend_current is current's read: the locators, each led to where its forwards end. */
static bool
chunks_packs_backend_current(void *captures, ChunksPacksViewView *view, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_backend_current_frame_0_t *asked = captures;
	chunks_packs_backend_current_frame_0_t frame = *asked;

	frame.view = view;
	frame.io = io;
	frame.error = error;
	std_vec_new(&frame.current, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	std_landing(&frame, chunks_packs_backend_current_body_0, chunks_packs_backend_current_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_backend_Packs_current returns the locators with each one into
 * a pack that is no longer listed replaced by the place its forwards lead
 * to, or none when none needs it.
 */
bool
chunks_packs_backend_Packs_current(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								   const ChunksTrailerLocator *locators, size_t locators_len, bool *has_result,
								   StdVec *result, ChunksErrorError *error)
{
	chunks_packs_backend_current_frame_0_t asked = {.locators = locators, .locators_len = locators_len,
	.has_result = has_result, .result = result};

	(void) reading;
	return chunks_packs_backend_Packs_viewed(self, io, chunks_packs_backend_current, &asked, error);
}

/* chunks_packs_backend_Packs_advise tells the operating system that the chunk at a locator in a listed pack will be read soon. */
bool
chunks_packs_backend_Packs_advise(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								  ChunksTrailerLocator locator, ChunksErrorError *error)
{
	bool		has_pack;
	StdArcInner *pack;

	if (!chunks_trailer_Locator_is_known(&locator))
		return true;
	if (!chunks_packs_backend_Packs_pack_reader(self, io, reading, locator.pack, &has_pack, &pack, error))
		return false;
	if (has_pack)
	{
		chunks_pack_PackReader_advise(std_arc_Deref_deref(pack, _Alignof(ChunksPackPackReader)), io, locator);
		chunks_pack_ArcPackReader_drop(pack);
	}
	return true;
}

/* chunks_packs_backend_Packs_tablespace returns the id of a tablespace the repository's packs are in, its own for the default number. */
static uint32
chunks_packs_backend_Packs_tablespace(const ChunksPacksBackendPacks *self, uint32 space)
{
	return space == CHUNKS_PACKS_DEFAULT_SPACE ? self->own : space;
}

/* chunks_packs_backend_Packs_merge_runs_frame_0_t is merge_runs' state across its pad: the view's guard, the reservation, the merge flag, the names, and the files. */
typedef struct chunks_packs_backend_Packs_merge_runs_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
	ChunksPacksRemovalReservation reservation;
	bool		reservation_live;
	HostSharedFlagGuard merging;
	bool		merging_live;
	StdVec		names;
	bool		names_live;
	StdVec		files;
	bool		files_live;
} chunks_packs_backend_Packs_merge_runs_frame_0_t;

/* chunks_packs_backend_Packs_merge_runs_pad_0 is merge_runs' pad: what it holds, the latest first. */
static void
chunks_packs_backend_Packs_merge_runs_pad_0(void *frame_)
{
	chunks_packs_backend_Packs_merge_runs_frame_0_t *frame = frame_;

	if (frame->files_live)
	{
		frame->files_live = false;
		std_vec_free_Checked(&frame->files, sizeof(ChunksPacksRemovalFile), _Alignof(ChunksPacksRemovalFile));
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (frame->merging_live)
	{
		frame->merging_live = false;
		host_shared_FlagGuard_drop(&frame->merging);
	}
	if (frame->reservation_live)
	{
		frame->reservation_live = false;
		chunks_packs_removal_Reservation_drop(&frame->reservation);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/*
 * chunks_packs_backend_Packs_merge_runs_body_0 is merge_runs' body: room
 * reserved for the runs a due merge replaces, the merge run under the merge
 * flag, and the replaced runs' files queued.
 */
static void
chunks_packs_backend_Packs_merge_runs_body_0(void *frame_)
{
	chunks_packs_backend_Packs_merge_runs_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedRecord record = host_shared_Held_record(&self->held);
	ChunksPacksViewView *value;
	HostSharedFlagIo flag_io;
	HostError	host;
	uint32		space;
	size_t		from;
	size_t		to;
	bool		merged;

	frame->ok = false;
	*frame->result = false;
	frame->view = chunks_packs_backend_Packs_view_lock(self, HOST_IO_IO_TAKES_SHORT(frame->io));
	frame->view_live = true;
	value = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	if (!chunks_packs_view_View_refresh(value, HOST_IO_IO_TAKES_SHORT(frame->io), record, frame->error))
		goto done;
	if (!chunks_packs_merge_due(value, &space, &from, &to))
	{
		frame->ok = true;
		goto done;
	}
	if (!chunks_packs_removal_reserve(frame->io, STD_SUB(usize, to, from, 603, 43), &frame->reservation, frame->error))
		goto done;
	frame->reservation_live = true;
	if (chunks_packs_removal_Reservation_entries(&frame->reservation) < STD_SUB(usize, to, from, 604, 40))
	{
		frame->ok = true;
		goto done;
	}
	if (!host_shared_Record_flag(record, host_lock_Lending_io(&frame->view), CHUNKS_PACKS_SHARED_MERGING_INDEX,
								 &frame->merging, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->merging_live = true;
	flag_io = host_shared_FlagGuard_io(&frame->merging);
	if (!chunks_packs_merge_merge_runs(value, record, &flag_io, &merged, &space, &frame->names, frame->error))
		goto done;
	frame->names_live = merged;
	frame->merging_live = false;
	host_shared_FlagGuard_drop(&frame->merging);
	frame->view_live = false;
	host_lock_Lending_drop(&frame->view);
	if (!merged)
	{
		frame->ok = true;
		goto done;
	}
	std_vec_new(&frame->files, sizeof(ChunksPacksRemovalFile), _Alignof(ChunksPacksRemovalFile));
	frame->files_live = true;
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		ChunksPacksRemovalFile file = {chunks_packs_backend_Packs_tablespace(self, space),
		{CHUNKS_PACKS_SINGLE_QUEUED_RUN}};

		if (!chunks_pack_PackId_of_name(name->ptr, name->len, &file.f2))
		{
			*frame->error = chunks_error_Error_corrupt_RefStr("a merged run's file name begins with no id",
															  strlen("a merged run's file name begins with no id"));
			goto done;
		}
		std_vec_push_Checked(&frame->files, &file, sizeof(file), _Alignof(ChunksPacksRemovalFile));
	}
	frame->reservation_live = false;
	if (!chunks_packs_removal_queue(frame->io, &frame->reservation, (uint32) self->repository, frame->files.ptr,
									frame->files.len, frame->error) ||
		!chunks_packs_backend_Packs_compact(self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading, false,
											frame->error))
		goto done;
	*frame->result = true;
	frame->ok = true;
done:
	chunks_packs_backend_Packs_merge_runs_pad_0(frame);
}

/*
 * chunks_packs_backend_Packs_merge_runs merges the newest runs of a
 * tablespace's address index when a merge is due, under the merge flag, and
 * queues the files of the runs the merged run replaced for removal,
 * reporting whether it merged. It reserves the queue's entries for those
 * runs first, and with no room for them it starts nothing.
 */
bool
chunks_packs_backend_Packs_merge_runs(const ChunksPacksBackendPacks *self, HostIoIo *io,
									  const HostSharedReader *reading, bool *result, ChunksErrorError *error)
{
	chunks_packs_backend_Packs_merge_runs_frame_0_t frame = {.self = self, .io = io, .reading = reading,
	.result = result, .error = error};

	std_landing(&frame, chunks_packs_backend_Packs_merge_runs_body_0, chunks_packs_backend_Packs_merge_runs_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* ChunksPacksBackendTestsNumbers is two actors' captures: the record's location, and the two numbers they share. */
typedef struct ChunksPacksBackendTestsNumbers
{
	uint64		location;
	uint64		numbers[2];
} ChunksPacksBackendTestsNumbers;

/* ChunksPacksBackendTestsFault is a backend actor's captures: the record's location, and the simulation that fails its exit's append. */
typedef struct ChunksPacksBackendTestsFault
{
	uint64		location;
	const SimcSim *fault;
} ChunksPacksBackendTestsFault;

/* ChunksPacksBackendTestsMoved is the packs one actor makes and another drops, while it holds them. */
typedef struct ChunksPacksBackendTestsMoved
{
	bool		has;
	ChunksPacksBackendPacks packs;
} ChunksPacksBackendTestsMoved;

/* ChunksPacksBackendTestsKept is a read of the manifest: its newest generation's bytes and the fallback it took, or nothing. */
typedef struct ChunksPacksBackendTestsKept
{
	bool		has;
	StdVec		bytes;
	bool		has_fallback;
	ChunksFileFallback fallback;
} ChunksPacksBackendTestsKept;

/* chunks_packs_backend_tests_opened returns a backend's packs over the record at a location, of the tests' repository. */
static void
chunks_packs_backend_tests_opened(HostIoIo *io, uint64 location, ChunksPacksBackendPacks *result)
{
	HostSharedHeld held;
	HostError	host;
	ChunksErrorError error;
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &held,
						  &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 633, 52);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(633, 61);
	if (!chunks_packs_backend_Packs_over(io, &held, location, result, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 634, 41);
}

/*
 * chunks_packs_backend_tests_written writes a pack of one chunk with a body
 * through a backend's writer in the repository's own directory, and returns
 * the finished pack with the chunk's address and place.
 */
static void
chunks_packs_backend_tests_written(HostIoIo *io, const ChunksPacksBackendPacks *packs, const char *body,
								   ChunksPackPack *pack, ChunksAddressAddress *address, ChunksTrailerLocator *at)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 640, 46);
	ChunksPackPackWriterInDir writer = chunks_packs_tests_writer(RUST_FILE, packs, io, &reading, 641, 56);
	StdVec		bytes;
	ChunksErrorError error;

	chunks_packs_fixtures_stored((const uint8 *) body, strlen(body), address, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, *address, bytes.ptr, bytes.len, false, at, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 643, 59);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 644, 28);
	host_shared_Reader_drop(&reading);
}

/* chunks_packs_backend_tests_kept_from returns the lowest pack number a collection keeps, which unwraps at a line and column. */
static uint64
chunks_packs_backend_tests_kept_from(const ChunksPacksBackendPacks *packs, uint32 line, uint32 col)
{
	uint64		from;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_kept_from(packs, &from, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return from;
}

/* chunks_packs_backend_tests_eq asserts at a line and column that a number is the one given, with a message or none. */
static void
chunks_packs_backend_tests_eq(uint64 got, uint64 want, const StdFmtArguments *message, uint32 line, uint32 col)
{
	if (got != want)
		std_assert_failed(RUST_FILE, line, col, "==", &got, std_u64_Debug_fmt, &want, std_u64_Debug_fmt, message);
}

/* chunks_packs_backend_tests_tuple_eq asserts at a line that a tuple of numbers is the one given, with a message or none. */
static void
chunks_packs_backend_tests_tuple_eq(const uint64 *got, const uint64 *want, size_t len, const StdFmtArguments *message,
									uint32 line)
{
	StdTupleRef got_tuple = {len};
	StdTupleRef want_tuple = {len};

	if (memcmp(got, want, len * sizeof(uint64)) == 0)
		return;
	for (size_t k = 0; k < len; k++)
	{
		got_tuple.items[k] = STD_ITEM(&got[k], std_u64_Debug_fmt);
		want_tuple.items[k] = STD_ITEM(&want[k], std_u64_Debug_fmt);
	}
	std_assert_failed(RUST_FILE, line, 9, "==", &got_tuple, std_tuple_Debug_fmt, &want_tuple, std_tuple_Debug_fmt,
					  message);
}

/* chunks_packs_backend_tests_pair_eq asserts at a line that a pair of numbers is the one given, with a message or none. */
static void
chunks_packs_backend_tests_pair_eq(uint64 a, uint64 b, uint64 want_a, uint64 want_b, const StdFmtArguments *message,
								   uint32 line)
{
	uint64		got[] = {a, b};
	uint64		want[] = {want_a, want_b};

	chunks_packs_backend_tests_tuple_eq(got, want, lengthof(got), message, line);
}

/* chunks_packs_backend_tests_triple_eq asserts at a line that three numbers are the ones given. */
static void
chunks_packs_backend_tests_triple_eq(uint64 a, uint64 b, uint64 c, uint64 want_a, uint64 want_b, uint64 want_c,
									 uint32 line)
{
	uint64		got[] = {a, b, c};
	uint64		want[] = {want_a, want_b, want_c};

	chunks_packs_backend_tests_tuple_eq(got, want, lengthof(got), NULL, line);
}

/* chunks_packs_backend_tests_load returns the value of one of the record's atomics. */
static uint64
chunks_packs_backend_tests_load(const ChunksPacksBackendPacks *packs, size_t index)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(host_shared_Held_record(&packs->held), index, load);
}

/* chunks_packs_backend_tests_Kept_Debug_fmt is a read of the manifest's Debug, the Rust's Option<(Vec<u8>, Option<Fallback>)>. */
static bool
chunks_packs_backend_tests_Kept_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksBackendTestsKept *kept = value;
	StdBytes	bytes = {NULL, 0};
	StdOptionRef fallback = {false, STD_ITEM(&kept->fallback, chunks_file_Fallback_Debug_fmt)};
	StdTupleRef pair;
	StdOptionRef some;

	if (kept->has)
	{
		bytes.ptr = kept->bytes.ptr;
		bytes.len = kept->bytes.len;
		fallback.has = kept->has_fallback;
	}
	pair = STD_TUPLE(2, STD_ITEM(&bytes, std_bytes_Debug_fmt), STD_ITEM(&fallback, std_option_Debug_fmt));
	some = (StdOptionRef)
	{
		kept->has, STD_ITEM(&pair, std_tuple_Debug_fmt)
	};
	return std_option_Debug_fmt(&some, f);
}

/* chunks_packs_backend_tests_read_kept reads the manifest through a backend's packs. */
static bool
chunks_packs_backend_tests_read_kept(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									 ChunksPacksBackendTestsKept *result, ChunksErrorError *error)
{
	result->has_fallback = false;
	return chunks_packs_backend_Packs_read_kept(packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &result->has,
												&result->bytes, &result->has_fallback, &result->fallback, error);
}

/* chunks_packs_backend_tests_read_kept_unwrapped reads the manifest through a backend's packs, which unwraps at a line and column. */
static ChunksPacksBackendTestsKept
chunks_packs_backend_tests_read_kept_unwrapped(const ChunksPacksBackendPacks *packs, HostIoIo *io, uint32 line,
											   uint32 col)
{
	ChunksPacksBackendTestsKept kept;
	ChunksErrorError error;

	if (!chunks_packs_backend_tests_read_kept(packs, io, &kept, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return kept;
}

/* chunks_packs_backend_tests_kept_is reports whether a read of the manifest is a newest generation of the bytes given, with no fallback. */
static bool
chunks_packs_backend_tests_kept_is(const ChunksPacksBackendTestsKept *kept, const char *want)
{
	return kept->has && !kept->has_fallback && kept->bytes.len == strlen(want) &&
		memcmp(kept->bytes.ptr, want, kept->bytes.len) == 0;
}

/* chunks_packs_backend_tests_Kept_drop frees a read of the manifest. */
static void
chunks_packs_backend_tests_Kept_drop(ChunksPacksBackendTestsKept *kept)
{
	if (kept->has)
		std_vec_free_Checked(&kept->bytes, 1, 1);
}

/*
 * chunks_packs_backend_tests_kept_eq asserts at a line and column that a
 * read of the manifest, which it frees, is a newest generation of the bytes
 * given with no fallback, or nothing for none, with a message or none.
 */
static void
chunks_packs_backend_tests_kept_eq(ChunksPacksBackendTestsKept *got, const char *want, const StdFmtArguments *message,
								   uint32 line, uint32 col)
{
	if (want == NULL ? got->has : !chunks_packs_backend_tests_kept_is(got, want))
	{
		ChunksPacksBackendTestsKept wanted = {.has = want != NULL};

		if (want != NULL)
			std_slice_to_vec_Checked(&wanted.bytes, want, strlen(want), 1, 1);
		std_assert_failed(RUST_FILE, line, col, "==", got, chunks_packs_backend_tests_Kept_Debug_fmt, &wanted,
						  chunks_packs_backend_tests_Kept_Debug_fmt, message);
	}
	chunks_packs_backend_tests_Kept_drop(got);
}

/* chunks_packs_backend_tests_written_kept writes the manifest through a backend's packs, which unwraps at a line and column. */
static void
chunks_packs_backend_tests_written_kept(const ChunksPacksBackendPacks *packs, HostIoIo *io, const char *bytes,
										uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_write_kept(packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, (const uint8 *) bytes,
											   strlen(bytes), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_backend_tests_Names_Debug_fmt is a Vec<String>'s Debug. */
static bool
chunks_packs_backend_tests_Names_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *names = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(names, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_backend_tests_Names_drop frees a Vec<String>. */
static void
chunks_packs_backend_tests_Names_drop(StdVec *names)
{
	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(names, StdString, k), 1, 1);
	std_vec_free_Checked(names, sizeof(StdString), _Alignof(StdString));
}

/* chunks_packs_backend_tests_names returns the names of a directory's files, which unwraps at a line and column. */
static StdVec
chunks_packs_backend_tests_names(HostIoIo *io, const StdPathBuf *dir, uint32 line, uint32 col)
{
	StdVec		names;
	ChunksErrorError error;

	if (!chunks_file_file_names(HOST_IO_IO_READ_DOORS(io), dir->bytes.ptr, dir->bytes.len, &names, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return names;
}

/* chunks_packs_backend_tests_names_eq asserts at a line that two lists of names, which it frees, are the same. */
static void
chunks_packs_backend_tests_names_eq(StdVec *got, StdVec *want, const StdFmtArguments *message, uint32 line)
{
	bool		same = got->len == want->len;

	for (size_t k = 0, k_len = got->len; same && k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(got, StdString, k);
		const StdString *b = &STD_VEC_AT(want, StdString, k);

		same = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	if (!same)
		std_assert_failed(RUST_FILE, line, 9, "==", got, chunks_packs_backend_tests_Names_Debug_fmt, want,
						  chunks_packs_backend_tests_Names_Debug_fmt, message);
	chunks_packs_backend_tests_Names_drop(got);
	chunks_packs_backend_tests_Names_drop(want);
}

/* chunks_packs_backend_tests_size returns a file's length, which unwraps at a line and column. */
static uint64
chunks_packs_backend_tests_size(const StdPathBuf *path, uint32 line, uint32 col)
{
	StdFsMetadata metadata;
	StdIoError	io_error;
	uint64		len;

	if (!std_fs_metadata(path->bytes.ptr, path->bytes.len, &metadata, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	len = std_fs_Metadata_len(&metadata);
	std_fs_Metadata_drop(&metadata);
	return len;
}

/* chunks_packs_backend_tests_published returns the location of a new repository's record, which it publishes, made in its home. */
static uint64
chunks_packs_backend_tests_published(HostIoIo *io)
{
	StdPathBuf	dir;
	HostSharedOwned owned;
	uint64		location;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	location = host_shared_Owned_publish(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return location;
}

/*
 * test_chunks_packs_backend_tests_a_collection_keeps_every_pack_from_the_lowest_a_process_has_open:
 * a collection keeps every pack numbered from the lowest a process has open,
 * through whichever of its packs of the repository: an unfinished pack's
 * number, a finished pack's until it is added, and that of a pack added
 * while its writer goes on, until its hold drops; with none open it keeps
 * from one past the numbers reserved. A writer dropped unfinished, and a
 * finished pack dropped before it is added, let their numbers go.
 */
void
test_chunks_packs_backend_tests_a_collection_keeps_every_pack_from_the_lowest_a_process_has_open(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 725, 46);
	uint64		location = chunks_packs_backend_tests_published(io);
	ChunksPacksBackendPacks mine;
	ChunksPacksBackendPacks theirs;
	ChunksPackPack first;
	ChunksPackPack second;
	ChunksPackPack finished;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
	ChunksPacksWritersWriting still;
	ChunksPackPackWriterInDir unfinished;
	ChunksPackPackWriterInDir next;
	ChunksPackPackWriterInDir abandoned;
	ChunksErrorError error;
	uint64		from;
	bool		writing;
	StdFmtPiece pieces[] = {STD_FMT_LIT("another of the process's packs let it go")};
	StdFmtArguments let_go = {pieces, lengthof(pieces), NULL};

	chunks_packs_backend_tests_opened(io, location, &mine);
	chunks_packs_backend_tests_opened(io, location, &theirs);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&theirs, 729, 39), 1, NULL, 729, 9);
	chunks_packs_backend_tests_written(io, &mine, "one", &first, &address, &at);
	chunks_packs_backend_tests_written(io, &mine, "two", &second, &address, &at);
	chunks_packs_backend_tests_triple_eq(first.number, second.number,
										 chunks_packs_backend_tests_kept_from(&theirs, 732, 69), 1, 2, 1, 732);
	if (!chunks_packs_backend_Packs_add(&mine, io, &reading, &first, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 733, 39);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&theirs, 734, 39), 2, NULL, 734, 9);
	if (!chunks_packs_backend_Packs_add_still_writing(&mine, io, &reading, &second, &still, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 735, 66);
	unfinished = chunks_packs_tests_writer(RUST_FILE, &mine, io, &reading, 736, 55);
	chunks_packs_backend_tests_pair_eq(chunks_pack_PackWriterInDir_number(&unfinished),
									   chunks_packs_backend_tests_kept_from(&theirs, 737, 61), 3, 2, NULL, 737);
	chunks_packs_writers_Writing_drop(&still);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&theirs, 739, 39), 3, NULL, 739, 9);
	chunks_pack_PackWriterInDir_drop(&unfinished);
	from = chunks_packs_backend_tests_kept_from(&theirs, 741, 40);
	writing = chunks_packs_backend_Packs_writing(&mine, io, 3);
	if (from != 4 || writing)
	{
		uint64		want_from = 4;
		bool		want_writing = false;
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&from, std_u64_Debug_fmt), STD_ITEM(&writing, std_bool_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&want_from, std_u64_Debug_fmt),
									 STD_ITEM(&want_writing, std_bool_Debug_fmt));

		std_assert_failed(RUST_FILE, 741, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	next = chunks_packs_tests_writer(RUST_FILE, &mine, io, &reading, 742, 49);
	chunks_packs_backend_tests_pair_eq(chunks_pack_PackWriterInDir_number(&next),
									   chunks_packs_backend_tests_kept_from(&theirs, 743, 55), 4, 4, NULL, 743);
	chunks_packs_backend_tests_written(io, &mine, "three", &finished, &address, &at);
	abandoned = chunks_packs_tests_writer(RUST_FILE, &theirs, io, &reading, 745, 40);
	if (!chunks_pack_PackWriterInDir_abandon(&abandoned, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 745, 61);
	chunks_packs_backend_tests_pair_eq(finished.number, chunks_packs_backend_tests_kept_from(&theirs, 746, 57), 5, 4,
									   &let_go, 746);
	if (!chunks_pack_PackWriterInDir_abandon(&next, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 747, 26);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&theirs, 748, 39), 5, NULL, 748, 9);
	chunks_pack_Pack_drop(&finished);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&theirs, 750, 39), 7, NULL, 750, 9);
	chunks_packs_backend_Packs_drop(&theirs);
	chunks_packs_backend_Packs_drop(&mine);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_backend_tests_a_process_writes_the_packs_of_one_repository_at_a_time:
 * a process writes the packs of one repository at a time: with a pack of one
 * open it is refused a writer in another, whose packs refuse the first's
 * pack, until the pack is added, abandoned, or dropped.
 */
void
test_chunks_packs_backend_tests_a_process_writes_the_packs_of_one_repository_at_a_time(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 759, 46);
	ChunksPacksBackendPacks one;
	ChunksPacksBackendPacks two;
	ChunksPackPack pack;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
	ChunksPackPackWriterInDir writer;
	ChunksPackPackWriterInDir next;
	ChunksErrorError error;
	bool		refused;
	uint64		two_from;
	uint64		one_from;

	chunks_packs_fixtures_created(io, 31, &one);
	chunks_packs_fixtures_created(io, 32, &two);
	chunks_packs_backend_tests_written(io, &one, "one", &pack, &address, &at);
	refused = !chunks_packs_backend_Packs_writer(&two, io, &reading, 0, &writer, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_INVARIANT;
		chunks_error_Error_drop(&error);
	}
	else
		chunks_pack_PackWriterInDir_drop(&writer);
	STD_ASSERT(762, 9, refused, "matches!(two.writer(io, &reading, 0), Err(Error::Invariant(_)))");
	refused = !chunks_packs_backend_Packs_add(&two, io, &reading, &pack, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
		chunks_error_Error_drop(&error);
	}
	STD_ASSERT(763, 9, refused, "matches!(two.add(io, &reading, pack), Err(Error::Corrupt(_)))");
	next = chunks_packs_tests_writer(RUST_FILE, &two, io, &reading, 764, 48);
	two_from = chunks_packs_backend_tests_kept_from(&two, 765, 52);
	one_from = chunks_packs_backend_tests_kept_from(&one, 765, 78);
	chunks_packs_backend_tests_triple_eq(chunks_pack_PackWriterInDir_number(&next), two_from, one_from, 1, 1, 2, 765);
	if (!chunks_pack_PackWriterInDir_abandon(&next, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 766, 26);
	chunks_packs_backend_Packs_drop(&two);
	chunks_packs_backend_Packs_drop(&one);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_open_writer is the writer actor: a writer kept open until the collector has seen its number. */
static bool
chunks_packs_backend_tests_open_writer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsNumbers *flags = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;
	bool		ok;

	if (!host_shared_read(io, &reading, error))
		return false;
	chunks_packs_backend_tests_opened(io, flags->location, &packs);
	ok = chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed);
	if (ok)
	{
		flags->numbers[0] = chunks_pack_PackWriterInDir_number(&writer);
		while (flags->numbers[1] == 0)
			(void) host_unique();
		chunks_pack_PackWriterInDir_drop(&writer);
	}
	else
		*error = chunks_packs_tests_raised(&failed);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	return ok;
}

/* chunks_packs_backend_tests_open_collector is the collector actor: the packs it keeps read once the writer's number is set. */
static bool
chunks_packs_backend_tests_open_collector(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsNumbers *flags = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint64		kept;
	bool		ok;

	chunks_packs_backend_tests_opened(io, flags->location, &packs);
	while (flags->numbers[0] == 0)
		(void) host_unique();
	ok = chunks_packs_backend_Packs_kept_from(&packs, &kept, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	else
	{
		flags->numbers[1] = 1;
		ok = kept == flags->numbers[0];
		if (!ok)
		{
			StdString	message;
			StdFmtPiece pieces[] = {STD_FMT_LIT("a collection keeps packs from "), STD_FMT_ARG(0),
			STD_FMT_LIT(" with pack 1 open")};
			StdFmtArg	args[] = {{&kept, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&message, &arguments);
			*error = chunks_packs_tests_failed(&message);
		}
	}
	chunks_packs_backend_Packs_drop(&packs);
	return ok;
}

/*
 * test_chunks_packs_backend_tests_a_collection_reads_the_number_another_backends_writer_has_open:
 * a collection in one backend keeps the pack another backend's writer has
 * open, by the number in that process's slot, and keeps nothing for it once
 * that backend has exited with the writer open.
 */
void
test_chunks_packs_backend_tests_a_collection_reads_the_number_another_backends_writer_has_open(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendTestsNumbers flags = {chunks_packs_backend_tests_published(io)};
	SimcActor	writer = {chunks_packs_backend_tests_open_writer, &flags};
	SimcActor	collector = {chunks_packs_backend_tests_open_collector, &flags};
	SimcRun		run = simc_Run_new(3);
	SimcReport	report;
	ChunksPacksBackendPacks after;
	StdFmtPiece pieces[] = {STD_FMT_LIT("an exited backend's writer is still kept")};
	StdFmtArguments still_kept = {pieces, lengthof(pieces), NULL};

	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "collector", 9, collector);
	report = simc_Run_finish(run);
	chunks_packs_tests_ended(RUST_FILE, &report, 911, 9);
	simc_Report_drop(&report);
	chunks_packs_backend_tests_opened(io, flags.location, &after);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_kept_from(&after, 917, 53), 2, &still_kept, 917, 9);
	chunks_packs_backend_Packs_drop(&after);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_backend_tests_a_kept_file_written_by_one_backend_reads_in_another:
 * a kept file written through one backend's packs reads through another's as
 * its newest generation, with the one before it as its previous, and the
 * count of rotations is even after each write.
 */
void
test_chunks_packs_backend_tests_a_kept_file_written_by_one_backend_reads_in_another(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 926, 46);
	uint64		location = chunks_packs_backend_tests_published(io);
	ChunksPacksBackendPacks packs[2];
	ChunksPacksBackendTestsKept kept;

	chunks_packs_backend_tests_opened(io, location, &packs[0]);
	chunks_packs_backend_tests_opened(io, location, &packs[1]);
	kept = chunks_packs_backend_tests_read_kept_unwrapped(&packs[1], io, 930, 57);
	chunks_packs_backend_tests_kept_eq(&kept, NULL, NULL, 930, 9);
	chunks_packs_backend_tests_written_kept(&packs[0], io, "first", 931, 55);
	chunks_packs_backend_tests_written_kept(&packs[1], io, "second", 932, 58);
	for (size_t k = 0; k < lengthof(packs); k++)
	{
		ChunksErrorError error;
		bool		has;
		StdVec		previous;

		kept = chunks_packs_backend_tests_read_kept_unwrapped(&packs[k], io, 934, 60);
		chunks_packs_backend_tests_kept_eq(&kept, "second", NULL, 934, 13);
		if (!chunks_packs_backend_Packs_read_previous(&packs[k], io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, &has,
													  &previous, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 935, 64);
		if (!has || previous.len != strlen("first") || memcmp(previous.ptr, "first", previous.len) != 0)
		{
			StdBytes	got_bytes = {NULL, 0};
			StdBytes	want_bytes = {(const uint8 *) "first", strlen("first")};
			StdOptionRef got = {has, STD_ITEM(&got_bytes, std_bytes_Debug_fmt)};
			StdOptionRef want = {true, STD_ITEM(&want_bytes, std_bytes_Debug_fmt)};

			if (has)
			{
				got_bytes.ptr = previous.ptr;
				got_bytes.len = previous.len;
			}
			std_assert_failed(RUST_FILE, 935, 13, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt,
							  NULL);
		}
		std_vec_free_Checked(&previous, 1, 1);
		chunks_packs_tests_numbers_eq(RUST_FILE, &packs[k], io, &reading, NULL, 0, NULL, 936, 13, 52);
	}
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_load(&packs[0], CHUNKS_PACKS_SHARED_ROTATIONS_INDEX), 4,
								  NULL, 938, 9);
	chunks_packs_backend_Packs_drop(&packs[1]);
	chunks_packs_backend_Packs_drop(&packs[0]);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_kept_reader is the reader actor: a read of the manifest, which must find one whole generation. */
static bool
chunks_packs_backend_tests_kept_reader(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsNumbers *shared = captures;
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendTestsKept kept;
	ChunksErrorError failed;
	bool		ok;

	chunks_packs_backend_tests_opened(io, shared->location, &packs);
	ok = chunks_packs_backend_tests_read_kept(&packs, io, &kept, &failed);
	chunks_packs_backend_Packs_drop(&packs);
	shared->numbers[0] += chunks_packs_view_rereads();
	if (ok && (chunks_packs_backend_tests_kept_is(&kept, "generation 1") ||
			   chunks_packs_backend_tests_kept_is(&kept, "generation 2")))
	{
		chunks_packs_backend_tests_Kept_drop(&kept);
		return true;
	}
	{
		StdString	message;
		ChunksPacksTestsResult read = {ok, ok ? STD_ITEM(&kept, chunks_packs_backend_tests_Kept_Debug_fmt) :
		STD_ITEM(&failed, chunks_error_Error_Debug_fmt)};
		StdFmtPiece pieces[] = {STD_FMT_LIT("a read of a kept file returned "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&read, chunks_packs_tests_Result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		if (ok)
			chunks_packs_backend_tests_Kept_drop(&kept);
		else
			chunks_error_Error_drop(&failed);
		*error = chunks_packs_tests_failed(&message);
	}
	return false;
}

/* chunks_packs_backend_tests_kept_writer is the writer actor: a write of the manifest's next generation. */
static bool
chunks_packs_backend_tests_kept_writer(void *captures, HostIoIo *io, HostError *error)
{
	const ChunksPacksBackendTestsNumbers *shared = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	bool		ok;

	chunks_packs_backend_tests_opened(io, shared->location, &packs);
	ok = chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
											   (const uint8 *) "generation 2", strlen("generation 2"), &failed);
	chunks_packs_backend_Packs_drop(&packs);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	return ok;
}

/*
 * test_chunks_packs_backend_tests_a_read_of_a_kept_file_at_any_step_of_its_write_returns_one_whole_generation:
 * a read of a kept file begun at any step of another backend's write of it,
 * with the write going on from any step of the read, returns the generation
 * before the write or the one it wrote, as the newest, never the previous
 * generation taken for a fallback, nothing, or an error; some of those
 * interleavings put the write's renames between the read's steps, which then
 * reads again under the repository's files' flag.
 */
void
test_chunks_packs_backend_tests_a_read_of_a_kept_file_at_any_step_of_its_write_returns_one_whole_generation(void)
{
	uint64		reread = 0;

	for (uint64 reads = 0; reads < 6; reads++)
		for (uint64 writes = 0; writes < 40; writes++)
		{
			SimcTestIo	test_io = simc_io();
			HostIoIo   *io = &test_io.io;
			ChunksPacksBackendTestsNumbers shared = {chunks_packs_backend_tests_published(io)};
			ChunksPacksBackendPacks before;
			SimcActor	reader = {chunks_packs_backend_tests_kept_reader, &shared};
			SimcActor	writer = {chunks_packs_backend_tests_kept_writer, &shared};
			SimcStep	steps[] = {{"reader", 6, reads}, {"writer", 6, writes}, {"reader", 6, PG_UINT64_MAX}};
			SimcRun		run;
			SimcReport	report;
			ChunksPacksBackendTestsKept after;

			chunks_packs_backend_tests_opened(io, shared.location, &before);
			chunks_packs_backend_tests_written_kept(&before, io, "generation 0", 954, 68);
			chunks_packs_backend_tests_written_kept(&before, io, "generation 1", 955, 68);
			run = simc_Run_new(1);
			simc_Run_actor(&run, "reader", 6, reader);
			simc_Run_actor(&run, "writer", 6, writer);
			simc_Run_script(&run, steps, lengthof(steps));
			report = simc_Run_finish(run);
			if (!chunks_packs_tests_returned(&report.outcome))
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("the read stopped at step "), STD_FMT_ARG(0),
					STD_FMT_LIT(" for "), STD_FMT_ARG(1), STD_FMT_LIT(" steps of the write: "),
				STD_FMT_ARG(2)};
				StdFmtArg	args[] = {{&reads, std_u64_Display_fmt}, {&writes, std_u64_Display_fmt},
				{&report.outcome, simc_Outcome_Debug_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				std_panic_fmt(RUST_FILE, 977, 13, &arguments);
			}
			after = chunks_packs_backend_tests_read_kept_unwrapped(&before, io, 983, 62);
			chunks_packs_backend_tests_kept_eq(&after, "generation 2", NULL, 984, 13);
			reread += shared.numbers[0];
			simc_Report_drop(&report);
			chunks_packs_backend_Packs_drop(&before);
			chunks_packs_tests_freed(RUST_FILE, io, shared.location, 986, 79, 88);
			simc_TestIo_drop(&test_io);
		}
	STD_ASSERT_MSG(988, 9, reread > 0, "no interleaving put a write between a read's steps");
}

/* chunks_packs_backend_tests_slot_writer is the writer actor: a writer's number, given after its slot is stored, then its abandon. */
static bool
chunks_packs_backend_tests_slot_writer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsNumbers *numbers = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;
	bool		ok;

	if (!host_shared_read(io, &reading, error))
		return false;
	chunks_packs_backend_tests_opened(io, numbers->location, &packs);
	ok = chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed);
	if (ok)
	{
		numbers->numbers[0] = chunks_pack_PackWriterInDir_number(&writer);
		ok = chunks_pack_PackWriterInDir_abandon(&writer, HOST_IO_IO_TAKES_SHORT(io), &failed);
	}
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	return ok;
}

/* chunks_packs_backend_tests_slot_collector is the collector actor: the lowest number it keeps. */
static bool
chunks_packs_backend_tests_slot_collector(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsNumbers *numbers = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	bool		ok;

	chunks_packs_backend_tests_opened(io, numbers->location, &packs);
	ok = chunks_packs_backend_Packs_kept_from(&packs, &numbers->numbers[1], &failed);
	chunks_packs_backend_Packs_drop(&packs);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	return ok;
}

/*
 * test_chunks_packs_backend_tests_a_collection_between_a_writers_slot_and_its_number_keeps_the_writers_pack:
 * a collection that reads the writers' slots after a backend stored its slot
 * and before its writer's number was reserved keeps packs from a number no
 * higher than the one the writer then gets.
 */
void
test_chunks_packs_backend_tests_a_collection_between_a_writers_slot_and_its_number_keeps_the_writers_pack(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendTestsNumbers numbers = {chunks_packs_backend_tests_published(io)};
	SimcActor	writer = {chunks_packs_backend_tests_slot_writer, &numbers};
	SimcActor	collector = {chunks_packs_backend_tests_slot_collector, &numbers};
	SimcStep	steps[] = {{"collector", 9, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcRun		run = simc_Run_new(13);
	SimcReport	report;
	uint64		got;
	uint64		kept;

	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "collector", 9, collector);
	simc_Run_until(&run, "writer", 6, "doltgres-chunks-writer-stored");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	chunks_packs_tests_ended(RUST_FILE, &report, 1022, 9);
	simc_Report_drop(&report);
	got = numbers.numbers[0];
	kept = numbers.numbers[1];
	if (got == 0 || kept > got)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a collection keeps packs from "), STD_FMT_ARG(0),
		STD_FMT_LIT(", past the writer's pack "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&kept, std_u64_Display_fmt}, {&got, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1029, 9, &arguments);
	}
	simc_TestIo_drop(&test_io);
}

/*
 * chunks_packs_backend_tests_deferred makes a pack of one chunk through a
 * backend's packs and defers its add, or fails as its actor does.
 */
static bool
chunks_packs_backend_tests_deferred(HostIoIo *io, const ChunksPacksBackendPacks *packs, const HostSharedReader *reading,
									HostError *error)
{
	ChunksPackPackWriterInDir writer;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
	ChunksPackPack pack;
	ChunksErrorError failed;
	StdVec		bytes;
	bool		ok;

	if (!chunks_packs_backend_Packs_writer(packs, io, reading, 0, &writer, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	chunks_packs_fixtures_stored((const uint8 *) "one", 3, &address, &bytes);
	ok = chunks_pack_PackWriterInDir_write(&writer, io, address, bytes.ptr, bytes.len, false, &at, &failed);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!ok)
		chunks_pack_PackWriterInDir_drop(&writer);
	else
		ok = chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &failed) &&
			chunks_packs_backend_Packs_add_deferred(packs, HOST_IO_IO_TAKES_SHORT(io), reading, &pack, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	return ok;
}

/*
 * chunks_packs_backend_tests_exiting is the backend actor terminated after
 * it dropped a writer unfinished, holding packs that defer an add.
 */
static bool
chunks_packs_backend_tests_exiting(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;

	if (!host_shared_read(io, &reading, error))
		return false;
	chunks_packs_backend_tests_opened(io, *location, &packs);
	if (!chunks_packs_backend_tests_deferred(io, &packs, &reading, error))
		goto packs;
	if (!chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto packs;
	}
	chunks_pack_PackWriterInDir_drop(&writer);
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-exiting", error))
		goto packs;
	for (;;)
		(void) host_unique();
packs:
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	return false;
}

/*
 * test_chunks_packs_backend_tests_a_terminated_backends_exit_removes_its_dropped_writers_file_and_appends_its_deferred_adds:
 * a backend terminated after it dropped a writer unfinished removes the
 * writer's file as it exits, and, holding packs that defer an add, appends
 * the add to the list files as it exits: a record made again from them lists
 * the pack.
 */
void
test_chunks_packs_backend_tests_a_terminated_backends_exit_removes_its_dropped_writers_file_and_appends_its_deferred_adds(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 1039, 46);
	ChunksPacksBackendPacks packs;
	uint64		location;
	StdPathBuf	dir = host_tablespace_dir(37, HOST_PG_DEFAULT);
	SimcActor	backend = {chunks_packs_backend_tests_exiting, &location};
	SimcRun		run = simc_Run_new(41);
	SimcReport	report;
	StdVec		names;
	StdVec		unfinished;
	StdVec		none;
	ChunksPacksBackendPacks again;
	static const uint64 one[] = {1};
	StdFmtPiece left_pieces[] = {STD_FMT_LIT("the exit left a dropped writer's file")};
	StdFmtArguments left_file = {left_pieces, lengthof(left_pieces), NULL};
	StdFmtPiece deferred_pieces[] = {STD_FMT_LIT("the exit left an add deferred")};
	StdFmtArguments left_deferred = {deferred_pieces, lengthof(deferred_pieces), NULL};

	chunks_packs_fixtures_created(io, 37, &packs);
	location = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	simc_Run_actor(&run, "backend", 7, backend);
	simc_Run_until(&run, "backend", 7, "doltgres-chunks-case-exiting");
	simc_Run_terminate_at(&run, "backend", 7, 5000);
	report = simc_Run_finish(run);
	chunks_packs_tests_terminated(RUST_FILE, &report, 1062, 67, 1063, 20, 9);
	names = chunks_packs_backend_tests_names(io, &dir, 1064, 55);
	std_vec_new(&unfinished, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&names, StdString, k);

		if (name->len >= strlen(".pack.tmp") &&
			memcmp((const char *) name->ptr + name->len - strlen(".pack.tmp"), ".pack.tmp", strlen(".pack.tmp")) == 0)
			std_vec_push_Checked(&unfinished, name, sizeof(StdString), _Alignof(StdString));
		else
			std_vec_free_Checked(name, 1, 1);
	}
	std_vec_free_Checked(&names, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&none, sizeof(StdString), _Alignof(StdString));
	chunks_packs_backend_tests_names_eq(&unfinished, &none, &left_file, 1066);
	chunks_packs_tests_freed(RUST_FILE, io, location, 1067, 75, 84);
	chunks_packs_tests_opened(RUST_FILE, io, 37, 1068, 67, 95, &again);
	chunks_packs_tests_numbers_eq(RUST_FILE, &again, io, &reading, one, lengthof(one), &left_deferred, 1069, 9, 48);
	chunks_packs_backend_Packs_drop(&again);
	simc_Report_drop(&report);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_failing is the backend actor terminated holding packs that defer an add, whose exit's append fails. */
static bool
chunks_packs_backend_tests_failing(void *captures, HostIoIo *io, HostError *error)
{
	const ChunksPacksBackendTestsFault *fault = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;

	if (!host_shared_read(io, &reading, error))
		return false;
	chunks_packs_backend_tests_opened(io, fault->location, &packs);
	if (!chunks_packs_backend_tests_deferred(io, &packs, &reading, error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-deferring", error))
	{
		chunks_packs_backend_Packs_drop(&packs);
		host_shared_Reader_drop(&reading);
		return false;
	}
	simc_Sim_full_at(fault->fault, simc_Sim_calls(fault->fault) + 1);
	for (;;)
		(void) host_unique();
}

/*
 * test_chunks_packs_backend_tests_a_failed_append_at_a_backends_exit_leaves_the_add_for_the_next_close:
 * a backend terminated holding packs that defer an add, whose exit's append
 * of it fails, leaves the list files as they were and the record deferring
 * the add, which the next close of the repository's packs appends: a record
 * made again from the list files lists the pack.
 */
void
test_chunks_packs_backend_tests_a_failed_append_at_a_backends_exit_leaves_the_add_for_the_next_close(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 1080, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks closing;
	ChunksPacksBackendPacks again;
	ChunksErrorError error;
	uint64		zero = 0;
	StdPathBuf	dir = host_tablespace_dir(38, HOST_PG_DEFAULT);
	StdPathBuf	table;
	StdPathBuf	data;
	SimcSim		sim;
	ChunksPacksBackendTestsFault fault;
	SimcActor	backend = {chunks_packs_backend_tests_failing, &fault};
	SimcRun		run;
	SimcReport	report;
	uint64		before;
	uint64		after;
	static const uint64 one[] = {1};
	StdFmtPiece failed_pieces[] = {STD_FMT_LIT("the exit's append wasn't made to fail")};
	StdFmtArguments not_failed = {failed_pieces, lengthof(failed_pieces), NULL};
	StdFmtPiece lost_pieces[] = {STD_FMT_LIT("an add deferred at a failed exit was lost")};
	StdFmtArguments lost = {lost_pieces, lengthof(lost_pieces), NULL};

	chunks_packs_fixtures_created(io, 38, &packs);
	if (!chunks_packs_backend_Packs_changed(&packs, HOST_IO_IO_TAKES_SHORT(io), &reading, chunks_packs_backend_reserve,
											&zero, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1082, 65);
	fault.location = chunks_packs_backend_Packs_location(&packs);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	chunks_packs_backend_Packs_drop(&packs);
	before = chunks_packs_backend_tests_size(&table, 1086, 48);
	data = simc_data_dir();
	sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 59);
	std_vec_free_Checked(&data.bytes, 1, 1);
	fault.fault = &sim;
	run = simc_Run_new(59);
	simc_Run_actor(&run, "backend", 7, backend);
	simc_Run_until(&run, "backend", 7, "doltgres-chunks-case-deferring");
	simc_Run_terminate_at(&run, "backend", 7, 5000);
	report = simc_Run_finish(run);
	simc_Sim_stop(&sim);
	chunks_packs_tests_terminated(RUST_FILE, &report, 1108, 67, 1109, 20, 9);
	after = chunks_packs_backend_tests_size(&table, 1110, 46);
	chunks_packs_backend_tests_eq(after, before, &not_failed, 1110, 9);
	chunks_packs_backend_tests_opened(io, fault.location, &closing);
	if (!chunks_packs_backend_Packs_close(&closing, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1112, 50);
	after = chunks_packs_backend_tests_size(&table, 1113, 43);
	STD_ASSERT_MSG(1113, 9, after > before, "the next close appended nothing");
	chunks_packs_tests_freed(RUST_FILE, io, fault.location, 1114, 75, 84);
	chunks_packs_tests_opened(RUST_FILE, io, 38, 1115, 67, 95, &again);
	chunks_packs_tests_numbers_eq(RUST_FILE, &again, io, &reading, one, lengthof(one), &lost, 1116, 9, 48);
	chunks_packs_backend_Packs_drop(&again);
	simc_Report_drop(&report);
	simc_Sim_drop(&sim);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * chunks_packs_backend_tests_tabled_eq asserts at a line that numbers, then
 * the bound the table's file holds and its records read past its whole, are
 * those given, the table's two as a tuple of their own after the numbers.
 */
static void
chunks_packs_backend_tests_tabled_eq(HostIoIo *io, const StdPathBuf *path, const uint64 *lead, const uint64 *lead_want,
									 size_t lead_len, uint64 want_bound, uint64 want_past, uint32 line)
{
	ChunksPacksViewReadTable read;
	ChunksErrorError error;
	StdTupleRef got_table;
	StdTupleRef want_table;
	StdTupleRef got = {lead_len + 1};
	StdTupleRef want = {lead_len + 1};

	chunks_packs_Table_Default_default(&read.list);
	if (!chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, HOST_IO_IO_READ_DOORS(io), path->bytes.ptr,
									  path->bytes.len, &read.list, &read.read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1130, 55);
	if ((lead_len == 0 || memcmp(lead, lead_want, lead_len * sizeof(uint64)) == 0) &&
		read.list.reserved == want_bound && read.read.past == want_past)
	{
		chunks_packs_Table_drop(&read.list);
		return;
	}
	got_table = STD_TUPLE(2, STD_ITEM(&read.list.reserved, std_u64_Debug_fmt),
						  STD_ITEM(&read.read.past, std_u64_Debug_fmt));
	want_table = STD_TUPLE(2, STD_ITEM(&want_bound, std_u64_Debug_fmt), STD_ITEM(&want_past, std_u64_Debug_fmt));
	if (lead_len == 0)
		std_assert_failed(RUST_FILE, line, 9, "==", &got_table, std_tuple_Debug_fmt, &want_table, std_tuple_Debug_fmt,
						  NULL);
	for (size_t k = 0; k < lead_len; k++)
	{
		got.items[k] = STD_ITEM(&lead[k], std_u64_Debug_fmt);
		want.items[k] = STD_ITEM(&lead_want[k], std_u64_Debug_fmt);
	}
	got.items[lead_len] = STD_ITEM(&got_table, std_tuple_Debug_fmt);
	want.items[lead_len] = STD_ITEM(&want_table, std_tuple_Debug_fmt);
	std_assert_failed(RUST_FILE, line, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
}

/*
 * test_chunks_packs_backend_tests_a_writers_number_is_under_a_bound_the_table_holds:
 * the first writer of a record has the table's file hold a bound before it
 * is given its number, and the writers after it take theirs with no write; a
 * raise asked for with a bound already passed writes nothing; a bound set
 * back is the highest number given, in the record and in the file, once; and
 * the next writer raises it again.
 */
void
test_chunks_packs_backend_tests_a_writers_number_is_under_a_bound_the_table_holds(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 1126, 46);
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(601, HOST_PG_DEFAULT);
	StdPathBuf	path;
	ChunksPackPackWriterInDir first;
	ChunksPackPackWriterInDir second;
	ChunksPackPackWriterInDir third;
	ChunksErrorError error;
	uint64		zero = 0;
	uint64		lead[2];
	uint64		lead_want[2];

	chunks_packs_fixtures_created(io, 601, &packs);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	chunks_packs_backend_tests_tabled_eq(io, &path, NULL, NULL, 0, 0, 0, 1133);
	first = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 1134, 51);
	lead[0] = chunks_pack_PackWriterInDir_number(&first);
	lead_want[0] = 1;
	chunks_packs_backend_tests_tabled_eq(io, &path, lead, lead_want, 1, CHUNKS_PACKS_SHARED_AHEAD, 1, 1135);
	second = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 1136, 52);
	lead[0] = chunks_pack_PackWriterInDir_number(&second);
	lead_want[0] = 2;
	chunks_packs_backend_tests_tabled_eq(io, &path, lead, lead_want, 1, CHUNKS_PACKS_SHARED_AHEAD, 1, 1137);
	if (!chunks_packs_backend_Packs_changed(&packs, HOST_IO_IO_TAKES_SHORT(io), &reading, chunks_packs_backend_reserve,
											&zero, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1138, 65);
	lead[0] = chunks_packs_backend_tests_load(&packs, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	lead_want[0] = chunks_packs_shared_reserved(CHUNKS_PACKS_SHARED_AHEAD, 2);
	chunks_packs_backend_tests_tabled_eq(io, &path, lead, lead_want, 1, CHUNKS_PACKS_SHARED_AHEAD, 1, 1140);
	if (!chunks_packs_backend_Packs_unreserve(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1142, 39);
	lead[0] = chunks_packs_backend_tests_load(&packs, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	lead_want[0] = chunks_packs_shared_reserved(2, 2);
	chunks_packs_backend_tests_tabled_eq(io, &path, lead, lead_want, 1, 2, 2, 1143);
	if (!chunks_packs_backend_Packs_unreserve(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1144, 39);
	chunks_packs_backend_tests_tabled_eq(io, &path, NULL, NULL, 0, 2, 2, 1145);
	third = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 1146, 51);
	lead[0] = chunks_pack_PackWriterInDir_number(&third);
	lead[1] = chunks_packs_backend_tests_load(&packs, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	lead_want[0] = 3;
	lead_want[1] = chunks_packs_shared_reserved(2 + CHUNKS_PACKS_SHARED_AHEAD, 3);
	chunks_packs_backend_tests_tabled_eq(io, &path, lead, lead_want, 2, 2 + CHUNKS_PACKS_SHARED_AHEAD, 3, 1147);
	chunks_pack_PackWriterInDir_drop(&third);
	chunks_pack_PackWriterInDir_drop(&second);
	chunks_pack_PackWriterInDir_drop(&first);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_backend_tests_a_repository_at_its_last_number_refuses_a_writer:
 * a repository gives its last number, the highest a locator holds, the bound
 * raised no further than it, and then refuses a writer with Postgres'
 * program limit, naming the database; the pack numbered last is added,
 * listed, and read.
 */
void
test_chunks_packs_backend_tests_a_repository_at_its_last_number_refuses_a_writer(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 1157, 46);
	ChunksPacksBackendPacks packs;
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
	chunks_TupleAddressLocator_t added;
	StdVec		numbers;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError error;
	uint64		last = CHUNKS_PACKS_SHARED_LAST;
	static const char wording[] = "database with OID 602 has used every pack number";
	bool		refused;
	bool		matched = false;

	chunks_packs_fixtures_created(io, 602, &packs);
	exchange.u.exchange.f1 = chunks_packs_shared_reserved(CHUNKS_PACKS_SHARED_LAST - 1, CHUNKS_PACKS_SHARED_LAST - 1);
	(void) host_shared_Record_atomic(host_shared_Held_record(&packs.held), CHUNKS_PACKS_SHARED_RESERVED_INDEX, exchange);
	added = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "last", 4);
	if (!chunks_packs_backend_Packs_numbers(&packs, io, &reading, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1161, 58);
	if (added.f1.pack != last || numbers.len != 1 || STD_VEC_AT(&numbers, uint64, 0) != last)
	{
		StdVec		want_numbers;
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&added.f1.pack, std_u64_Debug_fmt),
									STD_ITEM(&numbers, chunks_packs_tests_Numbers_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&last, std_u64_Debug_fmt),
									 STD_ITEM(&want_numbers, chunks_packs_tests_Numbers_Debug_fmt));

		std_slice_to_vec_Checked(&want_numbers, &last, 1, sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 1161, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	chunks_packs_tests_found(RUST_FILE, &packs, io, &reading, &added.f0, &added.f1, 1162, 9, 55);
	refused = !chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &error);
	if (!refused)
		chunks_pack_PackWriterInDir_drop(&writer);
	else if (error.tag == CHUNKS_ERROR_ERROR_HOST)
	{
		StdStr		message = host_Error_message(&error.u.host);

		matched = memcmp(host_Error_sqlstate(&error.u.host), "54000", 5) == 0 && message.ptr != NULL &&
			message.len == strlen(wording) && memcmp(message.ptr, wording, message.len) == 0;
	}
	if (!matched)
	{
		StdOptionRef shown = {refused, STD_ITEM(&error, chunks_error_Error_Debug_fmt)};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&shown, std_option_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1165, 9, &arguments);
	}
	chunks_error_Error_drop(&error);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_backend_tests_a_manifests_write_marks_the_record_while_its_generations_may_differ:
 * a write of the manifest marks the record while its generations may
 * differ: a write of other bytes sets the mark, a write of the same bytes
 * clears it, and a write that fails leaves it set.
 */
void
test_chunks_packs_backend_tests_a_manifests_write_marks_the_record_while_its_generations_may_differ(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	static const char *const writes[] = {"first", "first", "other", "other"};
	static const uint64 marks[] = {1, 0, 1, 0};
	StdPathBuf	data;
	SimcSim		sim;
	ChunksErrorError error;
	bool		failed;

	chunks_packs_backend_tests_opened(io, chunks_packs_backend_tests_published(io), &packs);
	for (size_t k = 0; k < lengthof(writes); k++)
	{
		StdBytes	bytes = {(const uint8 *) writes[k], strlen(writes[k])};
		StdFmtPiece pieces[] = {STD_FMT_LIT("after "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&bytes, std_bytes_Debug_fmt}};
		StdFmtArguments after = {pieces, lengthof(pieces), args};

		chunks_packs_backend_tests_written_kept(&packs, io, writes[k], 1181, 57);
		chunks_packs_backend_tests_eq(chunks_packs_backend_tests_load(&packs, CHUNKS_PACKS_SHARED_DIFFERS_INDEX),
									  marks[k], &after, 1182, 13);
	}
	data = simc_data_dir();
	sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 31);
	std_vec_free_Checked(&data.bytes, 1, 1);
	simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + 1);
	failed = !chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
													(const uint8 *) "other", 5, &error);
	if (failed)
		chunks_error_Error_drop(&error);
	STD_ASSERT(1186, 9, failed, "packs.write_kept(io, Kept::Manifest, b\"other\").is_err()");
	simc_Sim_stop(&sim);
	chunks_packs_backend_tests_eq(chunks_packs_backend_tests_load(&packs, CHUNKS_PACKS_SHARED_DIFFERS_INDEX), 1, NULL,
								  1188, 9);
	simc_Sim_drop(&sim);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_evener is the evener actor: the reclaim's write of a marked manifest once more. */
static bool
chunks_packs_backend_tests_evener(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	bool		ok;

	chunks_packs_backend_tests_opened(io, *location, &packs);
	ok = chunks_packs_backend_Packs_even_manifest(&packs, io, &failed);
	chunks_packs_backend_Packs_drop(&packs);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	return ok;
}

/* chunks_packs_backend_tests_third_writer is the writer actor: a write of the manifest's third bytes. */
static bool
chunks_packs_backend_tests_third_writer(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	bool		ok;

	chunks_packs_backend_tests_opened(io, *location, &packs);
	ok = chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
											   (const uint8 *) "third", 5, &failed);
	chunks_packs_backend_Packs_drop(&packs);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	return ok;
}

/*
 * test_chunks_packs_backend_tests_a_manifest_written_once_more_never_undoes_another_write_of_it:
 * the reclaim's write of a marked manifest once more, begun at any step of
 * another backend's write of the manifest with that write going on from any
 * step of it, never puts back the bytes the other write replaced: the newest
 * manifest is the other write's after both.
 */
void
test_chunks_packs_backend_tests_a_manifest_written_once_more_never_undoes_another_write_of_it(void)
{
	for (uint64 evens = 0; evens < 8; evens++)
		for (uint64 writes = 0; writes < 40; writes++)
		{
			SimcTestIo	test_io = simc_io();
			HostIoIo   *io = &test_io.io;
			uint64		location = chunks_packs_backend_tests_published(io);
			ChunksPacksBackendPacks before;
			SimcActor	evener = {chunks_packs_backend_tests_evener, &location};
			SimcActor	writer = {chunks_packs_backend_tests_third_writer, &location};
			SimcStep	steps[] = {{"evener", 6, evens}, {"writer", 6, writes}, {"evener", 6, PG_UINT64_MAX}};
			SimcRun		run;
			SimcReport	report;
			ChunksPacksBackendTestsKept newest;
			StdFmtPiece pieces[] = {STD_FMT_LIT("at step "), STD_FMT_ARG(0), STD_FMT_LIT(" for "), STD_FMT_ARG(1),
			STD_FMT_LIT(" steps of the write")};
			StdFmtArg	args[] = {{&evens, std_u64_Display_fmt}, {&writes, std_u64_Display_fmt}};
			StdFmtArguments at_step = {pieces, lengthof(pieces), args};

			chunks_packs_backend_tests_opened(io, location, &before);
			chunks_packs_backend_tests_written_kept(&before, io, "first", 1201, 61);
			chunks_packs_backend_tests_written_kept(&before, io, "second", 1202, 62);
			run = simc_Run_new(1);
			simc_Run_actor(&run, "evener", 6, evener);
			simc_Run_actor(&run, "writer", 6, writer);
			simc_Run_script(&run, steps, lengthof(steps));
			report = simc_Run_finish(run);
			if (!chunks_packs_tests_returned(&report.outcome))
			{
				StdFmtPiece stopped_pieces[] = {STD_FMT_LIT("the rewrite stopped at step "), STD_FMT_ARG(0),
					STD_FMT_LIT(" for "), STD_FMT_ARG(1), STD_FMT_LIT(" steps of the write: "),
				STD_FMT_ARG(2)};
				StdFmtArg	stopped_args[] = {{&evens, std_u64_Display_fmt}, {&writes, std_u64_Display_fmt},
				{&report.outcome, simc_Outcome_Debug_fmt}};
				StdFmtArguments stopped = {stopped_pieces, lengthof(stopped_pieces), stopped_args};

				std_panic_fmt(RUST_FILE, 1212, 13, &stopped);
			}
			newest = chunks_packs_backend_tests_read_kept_unwrapped(&before, io, 1218, 63);
			chunks_packs_backend_tests_kept_eq(&newest, "third", &at_step, 1219, 13);
			simc_Report_drop(&report);
			chunks_packs_backend_Packs_drop(&before);
			chunks_packs_tests_freed(RUST_FILE, io, location, 1221, 79, 88);
			simc_TestIo_drop(&test_io);
		}
}

/*
 * test_chunks_packs_backend_tests_a_conditional_write_of_the_manifest_lands_only_over_the_one_it_read:
 * a conditional write of the manifest lands only while the count of
 * rotations is the one it was given: the reclaim's write of a marked
 * manifest once more after that count was read refuses it, writing nothing,
 * and a write at the count as it then is lands.
 */
void
test_chunks_packs_backend_tests_a_conditional_write_of_the_manifest_lands_only_over_the_one_it_read(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendTestsKept kept;
	ChunksErrorError error;
	uint64		read;
	bool		landed;

	chunks_packs_backend_tests_opened(io, chunks_packs_backend_tests_published(io), &packs);
	chunks_packs_backend_tests_written_kept(&packs, io, "first", 1234, 56);
	chunks_packs_backend_tests_written_kept(&packs, io, "second", 1235, 57);
	read = chunks_packs_backend_Packs_rotations(&packs);
	if (!chunks_packs_backend_Packs_even_manifest(&packs, io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1237, 33);
	if (!chunks_packs_backend_Packs_write_kept_if(&packs, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												  (const uint8 *) "third", 5, read, &landed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1238, 74);
	STD_ASSERT(1238, 9, !landed, "!packs.write_kept_if(io, Kept::Manifest, b\"third\", read).unwrap()");
	kept = chunks_packs_backend_tests_read_kept_unwrapped(&packs, io, 1239, 56);
	chunks_packs_backend_tests_kept_eq(&kept, "second", NULL, 1239, 9);
	if (!chunks_packs_backend_Packs_write_kept_if(&packs, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												  (const uint8 *) "third", 5,
												  chunks_packs_backend_Packs_rotations(&packs), &landed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1240, 86);
	STD_ASSERT(1240, 9, landed, "packs.write_kept_if(io, Kept::Manifest, b\"third\", packs.rotations()).unwrap()");
	kept = chunks_packs_backend_tests_read_kept_unwrapped(&packs, io, 1241, 56);
	chunks_packs_backend_tests_kept_eq(&kept, "third", NULL, 1241, 9);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_maker is the opener actor: a new repository's packs, which it leaves for the closer. */
static bool
chunks_packs_backend_tests_maker(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsMoved *moved = captures;

	(void) error;
	chunks_packs_fixtures_created(io, 693, &moved->packs);
	moved->has = true;
	return true;
}

/* chunks_packs_backend_tests_closer is the closer actor: a drop of the packs the opener made. */
static bool
chunks_packs_backend_tests_closer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendTestsMoved *moved = captures;

	(void) io;
	(void) error;
	if (moved->has)
	{
		moved->has = false;
		chunks_packs_backend_Packs_drop(&moved->packs);
	}
	return true;
}

/* chunks_packs_backend_tests_holds_no_handle reports whether a run failed as a close by the closer of a record it holds no handle of. */
static bool
chunks_packs_backend_tests_holds_no_handle(const SimcOutcome *outcome)
{
	static const char panicked[] = "closer panicked: ";
	static const char no_handle[] = "holds no handle of";
	const StdString *failed = &outcome->u.failed;

	if (outcome->tag != SIMC_OUTCOME_FAILED || failed->len < strlen(panicked) ||
		memcmp(failed->ptr, panicked, strlen(panicked)) != 0)
		return false;
	for (size_t k = 0; k + strlen(no_handle) <= failed->len; k++)
		if (memcmp((const char *) failed->ptr + k, no_handle, strlen(no_handle)) == 0)
			return true;
	return false;
}

/*
 * test_chunks_packs_backend_tests_packs_closed_in_another_backend_are_refused:
 * a backend's packs closed in another backend, which no type refuses since
 * the packs move between threads, are caught: the simulation refuses a close
 * of the pack record by a process that holds no handle of it.
 */
void
test_chunks_packs_backend_tests_packs_closed_in_another_backend_are_refused(void)
{
	ChunksPacksBackendTestsMoved moved = {false};
	SimcActor	opener = {chunks_packs_backend_tests_maker, &moved};
	SimcActor	closer = {chunks_packs_backend_tests_closer, &moved};
	SimcStep	steps[] = {{"opener", 6, PG_UINT64_MAX}, {"closer", 6, PG_UINT64_MAX}};
	SimcRun		run = simc_Run_new(59);
	SimcReport	report;

	simc_Run_actor(&run, "opener", 6, opener);
	simc_Run_actor(&run, "closer", 6, closer);
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (!chunks_packs_backend_tests_holds_no_handle(&report.outcome))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1261, 9, &arguments);
	}
	simc_Report_drop(&report);
}

/* chunks_packs_backend_tests_joined returns a path with a name joined to it. */
static StdPathBuf
chunks_packs_backend_tests_joined(const StdPathBuf *dir, const char *name)
{
	StdPathBuf	path;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	return path;
}

/*
 * test_chunks_packs_backend_tests_a_copy_copies_a_directory_of_the_repository_as_a_directory:
 * bugs row 182: a repository's copy copies a directory of its own, as the
 * delta's runs keep one, as a directory, its files whole within it, beside
 * the repository's own files.
 */
void
test_chunks_packs_backend_tests_a_copy_copies_a_directory_of_the_repository_as_a_directory(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 1275, 46);
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(39, HOST_PG_DEFAULT);
	StdPathBuf	delta = chunks_packs_backend_tests_joined(&dir, "delta");
	StdPathBuf	nested = chunks_packs_backend_tests_joined(&delta, "run");
	StdPathBuf	rows = chunks_packs_backend_tests_joined(&nested, "rows");
	StdPathBuf	versioned = host_versioned_dir(HOST_PG_DEFAULT);
	StdPathBuf	into = chunks_packs_backend_tests_joined(&versioned, "39.moving");
	StdPathBuf	into_delta = chunks_packs_backend_tests_joined(&into, "delta");
	StdPathBuf	into_run = chunks_packs_backend_tests_joined(&into_delta, "run");
	StdPathBuf	into_rows = chunks_packs_backend_tests_joined(&into_run, "rows");
	HostFile	file;
	HostFile	copied;
	HostError	host;
	ChunksErrorError error;
	StdVec		got;
	StdVec		want;
	StdVec		read;
	StdString	run;

	chunks_packs_fixtures_created(io, 39, &packs);
	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), delta.bytes.ptr, delta.bytes.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1279, 44);
	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(io), nested.bytes.ptr, nested.bytes.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1280, 33);
	if (!chunks_file_create(HOST_IO_IO_IO_DOORS(io), rows.bytes.ptr, rows.bytes.len, &file, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1281, 53);
	if (!chunks_file_write_at(HOST_IO_IO_IO_DOORS(io), &file, rows.bytes.ptr, rows.bytes.len, 0,
							  (const uint8 *) "spilled", 7, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1282, 66);
	if (!chunks_file_sync(HOST_IO_IO_IO_DOORS(io), &file, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1283, 25);
	if (!chunks_packs_backend_Packs_copied(&packs, io, &reading, into.bytes.ptr, into.bytes.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1285, 43);
	got = chunks_packs_backend_tests_names(io, &into, 1286, 42);
	want = chunks_packs_backend_tests_names(io, &dir, 1286, 73);
	chunks_packs_backend_tests_names_eq(&got, &want, NULL, 1286);
	got = chunks_packs_backend_tests_names(io, &into_delta, 1287, 56);
	std_vec_new(&want, sizeof(StdString), _Alignof(StdString));
	std_string_from_Checked(&run, "run", 3);
	std_vec_push_Checked(&want, &run, sizeof(StdString), _Alignof(StdString));
	chunks_packs_backend_tests_names_eq(&got, &want, NULL, 1287);
	if (!host_open_to_read(HOST_IO_IO_READ_DOORS(io), into_rows.bytes.ptr, into_rows.bytes.len, &copied, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 1288, 91);
	if (!chunks_file_read_at(HOST_IO_IO_READ_DOORS(io), &copied, 0, 7, &read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1289, 47);
	if (read.len != 7 || memcmp(read.ptr, "spilled", 7) != 0)
	{
		StdBytes	got_bytes = {read.ptr, read.len};
		StdBytes	want_bytes = {(const uint8 *) "spilled", 7};

		std_assert_failed(RUST_FILE, 1289, 9, "==", &got_bytes, std_bytes_Debug_fmt, &want_bytes, std_bytes_Debug_fmt,
						  NULL);
	}
	std_vec_free_Checked(&read, 1, 1);
	if (!chunks_packs_backend_Packs_close(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 1290, 35);
	host_File_drop(&copied);
	host_File_drop(&file);
	std_vec_free_Checked(&into_rows.bytes, 1, 1);
	std_vec_free_Checked(&into_run.bytes, 1, 1);
	std_vec_free_Checked(&into_delta.bytes, 1, 1);
	std_vec_free_Checked(&into.bytes, 1, 1);
	std_vec_free_Checked(&versioned.bytes, 1, 1);
	std_vec_free_Checked(&rows.bytes, 1, 1);
	std_vec_free_Checked(&nested.bytes, 1, 1);
	std_vec_free_Checked(&delta.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_answer_eq asserts at a line and column that an answer is the place given, or none when it is NULL, with a message or none. */
static void
chunks_packs_backend_tests_answer_eq(bool has, const ChunksTrailerLocator *found, const ChunksTrailerLocator *want,
									 const StdFmtArguments *message, uint32 line, uint32 col)
{
	if (has != (want != NULL) || (has && !chunks_trailer_Locator_PartialEq_eq(found, want)))
	{
		StdOptionRef got = {has, STD_ITEM(found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef wanted = {want != NULL, STD_ITEM(want, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, col, "==", &got, std_option_Debug_fmt, &wanted, std_option_Debug_fmt,
						  message);
	}
}

/* chunks_packs_backend_tests_find_eq asserts at a line that a find, which unwraps at a column, answers the place given, or none when it is NULL. */
static void
chunks_packs_backend_tests_find_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
								   const HostSharedReader *reading, const ChunksAddressAddress *address,
								   const ChunksTrailerLocator *want, uint32 line, uint32 assert_col, uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_backend_Packs_find(packs, io, reading, address, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	chunks_packs_backend_tests_answer_eq(has, &found, want, NULL, line, assert_col);
}

/* chunks_packs_backend_tests_read_bytes reads a chunk through a repository's packs and returns its bytes, which unwraps at a line and column. */
static StdVec
chunks_packs_backend_tests_read_bytes(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									  const HostSharedReader *reading, const ChunksAddressAddress *address,
									  ChunksTrailerLocator at, uint32 line, uint32 col)
{
	ChunksChunkChunk chunk;
	ChunksErrorError error;
	StdVec		bytes;

	if (!chunks_packs_backend_Packs_read(packs, io, reading, address, at, &chunk, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	chunks_chunk_Chunk_into_bytes(&chunk, &bytes);
	return bytes;
}

/* chunks_packs_backend_tests_bytes_eq asserts at a line and column that two byte vectors, which it frees, are the same. */
static void
chunks_packs_backend_tests_bytes_eq(StdVec *got, StdVec *want, uint32 line, uint32 col)
{
	if (got->len != want->len || memcmp(got->ptr, want->ptr, got->len) != 0)
	{
		StdBytes	left = {got->ptr, got->len};
		StdBytes	right = {want->ptr, want->len};

		std_assert_failed(RUST_FILE, line, col, "==", &left, std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt, NULL);
	}
	std_vec_free_Checked(got, 1, 1);
	std_vec_free_Checked(want, 1, 1);
}

/* chunks_packs_backend_tests_Addresses_Debug_fmt is a Vec<Address>'s Debug. */
static bool
chunks_packs_backend_tests_Addresses_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *addresses = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = addresses->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(addresses, ChunksAddressAddress, k), chunks_address_Address_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_backend_tests_Locators_Debug_fmt is a Vec<Locator>'s Debug. */
static bool
chunks_packs_backend_tests_Locators_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *locators = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = locators->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(locators, ChunksTrailerLocator, k), chunks_trailer_Locator_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_backend_tests_refused_corrupt reports whether a call failed with a corrupt error, which it drops. */
static bool
chunks_packs_backend_tests_refused_corrupt(bool ok, ChunksErrorError *error)
{
	bool		refused = !ok && error->tag == CHUNKS_ERROR_ERROR_CORRUPT;

	if (!ok)
		chunks_error_Error_drop(error);
	return refused;
}

/* chunks_packs_backend_tests_listed_each asserts at line 668 through 679 what each of two backends' packs over one record lists, finds, and reads. */
static void
chunks_packs_backend_tests_listed_each(const ChunksPacksBackendPacks *packs, const ChunksPacksBackendPacks *mine,
									   HostIoIo *io, const HostSharedReader *reading,
									   const chunks_TupleAddressLocator_t *one, const chunks_TupleAddressLocator_t *two,
									   const ChunksPackPackId *second_id)
{
	static const uint64 both[] = {1, 2};
	ChunksTrailerLocator found;
	ChunksTrailerLocator locators[2] = {one->f1, two->f1};
	ChunksErrorError error;
	StdVec		read;
	StdVec		again;
	StdString	shown;
	StdVec		prefixed;
	StdArcInner *arc;
	const ChunksPackPackReader *reader;
	StdVec		current;
	bool		has;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&two->f0, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	chunks_packs_tests_numbers_eq(RUST_FILE, packs, io, reading, both, lengthof(both), NULL, 668, 13, 52);
	chunks_packs_tests_found(RUST_FILE, packs, io, reading, &one->f0, &one->f1, 669, 13, 55);
	if (!chunks_packs_backend_Packs_find_in(packs, io, reading, &two->f0, 0, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 670, 61);
	chunks_packs_backend_tests_answer_eq(has, &found, &two->f1, NULL, 670, 13);
	read = chunks_packs_backend_tests_read_bytes(packs, io, reading, &two->f0, two->f1, 672, 56);
	again = chunks_packs_backend_tests_read_bytes(mine, io, reading, &two->f0, two->f1, 673, 55);
	chunks_packs_backend_tests_bytes_eq(&read, &again, 671, 13);
	std_fmt_format_Checked(&shown, &arguments);
	std_str_index_range_to(shown.ptr, shown.len, 10, RUST_FILE, 675, 57);
	if (!chunks_packs_backend_Packs_with_prefix(packs, io, reading, shown.ptr, 10, &prefixed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 675, 80);
	std_String_drop(&shown);
	if (prefixed.len != 1 ||
		!chunks_address_Address_PartialEq_eq(&STD_VEC_AT(&prefixed, ChunksAddressAddress, 0), &two->f0))
	{
		StdVec		want = {unconstify(ChunksAddressAddress *, &two->f0), 1, 1};

		std_assert_failed(RUST_FILE, 675, 13, "==", &prefixed, chunks_packs_backend_tests_Addresses_Debug_fmt, &want,
						  chunks_packs_backend_tests_Addresses_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&prefixed, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (!chunks_packs_backend_Packs_pack_reader(packs, io, reading, 2, &has, &arc, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 676, 59);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(676, 68);
	reader = std_arc_Deref_deref(arc, _Alignof(ChunksPackPackReader));
	STD_ASSERT_EQ(676, 13, chunks_pack_PackId_PartialEq_eq(&reader->id, second_id), &reader->id,
				  chunks_pack_PackId_Debug_fmt, second_id, chunks_pack_PackId_Debug_fmt);
	chunks_pack_ArcPackReader_drop(arc);
	if (!chunks_packs_backend_Packs_forwarded(packs, io, reading, one->f1, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 677, 62);
	chunks_packs_backend_tests_answer_eq(has, &found, NULL, NULL, 677, 13);
	if (!chunks_packs_backend_Packs_current(packs, io, reading, locators, lengthof(locators), &has, &current, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 678, 71);
	if (has)
	{
		StdOptionRef got = {true, STD_ITEM(&current, chunks_packs_backend_tests_Locators_Debug_fmt)};
		StdOptionRef want = {false};

		std_assert_failed(RUST_FILE, 678, 13, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
	if (!chunks_packs_backend_Packs_advise(packs, io, reading, one->f1, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 679, 48);
}

/*
 * test_chunks_packs_backend_tests_two_backends_packs_over_one_record_list_what_either_adds:
 * two backends' packs over one record list what either adds, read each
 * other's chunks, find them by a prefix of their addresses, leave out a copy
 * the other marked dead, and merge their runs; a forward counts for nothing
 * while its pack is listed, and a checkpoint's whole write of the lists
 * changes nothing they list. A finished pack whose add was refused, left on
 * no list, is listed again by the next marking's start (the collector text's
 * §6).
 */
void
test_chunks_packs_backend_tests_two_backends_packs_over_one_record_list_what_either_adds(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 656, 46);
	uint64		location = chunks_packs_backend_tests_published(io);
	ChunksPacksBackendPacks mine;
	ChunksPacksBackendPacks theirs;
	ChunksPackPack first;
	ChunksPackPack second;
	ChunksPackPack stray;
	ChunksPackPack copy;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
	ChunksTrailerLocator at_copy;
	ChunksPackPackId second_id;
	ChunksPacksWritersWriting still;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRetireForwarded forwarded;
	ChunksPackPackWriterInDir unfinished;
	ChunksTrailerLocator found;
	ChunksErrorError error;
	StdFmtPiece counted_pieces[] = {STD_FMT_LIT("a forward counted while its pack was listed")};
	StdFmtArguments counted = {counted_pieces, lengthof(counted_pieces), NULL};
	static const uint64 all[] = {1, 2, 4, 5};
	uint64		offset;
	uint64		number;
	bool		has;
	bool		set;
	bool		ok;
	const ChunksPacksBackendPacks *each[2];

	chunks_packs_backend_tests_opened(io, location, &mine);
	chunks_packs_backend_tests_opened(io, location, &theirs);
	each[0] = &mine;
	each[1] = &theirs;
	chunks_packs_backend_tests_written(io, &mine, "one", &first, &one.f0, &one.f1);
	chunks_packs_backend_tests_written(io, &theirs, "two", &second, &two.f0, &two.f1);
	chunks_packs_backend_tests_triple_eq(first.number, second.number, chunks_packs_backend_Packs_reserved(&theirs), 1, 2,
										 2, 661);
	STD_ASSERT(662, 9, chunks_packs_backend_Packs_writing(&mine, io, 1) && chunks_packs_backend_Packs_writing(&mine, io, 2) &&
			   !chunks_packs_backend_Packs_writing(&mine, io, 3),
			   "mine.writing(io, 1) && mine.writing(io, 2) && !mine.writing(io, 3)");
	second_id = second.id;
	if (!chunks_packs_backend_Packs_add(&mine, io, &reading, &first, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 664, 39);
	if (!chunks_packs_backend_Packs_add_deferred(&theirs, HOST_IO_IO_TAKES_SHORT(io), &reading, &second, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 665, 51);
	STD_ASSERT(666, 9, !chunks_packs_backend_Packs_writing(&mine, io, 1) && !chunks_packs_backend_Packs_writing(&theirs, io, 2),
			   "!mine.writing(io, 1) && !theirs.writing(io, 2)");
	for (size_t k = 0; k < lengthof(each); k++)
		chunks_packs_backend_tests_listed_each(each[k], &mine, io, &reading, &one, &two, &second_id);
	chunks_packs_backend_tests_written(io, &mine, "stray", &stray, &address, &at);
	if (stray.has_owner)
	{
		stray.has_owner = false;
		chunks_packs_writers_Writing_drop(&stray.owner);
	}
	ok = chunks_packs_backend_Packs_add(&theirs, io, &reading, &stray, &error);
	STD_ASSERT_MSG(683, 9, chunks_packs_backend_tests_refused_corrupt(ok, &error),
				   "a pack whose number no writer holds was added");

	chunks_packs_backend_tests_written(io, &theirs, "one", &copy, &address, &at_copy);
	if (!chunks_packs_backend_Packs_add_still_writing(&theirs, io, &reading, &copy, &still, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 689, 66);
	STD_ASSERT(690, 9, chunks_packs_writers_Writing_number(&still) == 4 && chunks_packs_backend_Packs_writing(&theirs, io, 4),
			   "still.number() == 4 && theirs.writing(io, 4)");
	chunks_packs_writers_Writing_drop(&still);
	STD_ASSERT(692, 9, !chunks_packs_backend_Packs_writing(&theirs, io, 4), "!theirs.writing(io, 4)");
	chunks_packs_backend_tests_find_eq(&mine, io, &reading, &one.f0, &at_copy, 693, 9, 50);
	offset = chunks_trailer_Locator_offset(&at_copy);
	if (!chunks_packs_fixtures_dead(io, &theirs, 4, 1, &offset, 1, &set, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 694, 62);
	STD_ASSERT(694, 9, set, "dead(io, &theirs, 4, 1, &[at_copy.offset()]).unwrap()");
	chunks_packs_backend_tests_find_eq(&mine, io, &reading, &one.f0, &one.f1, 695, 9, 50);
	ok = chunks_packs_fixtures_dead(io, &mine, 9, 1, &offset, 0, &set, &error);
	STD_ASSERT(696, 9, chunks_packs_backend_tests_refused_corrupt(ok, &error),
			   "matches!(dead(io, &mine, 9, 1, &[]), Err(Error::Corrupt(_)))");
	moves[0].f0 = at_copy;
	moves[0].f1 = one.f1;
	if (!chunks_packs_backend_Packs_forward(&mine, io, &reading, moves, lengthof(moves), &forwarded, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 697, 63);
	chunks_packs_retire_Forwarded_drop(&forwarded);
	if (!chunks_packs_backend_Packs_forwarded(&theirs, io, &reading, at_copy, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 699, 53);
	chunks_packs_backend_tests_answer_eq(has, &found, NULL, &counted, 698, 9);
	unfinished = chunks_packs_tests_writer(RUST_FILE, &mine, io, &reading, 703, 55);
	number = chunks_pack_PackWriterInDir_number(&unfinished);
	if (!chunks_pack_PackWriterInDir_abandon(&unfinished, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 705, 32);
	STD_ASSERT(706, 9, !chunks_packs_backend_Packs_writing(&mine, io, number), "!mine.writing(io, number)");

	if (!chunks_packs_backend_Packs_merge_runs(&theirs, io, &reading, &ok, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 708, 49);
	STD_ASSERT(708, 9, ok, "theirs.merge_runs(io, &reading).unwrap()");
	if (!chunks_packs_backend_Packs_compact_lists(&mine, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 709, 42);
	for (size_t k = 0; k < lengthof(each); k++)
	{
		chunks_packs_tests_numbers_eq(RUST_FILE, each[k], io, &reading, all, lengthof(all), NULL, 711, 13, 52);
		chunks_packs_tests_found(RUST_FILE, each[k], io, &reading, &one.f0, &one.f1, 712, 13, 55);
		chunks_packs_tests_found(RUST_FILE, each[k], io, &reading, &two.f0, &two.f1, 713, 13, 55);
	}
	chunks_packs_backend_Packs_drop(&theirs);
	chunks_packs_backend_Packs_drop(&mine);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_runs counts the names of runs' files among a directory's names, which it drops. */
static size_t
chunks_packs_backend_tests_runs(StdVec *names)
{
	size_t		count = 0;

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(names, StdString, k);

		if (name->len >= 4 && memcmp((const char *) name->ptr + name->len - 4, ".idx", 4) == 0)
			count++;
	}
	chunks_packs_backend_tests_Names_drop(names);
	return count;
}

/*
 * test_chunks_packs_backend_tests_a_merge_with_no_room_for_its_replaced_runs_is_not_started:
 * a merge of runs with no room in the removal queue for the runs it would
 * replace isn't started and changes nothing; with room it merges, and the
 * replaced runs' files stay until every reader that began before the merge
 * has ended.
 */
void
test_chunks_packs_backend_tests_a_merge_with_no_room_for_its_replaced_runs_is_not_started(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 776, 46);
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(33, HOST_PG_DEFAULT);
	ChunksPacksRemovalReservation all;
	ChunksErrorError error;
	StdVec		before;
	StdVec		names;
	size_t		count;
	size_t		three = 3;
	size_t		one = 1;
	static const uint64 both[] = {1, 2};
	bool		merged;

	chunks_packs_fixtures_created(io, 33, &packs);
	(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	if (!chunks_packs_removal_reserve(io, SIZE_MAX, &all, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 783, 54);
	before = chunks_packs_backend_tests_names(io, &dir, 781, 73);
	if (!chunks_packs_backend_Packs_merge_runs(&packs, io, &reading, &merged, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 784, 49);
	STD_ASSERT(784, 9, !merged, "!packs.merge_runs(io, &reading).unwrap()");
	names = chunks_packs_backend_tests_names(io, &dir, 781, 73);
	chunks_packs_backend_tests_names_eq(&names, &before, NULL, 785);
	chunks_packs_removal_Reservation_drop(&all);
	if (!chunks_packs_backend_Packs_merge_runs(&packs, io, &reading, &merged, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 787, 48);
	STD_ASSERT(787, 9, merged, "packs.merge_runs(io, &reading).unwrap()");
	names = chunks_packs_backend_tests_names(io, &dir, 781, 73);
	count = chunks_packs_backend_tests_runs(&names);
	STD_ASSERT_EQ(788, 9, count == three, &count, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	host_shared_Reader_drop(&reading);
	if (!chunks_packs_removal_remove_due(io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 790, 31);
	names = chunks_packs_backend_tests_names(io, &dir, 781, 73);
	count = chunks_packs_backend_tests_runs(&names);
	STD_ASSERT_EQ(791, 9, count == one, &count, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	reading = chunks_packs_tests_reading(RUST_FILE, io, 792, 46);
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, both, lengthof(both), NULL, 793, 9, 48);
	host_shared_Reader_drop(&reading);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* CHUNKS_PACKS_BACKEND_TESTS_WINDOWS is cfg!(windows): whether the build is Windows'. */
#ifdef WIN32
#define CHUNKS_PACKS_BACKEND_TESTS_WINDOWS true
#else
#define CHUNKS_PACKS_BACKEND_TESTS_WINDOWS false
#endif

/* chunks_packs_backend_tests_held reports whether a refusal is the system's to take away a directory or file a process has open, which is Windows' rule. */
static bool
chunks_packs_backend_tests_held(const StdIoError *refused)
{
	int32		code;

	return CHUNKS_PACKS_BACKEND_TESTS_WINDOWS &&
		(std_io_Error_kind(refused) == STD_ERROR_KIND_PERMISSION_DENIED ||
		 (std_io_Error_raw_os_error(refused, &code) && code == 32));
}

/* chunks_packs_backend_tests_held_eq asserts at a line and column that a refusal is the one held, with the refusal shown as the message. */
static void
chunks_packs_backend_tests_held_eq(const StdIoError *refused, uint32 line, uint32 col)
{
	if (!chunks_packs_backend_tests_held(refused))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{refused, std_io_Error_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, col, &arguments);
	}
}

/* chunks_packs_backend_tests_renamed renames a file or directory, which unwraps at a line and column. */
static void
chunks_packs_backend_tests_renamed(const StdPathBuf *from, const StdPathBuf *to, uint32 line, uint32 col)
{
	StdIoError	io_error;

	if (!std_fs_rename(from->bytes.ptr, from->bytes.len, to->bytes.ptr, to->bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
}

/* chunks_packs_backend_tests_contains reports whether a string holds another. */
static bool
chunks_packs_backend_tests_contains(const char *whole, size_t whole_len, const char *part, size_t part_len)
{
	for (size_t k = 0; k + part_len <= whole_len; k++)
		if (memcmp(whole + k, part, part_len) == 0)
			return true;
	return false;
}

/* chunks_packs_backend_tests_Message_Debug_fmt is an Option<&str>'s Debug, of a host error's message. */
static bool
chunks_packs_backend_tests_Message_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *message = value;
	StdOptionRef option = {message->ptr != NULL, STD_ITEM(message, std_str_Debug_fmt)};

	return std_option_Debug_fmt(&option, f);
}

/*
 * test_chunks_packs_backend_tests_a_view_keeps_the_packs_of_a_missing_tablespace_unread_until_it_is_back:
 * a view keeps the packs the table lists in a tablespace whose directory is
 * missing unread, whether its runs file or, for a tablespace whose first
 * pack's add is deferred, the pack itself is the first file it fails to open
 * there: it lists them, finds and reads the chunks of other tablespaces,
 * answers nothing for a chunk only there, and raises Postgres' error for a
 * missing file, naming the pack's file in the directory, for a read into
 * one. A view that opened the pack before the directory went still reads it,
 * and once the directory is back the next read looks again and finds it. A
 * pack missing from a directory that is there is an error. Where the system
 * refuses to take away a directory, or a file, that a process has open,
 * which is Windows' rule, the case lets the packs that hold it go first and
 * asserts the rest.
 */
void
test_chunks_packs_backend_tests_a_view_keeps_the_packs_of_a_missing_tablespace_unread_until_it_is_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 809, 46);
	ChunksPacksBackendPacks before;
	ChunksPacksBackendPacks after;
	ChunksPacksBackendPacks lost;
	chunks_TupleAddressLocator_t zero;
	static const uint32 spaces[] = {5, 6};
	static const bool defers[] = {false, true};
	StdPathBuf	dirs[2];
	StdPathBuf	aways[2];
	ChunksAddressAddress addresses[2];
	ChunksTrailerLocator ats[2];
	StdVec		bodies[2];
	ChunksErrorError error;
	StdIoError	io_error;
	StdArcInner *arc;
	StdPathBuf	gone;
	StdVec		numbers;
	bool		has_before = true;
	bool		has_after = true;
	bool		has;
	bool		ok;
	static const uint64 three[] = {1, 2, 3};

	chunks_packs_fixtures_created(io, 34, &before);
	zero = chunks_packs_fixtures_added(io, &before, (const uint8 *) "zero", 4);
	for (size_t k = 0; k < lengthof(spaces); k++)
	{
		StdStr		parent;
		ChunksPackPackWriterInDir writer;
		uint8		body = (uint8) spaces[k];
		ChunksPackPack pack;

		dirs[k] = host_tablespace_dir(34, spaces[k]);
		if (!std_path_parent(dirs[k].bytes.ptr, dirs[k].bytes.len, &parent))
			STD_OPTION_UNWRAP_FAILED(815, 50);
		if (!std_fs_create_dir_all(parent.ptr, parent.len, &io_error))
			STD_RESULT_EXPECT_FAILED(815, 60, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		if (!chunks_packs_backend_Packs_writer(&before, io, &reading, spaces[k], &writer, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 816, 86);
		chunks_packs_fixtures_stored(&body, 1, &addresses[k], &bodies[k]);
		if (!chunks_pack_PackWriterInDir_write(&writer, io, addresses[k], bodies[k].ptr, bodies[k].len, false, &ats[k],
											   &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 817, 63);
		if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 818, 42);
		if (defers[k])
		{
			if (!chunks_packs_backend_Packs_add_deferred(&before, HOST_IO_IO_TAKES_SHORT(io), &reading, &pack, &error))
				chunks_packs_tests_unwrapped(RUST_FILE, &error, 820, 65);
		}
		else if (!chunks_packs_backend_Packs_add(&before, io, &reading, &pack, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 821, 57);
		chunks_packs_backend_tests_find_eq(&before, io, &reading, &addresses[k], &ats[k], 823, 13, 60);
		std_path_with_extension(dirs[k].bytes.ptr, dirs[k].bytes.len, "away", 4, &aways[k]);
	}

	for (size_t k = 0; k < lengthof(spaces); k++)
	{
		if (!std_fs_rename(dirs[k].bytes.ptr, dirs[k].bytes.len, aways[k].bytes.ptr, aways[k].bytes.len, &io_error))
		{
			chunks_packs_backend_tests_held_eq(&io_error, 834, 17);
			if (has_before)
				chunks_packs_backend_Packs_drop(&before);
			has_before = false;
			chunks_packs_backend_tests_renamed(&dirs[k], &aways[k], 836, 44);
		}
	}
	if (!chunks_packs_open_Packs_open(io, 34, HOST_PG_DEFAULT, &after, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 839, 95);
	chunks_packs_tests_numbers_eq(RUST_FILE, &after, io, &reading, three, lengthof(three), NULL, 840, 9, 48);
	chunks_packs_backend_tests_find_eq(&after, io, &reading, &zero.f0, &zero.f1, 841, 9, 52);
	for (size_t k = 0; k < lengthof(spaces); k++)
	{
		ChunksTrailerLocator found;
		ChunksTrailerLocator located;
		bool		has_found;
		bool		has_located;
		ChunksChunkChunk chunk;
		StdCowStr	lossy;
		StdString	shown;
		StdStr		message;
		bool		named;

		if (!chunks_packs_backend_Packs_find(&after, io, &reading, &addresses[k], &has_found, &found, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 844, 52);
		if (!chunks_packs_marking_Packs_locate(&after, io, &reading, &addresses[k], &has_located, &located, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 844, 98);
		if (has_found || has_located)
		{
			StdOptionRef got_found = {has_found, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
			StdOptionRef got_located = {has_located, STD_ITEM(&located, chunks_trailer_Locator_Debug_fmt)};
			StdOptionRef none = {false};
			StdTupleRef left = STD_TUPLE(2, STD_ITEM(&got_found, std_option_Debug_fmt),
										 STD_ITEM(&got_located, std_option_Debug_fmt));
			StdTupleRef right = STD_TUPLE(2, STD_ITEM(&none, std_option_Debug_fmt), STD_ITEM(&none, std_option_Debug_fmt));

			std_assert_failed(RUST_FILE, 843, 13, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
		}
		if (chunks_packs_backend_Packs_read(&after, io, &reading, &addresses[k], ats[k], &chunk, &error))
		{
			static const char expected[] = "a read into a missing tablespace raises";

			std_result_unwrap_failed(expected, sizeof(expected) - 1, &chunk, chunks_chunk_Chunk_Debug_fmt, RUST_FILE,
									 847, 65);
		}
		if (error.tag != CHUNKS_ERROR_ERROR_HOST)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&error, chunks_error_Error_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 848, 53, &arguments);
		}
		std_path_to_string_lossy(dirs[k].bytes.ptr, dirs[k].bytes.len, &lossy);
		std_CowStr_into_owned(&lossy, &shown);
		for (size_t n = 0; n < shown.len; n++)
			if (((char *) shown.ptr)[n] == '\\')
				((char *) shown.ptr)[n] = '/';
		message = host_Error_message(&error.u.host);
		named = message.ptr != NULL && chunks_packs_backend_tests_contains(message.ptr, message.len, shown.ptr, shown.len);
		if (!(memcmp(host_Error_sqlstate(&error.u.host), "58P01", 5) == 0 && named))
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&message, chunks_packs_backend_tests_Message_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 851, 13, &arguments);
		}
		std_String_drop(&shown);
		chunks_error_Error_drop(&error);
		if (has_before)
		{
			StdVec		read = chunks_packs_backend_tests_read_bytes(&before, io, &reading, &addresses[k], ats[k], 853,
																	 69);
			StdVec		body;

			std_slice_to_vec_Checked(&body, bodies[k].ptr, bodies[k].len, 1, 1);
			chunks_packs_backend_tests_bytes_eq(&read, &body, 853, 17);
		}
	}

	for (size_t k = 0; k < lengthof(spaces); k++)
		chunks_packs_backend_tests_renamed(&aways[k], &dirs[k], 857, 77);
	for (size_t k = 0; k < lengthof(spaces); k++)
	{
		StdVec		read = chunks_packs_backend_tests_read_bytes(&after, io, &reading, &addresses[k], ats[k], 859, 64);
		StdVec		body;

		std_slice_to_vec_Checked(&body, bodies[k].ptr, bodies[k].len, 1, 1);
		chunks_packs_backend_tests_bytes_eq(&read, &body, 859, 13);
		chunks_packs_backend_tests_find_eq(&after, io, &reading, &addresses[k], &ats[k], 860, 13, 58);
	}

	if (!chunks_packs_backend_Packs_pack_reader(&after, io, &reading, 2, &has, &arc, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 863, 55);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(863, 64);
	std_PathBuf_Clone_clone(&((const ChunksPackPackReader *) std_arc_Deref_deref(arc, _Alignof(ChunksPackPackReader)))->path,
							&gone);
	chunks_pack_ArcPackReader_drop(arc);
	if (!std_fs_remove_file(gone.bytes.ptr, gone.bytes.len, &io_error))
	{
		chunks_packs_backend_tests_held_eq(&io_error, 865, 13);
		chunks_packs_backend_Packs_drop(&after);
		has_after = false;
		if (!std_fs_remove_file(gone.bytes.ptr, gone.bytes.len, &io_error))
			STD_RESULT_EXPECT_FAILED(867, 41, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	if (!chunks_packs_open_Packs_open(io, 34, HOST_PG_DEFAULT, &lost, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 869, 94);
	ok = chunks_packs_backend_Packs_numbers(&lost, io, &reading, &numbers, &error);
	if (ok)
		std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	else
		chunks_error_Error_drop(&error);
	STD_ASSERT_MSG(870, 9, !ok, "a pack missing from a directory that is there was kept unread");
	chunks_packs_backend_Packs_drop(&lost);
	std_vec_free_Checked(&gone.bytes, 1, 1);
	if (has_after)
		chunks_packs_backend_Packs_drop(&after);
	for (size_t k = 0; k < lengthof(spaces); k++)
	{
		std_vec_free_Checked(&bodies[k], 1, 1);
		std_vec_free_Checked(&aways[k].bytes, 1, 1);
		std_vec_free_Checked(&dirs[k].bytes, 1, 1);
	}
	if (has_before)
		chunks_packs_backend_Packs_drop(&before);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_backend_tests_oom is the failed-allocation case's hook: a failed allocation panics, as pg's hook unwinds. */
static void
chunks_packs_backend_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* CHUNKS_PACKS_BACKEND_TESTS_OOM_HOOK is the failed-allocation case's hook, set while it fails a reallocation. */
static const StdAllocErrorHook CHUNKS_PACKS_BACKEND_TESTS_OOM_HOOK = {chunks_packs_backend_tests_oom};

/* ChunksPacksBackendTestsOver is the failed-allocation case's over: its record, location, packs, and answer. */
typedef struct ChunksPacksBackendTestsOver
{
	HostIoIo   *io;
	HostSharedHeld held;
	uint64		location;
	ChunksPacksBackendPacks packs;
	ChunksErrorError error;
	bool		ok;
} ChunksPacksBackendTestsOver;

/* chunks_packs_backend_tests_over is the failed-allocation case's call: packs over the record. */
static void
chunks_packs_backend_tests_over(void *captures)
{
	ChunksPacksBackendTestsOver *over = captures;

	over->ok = chunks_packs_backend_Packs_over(over->io, &over->held, over->location, &over->packs, &over->error);
}

/* chunks_packs_backend_tests_listed counts OPEN's entries at a location, taking OPEN's lock, and takes off the rest past a length. */
static size_t
chunks_packs_backend_tests_listed(HostIoIo *io, uint64 location, size_t keep)
{
	HostLocalLocal local = host_local_BackendLocal_get(&CHUNKS_PACKS_BACKEND_OPEN.local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	HostLockLockedRwLockWriteGuard guard;
	ChunksPacksBackendOpen *open = chunks_packs_backend_open_lock(&local, &io->held, &guard);
	size_t		found = 0;

	for (size_t k = 0; k < open->locations.len; k++)
		found += STD_VEC_AT(&open->locations, uint64, k) == location;
	open->locations.len = Min(open->locations.len, keep);
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	host_local_Local_drop(&local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	return found;
}

/*
 * test_chunks_packs_backend_tests_a_failed_allocation_in_a_packs_over_keeps_the_process_list:
 * with the process's list of open pack records at its room, packs over a
 * record whose list's growth fails answer out of memory and leave every
 * earlier entry for the next take of the list's lock, which the exit reads to
 * append deferred adds (bugs row 517). Before, the growth unwound under the
 * lock, which poisoned it, and the next take rebuilt an empty list.
 */
void
test_chunks_packs_backend_tests_a_failed_allocation_in_a_packs_over_keeps_the_process_list(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendTestsOver over = {.io = io};
	HostLocalLocal local;
	HostLockLockedRwLockWriteGuard guard;
	ChunksPacksBackendOpen *open;
	size_t		listed;
	uint64		placeholder = PG_UINT64_MAX;
	HostError	host;
	StdCaught	caught;
	bool		has;
	bool		returned;

	chunks_packs_fixtures_created(io, 41, &packs);
	over.location = chunks_packs_backend_Packs_location(&packs);
	local = host_local_BackendLocal_get(&CHUNKS_PACKS_BACKEND_OPEN.local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	open = chunks_packs_backend_open_lock(&local, &io->held, &guard);
	listed = open->locations.len;
	while (open->locations.len < open->locations.cap)
		std_vec_push_Checked(&open->locations, &placeholder, sizeof(uint64), _Alignof(uint64));
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	host_local_Local_drop(&local, &CHUNKS_PACKS_BACKEND_OPEN_TYPE);
	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, over.location, &has,
						  &over.held, &host) || !has)
		std_panic(__FILE__, __LINE__, 0, "the record didn't open", sizeof("the record didn't open") - 1);
	std_alloc_set_alloc_error_hook(&CHUNKS_PACKS_BACKEND_TESTS_OOM_HOOK);
	test_std_alloc_tests_fail_reallocation(1);
	returned = std_catch_unwind(chunks_packs_backend_tests_over, &over, &caught);
	test_std_alloc_tests_fail_reallocation(0);
	std_alloc_set_alloc_error_hook(NULL);
	if (!returned)
		std_Caught_drop(&caught);
	STD_ASSERT(__LINE__, 5, returned && !over.ok, "packs over a record whose list's growth fails answer out of memory");
	chunks_error_Error_drop(&over.error);
	STD_ASSERT(__LINE__, 5, chunks_packs_backend_tests_listed(io, over.location, listed) == 1,
			   "the first packs' entry outlived a failed growth of the list");
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
