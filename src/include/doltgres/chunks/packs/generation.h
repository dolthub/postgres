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
 * The generation a base backup keeps of a repository's list files and
 * manifest (chunks' packs/generation.rs, bugs row 178).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_GENERATION_H
#define DOLTGRES_CHUNKS_PACKS_GENERATION_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"

extern bool chunks_packs_generation_Packs_keep(const ChunksPacksBackendPacks *self, HostIoIo *io, HostLsn checkpoint,
											   ChunksErrorError *error);
extern bool chunks_packs_generation_Packs_restore_generation(HostIoIo *io, uint32 repository, uint32 own,
															 HostLsn checkpoint, ChunksErrorError *error);
extern bool chunks_packs_generation_Packs_drop_generations(HostIoIoDoors io, uint32 repository, uint32 own,
														   ChunksErrorError *error);
#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_generation_tests_a_restore_settles_from_the_kept_generation_without_a_later_pack(void);
extern void test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal(void);
extern void test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_GENERATION_H */
