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
 * A pack's deletion vector: the offsets of the chunks a full collection
 * found dead in a pack it kept, in a file beside the pack named by the
 * pack's id and the vector's generation (chunks' dead.rs).
 */
#ifndef DOLTGRES_CHUNKS_DEAD_H
#define DOLTGRES_CHUNKS_DEAD_H

#include "doltgres/chunks/error.h"
#include "doltgres/host/io.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/vec.h"

/*
 * ChunksDeadDead is Dead: a pack's deletion vector, the offsets of its dead
 * chunks, in order, which only a checked read makes.
 */
typedef struct ChunksDeadDead
{
	/* offsets is a Vec<u64>. */
	StdVec		offsets;
} ChunksDeadDead;

extern bool chunks_dead_Dead_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_dead_Dead_PartialEq_eq(const void *self_, const void *other_);
extern StdPathBuf chunks_dead_Dead_path(const char *pack, size_t pack_len, uint64 generation);
extern bool chunks_dead_Dead_write(HostIoIoDoors io, const char *pack, size_t pack_len, uint64 generation,
								   const StdBTreeMap *offsets, ChunksErrorError *error);
extern bool chunks_dead_Dead_read(HostIoReadDoors io, const char *pack, size_t pack_len, uint64 generation,
								  ChunksDeadDead *result, ChunksErrorError *error);
extern bool chunks_dead_Dead_holds(const ChunksDeadDead *self, uint64 offset);
extern void chunks_dead_Dead_drop(void *self_);
extern StdBTreeMap std_btree_BTreeSetU64_FromArray_from(uint64 *arr, size_t n);
extern void std_btree_BTreeMapU64Set_drop(void *self);

#ifdef DOLTGRES_TEST_CHUNKS
extern StdBTreeMap std_btree_BTreeSetU64_new(void);
extern void test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written(void);
extern void test_chunks_dead_tests_a_missing_vector_fails_its_read(void);
extern void test_chunks_dead_tests_a_damaged_vector_is_refused(void);
#endif

#endif							/* DOLTGRES_CHUNKS_DEAD_H */
