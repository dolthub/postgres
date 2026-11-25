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
 * The queue of removals: its reservations, its entries, and the removal of
 * what is due (chunks' packs/removal.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_REMOVAL_H
#define DOLTGRES_CHUNKS_PACKS_REMOVAL_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"

/* ChunksPacksRemovalReservation is Reservation: room for entries in the removal queue, held by its word. */
typedef struct ChunksPacksRemovalReservation
{
	size_t		entries;
	HostSharedMine word;
} ChunksPacksRemovalReservation;

/* ChunksPacksRemovalFile is File: a file of a repository to queue, its tablespace's id, its kind, and the id its name begins with. */
typedef struct ChunksPacksRemovalFile
{
	uint32		f0;
	ChunksPacksSingleQueued f1;
	ChunksPackPackId f2;
} ChunksPacksRemovalFile;

extern const HostSharedProcSlotsOps CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS;

extern bool chunks_packs_removal_reserve(HostIoIo *io, size_t wanted, ChunksPacksRemovalReservation *result,
										 ChunksErrorError *error);
extern size_t chunks_packs_removal_Reservation_entries(const ChunksPacksRemovalReservation *self);
extern void chunks_packs_removal_Reservation_drop(ChunksPacksRemovalReservation *self);
extern bool chunks_packs_removal_queue(HostIoIo *io, ChunksPacksRemovalReservation *reservation, uint32 repository,
									   const ChunksPacksRemovalFile *files, size_t files_len, ChunksErrorError *error);
extern bool chunks_packs_removal_queued(HostIoIo *io, uint32 repository, StdVec *result, ChunksErrorError *error);
extern bool chunks_packs_removal_queued_packs(HostIoIo *io, uint32 repository, StdVec *result,
											  ChunksErrorError *error);
extern StdString chunks_packs_removal_file_name(const ChunksPacksSingleRemoval *entry);
extern bool chunks_packs_removal_remove_due(HostIoIo *io, ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_removal_tests_a_queued_file_is_removed_once_every_reader_that_may_hold_it_has_ended(void);
extern void test_chunks_packs_removal_tests_a_reservation_takes_room_until_it_is_filled_or_dropped(void);
extern void test_chunks_packs_removal_tests_a_removal_that_fails_is_returned_with_its_entry_left_queued(void);
extern void test_chunks_packs_removal_tests_a_process_terminated_with_a_reservation_gives_its_room_back(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_REMOVAL_H */
