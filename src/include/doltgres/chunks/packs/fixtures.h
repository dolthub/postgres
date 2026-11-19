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
 * What the tests of a repository's pack record, its views, and its changes
 * share (chunks' packs/fixtures.rs): packs of one chunk, list files written by
 * hand, and a snapshot installed as a change installs it.
 */
#ifndef DOLTGRES_CHUNKS_PACKS_FIXTURES_H
#define DOLTGRES_CHUNKS_PACKS_FIXTURES_H

#ifdef DOLTGRES_TEST_CHUNKS

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/host/shared.h"
#include "doltgres/simc/simc.h"

/* CHUNKS_PACKS_FIXTURES_REPOSITORY is REPOSITORY: the id of the repository the tests of views and changes make by hand. */
#define CHUNKS_PACKS_FIXTURES_REPOSITORY 7

/* CHUNKS_PACKS_FIXTURES_UNWRAP is Result::unwrap's message on an Err. */
#define CHUNKS_PACKS_FIXTURES_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* ChunksPacksFixturesPacked is a pack of one chunk: the table's entry of it, with the chunk's address and place. */
typedef struct ChunksPacksFixturesPacked
{
	ChunksPacksTableEntry entry;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
} ChunksPacksFixturesPacked;

/* ChunksPacksFixturesLists is Lists: where the next record of each of a test repository's list files goes. */
typedef struct ChunksPacksFixturesLists
{
	ChunksFileTornTail table;
	ChunksFileTornTail runs;
	ChunksFileTornTail forwarding;
} ChunksPacksFixturesLists;


/* ChunksPacksTestsResult is a Result to format: whether it is Ok, and its value or its error. */
typedef struct ChunksPacksTestsResult
{
	bool		ok;
	StdDebugRef value;
} ChunksPacksTestsResult;

extern ChunksPacksFixturesPacked chunks_packs_fixtures_pack(HostIoIo *io, const StdPathBuf *dir, uint64 number);
extern void chunks_packs_fixtures_stored(const uint8 *body, size_t body_len, ChunksAddressAddress *address,
										 StdVec *stored);
extern ChunksPacksFixturesPacked chunks_packs_fixtures_pack_of(HostIoIo *io, const StdPathBuf *dir, uint64 number,
															   const uint8 *body, size_t body_len);
extern void chunks_packs_fixtures_home(StdPathBuf *result);
extern void chunks_packs_fixtures_created(HostIoIo *io, uint32 id, ChunksPacksBackendPacks *result);
extern chunks_TupleAddressLocator_t chunks_packs_fixtures_added(HostIoIo *io, const ChunksPacksBackendPacks *packs,
																const uint8 *body, size_t body_len);
extern bool chunks_packs_fixtures_dead(HostIoIo *io, const ChunksPacksBackendPacks *packs, uint64 number,
									   uint64 generation, const uint64 *offsets, size_t offsets_len, bool *result,
									   ChunksErrorError *error);
extern bool chunks_packs_fixtures_queued(HostIoIo *io, const HostSharedReader *reading, bool retired_ok,
										 ChunksPacksRetireRetired *retired, StdVec *result, ChunksErrorError *error);
extern StdString chunks_packs_fixtures_line(const ChunksPacksTableEntry *entry);
extern void chunks_packs_fixtures_Lists_drop(ChunksPacksFixturesLists *self);
extern ChunksPacksFixturesLists chunks_packs_fixtures_written(HostIoIoDoors io, const StdPathBuf *dir,
															  uint64 generation, const ChunksPacksTableEntry *packs,
															  size_t packs_len);
extern void chunks_packs_fixtures_install(HostSharedGuard *state, HostSharedRecord record,
										  const ChunksPacksFixturesLists *lists, StdVec *deferred);
extern void chunks_packs_fixtures_view(ChunksPacksViewView *result);

extern void chunks_packs_tests_unwrapped(const char *file, const ChunksErrorError *error, uint32 line, uint32 col);
extern void chunks_packs_tests_host_unwrapped(const char *file, const HostError *error, uint32 line, uint32 col);
extern void chunks_packs_tests_listed(const char *file, const ChunksPacksViewView *view,
									  const ChunksPacksTableEntry *want, size_t want_len, uint32 line, uint32 col);
extern bool chunks_packs_tests_Numbers_Debug_fmt(const void *value, StdFormatter *f);
extern void chunks_packs_tests_keys(const char *file, const ChunksPacksViewView *view, const uint64 *want,
									size_t want_len, const StdFmtArguments *message, uint32 line, uint32 col);
extern const ChunksIndexAddressIndexLocator *chunks_packs_tests_index(const char *file,
																	  const ChunksPacksViewView *view, uint32 space,
																	  uint32 line, uint32 col);
extern void chunks_packs_tests_got(const char *file, const ChunksPacksViewView *view, HostIoIo *io, uint32 space,
								   const ChunksAddressAddress *address, const ChunksTrailerLocator *at, uint32 line,
								   const uint32 *cols);
extern StdArcInner *const *chunks_packs_tests_runs(const char *file, const ChunksPacksViewView *view, uint32 space,
												   uint32 line, uint32 col, size_t *result_len);
extern StdArcInner *chunks_packs_tests_run(const char *file, const ChunksPacksViewView *view, size_t place,
										   uint32 line, uint32 unwrap_col, uint32 index_col);
extern void chunks_packs_tests_refreshed(const char *file, ChunksPacksViewView *view, HostIoIo *io,
										 HostSharedRecord record, uint32 line, uint32 col);
extern void chunks_packs_tests_installed(const char *file, HostIoIo *io, HostSharedRecord record,
										 const ChunksPacksFixturesLists *lists, StdVec *deferred, uint32 line,
										 uint32 col);
extern void chunks_packs_tests_appended(const char *file, HostIoIo *io, ChunksFileTornTail *torn, StdVec *record,
										uint32 line, uint32 col);
extern HostSharedOwned chunks_packs_tests_made(const char *file, HostIoIo *io, uint32 line, uint32 col);
extern StdVec chunks_packs_tests_no_deferred(void);
extern const ChunksPacksTail *chunks_packs_tests_runs_at(const ChunksPacksTable *table, uint32 space);
extern HostError chunks_packs_tests_failed(StdString *message);
extern HostError chunks_packs_tests_failed_text(const char *text);
extern HostError chunks_packs_tests_raised(ChunksErrorError *error);
extern bool chunks_packs_tests_returned(const SimcOutcome *outcome);
extern void chunks_packs_tests_ended(const char *file, const SimcReport *report, uint32 line, uint32 col);
extern void chunks_packs_tests_terminated(const char *file, const SimcReport *report, uint32 else_line,
										  uint32 else_col, uint32 line, uint32 index_col, uint32 col);
extern bool chunks_packs_tests_Result_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_packs_tests_Endings_Debug_fmt(const void *value, StdFormatter *f);
extern HostSharedReader chunks_packs_tests_reading(const char *file, HostIoIo *io, uint32 line, uint32 col);
extern void chunks_packs_tests_opened(const char *file, HostIoIo *io, uint32 id, uint32 line, uint32 id_col,
									  uint32 open_col, ChunksPacksBackendPacks *result);
extern void chunks_packs_tests_numbers_eq(const char *file, const ChunksPacksBackendPacks *packs, HostIoIo *io,
										  const HostSharedReader *reading, const uint64 *want, size_t want_len,
										  const StdFmtArguments *message, uint32 line, uint32 assert_col,
										  uint32 unwrap_col);
extern void chunks_packs_tests_found(const char *file, const ChunksPacksBackendPacks *packs, HostIoIo *io,
									 const HostSharedReader *reading, const ChunksAddressAddress *address,
									 const ChunksTrailerLocator *at, uint32 line, uint32 assert_col, uint32 unwrap_col);
extern void chunks_packs_tests_freed(const char *file, HostIoIo *io, uint64 location, uint32 line, uint32 claim_col,
									 uint32 some_col);
extern ChunksPackPackWriterInDir chunks_packs_tests_writer(const char *file, const ChunksPacksBackendPacks *packs,
														   HostIoIo *io, const HostSharedReader *reading, uint32 line,
														   uint32 col);

#endif

#endif							/* DOLTGRES_CHUNKS_PACKS_FIXTURES_H */
