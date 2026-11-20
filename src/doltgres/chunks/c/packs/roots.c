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
 * The C port of chunks' packs/roots.rs: the roots a process registers with
 * its repositories' packs as unpublished, SLOTS slots of seven words each in
 * the process's own words, lent to one entry at a time.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/record.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "roots.rs"

/* CHUNKS_PACKS_ROOTS_SLOT is SLOT: the number of a root slot's words. */
#define CHUNKS_PACKS_ROOTS_SLOT 7

/* A slot's words: the repository, the root's address from ADDRESS on, its offset below its length, and its pack. */
#define CHUNKS_PACKS_ROOTS_REPOSITORY 0
#define CHUNKS_PACKS_ROOTS_ADDRESS 1
#define CHUNKS_PACKS_ROOTS_OFFSET 5
#define CHUNKS_PACKS_ROOTS_PACK 6

/* CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS is Roots' ProcSlots: each process's slots for its registered roots. */
const HostSharedProcSlotsOps CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS = {
	.id = UINT64CONST(0x6368756e6b730004), .words = CHUNKS_PACKS_ROOTS_SLOTS * CHUNKS_PACKS_ROOTS_SLOT,
};

/* chunks_packs_roots_place returns the place of one of a slot's words: a slot's words follow those before it. */
static HostSharedAt
chunks_packs_roots_place(size_t slot, size_t word)
{
	HostSharedAt at = {slot * CHUNKS_PACKS_ROOTS_SLOT + word};

	return at;
}

/*
 * ChunksPacksRootsHome is Home: the process's root slots while no entry has
 * them, whether one does, and which slots a dropped registration left for
 * the next registration there to settle.
 */
typedef struct ChunksPacksRootsHome
{
	bool		lent;
	bool		dropped[CHUNKS_PACKS_ROOTS_SLOTS];
} ChunksPacksRootsHome;

/* chunks_packs_roots_Home_Default_default is Home's Default: not lent, and no slot dropped. */
static void
chunks_packs_roots_Home_Default_default(ChunksPacksRootsHome *result)
{
	memset(result, 0, sizeof(*result));
}

/* taken at Home and IoMutex<Home>, which roots.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Home
#define HOST_LOCK_TAKEN_T ChunksPacksRootsHome
#define HOST_LOCK_TAKEN_DEFAULT(place) chunks_packs_roots_Home_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) ((void) (place))
#define HOST_LOCK_TAKEN_TYPE CHUNKS_PACKS_ROOTS_HOME_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Home
#define HOST_LOCK_T ChunksPacksRootsHome
#define HOST_LOCK_DEFAULT(place) chunks_packs_roots_Home_Default_default(place)
#include "doltgres/host/lock_template.h"

/* chunks_packs_roots_IoMutexHome_drop is IoMutex<Home>'s drop glue, which holds nothing to drop. */
static void
chunks_packs_roots_IoMutexHome_drop(void *value)
{
	(void) value;
}

/* CHUNKS_PACKS_ROOTS_HOME_TYPE is IoMutex<Home>'s size, Default, and drop. */
static const StdDefaultType CHUNKS_PACKS_ROOTS_HOME_TYPE = {sizeof(HostLockIoMutexHome),
_Alignof(HostLockIoMutexHome), host_lock_IoMutexHome_Default_default, chunks_packs_roots_IoMutexHome_drop};

/* CHUNKS_PACKS_ROOTS_HOME is HOME: its BackendLocal header and the process's value, which follows it. */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexHome process;
}			CHUNKS_PACKS_ROOTS_HOME = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* chunks_packs_roots_home_lock takes HOME's lock through a local of it and returns the home it guards. */
static ChunksPacksRootsHome *
chunks_packs_roots_home_lock(HostLocalLocal *home, HostIoHeld *held, HostLockLockedRwLockWriteGuard *guard)
{
	*guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(home,
																					 &CHUNKS_PACKS_ROOTS_HOME_TYPE),
									&CHUNKS_PACKS_ROOTS_HOME_LOCK_TYPE, held);
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(guard);
}

/* chunks_packs_roots_invariant returns Error::Invariant with a message. */
static ChunksErrorError
chunks_packs_roots_invariant(const char *message)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_INVARIANT};

	std_string_from_Checked(&error.u.invariant, message, strlen(message));
	return error;
}

/*
 * chunks_packs_roots_RootSlots_register_frame_0_t is register's state
 * across its pad: HOME's local and guard while it reads the home, run's
 * captures until run is done with them, and the slots once made.
 */
typedef struct chunks_packs_roots_RootSlots_register_frame_0_t
{
	HostIoIo   *io;
	bool		(*run) (void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
						void *result, ChunksErrorError *error);
	void	   *run_captures;
	void		(*run_drop) (void *);
	void	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostLocalLocal home;
	bool		home_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	bool		run_live;
	ChunksPacksRootsRootSlots slots;
	bool		slots_live;
} chunks_packs_roots_RootSlots_register_frame_0_t;

/* chunks_packs_roots_RootSlots_register_pad_0 is register's pad: the guard, the local, run's captures, then the slots. */
static void
chunks_packs_roots_RootSlots_register_pad_0(void *frame_)
{
	chunks_packs_roots_RootSlots_register_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->home_live)
	{
		frame->home_live = false;
		host_local_Local_drop(&frame->home, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
	}
	if (frame->run_live)
	{
		frame->run_live = false;
		if (frame->run_drop != NULL)
			frame->run_drop(frame->run_captures);
	}
	if (frame->slots_live)
	{
		frame->slots_live = false;
		chunks_packs_roots_RootSlots_drop(&frame->slots);
	}
}

/*
 * chunks_packs_roots_RootSlots_register_body_0 is register's body: the
 * slots taken from the home, refused when an entry has them unless the
 * process is exiting, then lent to run.
 */
static void
chunks_packs_roots_RootSlots_register_body_0(void *frame_)
{
	chunks_packs_roots_RootSlots_register_frame_0_t *frame = frame_;
	ChunksPacksRootsHome *home;
	bool		dropped[CHUNKS_PACKS_ROOTS_SLOTS];
	bool		lent;
	HostIoRegistering registering;

	frame->ok = false;
	frame->home = host_local_BackendLocal_get(&CHUNKS_PACKS_ROOTS_HOME.local, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
	frame->home_live = true;
	home = chunks_packs_roots_home_lock(&frame->home, &frame->io->held, &frame->guard);
	frame->guard_live = true;
	lent = home->lent;
	home->lent = true;
	if (!lent)
		memcpy(dropped, home->dropped, sizeof(dropped));
	else if (host_exiting())
	{
		for (size_t k = 0; k < CHUNKS_PACKS_ROOTS_SLOTS; k++)
			dropped[k] = true;
	}
	else
	{
		*frame->error = chunks_packs_roots_invariant("a process's root slots are taken while an entry has them");
		chunks_packs_roots_RootSlots_register_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < CHUNKS_PACKS_ROOTS_SLOTS; k++)
	{
		frame->slots.slots[k].index = k;
		frame->slots.slots[k].dropped = dropped[k];
	}
	frame->slots_live = true;
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	frame->home_live = false;
	host_local_Local_drop(&frame->home, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
	registering = host_io_Registering_new(frame->io);
	frame->ok = frame->run(frame->run_captures, &registering, &frame->slots, frame->result, frame->error);
	chunks_packs_roots_RootSlots_register_pad_0(frame);
}

/*
 * chunks_packs_roots_RootSlots_register lends the process's slots, with a
 * view of the entry's Io, to run for as long as its registrations last,
 * refusing slots an entry already has unless the process is exiting, when
 * it takes them over each marked for its next registration to settle. It
 * drops run's captures once, whether or not it calls run.
 */
bool
chunks_packs_roots_RootSlots_register(HostIoIo *io,
									  bool (*run) (void *captures, HostIoRegistering *registering,
												   ChunksPacksRootsRootSlots *slots, void *result,
												   ChunksErrorError *error),
									  void *run_captures, void (*run_drop) (void *), void *result,
									  ChunksErrorError *error)
{
	chunks_packs_roots_RootSlots_register_frame_0_t frame = {.io = io, .run = run, .run_captures = run_captures,
	.run_drop = run_drop, .result = result, .error = error, .run_live = true};

	std_landing(&frame, chunks_packs_roots_RootSlots_register_body_0, chunks_packs_roots_RootSlots_register_pad_0);
	return frame.ok;
}

/* chunks_packs_roots_RootSlots_one lends a slot, for an entry that registers one root at a time. */
void
chunks_packs_roots_RootSlots_one(ChunksPacksRootsRootSlots *self, ChunksPacksRootsRootSlot **result)
{
	*result = &self->slots[0];
}

/* chunks_packs_roots_RootSlots_fold lends the four slots, for a fold, which registers its base's four new trees. */
void
chunks_packs_roots_RootSlots_fold(ChunksPacksRootsRootSlots *self, ChunksPacksRootsFold *result)
{
	StaticAssertStmt(CHUNKS_PACKS_ROOTS_SLOTS == 4, "fold names each of the four slots");
	result->s0 = &self->slots[0];
	result->s1 = &self->slots[1];
	result->s2 = &self->slots[2];
	result->s3 = &self->slots[3];
}

/*
 * chunks_packs_roots_RootSlot_unused ends a lent slot's borrow without a pin,
 * as the Rust's borrow of a slot run doesn't pin ends with run, which only
 * the analysis sees.
 */
void
chunks_packs_roots_RootSlot_unused(ChunksPacksRootsRootSlot *slot)
{
	(void) slot;
}

/* chunks_packs_roots_RootSlots_drop gives the slots back to the home, with which of them a dropped registration left. */
void
chunks_packs_roots_RootSlots_drop(ChunksPacksRootsRootSlots *self)
{
	HostLocalLocal local;
	HostIoInDrop in_drop;
	StdRwLockWriteGuard guard;

	if (!host_local_BackendLocal_existing(&CHUNKS_PACKS_ROOTS_HOME.local, &local))
		return;
	in_drop = host_io_InDrop_new();
	if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&local,
																						&CHUNKS_PACKS_ROOTS_HOME_TYPE),
									   &in_drop, &guard))
	{
		ChunksPacksRootsHome *home = guard.data;

		home->lent = false;
		for (size_t k = 0; k < CHUNKS_PACKS_ROOTS_SLOTS; k++)
			home->dropped[k] = self->slots[k].dropped;
		std_RwLockWriteGuard_drop(&guard);
	}
	host_local_Local_drop(&local, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
}

