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
 * The files that list a repository's packs (chunks' packs.rs): the pack
 * table's records, and the lines of each tablespace's runs file.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_H
#define DOLTGRES_CHUNKS_PACKS_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* CHUNKS_PACKS_DEFAULT_SPACE is the tablespace a repository's packs are written in unless it names another. */
#define CHUNKS_PACKS_DEFAULT_SPACE ((uint32) 0)

/* CHUNKS_PACKS_TABLE is the file in the packs directory that holds the pack table. */
#define CHUNKS_PACKS_TABLE "table"

/* CHUNKS_PACKS_RUNS is the file in each tablespace's directory of packs that names its address index's runs, oldest first, as appended records. */
#define CHUNKS_PACKS_RUNS "runs"

/* CHUNKS_PACKS_FORWARDING is the file in the packs directory that names the runs of the repository's forwards, oldest first, as a runs file's records do. */
#define CHUNKS_PACKS_FORWARDING "forwarding"

/* CHUNKS_PACKS_MANIFEST is the kept file in the repository's own directory that names the tree of its storages' bases. */
#define CHUNKS_PACKS_MANIFEST "manifest"

/* CHUNKS_PACKS_DELTA is the directory of the checkpoint's runs in the repository's own directory, one directory in it for each checkpoint's redo point. */
#define CHUNKS_PACKS_DELTA "delta"

/* CHUNKS_PACKS_ENTRY is the length of a pack's entry in a pack table's record that adds it: its number, its tablespace, and its id. */
#define CHUNKS_PACKS_ENTRY ((size_t) (6 + 4 + 16))

/* CHUNKS_PACKS_TAIL is the length of a tail in a pack table's record. */
#define CHUNKS_PACKS_TAIL ((size_t) 16)

/* CHUNKS_PACKS_LOSSY is the mark, after a run's file name and its packs' numbers, of a run a settle wrote again from a pack whose address table didn't read whole. */
#define CHUNKS_PACKS_LOSSY "lossy"

/* ChunksPacksTableEntry is TableEntry, a pack of the table: its number, its tablespace, and its id. */
typedef struct ChunksPacksTableEntry
{
	uint64		f0;
	uint32		f1;
	ChunksPackPackId f2;
} ChunksPacksTableEntry;

/* ChunksPacksListed is a pack as the table lists it: its tablespace, its id, and its deletion vector's generation once a full collection wrote it one. */
typedef struct ChunksPacksListed
{
	uint32		space;
	ChunksPackPackId id;

	/*
	 * Whether a full collection wrote the pack a deletion vector, whose
	 * generation is `dead`.
	 */
	bool		has_dead;
	uint64		dead;
} ChunksPacksListed;

/* ChunksPacksTail is where a list file's whole records end, with the generation of the whole write they follow. */
typedef struct ChunksPacksTail
{
	uint64		end;
	uint64		generation;
} ChunksPacksTail;

/* chunks_TupleU64Listed_t is a (u64, Listed): a pack's number and how the table lists it. */
typedef struct chunks_TupleU64Listed_t
{
	uint64		f0;
	ChunksPacksListed f1;
} chunks_TupleU64Listed_t;

/* chunks_TupleU32Tail_t is a (u32, Tail): a tablespace and where its runs file ends. */
typedef struct chunks_TupleU32Tail_t
{
	uint32		f0;
	ChunksPacksTail f1;
} chunks_TupleU32Tail_t;

/*
 * ChunksPacksTable is a pack table as its file's records give it: the
 * generation of its whole write, the highest number a pack was added at, the
 * bound no pack writer's number passes, each pack as it is listed by its
 * number, and where each tablespace's runs file ends.
 */
typedef struct ChunksPacksTable
{
	uint64		generation;
	uint64		added;

	/*
	 * The bound the file holds durably before a writer is given a number
	 * under it, so that a record made again from the file starts past every
	 * number ever given.
	 */
	uint64		reserved;

	/*
	 * The BTreeMap<u64, Listed> of the packs: a Vec of
	 * chunks_TupleU64Listed_t in number order, each number once.
	 */
	StdVec		packs;

	/*
	 * The BTreeMap<u32, Tail> of the runs files' tails: a Vec of
	 * chunks_TupleU32Tail_t in tablespace order, each once.
	 */
	StdVec		runs;
} ChunksPacksTable;

/*
 * ChunksPacksRuns is Runs: a tablespace's runs as its runs file's records
 * give them, the generation of its whole write and a line for each run,
 * oldest first.
 */
typedef struct ChunksPacksRuns
{
	uint64		generation;
	/* lines is a Vec<String>. */
	StdVec		lines;
} ChunksPacksRuns;

