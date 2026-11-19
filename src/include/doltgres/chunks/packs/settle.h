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
 * The startup process's settle of a repository's packs (chunks'
 * packs/settle.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACKS_SETTLE_H
#define DOLTGRES_CHUNKS_PACKS_SETTLE_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/host/io.h"
#include "doltgres/std/path.h"
#include "doltgres/std/vec.h"

/* chunks_TuplePathBufFallback_t is a list file and why a settle read it as its previous generation. */
typedef struct chunks_TuplePathBufFallback_t
{
	StdPathBuf	f0;
	ChunksFileFallback f1;
} chunks_TuplePathBufFallback_t;

/* chunks_TuplePathBufDamage_t is a list file and where a settle found it damaged. */
typedef struct chunks_TuplePathBufDamage_t
{
	StdPathBuf	f0;
	ChunksFileDamage f1;
} chunks_TuplePathBufDamage_t;

/* chunks_TuplePathBufLost_t is a pack whose run a settle wrote again, and what the pack lost. */
typedef struct chunks_TuplePathBufLost_t
{
	StdPathBuf	f0;
	ChunksPackLost f1;
} chunks_TuplePathBufLost_t;

/* ChunksPacksSettleSettled is Settled: what a settle changed, which the startup process reports once. */
typedef struct ChunksPacksSettleSettled
{
	/*
	 * removed is a Vec<PathBuf> of the files it removed: the unfinished packs
	 * a crash left, the runs' files no runs file names, and the forwards'
	 * files the forwarding file doesn't name.
	 */
	StdVec		removed;

	/*
	 * moved is a Vec<u32> of the tablespaces whose runs file's end the pack
	 * table didn't name, which it recorded in the table.
	 */
	StdVec		moved;

	/*
	 * fallbacks is a Vec<(PathBuf, Fallback)> of the list files it read as
	 * their previous generation, which it wrote whole again.
	 */
	StdVec		fallbacks;

	/*
	 * damaged is a Vec<(PathBuf, Damage)> of the list files damaged past some
	 * of their records, which it kept beside them and wrote whole again from
	 * the records before the damage.
	 */
	StdVec		damaged;

	/*
	 * rebuilt is a Vec<(PathBuf, Lost)> of the packs whose run it wrote again
	 * from the pack.
	 */
	StdVec		rebuilt;

	/*
	 * unlisted is a Vec<u64> of the packs a pack table that didn't read whole
	 * listed whose files are gone, which it left out.
	 */
	StdVec		unlisted;

	/*
	 * dropped is whether the repository was dropped, by the mark in its own
	 * directory: its directories were removed, and nothing else was settled.
	 */
	bool		dropped;
} ChunksPacksSettleSettled;

extern void chunks_packs_settle_Settled_Default_default(ChunksPacksSettleSettled *result);
extern bool chunks_packs_settle_Settled_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_settle_Settled_PartialEq_eq(const ChunksPacksSettleSettled *self,
													 const ChunksPacksSettleSettled *other);
extern void chunks_packs_settle_Settled_drop(ChunksPacksSettleSettled *self);
extern bool chunks_packs_settle_rebuilt(HostIoIo *io, const char *pack, size_t pack_len, uint64 number,
										const char *run, size_t run_len, ChunksPackLost *result,
										ChunksErrorError *error);
extern bool chunks_packs_settle_Packs_settle(HostIoIo *io, uint32 repository, uint32 own,
											 ChunksPacksSettleSettled *result, ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_settle_tests_a_settle_removes_the_unfinished_packs_a_crash_left(void);
extern void test_chunks_packs_settle_tests_a_settle_records_a_runs_files_whole_write_the_table_does_not_name(void);
extern void test_chunks_packs_settle_tests_a_settle_keeps_a_damaged_list_files_records_before_the_damage(void);
extern void test_chunks_packs_settle_tests_a_settle_marks_a_run_rebuilt_with_loss_and_rebuilds_it_until_its_pack_is_whole(void);
extern void test_chunks_packs_settle_tests_a_settle_removes_an_unfinished_pack_alone_in_its_tablespace(void);
extern void test_chunks_packs_settle_tests_a_settle_writes_a_pack_table_that_fell_back_whole_again(void);
extern void test_chunks_packs_settle_tests_a_settle_rebuilds_the_runs_a_listed_pack_lacks_and_removes_the_strays(void);
extern void test_chunks_packs_settle_tests_a_settle_removes_a_forwards_file_the_forwarding_file_doesnt_name(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_SETTLE_H */
