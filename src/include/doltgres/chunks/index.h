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
 * The address index, which finds a chunk by its address alone: a set of
 * runs, each a file of entries sorted by address in 4 KB blocks, each ending
 * in a checksum of its entries, of which only the first address of each
 * block stays in memory (chunks' index.rs).
 */
#ifndef DOLTGRES_CHUNKS_INDEX_H
#define DOLTGRES_CHUNKS_INDEX_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/path.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"

/* CHUNKS_INDEX_BLOCK is the size of a run's blocks. */
#define CHUNKS_INDEX_BLOCK 4096

/* CHUNKS_INDEX_SUM is the size of the checksum that ends each block. */
#define CHUNKS_INDEX_SUM 8

/* CHUNKS_INDEX_FOOTER is the size of a run's footer: the entry count, the checksum, and the magic. */
#define CHUNKS_INDEX_FOOTER 24

/* ChunksIndexKeyedOps is Keyed's table: something sorted by an address, which key returns. */
typedef struct ChunksIndexKeyedOps
{
	const ChunksAddressAddress *(*key) (const void *self);
} ChunksIndexKeyedOps;

/*
 * ChunksIndexSourceOps is Source's table: something that answers questions
 * from what it holds, by a read that can fail, given what the read needs
 * from its caller.
 */
typedef struct ChunksIndexSourceOps
{
	bool		(*ask) (void *self, void *with, const void *question, void *result, void *error);
} ChunksIndexSourceOps;

/* ChunksIndexSource is a value that implements Source. */
typedef struct ChunksIndexSource
{
	const ChunksIndexSourceOps *ops;
	void	   *self;
} ChunksIndexSource;

/*
 * ChunksIndexFences is Fences: a run's fences, each a block's first key,
 * which are sorted, since only new makes them, from keys it finds in order.
 */
typedef struct ChunksIndexFences
{
	StdVec		keys;
} ChunksIndexFences;

/* ChunksIndexUnreadTag is Unread's variants. */
typedef enum ChunksIndexUnreadTag
{
	CHUNKS_INDEX_UNREAD_SOURCE,
	CHUNKS_INDEX_UNREAD_UNFIT,
} ChunksIndexUnreadTag;

/* ChunksIndexRunsOps is Runs' table: what reads a block of one of its runs. */
typedef struct ChunksIndexRunsOps
{
	bool		(*block) (const void *self, void *with, size_t run, size_t block, StdVec *result,
						  ChunksErrorError *error);
} ChunksIndexRunsOps;

/* ChunksIndexRuns is a value that implements Runs. */
typedef struct ChunksIndexRuns
{
	const ChunksIndexRunsOps *ops;
	const void *self;
} ChunksIndexRuns;

/* ChunksIndexBlocks is Blocks: the blocks of a set of runs, as a source of their entries. */
typedef struct ChunksIndexBlocks
{
	ChunksIndexRuns runs;
} ChunksIndexBlocks;

/* ChunksIndexBackingTag is Backing's variants. */
typedef enum ChunksIndexBackingTag
{
	CHUNKS_INDEX_BACKING_FILE,
	CHUNKS_INDEX_BACKING_MEMORY,
} ChunksIndexBackingTag;

/*
 * ChunksIndexBacking is Backing: where a run's bytes are read from, its
 * file, or memory for a run a process that changes no file built.
 */
typedef struct ChunksIndexBacking
{
	ChunksIndexBackingTag tag;
	union
	{
		HostFile	file;
		StdVec		memory;
	}			u;
} ChunksIndexBacking;

/* Unread<Error>, Run<Locator>, AddressIndex<Locator>, and their lookups, which index.c defines. */
#define CHUNKS_INDEX_SCOPE extern
#define CHUNKS_INDEX_DECLARE
#define CHUNKS_INDEX_ES Error
#define CHUNKS_INDEX_E ChunksErrorError
#define CHUNKS_INDEX_VS Locator
#define CHUNKS_INDEX_V ChunksTrailerLocator
#define CHUNKS_INDEX_ENTRY chunks_TupleAddressLocator_t
#include "doltgres/chunks/index_template.h"

extern const uint8 CHUNKS_INDEX_MAGIC[8];
extern const ChunksIndexKeyedOps CHUNKS_INDEX_ADDRESS_KEYED_OPS;
extern const ChunksIndexKeyedOps CHUNKS_INDEX_TUPLE_ADDRESS_KEYED_OPS;
extern const ChunksIndexSourceOps CHUNKS_INDEX_BLOCKS_SOURCE_OPS;
extern const ChunksIndexRunsOps CHUNKS_INDEX_ADDRESS_INDEX_LOCATOR_RUNS_OPS;

extern const ChunksAddressAddress *chunks_index_Address_Keyed_key(const void *self);
extern const ChunksAddressAddress *chunks_index_TupleAddress_Keyed_key(const void *self);
extern size_t chunks_index_first_from(size_t t_size, const ChunksIndexKeyedOps *t, const void *values,
									  size_t values_len, const ChunksAddressAddress *key);
extern size_t chunks_index_first_past(size_t t_size, const ChunksIndexKeyedOps *t, const void *values,
									  size_t values_len, const ChunksAddressAddress *key);
extern bool chunks_index_in_order(size_t t_size, const ChunksIndexKeyedOps *t, const void *values,
								  size_t values_len);
extern void chunks_index_block_range(const ChunksAddressAddress *fences, size_t fences_len,
									 const ChunksAddressAddress *key, size_t *out_0, size_t *out_1);
extern bool chunks_index_first_with(size_t entries_size, const void *entries, size_t entries_len,
									const ChunksAddressAddress *key, size_t *result);
extern bool chunks_index_block_fits(size_t entries_size, const void *entries, size_t entries_len,
									const ChunksAddressAddress *fences, size_t fences_len, size_t i);
extern bool chunks_index_Fences_new(StdVec *keys, ChunksIndexFences *result);
extern const ChunksAddressAddress *chunks_index_Fences_keys(const ChunksIndexFences *self, size_t *result_len);
extern bool chunks_index_Blocks_SourceTupleUsizeUsizeVecTupleAddressError_ask(void *self, void *with,
																			  const void *question, void *result,
																			  void *error);
extern bool chunks_index_written_run(HostIoIoDoors io, const char *path, size_t path_len,
									 const chunks_TupleAddressLocator_t *entries, size_t entries_len,
									 ChunksIndexRunLocator *result, ChunksErrorError *error);
extern void chunks_index_Locator_Fixed_write(const ChunksTrailerLocator *self, StdVec *out);
extern ChunksTrailerLocator chunks_index_Locator_Fixed_read(const uint8 *bytes, size_t bytes_len);
extern void chunks_index_u64_Fixed_write(const uint64 *self, StdVec *out);
extern uint64 chunks_index_u64_Fixed_read(const uint8 *bytes, size_t bytes_len);
extern void chunks_index_Fences_drop(void *self);
extern void chunks_index_Backing_drop(void *self);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_index_tests_finds_entries_and_merges_runs(void);
extern void test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode(void);
extern void test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read(void);
extern void test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode(void);
extern void test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would(void);
extern void test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks(void);
extern void test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address(void);
#endif

#endif							/* DOLTGRES_CHUNKS_INDEX_H */