/* chunks_packs_roots_released is RootReleased's event: the slot whose root was released, by its index. */
static void
chunks_packs_roots_released(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_ROOT_RELEASED;
	result->u.root_released.slot = *(const size_t *) captures;
}

/* chunks_packs_roots_registered_t is RootRegistered's captures: the slot's index and the root registered in it. */
typedef struct chunks_packs_roots_registered_t
{
	size_t		slot;
	ChunksTrailerLocator root;
} chunks_packs_roots_registered_t;

/* chunks_packs_roots_registered_event is RootRegistered's event: the slot and the root registered in it. */
static void
chunks_packs_roots_registered_event(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_roots_registered_t *registered = captures;

	result->tag = CHUNKS_TRACE_EVENT_ROOT_REGISTERED;
	result->u.root_registered.slot = registered->slot;
	result->u.root_registered.root = registered->root;
}

/*
 * chunks_packs_roots_Packs_pin_frame_0_t is pin's state across its pad: the
 * entries lock's guard while it holds it.
 */
typedef struct chunks_packs_roots_Packs_pin_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoTakesShort io;
	ChunksPacksRootsRootSlot *slot;
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	ChunksPacksRootsPin *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard entries;
	bool		entries_live;
} chunks_packs_roots_Packs_pin_frame_0_t;

/* chunks_packs_roots_Packs_pin_pad_0 is pin's pad: the entries lock's guard. */
static void
chunks_packs_roots_Packs_pin_pad_0(void *frame_)
{
	chunks_packs_roots_Packs_pin_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		host_shared_Guard_drop(&frame->entries);
	}
}

/* chunks_packs_roots_Packs_pin_body_0 is pin's body: the slot settled, then the root's words, then the repository's. */
static void
chunks_packs_roots_Packs_pin_body_0(void *frame_)
{
	chunks_packs_roots_Packs_pin_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(&frame->self->held);
	HostSharedMine roots;
	HostError	host;
	size_t		index = frame->slot->index;
	uint64		before;
	chunks_packs_roots_registered_t registered;

	frame->ok = false;
	if (!host_shared_mine(frame->io, &CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS, &roots, &host) ||
		!host_shared_Record_lock(record, frame->io, CHUNKS_PACKS_SHARED_ENTRIES_LOCK_INDEX, &frame->entries, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	frame->entries_live = true;
	before = host_shared_Mine_load_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_REPOSITORY));
	host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_REPOSITORY), 0);
	if (before != 0)
		chunks_trace_emit(before, chunks_packs_roots_released, &index, NULL);
	for (size_t word = 0; word < 4; word++)
		host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_ADDRESS + word),
								  std_u64_from_le_bytes(&frame->address.f0[word * 8]));
	host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_OFFSET),
							  chunks_trailer_Locator_offset(&frame->locator) |
							  ((uint64) chunks_trailer_Locator_length(&frame->locator)) << 40);
	host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_PACK), frame->locator.pack);
	host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_REPOSITORY),
							  frame->self->repository);
	registered.slot = index;
	registered.root = frame->locator;
	chunks_trace_emit(frame->self->repository, chunks_packs_roots_registered_event, &registered, NULL);
	frame->slot->dropped = false;
	frame->result->slot = frame->slot;
	frame->ok = true;
	chunks_packs_roots_Packs_pin_pad_0(frame);
}

/*
 * chunks_packs_roots_Packs_pin registers a root as unpublished in one of the
 * process's slots, under the repository's entries lock, so that a collection
 * of the repository keeps the root's chunks before anything durable names
 * it, settling the slot first when a dropped registration left it.
 */
bool
chunks_packs_roots_Packs_pin(const ChunksPacksBackendPacks *self, HostIoTakesShort io, ChunksPacksRootsRootSlot *slot,
							 ChunksAddressAddress address, ChunksTrailerLocator locator, ChunksPacksRootsPin *result,
							 ChunksErrorError *error)
{
	chunks_packs_roots_Packs_pin_frame_0_t frame = {.self = self, .io = io, .slot = slot, .address = address,
	.locator = locator, .result = result, .error = error};

	std_landing(&frame, chunks_packs_roots_Packs_pin_body_0, chunks_packs_roots_Packs_pin_pad_0);
	return frame.ok;
}

/* chunks_packs_roots_Pin_drop marks its slot dropped in the process's own memory, a store that can't fail. */
void
chunks_packs_roots_Pin_drop(ChunksPacksRootsPin *self)
{
	self->slot->dropped = true;
}

/*
 * chunks_packs_roots_Packs_registered_frame_0_t is registered's state across
 * its pad: the entries lock's guard, the processes, and the roots found.
 */
typedef struct chunks_packs_roots_Packs_registered_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard entries;
	bool		entries_live;
	StdVec		processes;
	bool		processes_live;
	StdVec		roots;
	bool		roots_live;
} chunks_packs_roots_Packs_registered_frame_0_t;

