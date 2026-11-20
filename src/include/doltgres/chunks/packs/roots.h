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
 * Root slots, through which an entry pins the trees it reads (chunks'
 * packs/roots.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_ROOTS_H
#define DOLTGRES_CHUNKS_PACKS_ROOTS_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/capability.h"

/*
 * ChunksPacksRootsRootSlot is RootSlot: one of the process's root slots, a
 * capability that register's run holds once one or fold lends it, until a pin
 * or unused releases it, as the Rust's &mut RootSlot is a borrow of it; a
 * pin's drop gives it back, as the borrow the Rust's Pin holds ends there.
 */
typedef struct ChunksPacksRootsRootSlot
{
	/* index is which of the process's slots it is. */
	size_t		index;

	/*
	 * dropped is whether a registration that dropped left the slot's shared
	 * words for the next one to settle.
	 */
	bool		dropped;
}			STD_CAPABILITY("root slot")
ChunksPacksRootsRootSlot;

/* CHUNKS_PACKS_ROOTS_SLOTS is SLOTS: how many root slots a process has. */
#define CHUNKS_PACKS_ROOTS_SLOTS 4

/* ChunksPacksRootsRootSlots is RootSlots: a process's root slots, lent to one entry at a time. */
typedef struct ChunksPacksRootsRootSlots
{
	ChunksPacksRootsRootSlot slots[CHUNKS_PACKS_ROOTS_SLOTS];
} ChunksPacksRootsRootSlots;

/*
 * ChunksPacksRootsFold is fold's four slots, each named, as the Rust's
 * destructuring of fold's array names them, so that each is a capability of
 * its own.
 */
typedef struct ChunksPacksRootsFold
{
	ChunksPacksRootsRootSlot *s0;
	ChunksPacksRootsRootSlot *s1;
	ChunksPacksRootsRootSlot *s2;
	ChunksPacksRootsRootSlot *s3;
} ChunksPacksRootsFold;

/* ChunksPacksRootsPin is Pin: a root pinned through a slot, which its drop marks dropped. */
typedef struct ChunksPacksRootsPin
{
	ChunksPacksRootsRootSlot *slot;
} ChunksPacksRootsPin;

/* CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS is Roots' ProcSlots: each process's slots for its registered roots. */
extern const HostSharedProcSlotsOps CHUNKS_PACKS_ROOTS_ROOTS_PROC_SLOTS_OPS;

/*
 * chunks_packs_roots_RootSlots_register is RootSlots::register: run's T is
 * its caller's, written through result, which run fills and register hands
 * back as run left it.
 */
extern bool chunks_packs_roots_RootSlots_register(HostIoIo *io,
												  bool (*run) (void *captures, HostIoRegistering *registering,
															   ChunksPacksRootsRootSlots *slots, void *result,
															   ChunksErrorError *error),
												  void *run_captures, void (*run_drop) (void *), void *result,
												  ChunksErrorError *error);
extern void chunks_packs_roots_RootSlots_one(ChunksPacksRootsRootSlots *self, ChunksPacksRootsRootSlot **result)
			STD_ACQUIRE(**result) STD_NO_THREAD_SAFETY_ANALYSIS;
extern void chunks_packs_roots_RootSlots_fold(ChunksPacksRootsRootSlots *self, ChunksPacksRootsFold *result)
			STD_ACQUIRE(*result->s0, *result->s1, *result->s2, *result->s3) STD_NO_THREAD_SAFETY_ANALYSIS;
extern void chunks_packs_roots_RootSlot_unused(ChunksPacksRootsRootSlot *slot)
			STD_RELEASE(*slot) STD_NO_THREAD_SAFETY_ANALYSIS;
extern void chunks_packs_roots_RootSlots_drop(ChunksPacksRootsRootSlots *self);
extern bool chunks_packs_roots_Packs_pin(const ChunksPacksBackendPacks *self, HostIoTakesShort io,
										 ChunksPacksRootsRootSlot *slot, ChunksAddressAddress address,
										 ChunksTrailerLocator locator, ChunksPacksRootsPin *result,
										 ChunksErrorError *error) STD_RELEASE(*slot) STD_NO_THREAD_SAFETY_ANALYSIS;
extern bool chunks_packs_roots_Packs_registered(const ChunksPacksBackendPacks *self, HostIoIo *io, StdVec *result,
												ChunksErrorError *error);
extern void chunks_packs_roots_Pin_drop(ChunksPacksRootsPin *self)
			STD_ACQUIRE(*self->slot) STD_NO_THREAD_SAFETY_ANALYSIS;

/* chunks_packs_roots_Packs_settle_roots is settle_roots, which backend.c's reads run first. */
extern bool chunks_packs_roots_Packs_settle_roots(const ChunksPacksBackendPacks *self, HostIoIo *io,
												  ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_roots_tests_root_slots_are_lent_to_one_entry_at_a_time(void);
extern void test_chunks_packs_roots_tests_a_collection_reads_the_roots_registered_with_its_repository(void);
extern void test_chunks_packs_roots_tests_a_collection_reads_another_backends_root_until_it_exits(void);
extern void test_chunks_packs_roots_tests_a_collection_passes_over_a_slot_registered_elsewhere_between_its_loads(void);
extern void test_chunks_packs_roots_tests_a_collection_reads_a_folds_three_roots_until_its_install(void);
extern void test_chunks_packs_roots_tests_an_entry_at_a_terminated_backends_exit_takes_its_standing_registrations_slots_over(void);
extern void test_chunks_packs_roots_tests_a_collection_reads_every_slot_a_fold_registers(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_ROOTS_H */
