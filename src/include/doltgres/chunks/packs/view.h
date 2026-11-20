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
 * A backend's view of a repository's packs (chunks' packs/view.rs), which
 * only the packs' own modules read.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_VIEW_H
#define DOLTGRES_CHUNKS_PACKS_VIEW_H

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/path.h"

/* ChunksPacksViewListOps is the List trait: a list file's records applied in order, a pack table or a runs file. */
typedef struct ChunksPacksViewListOps
{
	/*
	 * generation returns the generation of the whole write the records
	 * follow.
	 */
	uint64		(*generation) (const void *list);
	/* apply changes the list as one record of its file says. */
	bool		(*apply) (void *list, const uint8 *record, size_t record_len, ChunksErrorError *error);
	/* renew gives the list the generation of a new whole write. */
	void		(*renew) (void *list, uint64 generation);
	/* whole returns the record that begins the list's file. */
	void		(*whole) (const void *list, StdVec *result);
	/* reset makes the list its Default, dropping what it held. */
	void		(*reset) (void *list);

	/*
	 * init makes a place that holds nothing the list's Default, for a list
	 * moved out of it.
	 */
	void		(*init) (void *list);
} ChunksPacksViewListOps;

extern const ChunksPacksViewListOps CHUNKS_PACKS_VIEW_TABLE_LIST_OPS;
extern const ChunksPacksViewListOps CHUNKS_PACKS_VIEW_RUNS_LIST_OPS;

/*
 * ChunksPacksViewRead is the part of a Read<L> every list shares: where the
 * view's next read of the file begins, zero before its first, and how many
 * records it read past the file's whole.
 */
typedef struct ChunksPacksViewRead
{
	uint64		at;
	uint64		past;
} ChunksPacksViewRead;

/* ChunksPacksViewReadTable is Read<Table>. */
typedef struct ChunksPacksViewReadTable
{
	ChunksPacksViewRead read;
	ChunksPacksTable list;
} ChunksPacksViewReadTable;

/* ChunksPacksViewReadRuns is Read<Runs>. */
typedef struct ChunksPacksViewReadRuns
{
	ChunksPacksViewRead read;
	ChunksPacksRuns list;
} ChunksPacksViewReadRuns;

/* chunks_TupleU32ReadRuns_t is a tablespace's runs file as the view read it. */
typedef struct chunks_TupleU32ReadRuns_t
{
	uint32		f0;
	ChunksPacksViewReadRuns f1;
} chunks_TupleU32ReadRuns_t;

/* chunks_TupleU64ArcPackReader_t is an open pack by its number, an Arc<PackReader>. */
typedef struct chunks_TupleU64ArcPackReader_t
{
	uint64		f0;
	StdArcInner *f1;
} chunks_TupleU64ArcPackReader_t;

/* chunks_TupleU32AddressIndex_t is a tablespace's address index. */
typedef struct chunks_TupleU32AddressIndex_t
{
	uint32		f0;
	ChunksIndexAddressIndexLocator f1;
} chunks_TupleU32AddressIndex_t;

/* chunks_TupleU64Dead_t is a pack's deletion vector by its number, with the generation it is of. */
typedef struct chunks_TupleU64Dead_t
{
	uint64		f0;
	uint64		generation;
	ChunksDeadDead dead;
} chunks_TupleU64Dead_t;

/* ChunksPacksViewView is View: a backend's view of a repository's packs. */
typedef struct ChunksPacksViewView
{
	/*
	 * dir is the directory of the repository's packs, which holds the pack
	 * table's file and the forwarding file.
	 */
	StdPathBuf	dir;
	uint32		repository;

	/*
	 * has_current and current are the generation of the record the view is
	 * current to, none before its first refresh and after one that failed.
	 */
	bool		has_current;
	uint64		current;
	ChunksPacksViewReadTable table;

	/*
	 * runs is a BTreeMap<u32, Read<Runs>>: chunks_TupleU32ReadRuns_t in
	 * tablespace order.
	 */
	StdVec		runs;
	ChunksPacksViewReadRuns forwarding;

	/*
	 * deferred is a Vec<TableEntry> of the packs added that the list files
	 * don't hold yet.
	 */
	StdVec		deferred;

	/*
	 * readers is a BTreeMap<u64, Arc<PackReader>>:
	 * chunks_TupleU64ArcPackReader_t in number order.
	 */
	StdVec		readers;

	/*
	 * indexes is a BTreeMap<u32, AddressIndex>: chunks_TupleU32AddressIndex_t
	 * in tablespace order.
	 */
	StdVec		indexes;
	ChunksIndexAddressIndexLocator forwards;

	/*
	 * dead is a BTreeMap<u64, (NonZeroU64, Dead)>: chunks_TupleU64Dead_t in
	 * number order.
	 */
	StdVec		dead;

	/*
	 * missing is a BTreeSet<u32> of the tablespaces whose directory the view
	 * found missing, in order.
	 */
	StdVec		missing;
} ChunksPacksViewView;

extern void chunks_packs_view_Read_bring_reset(const ChunksPacksViewListOps *ops, ChunksPacksViewRead *read,
											   void *list);
extern void chunks_packs_view_View_Default_default(ChunksPacksViewView *result);
extern void chunks_packs_view_View_new(uint32 repository, uint32 own, ChunksPacksViewView *result);
extern void chunks_packs_view_View_drop(ChunksPacksViewView *self);
extern void chunks_packs_view_View_space_dir(const ChunksPacksViewView *self, uint32 space, StdPathBuf *result);
extern bool chunks_packs_view_View_refresh(ChunksPacksViewView *self, HostIoIo *io, HostSharedRecord record,
										   ChunksErrorError *error);
extern bool chunks_packs_view_View_refresh_under(ChunksPacksViewView *self, HostSharedLongIo *io,
												 HostSharedRecord record, ChunksPacksSharedSnapshot *result,
												 ChunksErrorError *error);
extern void chunks_packs_view_View_forget(ChunksPacksViewView *self);
extern ChunksErrorError chunks_packs_view_View_unread(const ChunksPacksViewView *self, uint32 space,
													  const ChunksPackPackId *id);
extern bool chunks_packs_view_View_open(ChunksPacksViewView *self, HostIoReadDoors io,
										const ChunksPacksTableEntry *deferred, size_t deferred_len,
										ChunksErrorError *error);
extern StdArcInner *const *chunks_packs_view_View_pack(const ChunksPacksViewView *self, uint64 number);
extern void chunks_packs_view_View_listed(const ChunksPacksViewView *self, StdVec *result);
extern const ChunksIndexAddressIndexLocator *chunks_packs_view_View_index(const ChunksPacksViewView *self,
																		  uint32 space);
extern const ChunksIndexAddressIndexLocator *chunks_packs_view_View_forwards(const ChunksPacksViewView *self);
extern const ChunksPacksTable *chunks_packs_view_View_table(const ChunksPacksViewView *self);
extern ChunksPacksViewReadRuns *chunks_packs_view_runs_get(const StdVec *runs, uint32 space);
extern ChunksPacksViewReadRuns *chunks_packs_view_runs_entry(StdVec *runs, uint32 space);
extern bool chunks_packs_view_missing_contains(const StdVec *missing, uint32 space);

/* reads.c's reads through a view. */
extern bool chunks_packs_reads_View_is_dead(ChunksPacksViewView *self, HostIoReadDoors io, ChunksTrailerLocator locator,
											bool *result, ChunksErrorError *error);
extern bool chunks_packs_reads_View_records(ChunksPacksViewView *self, HostIoReadDoors io,
											const ChunksAddressAddress *address, StdVec *result,
											ChunksErrorError *error);
extern bool chunks_packs_reads_View_find(ChunksPacksViewView *self, HostIoReadDoors io,
										 const ChunksAddressAddress *address, bool *has_result,
										 ChunksTrailerLocator *result, ChunksErrorError *error);
extern bool chunks_packs_reads_View_find_in(ChunksPacksViewView *self, HostIoReadDoors io,
											const ChunksAddressAddress *address, uint32 space, bool *has_result,
											ChunksTrailerLocator *result, ChunksErrorError *error);
extern bool chunks_packs_reads_View_read(ChunksPacksViewView *self, HostIoReadDoors io,
										 const ChunksAddressAddress *address, ChunksTrailerLocator locator,
										 ChunksChunkChunk *result, ChunksErrorError *error);

#ifdef DOLTGRES_TEST_CHUNKS
extern uint64 chunks_packs_view_rereads(void);
extern void chunks_packs_view_count_reread(void);
#endif

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_view_tests_a_view_reads_the_lists_to_their_tails_and_then_what_was_appended_since(void);
extern void test_chunks_packs_view_tests_a_view_reads_a_file_written_whole_since_from_its_start(void);
extern void test_chunks_packs_view_tests_a_view_lists_the_packs_its_snapshot_defers(void);
#endif

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_packs_reads_tests_a_find_answers_a_live_copy_over_a_dead_one(void);
extern void test_chunks_packs_reads_tests_a_read_follows_its_locator_the_forwards_or_the_index(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_VIEW_H */