/* chunks_packs_roots_Packs_registered_pad_0 is registered's pad: the roots, the processes, then the guard. */
static void
chunks_packs_roots_Packs_registered_pad_0(void *frame_)
{
	chunks_packs_roots_Packs_registered_frame_0_t *frame = frame_;

	if (frame->roots_live)
	{
		frame->roots_live = false;
		std_vec_free_Checked(&frame->roots, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->processes_live)
	{
		frame->processes_live = false;
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		host_shared_Guard_drop(&frame->entries);
	}
}

/*
 * chunks_packs_roots_Packs_registered_body_0 is registered's body: each
 * process's slots that name the repository, a slot's repository word read
 * before and after the root's words.
 */
static void
chunks_packs_roots_Packs_registered_body_0(void *frame_)
{
	chunks_packs_roots_Packs_registered_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(&frame->self->held);
	const HostSharedProcSlotsOps *ops = &CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS;
	HostError	host;

	frame->ok = false;
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io),
										CHUNKS_PACKS_SHARED_ENTRIES_LOCK_INDEX, &frame->entries, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	frame->entries_live = true;
	std_vec_new(&frame->roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->roots_live = true;
	if (!host_shared_processes(ops, &frame->processes, &host))
		goto failed;
	frame->processes_live = true;
	for (size_t p = 0, p_len = frame->processes.len; p < p_len; p++)
	{
		uint32		process = STD_VEC_AT(&frame->processes, uint32, p);

		for (size_t slot = 0; slot < CHUNKS_PACKS_ROOTS_SLOTS; slot++)
		{
			uint64		named;
			uint64		again;
			uint64		at;
			uint64		pack;
			chunks_TupleAddressLocator_t root;

			if (!host_shared_load_at(ops, process, chunks_packs_roots_place(slot, CHUNKS_PACKS_ROOTS_REPOSITORY),
									 &named, &host))
				goto failed;
			if (named != frame->self->repository)
				continue;
			if (!host_pause(HOST_SHARED_GUARD_PAUSES(&frame->entries), "doltgres-chunks-roots-named", &host))
				goto failed;
			for (size_t word = 0; word < 4; word++)
			{
				uint64		value;

				if (!host_shared_load_at(ops, process,
										 chunks_packs_roots_place(slot, CHUNKS_PACKS_ROOTS_ADDRESS + word), &value,
										 &host))
					goto failed;
				std_u64_to_le_bytes(value, &root.f0.f0[word * 8]);
			}
			if (!host_shared_load_at(ops, process, chunks_packs_roots_place(slot, CHUNKS_PACKS_ROOTS_OFFSET), &at,
									 &host) ||
				!host_shared_load_at(ops, process, chunks_packs_roots_place(slot, CHUNKS_PACKS_ROOTS_PACK), &pack,
									 &host) ||
				!host_shared_load_at(ops, process, chunks_packs_roots_place(slot, CHUNKS_PACKS_ROOTS_REPOSITORY),
									 &again, &host))
				goto failed;
			if (again == named)
			{
				root.f1 = chunks_trailer_Locator_at(pack, at & ((UINT64CONST(1) << 40) - 1), (uint32) (at >> 40));
				std_vec_push_Checked(&frame->roots, &root, sizeof(root), _Alignof(chunks_TupleAddressLocator_t));
			}
		}
	}
	*frame->result = frame->roots;
	frame->roots_live = false;
	frame->ok = true;
	chunks_packs_roots_Packs_registered_pad_0(frame);
	return;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
	chunks_packs_roots_Packs_registered_pad_0(frame);
}

/*
 * chunks_packs_roots_Packs_registered returns the roots processes have
 * registered with the repository's packs as unpublished, which a collection
 * keeps, read under the repository's entries lock, shared, passing over a
 * slot whose repository word changed while its root's words were read.
 */
bool
chunks_packs_roots_Packs_registered(const ChunksPacksBackendPacks *self, HostIoIo *io, StdVec *result,
									ChunksErrorError *error)
{
	chunks_packs_roots_Packs_registered_frame_0_t frame = {.self = self, .io = io, .result = result, .error = error};

	std_landing(&frame, chunks_packs_roots_Packs_registered_body_0, chunks_packs_roots_Packs_registered_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_roots_Packs_settle_roots_frame_0_t is settle_roots' state
 * across its pad: HOME's local and guard, and the entries lock's guard.
 */
typedef struct chunks_packs_roots_Packs_settle_roots_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	HostLocalLocal home;
	bool		home_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	HostSharedGuard entries;
	bool		entries_live;
} chunks_packs_roots_Packs_settle_roots_frame_0_t;

/* chunks_packs_roots_Packs_settle_roots_pad_0 is settle_roots' pad: the guards, then the local. */
static void
chunks_packs_roots_Packs_settle_roots_pad_0(void *frame_)
{
	chunks_packs_roots_Packs_settle_roots_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		host_shared_Guard_drop(&frame->entries);
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->home_live)
	{
		frame->home_live = false;
		host_local_Local_drop(&frame->home, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
	}
}

/*
 * chunks_packs_roots_Packs_settle_roots_body_0 is settle_roots' body: the
 * dropped slots' repository words cleared under the entries lock, then the
 * home's marks.
 */
static void
chunks_packs_roots_Packs_settle_roots_body_0(void *frame_)
{
	chunks_packs_roots_Packs_settle_roots_frame_0_t *frame = frame_;
	ChunksPacksRootsHome *home;
	bool		dropped[CHUNKS_PACKS_ROOTS_SLOTS] = {0};
	bool		any = false;
	HostSharedRecord record;
	HostSharedMine roots;
	HostError	host;

	frame->ok = false;
	frame->home = host_local_BackendLocal_get(&CHUNKS_PACKS_ROOTS_HOME.local, &CHUNKS_PACKS_ROOTS_HOME_TYPE);
	frame->home_live = true;
	home = chunks_packs_roots_home_lock(&frame->home, &frame->io->held, &frame->guard);
	frame->guard_live = true;
	if (!home->lent)
		memcpy(dropped, home->dropped, sizeof(dropped));
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	for (size_t k = 0; k < CHUNKS_PACKS_ROOTS_SLOTS; k++)
		any = any || dropped[k];
	if (!any)
	{
		frame->ok = true;
		chunks_packs_roots_Packs_settle_roots_pad_0(frame);
		return;
	}
	record = host_shared_Held_record(&frame->self->held);
	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(frame->io), &CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS, &roots, &host) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SHARED_ENTRIES_LOCK_INDEX,
								 &frame->entries, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		chunks_packs_roots_Packs_settle_roots_pad_0(frame);
		return;
	}
	frame->entries_live = true;
	for (size_t index = 0; index < CHUNKS_PACKS_ROOTS_SLOTS; index++)
	{
		uint64		before;

		if (!dropped[index])
			continue;
		before = host_shared_Mine_load_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_REPOSITORY));
		host_shared_Mine_store_at(&roots, chunks_packs_roots_place(index, CHUNKS_PACKS_ROOTS_REPOSITORY), 0);
		if (before != 0)
			chunks_trace_emit(before, chunks_packs_roots_released, &index, NULL);
	}
	frame->entries_live = false;
	host_shared_Guard_drop(&frame->entries);
	home = chunks_packs_roots_home_lock(&frame->home, &frame->io->held, &frame->guard);
	frame->guard_live = true;
	memset(home->dropped, 0, sizeof(home->dropped));
	frame->ok = true;
	chunks_packs_roots_Packs_settle_roots_pad_0(frame);
}

/*
 * chunks_packs_roots_Packs_settle_roots clears the process's slots that a
 * dropped registration left, once the entry that had the slots gave them
 * back, under this repository's entries lock, so a collection stops keeping
 * a root that something durable names.
 */