extern bool chunks_packs_Listed_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_Listed_PartialEq_eq(const ChunksPacksListed *self, const ChunksPacksListed *other);
extern bool chunks_packs_Tail_Debug_fmt(const void *value, StdFormatter *f);
extern ChunksPacksTail chunks_packs_Tail_Default_default(void);
extern bool chunks_packs_Tail_PartialEq_eq(const ChunksPacksTail *self, const ChunksPacksTail *other);
extern void chunks_packs_Tail_bytes(const ChunksPacksTail *self, uint8 *result);
extern ChunksPacksTail chunks_packs_Tail_read(const uint8 *bytes);
extern uint32 chunks_packs_word(const uint8 *bytes, size_t bytes_len, size_t at);
extern uint64 chunks_packs_long(const uint8 *bytes, size_t bytes_len, size_t at);
extern void chunks_packs_entry_bytes(const ChunksPacksTableEntry *entry, uint8 *result);
extern ChunksPacksTableEntry chunks_packs_entry_of(const uint8 *bytes);
extern void chunks_packs_Table_Default_default(ChunksPacksTable *result);
extern void chunks_packs_Table_drop(ChunksPacksTable *self);
extern bool chunks_packs_Table_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_Table_PartialEq_eq(const ChunksPacksTable *self, const ChunksPacksTable *other);
extern ChunksPacksListed *chunks_packs_table_packs_get(const StdVec *packs, uint64 number);
extern void chunks_packs_table_packs_insert(StdVec *packs, uint64 number, const ChunksPacksListed *listed);
extern bool chunks_packs_table_packs_remove(StdVec *packs, uint64 number);
extern ChunksPacksTail *chunks_packs_table_runs_get(const StdVec *runs, uint32 space);
extern void chunks_packs_table_runs_insert(StdVec *runs, uint32 space, ChunksPacksTail tail);
extern void chunks_packs_Table_whole(const ChunksPacksTable *self, StdVec *result);
extern void chunks_packs_Table_added(const ChunksPacksTableEntry *entry, ChunksPacksTail runs, StdVec *result);
extern void chunks_packs_Table_reserved(uint64 bound, StdVec *result);
extern void chunks_packs_Table_retired(const uint64 *numbers, size_t numbers_len, StdVec *result);
extern void chunks_packs_Table_vector(uint64 number, uint64 generation, StdVec *result);
extern void chunks_packs_Table_moved(uint32 space, ChunksPacksTail runs, StdVec *result);
extern bool chunks_packs_Table_apply(ChunksPacksTable *self, const uint8 *record, size_t record_len,
									 ChunksErrorError *error);
extern void chunks_packs_Runs_Default_default(ChunksPacksRuns *result);
extern void chunks_packs_Runs_drop(ChunksPacksRuns *self);
extern bool chunks_packs_Runs_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_Runs_PartialEq_eq(const ChunksPacksRuns *self, const ChunksPacksRuns *other);
extern void chunks_packs_Runs_whole(const ChunksPacksRuns *self, StdVec *result);
extern void chunks_packs_Runs_added(const char *line, size_t line_len, StdVec *result);
extern void chunks_packs_Runs_removed(const StdString *names, size_t names_len, StdVec *result);
extern void chunks_packs_Runs_replaced(const StdString *names, size_t names_len, const char *line, size_t line_len,
									   StdVec *result);
extern bool chunks_packs_Runs_apply(ChunksPacksRuns *self, const uint8 *record, size_t record_len,
									ChunksErrorError *error);
extern bool chunks_packs_followed(HostIoReadDoors io, const ChunksIndexAddressIndexLocator *forwards,
								  const ChunksPacksTable *table, ChunksTrailerLocator copy, ChunksTrailerLocator *result,
								  ChunksErrorError *error);
extern void chunks_packs_forward_entries(const chunks_TupleLocatorLocator_t *moves, size_t moves_len,
										 StdVec *result);
extern bool chunks_packs_kept_forwards(HostIoIoDoors io, const ChunksIndexAddressIndexLocator *forwards,
									   const std_TupleU64U64_t *dropped, size_t dropped_len, StdVec *result,
									   ChunksErrorError *error);
extern StdStr chunks_packs_run_name(const char *line, size_t line_len);
extern bool chunks_packs_lossy(const char *line, size_t line_len);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_tests_a_table_read_in_steps_from_an_offset_is_the_table_read_whole(void);
extern void test_chunks_packs_tests_a_whole_record_of_a_table_keeps_every_runs_tail(void);
extern void test_chunks_packs_tests_a_tables_records_name_each_packs_deletion_vector(void);
extern void test_chunks_packs_tests_a_table_record_no_writer_makes_is_refused(void);
extern void test_chunks_packs_tests_a_runs_files_records_add_replace_and_remove_runs(void);
extern void test_chunks_packs_tests_a_runs_forwards_are_in_the_order_of_their_places(void);
extern void test_chunks_packs_tests_a_forward_is_found_through_one_run_and_the_newest_of_two_wins(void);
extern void test_chunks_packs_tests_a_walk_follows_a_chain_to_its_end_and_stops_at_a_listed_pack(void);
extern void test_chunks_packs_tests_a_forward_to_a_pack_that_is_not_later_is_refused_where_it_is_read(void);
extern void test_chunks_packs_tests_a_merge_that_drops_places_keeps_every_other_forward(void);
extern void test_chunks_packs_tests_the_forwarding_file_lists_the_runs_a_walk_reads(void);
extern void test_chunks_packs_tests_a_table_lists_packs_by_their_whole_numbers(void);
extern void test_chunks_packs_tests_a_places_key_holds_its_whole_number_and_offset(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_H */
