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
 * The holds of the pack numbers a process's open writers write (chunks'
 * packs/writers.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_WRITERS_H
#define DOLTGRES_CHUNKS_PACKS_WRITERS_H

#include "doltgres/chunks/error.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/vec.h"

/* ChunksPacksWritersWriting is Writing: a pack's number and its process's hold of it while the pack is written. */
typedef struct ChunksPacksWritersWriting
{
	/* number is the pack's number, or zero in open until it is reserved. */
	uint64		number;
	HostSharedMine slot;
} ChunksPacksWritersWriting;

extern const HostSharedProcSlotsOps CHUNKS_PACKS_WRITERS_WRITERS_PROC_SLOTS_OPS;

extern bool chunks_packs_writers_kept_from(uint64 repository, uint64 reserved, uint64 *result, ChunksErrorError *error);
extern bool chunks_packs_writers_covers(const uint32 *slots, size_t slots_len, uint32 process, uint64 repository,
										uint64 number, bool *result, ChunksErrorError *error);
extern bool chunks_packs_writers_writing_in(uint64 repository, bool *result, ChunksErrorError *error);
extern bool chunks_packs_writers_slots(StdVec *result, ChunksErrorError *error);
extern void chunks_packs_writers_space_of(HostIoTakesShort io, uint64 repository, uint64 number, bool *some,
										  uint32 *result);
extern bool chunks_packs_writers_Writing_open(HostIoIo *io, uint64 repository, uint32 space,
											  uint64 (*reserved) (void *captures, HostSharedOp op),
											  void *reserved_captures, void (*reserved_drop) (void *captures),
											  bool (*raise) (void *captures, HostIoIo *io, uint64 numbers,
															 ChunksErrorError *error),
											  void *raise_captures, void (*raise_drop) (void *captures),
											  ChunksPacksWritersWriting *result, ChunksErrorError *error);
extern uint64 chunks_packs_writers_Writing_number(const ChunksPacksWritersWriting *self);
extern void chunks_packs_writers_Writing_drop(ChunksPacksWritersWriting *self);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_writers_tests_a_failed_allocation_in_a_writers_number_keeps_the_open_numbers(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_WRITERS_H */