bool
chunks_packs_roots_Packs_settle_roots(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_roots_Packs_settle_roots_frame_0_t frame = {.self = self, .io = io, .error = error};

	std_landing(&frame, chunks_packs_roots_Packs_settle_roots_body_0, chunks_packs_roots_Packs_settle_roots_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_PACKS_ROOTS_TESTS_TAKES_SHORT is a registration's short doors, which a pin takes. */
#define CHUNKS_PACKS_ROOTS_TESTS_TAKES_SHORT(registering) \
	((HostIoTakesShort) {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, (registering)})

/* chunks_packs_roots_tests_root returns a root's address and place. */
static chunks_TupleAddressLocator_t
chunks_packs_roots_tests_root(uint64 pack)
{
	chunks_TupleAddressLocator_t root;
	uint8		le[8];

	std_u64_to_le_bytes(pack, le);
	root.f0 = chunks_address_Address_of(le, sizeof(le));
	root.f1 = chunks_trailer_Locator_at((UINT64CONST(1) << 47) + pack, (UINT64CONST(1) << 40) - 1 - pack,
										(uint32) ((UINT64CONST(1) << 24) - 1));
	return root;
}

/* chunks_packs_roots_tests_Roots_Debug_fmt is a Vec<(Address, Locator)>'s Debug, or an array's, which reads alike. */
static bool
chunks_packs_roots_tests_Roots_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *roots = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = roots->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(roots, chunks_TupleAddressLocator_t, k),
							chunks_record_tests_TupleAddressLocator_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_roots_tests_read_failed returns the host error of a collection that read other roots than it should have. */
static HostError
chunks_packs_roots_tests_read_failed(const StdVec *read)
{
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("a collection read "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{read, chunks_packs_roots_tests_Roots_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* chunks_packs_roots_tests_same reports whether roots read are the roots given, in order. */
static bool
chunks_packs_roots_tests_same(const StdVec *read, const chunks_TupleAddressLocator_t *want, size_t want_len)
{
	if (read->len != want_len)
		return false;
	for (size_t k = 0; k < want_len; k++)
	{
		const chunks_TupleAddressLocator_t *got = &STD_VEC_AT(read, chunks_TupleAddressLocator_t, k);

		if (!chunks_address_Address_PartialEq_eq(&got->f0, &want[k].f0) ||
			!chunks_trailer_Locator_PartialEq_eq(&got->f1, &want[k].f1))
			return false;
	}
	return true;
}

/* chunks_packs_roots_tests_roots_eq asserts at a line and column that roots read are the roots given, with a literal message or none. */
static void
chunks_packs_roots_tests_roots_eq(const StdVec *read, const chunks_TupleAddressLocator_t *want, size_t want_len,
								  const char *message, uint32 line, uint32 col)
{
	if (!chunks_packs_roots_tests_same(read, want, want_len))
	{
		StdVec		wanted;
		StdFmtPiece pieces[] = {{message, message == NULL ? 0 : strlen(message)}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

		std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(*want), _Alignof(chunks_TupleAddressLocator_t));
		std_assert_failed(RUST_FILE, line, col, "==", read, chunks_packs_roots_tests_Roots_Debug_fmt, &wanted,
						  chunks_packs_roots_tests_Roots_Debug_fmt, message == NULL ? NULL : &arguments);
	}
}

/*
 * chunks_packs_roots_tests_registered asserts at a line and columns that a
 * repository's packs read the roots given, which unwraps at a column.
 */
static void
chunks_packs_roots_tests_registered(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									const chunks_TupleAddressLocator_t *want, size_t want_len, const char *message,
									uint32 line, uint32 assert_col, uint32 unwrap_col)
{
	StdVec		read;
	ChunksErrorError error;

	if (!chunks_packs_roots_Packs_registered(packs, io, &read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	chunks_packs_roots_tests_roots_eq(&read, want, want_len, message, line, assert_col);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* chunks_packs_roots_tests_no_numbers asserts at a line that a repository's packs have no number open, which unwraps at a column. */
static void
chunks_packs_roots_tests_no_numbers(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									const HostSharedReader *reading, uint32 line, uint32 col)
{
	StdVec		numbers;
	StdVec		none;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_numbers(packs, io, reading, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
	if (numbers.len != 0)
		std_assert_failed(RUST_FILE, line, 9, "==", &numbers, chunks_packs_tests_Numbers_Debug_fmt, &none,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_roots_tests_registers runs a registration whose result is (), which unwraps at a line and column. */
static void
chunks_packs_roots_tests_registers(HostIoIo *io,
								   bool (*run) (void *captures, HostIoRegistering *registering,
												ChunksPacksRootsRootSlots *slots, void *result,
												ChunksErrorError *error),
								   void *captures, uint32 line, uint32 col)
{
	ChunksErrorError error;
	uint8		unit;

	if (!chunks_packs_roots_RootSlots_register(io, run, captures, NULL, &unit, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_roots_tests_nothing is a registration that does nothing. */
static bool
chunks_packs_roots_tests_nothing(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								 void *result, ChunksErrorError *error)
{
	(void) captures;
	(void) registering;
	(void) slots;
	(void) result;
	(void) error;
	return true;
}

/* chunks_packs_roots_tests_lent is root_slots_are_lent_to_one_entry_at_a_time's first registration. */
static bool
chunks_packs_roots_tests_lent(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
							  void *result, ChunksErrorError *error)
{
	HostIoIo	testing = host_io_Io_testing();
	ChunksErrorError entered;
	uint8		unit;
	bool		refused;
	ChunksPacksRootsFold fold;
	ChunksPacksRootsPin pin;

	(void) captures;
	(void) registering;
	(void) result;
	(void) error;
	refused = !chunks_packs_roots_RootSlots_register(&testing, chunks_packs_roots_tests_nothing, NULL, NULL, &unit,
													 &entered) && entered.tag == CHUNKS_ERROR_ERROR_INVARIANT;
	if (!refused)
		STD_PANIC(275, 17, "assertion failed: matches!(entered, Err(Error::Invariant(_)))");
	chunks_error_Error_drop(&entered);
	chunks_packs_roots_RootSlots_fold(slots, &fold);
	pin.slot = fold.s0;
	chunks_packs_roots_Pin_drop(&pin);
	if (fold.s1->dropped)
		STD_PANIC(278, 17, "assertion failed: !second.dropped");
	chunks_packs_roots_RootSlot_unused(pin.slot);
	chunks_packs_roots_RootSlot_unused(fold.s0);
	chunks_packs_roots_RootSlot_unused(fold.s1);
	chunks_packs_roots_RootSlot_unused(fold.s2);
	chunks_packs_roots_RootSlot_unused(fold.s3);
	return true;
}

/* chunks_packs_roots_tests_again is root_slots_are_lent_to_one_entry_at_a_time's second registration. */
static bool
chunks_packs_roots_tests_again(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
							   void *result, ChunksErrorError *error)
{
	ChunksPacksRootsRootSlot *one;
	ChunksPacksRootsFold fold;
	bool		marked;

	(void) captures;
	(void) registering;
	(void) result;
	(void) error;
	chunks_packs_roots_RootSlots_one(slots, &one);
	chunks_packs_roots_RootSlots_fold(slots, &fold);
	marked = one->dropped && !fold.s1->dropped;
	chunks_packs_roots_RootSlot_unused(one);
	chunks_packs_roots_RootSlot_unused(fold.s0);
	chunks_packs_roots_RootSlot_unused(fold.s1);
	chunks_packs_roots_RootSlot_unused(fold.s2);
	chunks_packs_roots_RootSlot_unused(fold.s3);
	if (!marked)
		STD_PANIC(283, 17, "assertion failed: again.one().dropped && !again.fold()[1].dropped");
	return true;
}

/* chunks_packs_roots_tests_lent_twice is root_slots_are_lent_to_one_entry_at_a_time's actor: both registrations. */
static void
chunks_packs_roots_tests_lent_twice(void *captures)
{
	HostIoIo   *io = captures;

	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_lent, NULL, 281, 14);
	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_again, NULL, 286, 14);
}

/*
 * test_chunks_packs_roots_tests_root_slots_are_lent_to_one_entry_at_a_time: a
 * process's root slots are lent to one entry at a time: a registration
 * entered while they are lent, by a process that isn't exiting, is refused,
 * and the slots are back once the first registration's scope ends, with the
 * mark a dropped registration left on its slot. It runs as a backend of its
 * own, whose slots no other test marked (bugs row 165).
 */
void
test_chunks_packs_roots_tests_root_slots_are_lent_to_one_entry_at_a_time(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(165, chunks_packs_roots_tests_lent_twice, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_roots_tests_over returns a process's own handle of the packs whose record is at a location. */
static bool
chunks_packs_roots_tests_over(HostIoIo *io, uint64 location, ChunksPacksBackendPacks *result, HostError *error)
{
	HostSharedHeld held;
	ChunksErrorError failed;
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &held,
						  error))
		return false;
	if (!has)
	{
		StdString	message;

		std_string_from_Checked(&message, "no record", 9);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (!chunks_packs_backend_Packs_over(io, &held, location, result, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	return true;
}

/* ChunksPacksRootsTestsTwo is a registration's two repositories' packs and the roots it pins in them. */
typedef struct ChunksPacksRootsTestsTwo
{
	const ChunksPacksBackendPacks *first;
	const ChunksPacksBackendPacks *second;
	chunks_TupleAddressLocator_t a;
	chunks_TupleAddressLocator_t b;
} ChunksPacksRootsTestsTwo;

static bool chunks_packs_roots_tests_pinned(const ChunksPacksBackendPacks *packs, HostIoRegistering *registering,
											ChunksPacksRootsRootSlot *slot, const chunks_TupleAddressLocator_t *root,
											ChunksPacksRootsPin *result, ChunksErrorError *error) STD_RELEASE(*slot);

/* chunks_packs_roots_tests_pinned pins a root through a slot in a repository's packs, which takes the slot. */
static bool
chunks_packs_roots_tests_pinned(const ChunksPacksBackendPacks *packs, HostIoRegistering *registering,
								ChunksPacksRootsRootSlot *slot, const chunks_TupleAddressLocator_t *root,
								ChunksPacksRootsPin *result, ChunksErrorError *error)
{
	return chunks_packs_roots_Packs_pin(packs, CHUNKS_PACKS_ROOTS_TESTS_TAKES_SHORT(registering), slot, root->f0,
										root->f1, result, error);
}

/* chunks_packs_roots_tests_two_registered is a_collection_reads_the_roots_registered_with_its_repository's first registration. */
static bool
chunks_packs_roots_tests_two_registered(void *captures, HostIoRegistering *registering,
										ChunksPacksRootsRootSlots *slots, void *result, ChunksErrorError *error)
{
	const ChunksPacksRootsTestsTwo *two = captures;
	HostIoIo	collector = host_io_Io_testing();
	ChunksPacksRootsFold fold;
	ChunksPacksRootsPin pinned;
	ChunksPacksRootsPin other;
	ChunksPacksRootsPin again;
	chunks_TupleAddressLocator_t both[2] = {two->a, two->b};
	StdVec		read;
	size_t		read_len;
	size_t		held = 2;
	bool		ok = false;

	(void) result;
	chunks_packs_roots_RootSlots_fold(slots, &fold);
	chunks_packs_roots_RootSlot_unused(fold.s2);
	chunks_packs_roots_RootSlot_unused(fold.s3);
	if (!chunks_packs_roots_tests_pinned(two->first, registering, fold.s0, &two->a, &pinned, error))
	{
		chunks_packs_roots_RootSlot_unused(fold.s1);
		return false;
	}
	if (!chunks_packs_roots_tests_pinned(two->second, registering, fold.s1, &two->b, &other, error))
		goto pinned;
	if (!chunks_packs_roots_Packs_registered(two->first, &collector, &read, error))
		goto other;
	chunks_packs_roots_tests_roots_eq(&read, &two->a, 1, NULL, 318, 13);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (!chunks_packs_roots_Packs_registered(two->second, &collector, &read, error))
		goto other;
	chunks_packs_roots_tests_roots_eq(&read, &two->b, 1, NULL, 319, 13);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	chunks_packs_roots_Pin_drop(&other);
	if (!chunks_packs_roots_tests_pinned(two->first, registering, other.slot, &two->b, &again, error))
		goto pinned;
	if (!chunks_packs_roots_Packs_registered(two->first, &collector, &read, error))
		goto again;
	chunks_packs_roots_tests_roots_eq(&read, both, lengthof(both), NULL, 322, 13);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (!chunks_packs_roots_Packs_registered(two->second, &collector, &read, error))
		goto again;
	chunks_packs_roots_tests_roots_eq(&read, NULL, 0, NULL, 323, 13);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));

	chunks_packs_roots_Pin_drop(&pinned);
	chunks_packs_roots_Pin_drop(&again);
	chunks_packs_roots_RootSlot_unused(pinned.slot);
	chunks_packs_roots_RootSlot_unused(again.slot);
	if (!chunks_packs_roots_Packs_registered(two->first, &collector, &read, error))
		return false;
	read_len = read.len;
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	STD_ASSERT_EQ_MSG(326, 13, read_len == held, &read_len, std_usize_Debug_fmt, &held, std_usize_Debug_fmt,
					  "a dropped registration was cleared with no lock");
	return true;
again:
	chunks_packs_roots_Pin_drop(&again);
	chunks_packs_roots_RootSlot_unused(again.slot);
	goto pinned;
other:
	chunks_packs_roots_Pin_drop(&other);
	chunks_packs_roots_RootSlot_unused(other.slot);
pinned:
	chunks_packs_roots_Pin_drop(&pinned);
	chunks_packs_roots_RootSlot_unused(pinned.slot);
	return ok;
}

/* chunks_packs_roots_tests_one_pinned is a registration that pins one root through one slot and drops the pin. */
static bool
chunks_packs_roots_tests_one_pinned(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
									void *result, ChunksErrorError *error)
{
	const ChunksPacksRootsTestsTwo *two = captures;
	ChunksPacksRootsRootSlot *one;
	ChunksPacksRootsPin pin;

	(void) result;
	chunks_packs_roots_RootSlots_one(slots, &one);
	if (!chunks_packs_roots_tests_pinned(two->second, registering, one, &two->b, &pin, error))
		return false;
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	return true;
}

/*
 * test_chunks_packs_roots_tests_a_collection_reads_the_roots_registered_with_its_repository:
 * a collection reads the roots registered with its repository, each with its
 * whole address and place, and none registered with another; a registration
 * that dropped is read until the entry gave the slots back and the next pack
 * operation, of its repository or of another, settled its slot.
 */
void
test_chunks_packs_roots_tests_a_collection_reads_the_roots_registered_with_its_repository(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	HostError	host;
	ChunksPacksBackendPacks first;
	ChunksPacksBackendPacks second;
	ChunksPacksRootsTestsTwo two;

	if (!host_shared_read(io, &reading, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 310, 46);
	chunks_packs_fixtures_created(io, 11, &first);
	chunks_packs_fixtures_created(io, 12, &second);
	two.first = &first;
	two.second = &second;
	two.a = chunks_packs_roots_tests_root(3);
	two.b = chunks_packs_roots_tests_root(4);
	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_two_registered, &two, 329, 10);
	chunks_packs_roots_tests_no_numbers(&first, io, &reading, 330, 48);
	chunks_packs_roots_tests_registered(&first, io, NULL, 0, NULL, 331, 9, 41);

	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_one_pinned, &two, 333, 93);
	chunks_packs_roots_tests_registered(&second, io, &two.b, 1, NULL, 334, 9, 42);
	chunks_packs_roots_tests_no_numbers(&first, io, &reading, 335, 48);
	chunks_packs_roots_tests_registered(&second, io, NULL, 0, "another repository's pack operation left a dropped root",
										336, 9, 42);
	chunks_packs_backend_Packs_drop(&second);
	chunks_packs_backend_Packs_drop(&first);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksRootsTestsFlags is a_collection_reads_another_backends_root_until_it_exits' actors' shared state. */
typedef struct ChunksPacksRootsTestsFlags
{
	uint64		location;
	chunks_TupleAddressLocator_t a;
	volatile uint64 pinned;
	volatile uint64 seen;
	const ChunksPacksBackendPacks *packs;
} ChunksPacksRootsTestsFlags;

/* chunks_packs_roots_tests_holding is the writer's registration: a root pinned until the collector saw it. */
static bool
chunks_packs_roots_tests_holding(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								 void *result, ChunksErrorError *error)
{
	ChunksPacksRootsTestsFlags *flags = captures;
	ChunksPacksRootsRootSlot *one;
	ChunksPacksRootsPin pin;

	(void) result;
	chunks_packs_roots_RootSlots_one(slots, &one);
	if (!chunks_packs_roots_tests_pinned(flags->packs, registering, one, &flags->a, &pin, error))
		return false;
	flags->pinned = 1;
	while (flags->seen == 0)
		(void) host_unique();
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	return true;
}

/* chunks_packs_roots_tests_writer is a_collection_reads_another_backends_root_until_it_exits' writer. */
static bool
chunks_packs_roots_tests_writer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsFlags *flags = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint8		unit;
	bool		ok;

	if (!chunks_packs_roots_tests_over(io, flags->location, &packs, error))
		return false;
	flags->packs = &packs;
	ok = chunks_packs_roots_RootSlots_register(io, chunks_packs_roots_tests_holding, flags, NULL, &unit, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	chunks_packs_backend_Packs_drop(&packs);
	return ok;
}

/* chunks_packs_roots_tests_collector is a_collection_reads_another_backends_root_until_it_exits' collector. */
static bool
chunks_packs_roots_tests_collector(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsFlags *flags = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	StdVec		read;
	bool		ok;

	if (!chunks_packs_roots_tests_over(io, flags->location, &packs, error))
		return false;
	while (flags->pinned == 0)
		(void) host_unique();
	ok = chunks_packs_roots_Packs_registered(&packs, io, &read, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	else
	{
		flags->seen = 1;
		ok = chunks_packs_roots_tests_same(&read, &flags->a, 1);
		if (!ok)
			*error = chunks_packs_roots_tests_read_failed(&read);
		std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	}
	chunks_packs_backend_Packs_drop(&packs);
	return ok;
}

/* chunks_packs_roots_tests_ended asserts at a line that every actor of a run ended returning Ok, with the outcome's Debug as the message. */
static void
chunks_packs_roots_tests_ended(const SimcReport *report, uint32 line)
{
	bool		returned = report->outcome.tag == SIMC_OUTCOME_ENDED;

	for (size_t k = 0; returned && k < report->outcome.u.ended.len; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(&report->outcome.u.ended, simc_TupleStringEnding_t, k).f1;

		returned = ending->tag == SIMC_ENDING_RETURNED && !ending->is_err;
	}
	if (!returned)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, 9, &arguments);
	}
}

/*
 * test_chunks_packs_roots_tests_a_collection_reads_another_backends_root_until_it_exits:
 * a collection in one backend reads the root another backend registered,
 * and reads none once that backend has exited.
 */
void
test_chunks_packs_roots_tests_a_collection_reads_another_backends_root_until_it_exits(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks before;
	ChunksPacksRootsTestsFlags flags = {0};
	SimcActor	writer = {chunks_packs_roots_tests_writer, &flags};
	SimcActor	collector = {chunks_packs_roots_tests_collector, &flags};
	SimcRun		run;
	SimcReport	report;

	chunks_packs_fixtures_created(io, 21, &before);
	flags.location = chunks_packs_backend_Packs_location(&before);
	flags.a = chunks_packs_roots_tests_root(5);
	run = simc_Run_new(5);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "collector", 9, collector);
	report = simc_Run_finish(run);
	chunks_packs_roots_tests_ended(&report, 385);
	simc_Report_drop(&report);
	chunks_packs_roots_tests_registered(&before, io, NULL, 0, "an exited backend's root is still read", 391, 9, 42);
	chunks_packs_backend_Packs_drop(&before);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksRootsTestsElsewhere is a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads' state. */
typedef struct ChunksPacksRootsTestsElsewhere
{
	uint64		here;
	uint64		elsewhere;
	chunks_TupleAddressLocator_t a;
	chunks_TupleAddressLocator_t b;
	const ChunksPacksBackendPacks *one;
	const ChunksPacksBackendPacks *two;
} ChunksPacksRootsTestsElsewhere;

/* chunks_packs_roots_tests_paused pauses a registration at a point, as host::pause! does, which fails as the registration's error. */
static bool
chunks_packs_roots_tests_paused(HostIoRegistering *registering, const char *point, ChunksErrorError *error)
{
	HostError	host;

	if (host_pause(HOST_SHARED_REGISTERING_PAUSES(registering), point, &host))
		return true;
	*error = chunks_error_Error_FromError_from(&host);
	return false;
}

/* chunks_packs_roots_tests_moved_on is the writer's registration: a root of one repository dropped, then one of another pinned through the same slot. */
static bool
chunks_packs_roots_tests_moved_on(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								  void *result, ChunksErrorError *error)
{
	ChunksPacksRootsTestsElsewhere *state = captures;
	ChunksPacksRootsRootSlot *one;
	ChunksPacksRootsRootSlot *again;
	ChunksPacksRootsPin pin;

	(void) result;
	chunks_packs_roots_RootSlots_one(slots, &one);
	if (!chunks_packs_roots_tests_pinned(state->one, registering, one, &state->a, &pin, error))
		return false;
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	if (!chunks_packs_roots_tests_paused(registering, "doltgres-chunks-case-root-dropped", error))
		return false;
	chunks_packs_roots_RootSlots_one(slots, &again);
	if (!chunks_packs_roots_tests_pinned(state->two, registering, again, &state->b, &pin, error))
		return false;
	if (!chunks_packs_roots_tests_paused(registering, "doltgres-chunks-case-root-elsewhere", error))
	{
		chunks_packs_roots_Pin_drop(&pin);
		chunks_packs_roots_RootSlot_unused(pin.slot);
		return false;
	}
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	return true;
}

/* chunks_packs_roots_tests_elsewhere_writer is the writer actor of a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads. */
static bool
chunks_packs_roots_tests_elsewhere_writer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsElsewhere *state = captures;
	ChunksPacksBackendPacks one;
	ChunksPacksBackendPacks two;
	ChunksErrorError failed;
	uint8		unit;
	bool		ok;

	if (!chunks_packs_roots_tests_over(io, state->here, &one, error))
		return false;
	if (!chunks_packs_roots_tests_over(io, state->elsewhere, &two, error))
	{
		chunks_packs_backend_Packs_drop(&one);
		return false;
	}
	state->one = &one;
	state->two = &two;
	ok = chunks_packs_roots_RootSlots_register(io, chunks_packs_roots_tests_moved_on, state, NULL, &unit, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
	chunks_packs_backend_Packs_drop(&two);
	chunks_packs_backend_Packs_drop(&one);
	return ok;
}

/* chunks_packs_roots_tests_elsewhere_collector is the collector actor of a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads. */
static bool
chunks_packs_roots_tests_elsewhere_collector(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsElsewhere *state = captures;
	ChunksPacksBackendPacks here;
	ChunksErrorError failed;
	StdVec		read;
	bool		ok;

	if (!chunks_packs_roots_tests_over(io, state->here, &here, error))
		return false;
	ok = chunks_packs_roots_Packs_registered(&here, io, &read, &failed);
	chunks_packs_backend_Packs_drop(&here);
	if (!ok)
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	ok = read.len == 0;
	if (!ok)
		*error = chunks_packs_roots_tests_read_failed(&read);
	std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return ok;
}

/*
 * test_chunks_packs_roots_tests_a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads:
 * a collection that read a slot's repository word, and then finds the slot
 * settled and registered with another repository before it reads the root's
 * words, passes the slot over: it reads the other repository's root as none
 * of its own.
 */
void
test_chunks_packs_roots_tests_a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks first;
	ChunksPacksBackendPacks second;
	ChunksPacksRootsTestsElsewhere state = {0};
	SimcActor	writer = {chunks_packs_roots_tests_elsewhere_writer, &state};
	SimcActor	collector = {chunks_packs_roots_tests_elsewhere_collector, &state};
	SimcStep	steps[] = {{"collector", 9, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;

	chunks_packs_fixtures_created(io, 23, &first);
	chunks_packs_fixtures_created(io, 24, &second);
	state.here = chunks_packs_backend_Packs_location(&first);
	state.elsewhere = chunks_packs_backend_Packs_location(&second);
	state.a = chunks_packs_roots_tests_root(5);
	state.b = chunks_packs_roots_tests_root(6);
	run = simc_Run_new(7);
	simc_Run_actor(&run, "writer", 6, writer);
	simc_Run_actor(&run, "collector", 9, collector);
	simc_Run_until(&run, "writer", 6, "doltgres-chunks-case-root-dropped");
	simc_Run_until(&run, "collector", 9, "doltgres-chunks-roots-named");
	simc_Run_until(&run, "writer", 6, "doltgres-chunks-case-root-elsewhere");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	chunks_packs_roots_tests_ended(&report, 432);
	simc_Report_drop(&report);
	chunks_packs_backend_Packs_drop(&second);
	chunks_packs_backend_Packs_drop(&first);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksRootsTestsFold is a fold's registration's packs and the roots it pins. */
typedef struct ChunksPacksRootsTestsFold
{
	const ChunksPacksBackendPacks *packs;
	const chunks_TupleAddressLocator_t *roots;
	size_t		roots_len;
	uint32		line;
} ChunksPacksRootsTestsFold;

/* chunks_packs_roots_tests_dropped drops a pin and ends its slot's borrow. */
static void
chunks_packs_roots_tests_dropped(ChunksPacksRootsPin *pin)
{
	chunks_packs_roots_Pin_drop(pin);
	chunks_packs_roots_RootSlot_unused(pin->slot);
}

/*
 * chunks_packs_roots_tests_folded is a fold's registration: a root pinned
 * through each of its first three slots, or all four, read at once by a
 * collection.
 */
static bool
chunks_packs_roots_tests_folded(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								void *result, ChunksErrorError *error)
{
	const ChunksPacksRootsTestsFold *fold = captures;
	HostIoIo	collector = host_io_Io_testing();
	ChunksPacksRootsFold lent;
	ChunksPacksRootsPin first;
	ChunksPacksRootsPin second;
	ChunksPacksRootsPin third;
	ChunksPacksRootsPin fourth;
	StdVec		read;
	bool		ok = false;

	(void) result;
	chunks_packs_roots_RootSlots_fold(slots, &lent);
	if (!chunks_packs_roots_tests_pinned(fold->packs, registering, lent.s0, &fold->roots[0], &first, error))
	{
		chunks_packs_roots_RootSlot_unused(lent.s1);
		chunks_packs_roots_RootSlot_unused(lent.s2);
		chunks_packs_roots_RootSlot_unused(lent.s3);
		return false;
	}
	if (!chunks_packs_roots_tests_pinned(fold->packs, registering, lent.s1, &fold->roots[1], &second, error))
	{
		chunks_packs_roots_RootSlot_unused(lent.s2);
		chunks_packs_roots_RootSlot_unused(lent.s3);
		chunks_packs_roots_tests_dropped(&first);
		return false;
	}
	if (!chunks_packs_roots_tests_pinned(fold->packs, registering, lent.s2, &fold->roots[2], &third, error))
	{
		chunks_packs_roots_RootSlot_unused(lent.s3);
		chunks_packs_roots_tests_dropped(&second);
		chunks_packs_roots_tests_dropped(&first);
		return false;
	}
	if (fold->roots_len == 4)
	{
		if (!chunks_packs_roots_tests_pinned(fold->packs, registering, lent.s3, &fold->roots[3], &fourth, error))
		{
			chunks_packs_roots_tests_dropped(&third);
			chunks_packs_roots_tests_dropped(&second);
			chunks_packs_roots_tests_dropped(&first);
			return false;
		}
	}
	else
		chunks_packs_roots_RootSlot_unused(lent.s3);
	if (chunks_packs_roots_Packs_registered(fold->packs, &collector, &read, error))
	{
		chunks_packs_roots_tests_roots_eq(&read, fold->roots, fold->roots_len, NULL, fold->line, 13);
		std_vec_free_Checked(&read, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		ok = true;
	}
	if (fold->roots_len == 4)
		chunks_packs_roots_tests_dropped(&fourth);
	chunks_packs_roots_tests_dropped(&third);
	chunks_packs_roots_tests_dropped(&second);
	chunks_packs_roots_tests_dropped(&first);
	return ok;
}

/*
 * test_chunks_packs_roots_tests_a_collection_reads_a_folds_three_roots_until_its_install:
 * a fold's three new trees, its base's root, tuple id tree, and free space
 * map, registered at once, are all read by a collection of their repository
 * until the fold's install lets them go and the slots are settled.
 */
void
test_chunks_packs_roots_tests_a_collection_reads_a_folds_three_roots_until_its_install(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading;
	HostError	host;
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t roots[3];
	ChunksPacksRootsTestsFold fold;

	if (!host_shared_read(io, &reading, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 446, 46);
	chunks_packs_fixtures_created(io, 13, &packs);
	for (size_t k = 0; k < lengthof(roots); k++)
		roots[k] = chunks_packs_roots_tests_root(6 + k);
	fold.packs = &packs;
	fold.roots = roots;
	fold.roots_len = lengthof(roots);
	fold.line = 456;
	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_folded, &fold, 460, 10);
	chunks_packs_roots_tests_no_numbers(&packs, io, &reading, 461, 48);
	chunks_packs_roots_tests_registered(&packs, io, NULL, 0, NULL, 462, 9, 41);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* CHUNKS_PACKS_ROOTS_TESTS_BACKEND is BACKEND: the takeover case's backend's packs, an Arc<Packs>, which its exit work registers with. */
static StdArcInner *CHUNKS_PACKS_ROOTS_TESTS_BACKEND;

/* CHUNKS_PACKS_ROOTS_TESTS_SEEN is SEEN: the roots a collection read inside the exit work's registration, then after the pack operation that followed it. */
static StdVec CHUNKS_PACKS_ROOTS_TESTS_SEEN[2];

/* CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN is how many reads SEEN holds. */
static size_t CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN;

/* chunks_packs_roots_tests_Packs_drop is Packs' drop glue at a pointer, which an Arc of Packs drops with. */
static void
chunks_packs_roots_tests_Packs_drop(void *packs)
{
	chunks_packs_backend_Packs_drop(packs);
}

/* chunks_packs_roots_tests_backend returns BACKEND's packs, which unwraps at a line and column. */
static const ChunksPacksBackendPacks *
chunks_packs_roots_tests_backend(void)
{
	if (CHUNKS_PACKS_ROOTS_TESTS_BACKEND == NULL)
		STD_OPTION_UNWRAP_FAILED(482, 57);
	return std_arc_Deref_deref(CHUNKS_PACKS_ROOTS_TESTS_BACKEND, _Alignof(ChunksPacksBackendPacks));
}

/* ChunksPacksRootsTestsInside is the exit work's registration's packs and root. */
typedef struct ChunksPacksRootsTestsInside
{
	const ChunksPacksBackendPacks *packs;
	chunks_TupleAddressLocator_t b;
} ChunksPacksRootsTestsInside;

/* chunks_packs_roots_tests_inside is the exit work's registration: its own root pinned, and the roots a collection reads then, its result. */
static bool
chunks_packs_roots_tests_inside(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								void *result, ChunksErrorError *error)
{
	const ChunksPacksRootsTestsInside *inside = captures;
	HostIoIo	collector = host_io_Io_testing();
	ChunksPacksRootsRootSlot *one;
	ChunksPacksRootsPin pin;
	bool		ok;

	chunks_packs_roots_RootSlots_one(slots, &one);
	if (!chunks_packs_roots_tests_pinned(inside->packs, registering, one, &inside->b, &pin, error))
		return false;
	ok = chunks_packs_roots_Packs_registered(inside->packs, &collector, result, error);
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	return ok;
}

/* chunks_packs_roots_tests_AtExit_Exit_run is AtExit's run: the takeover case's exit work. */
static bool
chunks_packs_roots_tests_AtExit_Exit_run(HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsInside inside = {chunks_packs_roots_tests_backend(), chunks_packs_roots_tests_root(10)};
	HostSharedReader reading;
	ChunksErrorError failed;
	StdVec		read;
	StdVec		numbers;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!chunks_packs_roots_RootSlots_register(io, chunks_packs_roots_tests_inside, &inside, NULL, &read, &failed))
		goto raised;
	CHUNKS_PACKS_ROOTS_TESTS_SEEN[0] = read;
	CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN = 1;
	if (!chunks_packs_backend_Packs_numbers(inside.packs, io, &reading, &numbers, &failed))
		goto raised;
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	if (!chunks_packs_roots_Packs_registered(inside.packs, io, &read, &failed))
		goto raised;
	CHUNKS_PACKS_ROOTS_TESTS_SEEN[1] = read;
	CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN = 2;
	host_shared_Reader_drop(&reading);
	return true;
raised:
	*error = chunks_packs_tests_raised(&failed);
	host_shared_Reader_drop(&reading);
	return false;
}

/* CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_TYPE_ID is AtExit's identity, by which its exit work is registered once. */
static const StdTypeId CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_TYPE_ID = {"chunks::packs::roots::tests::AtExit", 0, 1, NULL};

/* CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_OPS is AtExit's Exit. */
static const HostExitOps CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_OPS = {chunks_packs_roots_tests_AtExit_Exit_run};

/* ChunksPacksRootsTestsTakeover is the takeover case's backend's state: its record's location and the roots it pins. */
typedef struct ChunksPacksRootsTestsTakeover
{
	uint64		location;
	chunks_TupleAddressLocator_t a;
	chunks_TupleAddressLocator_t c;
	const ChunksPacksBackendPacks *packs;
} ChunksPacksRootsTestsTakeover;

/* chunks_packs_roots_tests_standing is the backend's registration: two roots pinned, a pause, and then no end. */
static bool
chunks_packs_roots_tests_standing(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								  void *result, ChunksErrorError *error)
{
	const ChunksPacksRootsTestsTakeover *takeover = captures;
	ChunksPacksRootsFold fold;
	ChunksPacksRootsPin first;
	ChunksPacksRootsPin second;

	(void) result;
	chunks_packs_roots_RootSlots_fold(slots, &fold);
	chunks_packs_roots_RootSlot_unused(fold.s2);
	chunks_packs_roots_RootSlot_unused(fold.s3);
	if (!chunks_packs_roots_tests_pinned(takeover->packs, registering, fold.s0, &takeover->a, &first, error))
	{
		chunks_packs_roots_RootSlot_unused(fold.s1);
		return false;
	}
	if (!chunks_packs_roots_tests_pinned(takeover->packs, registering, fold.s1, &takeover->c, &second, error))
	{
		chunks_packs_roots_Pin_drop(&first);
		chunks_packs_roots_RootSlot_unused(first.slot);
		return false;
	}
	if (!chunks_packs_roots_tests_paused(registering, "doltgres-chunks-case-registered", error))
	{
		chunks_packs_roots_Pin_drop(&second);
		chunks_packs_roots_Pin_drop(&first);
		chunks_packs_roots_RootSlot_unused(second.slot);
		chunks_packs_roots_RootSlot_unused(first.slot);
		return false;
	}
	for (;;)
		(void) host_unique();
}

/* chunks_packs_roots_tests_takeover_backend is the takeover case's backend actor. */
static bool
chunks_packs_roots_tests_takeover_backend(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRootsTestsTakeover *takeover = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint8		unit;

	if (!host_at_exit(&CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_TYPE_ID, &CHUNKS_PACKS_ROOTS_TESTS_AT_EXIT_OPS, error))
		return false;
	if (!chunks_packs_roots_tests_over(io, takeover->location, &packs, error))
		return false;
	CHUNKS_PACKS_ROOTS_TESTS_BACKEND = std_arc_new(&packs, sizeof(packs), _Alignof(ChunksPacksBackendPacks));
	takeover->packs = std_arc_Deref_deref(std_arc_clone(CHUNKS_PACKS_ROOTS_TESTS_BACKEND),
										  _Alignof(ChunksPacksBackendPacks));
	if (chunks_packs_roots_RootSlots_register(io, chunks_packs_roots_tests_standing, takeover, NULL, &unit, &failed))
		return true;
	*error = chunks_packs_tests_raised(&failed);
	return false;
}

/* chunks_packs_roots_tests_Exit_Debug_fmt is an exit work's (String, [u8; 5], String)'s Debug. */
static bool
chunks_packs_roots_tests_Exit_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleStringSqlstateString_t *exit = value;
	StdBytes	sqlstate = {exit->f1, sizeof(exit->f1)};
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &exit->f0, std_String_Debug_fmt);
	std_DebugTuple_field(&b, &sqlstate, std_bytes_Debug_fmt);
	std_DebugTuple_field(&b, &exit->f2, std_String_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_roots_tests_Exits_Debug_fmt is a Vec<(String, [u8; 5], String)>'s Debug. */
static bool
chunks_packs_roots_tests_Exits_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *exits = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = exits->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(exits, simc_TupleStringSqlstateString_t, k),
							chunks_packs_roots_tests_Exit_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_roots_tests_Seen_Debug_fmt is SEEN's Vec<Read>'s Debug, of its reads. */
static bool
chunks_packs_roots_tests_Seen_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *seen = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = seen->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(seen, StdVec, k), chunks_packs_roots_tests_Roots_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * test_chunks_packs_roots_tests_an_entry_at_a_terminated_backends_exit_takes_its_standing_registrations_slots_over:
 * a backend terminated inside a registration never runs that frame again, so
 * an entry entered at its exit takes its slots over: the entry's own root
 * replaces the frame's in the slot it registers in, every slot is marked, so
 * the next pack operation settles the frame's other roots, and once the
 * backend has exited a collection reads none of them.
 */
void
test_chunks_packs_roots_tests_an_entry_at_a_terminated_backends_exit_takes_its_standing_registrations_slots_over(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks before;
	ChunksPacksRootsTestsTakeover takeover = {0};
	SimcActor	backend = {chunks_packs_roots_tests_takeover_backend, &takeover};
	SimcRun		run;
	SimcReport	report;
	chunks_TupleAddressLocator_t inside[2];
	bool		seen;

	chunks_packs_fixtures_created(io, 25, &before);
	takeover.location = chunks_packs_backend_Packs_location(&before);
	takeover.a = chunks_packs_roots_tests_root(8);
	takeover.c = chunks_packs_roots_tests_root(9);
	run = simc_Run_new(11);
	simc_Run_actor(&run, "backend", 7, backend);
	simc_Run_until(&run, "backend", 7, "doltgres-chunks-case-registered");
	simc_Run_terminate_at(&run, "backend", 7, 5000);
	report = simc_Run_finish(run);
	chunks_packs_tests_terminated(RUST_FILE, &report, 526, 67, 527, 20, 9);
	if (report.exits.len != 0)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.exits, chunks_packs_roots_tests_Exits_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 528, 9, &arguments);
	}
	inside[0] = chunks_packs_roots_tests_root(10);
	inside[1] = takeover.c;
	seen = CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN == 2 &&
		chunks_packs_roots_tests_same(&CHUNKS_PACKS_ROOTS_TESTS_SEEN[0], inside, lengthof(inside)) &&
		chunks_packs_roots_tests_same(&CHUNKS_PACKS_ROOTS_TESTS_SEEN[1], NULL, 0);
	if (!seen)
	{
		StdVec		got = {CHUNKS_PACKS_ROOTS_TESTS_SEEN, CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN,
		CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN};
		StdVec		reads[2];
		StdVec		want = {reads, 2, 2};

		std_slice_to_vec_Checked(&reads[0], inside, lengthof(inside), sizeof(inside[0]),
								 _Alignof(chunks_TupleAddressLocator_t));
		std_vec_new(&reads[1], sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
		std_assert_failed(RUST_FILE, 529, 9, "==", &got, chunks_packs_roots_tests_Seen_Debug_fmt, &want,
						  chunks_packs_roots_tests_Seen_Debug_fmt, NULL);
	}
	for (size_t k = 0; k < CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN; k++)
		std_vec_free_Checked(&CHUNKS_PACKS_ROOTS_TESTS_SEEN[k], sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	CHUNKS_PACKS_ROOTS_TESTS_SEEN_LEN = 0;
	simc_Report_drop(&report);
	if (CHUNKS_PACKS_ROOTS_TESTS_BACKEND != NULL)
		std_arc_drop(CHUNKS_PACKS_ROOTS_TESTS_BACKEND, sizeof(ChunksPacksBackendPacks), _Alignof(ChunksPacksBackendPacks),
					 chunks_packs_roots_tests_Packs_drop);
	CHUNKS_PACKS_ROOTS_TESTS_BACKEND = NULL;
	chunks_packs_roots_tests_registered(&before, io, NULL, 0, "an exited backend's root is still read", 531, 9, 42);
	chunks_packs_backend_Packs_drop(&before);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_roots_tests_a_collection_reads_every_slot_a_fold_registers:
 * bugs row 152: a fold's new trees registered in every slot it lends, at
 * once, are each read back whole by a collection of their repository, so no
 * slot's words fall on another's and none goes unread.
 */
void
test_chunks_packs_roots_tests_a_collection_reads_every_slot_a_fold_registers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t roots[CHUNKS_PACKS_ROOTS_SLOTS];
	ChunksPacksRootsTestsFold fold;

	chunks_packs_fixtures_created(io, 14, &packs);
	for (size_t k = 0; k < lengthof(roots); k++)
		roots[k] = chunks_packs_roots_tests_root(20 + k);
	fold.packs = &packs;
	fold.roots = roots;
	fold.roots_len = lengthof(roots);
	fold.line = 547;
	chunks_packs_roots_tests_registers(io, chunks_packs_roots_tests_folded, &fold, 551, 10);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
