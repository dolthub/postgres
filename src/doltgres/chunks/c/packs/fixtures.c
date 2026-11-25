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
 * The C port of chunks' packs/fixtures.rs: what the tests of a repository's
 * pack record, its views, and its changes share.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef DOLTGRES_TEST_CHUNKS

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/marking.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/retire.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "fixtures.rs"

/* chunks_packs_fixtures_unwrapped panics as Result::unwrap does on a chunks error, at a line and column. */
static void
chunks_packs_fixtures_unwrapped(const ChunksErrorError *error, uint32 line, uint32 col)
{
	STD_RESULT_EXPECT_FAILED(line, col, CHUNKS_PACKS_FIXTURES_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/*
 * chunks_packs_fixtures_pack writes a pack of one chunk in a directory at a
 * number, and returns the table's entry of it with the chunk's address and
 * place.
 */
ChunksPacksFixturesPacked
chunks_packs_fixtures_pack(HostIoIo *io, const StdPathBuf *dir, uint64 number)
{
	uint8		body[8];

	std_u64_to_le_bytes(number, body);
	return chunks_packs_fixtures_pack_of(io, dir, number, body, sizeof(body));
}

/* chunks_packs_fixtures_stored returns the address and the stored bytes of a leaf chunk with a body. */
void
chunks_packs_fixtures_stored(const uint8 *body, size_t body_len, ChunksAddressAddress *address, StdVec *stored)
{
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF,
	.addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS}};
	ChunksTrailerStamps stamps = {0};
	StdVec		hashed;

	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, body, body_len, &hashed);
	*address = chunks_address_Address_of(hashed.ptr, hashed.len);
	chunks_trailer_store(hashed.ptr, hashed.len, NULL, 0, &stamps, stored);
	std_vec_free_Checked(&hashed, 1, 1);
}

/*
 * chunks_packs_fixtures_pack_of writes a pack of one chunk with a body in a
 * directory at a number, and returns the table's entry of it with the
 * chunk's address and place, so two packs can hold a copy of one chunk.
 */
ChunksPacksFixturesPacked
chunks_packs_fixtures_pack_of(HostIoIo *io, const StdPathBuf *dir, uint64 number, const uint8 *body,
							  size_t body_len)
{
	ChunksPacksFixturesPacked packed = {.entry = {number}};
	ChunksPackPackWriterInDir writer;
	ChunksPackPack pack;
	ChunksErrorError error;
	StdVec		bytes;

	if (!chunks_pack_PackWriterInDir_create(io, dir->bytes.ptr, dir->bytes.len, number, &writer, &error))
		chunks_packs_fixtures_unwrapped(&error, 49, 58);
	chunks_packs_fixtures_stored(body, body_len, &packed.address, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, packed.address, bytes.ptr, bytes.len, false, &packed.at,
										   &error))
		chunks_packs_fixtures_unwrapped(&error, 51, 55);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
		chunks_packs_fixtures_unwrapped(&error, 52, 36);
	packed.entry.f2 = pack.id;
	chunks_pack_Pack_drop(&pack);
	return packed;
}

/*
 * chunks_packs_fixtures_home makes the directory of the tests' repository in
 * pg_default, in the test's data directory, and returns it.
 */
void
chunks_packs_fixtures_home(StdPathBuf *result)
{
	StdIoError	error;

	*result = host_tablespace_dir(CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT);
	if (!std_fs_create_dir_all(result->bytes.ptr, result->bytes.len, &error))
		STD_RESULT_EXPECT_FAILED(63, 35, CHUNKS_PACKS_FIXTURES_UNWRAP, &error, std_io_Error_Debug_fmt);
}

/* chunks_packs_fixtures_created returns the packs of a new repository with an id, in pg_default. */
void
chunks_packs_fixtures_created(HostIoIo *io, uint32 id, ChunksPacksBackendPacks *result)
{
	ChunksErrorError error;

	if (id == 0)
		STD_OPTION_UNWRAP_FAILED(69, 53);
	if (!chunks_packs_open_Packs_create(io, id, HOST_PG_DEFAULT, result, &error))
		chunks_packs_fixtures_unwrapped(&error, 69, 81);
}

/* chunks_packs_fixtures_restarted frees a repository's record, as a restart makes shared memory anew, taking its packs. */
void
chunks_packs_fixtures_restarted(HostIoIo *io, ChunksPacksBackendPacks *packs)
{
	uint64		location = chunks_packs_backend_Packs_location(packs);

	chunks_packs_backend_Packs_drop(packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 76, 71, 80);
}

/*
 * chunks_packs_fixtures_added writes a pack of one chunk with a body through
 * a repository's packs, lists it, and returns the chunk's address and place.
 */
chunks_TupleAddressLocator_t
chunks_packs_fixtures_added(HostIoIo *io, const ChunksPacksBackendPacks *packs, const uint8 *body, size_t body_len)
{
	chunks_TupleAddressLocator_t added;
	HostSharedReader reading;
	ChunksPackPackWriterInDir writer;
	ChunksPackPack pack;
	ChunksErrorError error;
	HostError	host;
	StdVec		bytes;

	if (!host_shared_read(io, &reading, &host))
		STD_RESULT_EXPECT_FAILED(75, 42, CHUNKS_PACKS_FIXTURES_UNWRAP, &host, host_Error_Debug_fmt);
	if (!chunks_packs_backend_Packs_writer(packs, io, &reading, 0, &writer, &error))
		chunks_packs_fixtures_unwrapped(&error, 76, 52);
	chunks_packs_fixtures_stored(body, body_len, &added.f0, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, added.f0, bytes.ptr, bytes.len, false, &added.f1, &error))
		chunks_packs_fixtures_unwrapped(&error, 78, 55);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
		chunks_packs_fixtures_unwrapped(&error, 79, 34);
	if (!chunks_packs_backend_Packs_add(packs, io, &reading, &pack, &error))
		chunks_packs_fixtures_unwrapped(&error, 80, 35);
	host_shared_Reader_drop(&reading);
	return added;
}

/* chunks_packs_fixtures_dead_t is dead's keep's captures: the pack, its vector's generation and offsets, and whether the vector was set. */
typedef struct chunks_packs_fixtures_dead_t
{
	uint64		number;
	uint64		generation;
	const StdBTreeMap *offsets;
	bool		set;
} chunks_packs_fixtures_dead_t;

static bool chunks_packs_fixtures_dead_closure_0(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
												 StdVec *result, ChunksErrorError *error)
			STD_REQUIRES(CHUNKS_PACKS_BARRIER_HOLDING);

/* chunks_packs_fixtures_dead_closure_0 is dead's keep: the pack's vector set, and no pack kept. */
static bool
chunks_packs_fixtures_dead_closure_0(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
									 StdVec *result, ChunksErrorError *error)
{
	chunks_packs_fixtures_dead_t *dead = captures;

	(void) result;
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return chunks_packs_retire_Marking_set_dead(marking, dead->number, dead->generation, dead->offsets, &dead->set,
												error);
}

/*
 * chunks_packs_fixtures_dead makes the chunks at offsets of a listed pack
 * dead with a generation of its deletion vector, as a collection that marks
 * the repository and retires nothing does, and reports whether the vector
 * was set.
 */
bool
chunks_packs_fixtures_dead(HostIoIo *io, const ChunksPacksBackendPacks *packs, uint64 number, uint64 generation,
						   const uint64 *offsets, size_t offsets_len, bool *result, ChunksErrorError *error)
{
	HostSharedReader reading;
	HostError	host;
	uint64		newest;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	chunks_packs_fixtures_dead_t dead = {.number = number, .generation = generation};
	StdVec		listed;
	StdVec		numbers;
	StdBTreeMap set;
	bool		ok;

	if (!host_shared_read(io, &reading, &host))
		STD_RESULT_EXPECT_FAILED(87, 42, CHUNKS_PACKS_FIXTURES_UNWRAP, &host, host_Error_Debug_fmt);
	ok = chunks_packs_marking_Packs_begin_marking(packs, io, &reading, &newest, error) &&
		chunks_packs_removal_reserve(io, 1, &reservation, error);
	if (ok && !chunks_packs_backend_Packs_forward(packs, io, &reading, NULL, 0, &forwarded, error))
	{
		chunks_packs_removal_Reservation_drop(&reservation);
		ok = false;
	}
	if (ok)
	{
		if (generation == 0)
			STD_OPTION_UNWRAP_FAILED(90, 62);
		std_slice_to_vec_Checked(&listed, offsets, offsets_len, sizeof(uint64), _Alignof(uint64));
		set = std_btree_BTreeSetU64_FromArray_from(listed.ptr, listed.len);
		std_vec_free_Checked(&listed, sizeof(uint64), _Alignof(uint64));
		dead.offsets = &set;
		ok = chunks_packs_retire_Packs_retire(packs, io, NULL, 0, &forwarded, &reservation,
											  chunks_packs_fixtures_dead_closure_0, &dead, NULL, &retired, error);
		ok = chunks_packs_fixtures_queued(io, &reading, ok, &retired, &numbers, error);
		if (ok)
			std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
		std_btree_BTreeMapU64Set_drop(&set);
		*result = dead.set;
	}
	host_shared_Reader_drop(&reading);
	return ok;
}

/*
 * chunks_packs_fixtures_queued queues a retirement's files at once, as a
 * collection with nothing to restore does, and returns the numbers it took
 * out; a retirement that failed fails it with its error. It takes the
 * retirement.
 */
bool
chunks_packs_fixtures_queued(HostIoIo *io, const HostSharedReader *reading, bool retired_ok,
							 ChunksPacksRetireRetired *retired, StdVec *result, ChunksErrorError *error)
{
	size_t		len;
	const uint64 *numbers;

	if (!retired_ok)
		return false;
	numbers = chunks_packs_retire_Retired_numbers(retired, &len);
	std_slice_to_vec_Checked(result, numbers, len, sizeof(uint64), _Alignof(uint64));
	if (chunks_packs_retire_Retired_queue(retired, io, reading, error))
		return true;
	std_vec_free_Checked(result, sizeof(uint64), _Alignof(uint64));
	return false;
}

/* chunks_packs_fixtures_line returns the line of a runs file for a pack's run. */
StdString
chunks_packs_fixtures_line(const ChunksPacksTableEntry *entry)
{
	StdString	name = chunks_pack_PackId_file_name(&entry->f2, "idx", 3);
	StdString	line;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&name, std_String_Display_fmt}, {&entry->f0, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&line, &arguments);
	std_vec_free_Checked(&name, 1, 1);
	return line;
}

/* chunks_packs_fixtures_Lists_drop is Lists' drop glue: each file's tail. */
void
chunks_packs_fixtures_Lists_drop(ChunksPacksFixturesLists *self)
{
	chunks_file_TornTail_drop(&self->table);
	chunks_file_TornTail_drop(&self->runs);
	chunks_file_TornTail_drop(&self->forwarding);
}

/* chunks_packs_fixtures_appended writes a list file of one whole record in a directory, of a generation. */
static ChunksFileTornTail
chunks_packs_fixtures_appended(HostIoIoDoors io, const StdPathBuf *dir, const char *name, const StdVec *whole,
							   uint64 generation, uint32 line, uint32 col)
{
	StdPathBuf	path;
	ChunksFileTornTail written;
	ChunksErrorError error;
	bool		placing = false;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	if (!chunks_file_write_appended(io, path.bytes.ptr, path.bytes.len, whole->ptr, whole->len, &placing, &written,
									&error))
		chunks_packs_fixtures_unwrapped(&error, line, col);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return chunks_file_TornTail_of_generation(&written, generation);
}

/*
 * chunks_packs_fixtures_written writes a repository's three list files whole
 * in its directory, each of a generation, with the table and the runs file
 * listing packs, and returns where their next records go.
 */
ChunksPacksFixturesLists
chunks_packs_fixtures_written(HostIoIoDoors io, const StdPathBuf *dir, uint64 generation,
							  const ChunksPacksTableEntry *packs, size_t packs_len)
{
	ChunksPacksFixturesLists lists;
	ChunksPacksRuns runs = {generation};
	ChunksPacksTable table;
	ChunksPacksTail runs_tail;
	StdVec		whole;

	std_vec_new(&runs.lines, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0; k < packs_len; k++)
	{
		StdString	line = chunks_packs_fixtures_line(&packs[k]);

		std_vec_push_Checked(&runs.lines, &line, sizeof(line), _Alignof(StdString));
	}
	chunks_packs_Runs_whole(&runs, &whole);
	chunks_packs_Runs_drop(&runs);
	lists.runs = chunks_packs_fixtures_appended(io, dir, CHUNKS_PACKS_RUNS, &whole, generation, 117, 72);
	std_vec_free_Checked(&whole, 1, 1);
	chunks_packs_Table_Default_default(&table);
	table.generation = generation;
	for (size_t k = 0; k < packs_len; k++)
	{
		ChunksPacksListed listed = {.space = packs[k].f1, .id = packs[k].f2};

		table.added = std_max_u64(table.added, packs[k].f0);
		chunks_packs_table_packs_insert(&table.packs, packs[k].f0, &listed);
	}
	runs_tail.end = chunks_file_TornTail_end(&lists.runs);
	runs_tail.generation = generation;
	chunks_packs_table_runs_insert(&table.runs, 0, runs_tail);
	chunks_packs_Table_whole(&table, &whole);
	chunks_packs_Table_drop(&table);
	lists.table = chunks_packs_fixtures_appended(io, dir, CHUNKS_PACKS_TABLE, &whole, generation, 125, 82);
	std_vec_free_Checked(&whole, 1, 1);
	runs.generation = generation;
	std_vec_new(&runs.lines, sizeof(StdString), _Alignof(StdString));
	chunks_packs_Runs_whole(&runs, &whole);
	chunks_packs_Runs_drop(&runs);
	lists.forwarding = chunks_packs_fixtures_appended(io, dir, CHUNKS_PACKS_FORWARDING, &whole, generation, 127, 83);
	std_vec_free_Checked(&whole, 1, 1);
	return lists;
}

/* chunks_packs_fixtures_tail returns the list tail of where a list file's next record goes, with nothing appended. */
static ChunksPacksSharedListTail
chunks_packs_fixtures_tail(const ChunksFileTornTail *end)
{
	ChunksPacksSharedListTail tail = {{chunks_file_TornTail_end(end), chunks_file_TornTail_generation(end)}};

	return tail;
}

/*
 * chunks_packs_fixtures_install writes the record's snapshot for where the
 * lists' next records go, with deferred adds, which it takes, and advances
 * its generation, as a change does under the state lock, whose guard it
 * takes.
 */
void
chunks_packs_fixtures_install(HostSharedGuard *state, HostSharedRecord record, const ChunksPacksFixturesLists *lists,
							  StdVec *deferred)
{
	ChunksPacksSharedSnapshot snapshot = {chunks_packs_fixtures_tail(&lists->table),
	chunks_packs_fixtures_tail(&lists->forwarding), *deferred};
	StdVec		bytes;
	HostError	error;

	chunks_packs_shared_Snapshot_bytes(&snapshot, &bytes);
	if (!host_shared_GuardExclusive_write_blob(state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB, bytes.ptr, bytes.len, &error))
		STD_RESULT_EXPECT_FAILED(141, 58, CHUNKS_PACKS_FIXTURES_UNWRAP, &error, host_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, 1, 1);
	chunks_packs_shared_Snapshot_drop(&snapshot);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_GENERATION_INDEX, 1);
	host_shared_Guard_drop(state);
}

/* chunks_packs_fixtures_view returns a view of the packs of the tests' repository. */
void
chunks_packs_fixtures_view(ChunksPacksViewView *result)
{
	chunks_packs_view_View_new(CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT, result);
}

/*
 * chunks_packs_tests_unwrapped panics as Result::unwrap does on a chunks
 * error, at a line and column of a test's Rust file.
 */
void
chunks_packs_tests_unwrapped(const char *file, const ChunksErrorError *error, uint32 line, uint32 col)
{
	std_result_unwrap_failed(CHUNKS_PACKS_FIXTURES_UNWRAP, sizeof(CHUNKS_PACKS_FIXTURES_UNWRAP) - 1, error,
							 chunks_error_Error_Debug_fmt, file, line, col);
}

/*
 * chunks_packs_tests_host_unwrapped panics as Result::unwrap does on a host
 * error, at a line and column of a test's Rust file.
 */
void
chunks_packs_tests_host_unwrapped(const char *file, const HostError *error, uint32 line, uint32 col)
{
	std_result_unwrap_failed(CHUNKS_PACKS_FIXTURES_UNWRAP, sizeof(CHUNKS_PACKS_FIXTURES_UNWRAP) - 1, error,
							 host_Error_Debug_fmt, file, line, col);
}

/* chunks_packs_tests_Placed_Debug_fmt is a listed pack's (u32, PackId)'s Debug: its tablespace and id, in a tuple. */
static bool
chunks_packs_tests_Placed_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksTableEntry *entry = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &entry->f1, std_u32_Debug_fmt);
	std_DebugTuple_field(&b, &entry->f2, chunks_pack_PackId_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_tests_Listed_Debug_fmt is listed's BTreeMap<u64, (u32, PackId)>'s Debug, of its entries in order. */
static bool
chunks_packs_tests_Listed_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *listed = value;
	StdDebugMap m = std_fmt_debug_map(f);

	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *entry = &STD_VEC_AT(listed, ChunksPacksTableEntry, k);

		std_DebugMap_entry(&m, &entry->f0, std_u64_Debug_fmt, entry, chunks_packs_tests_Placed_Debug_fmt);
	}
	return std_DebugMap_finish(&m);
}

/* chunks_packs_tests_Listed_PartialEq_eq is listed's BTreeMap's PartialEq: the same entries in order. */
static bool
chunks_packs_tests_Listed_PartialEq_eq(const StdVec *self, const StdVec *other)
{
	if (self->len != other->len)
		return false;
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *a = &STD_VEC_AT(self, ChunksPacksTableEntry, k);
		const ChunksPacksTableEntry *b = &STD_VEC_AT(other, ChunksPacksTableEntry, k);

		if (a->f0 != b->f0 || a->f1 != b->f1 || !chunks_pack_PackId_PartialEq_eq(&a->f2, &b->f2))
			return false;
	}
	return true;
}

/*
 * chunks_packs_tests_listed asserts at a line and column of a test's Rust
 * file that a view lists exactly the entries given.
 */
void
chunks_packs_tests_listed(const char *file, const ChunksPacksViewView *view, const ChunksPacksTableEntry *want,
						  size_t want_len, uint32 line, uint32 col)
{
	StdVec		got;
	StdVec		wanted;

	chunks_packs_view_View_listed(view, &got);
	std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	if (!chunks_packs_tests_Listed_PartialEq_eq(&got, &wanted))
		std_assert_failed(file, line, col, "==", &got, chunks_packs_tests_Listed_Debug_fmt, &wanted,
						  chunks_packs_tests_Listed_Debug_fmt, NULL);
	std_vec_free_Checked(&wanted, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_free_Checked(&got, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
}

/* chunks_packs_tests_Numbers_Debug_fmt is a Vec<u64>'s Debug, or a [u64]'s, which reads alike. */
bool
chunks_packs_tests_Numbers_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *numbers = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = numbers->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(numbers, uint64, k), std_u64_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * chunks_packs_tests_keys asserts at a line and column of a test's Rust file
 * that a view lists the packs numbered as given, in order, with a message,
 * or none when it is NULL.
 */
void
chunks_packs_tests_keys(const char *file, const ChunksPacksViewView *view, const uint64 *want, size_t want_len,
						const StdFmtArguments *message, uint32 line, uint32 col)
{
	StdVec		listed;
	StdVec		got;
	StdVec		wanted;
	bool		same;

	chunks_packs_view_View_listed(view, &listed);
	std_vec_new(&got, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = listed.len; k < k_len; k++)
		std_vec_push_Checked(&got, &STD_VEC_AT(&listed, ChunksPacksTableEntry, k).f0, sizeof(uint64),
							 _Alignof(uint64));
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(uint64), _Alignof(uint64));
	same = got.len == wanted.len && memcmp(got.ptr, wanted.ptr, got.len * sizeof(uint64)) == 0;
	if (!same)
		std_assert_failed(file, line, col, "==", &got, chunks_packs_tests_Numbers_Debug_fmt, &wanted,
						  chunks_packs_tests_Numbers_Debug_fmt, message);
	std_vec_free_Checked(&wanted, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&got, sizeof(uint64), _Alignof(uint64));
}

/*
 * chunks_packs_tests_index returns a view's index of a tablespace's runs,
 * which unwraps at a line and column of a test's Rust file.
 */
const ChunksIndexAddressIndexLocator *
chunks_packs_tests_index(const char *file, const ChunksPacksViewView *view, uint32 space, uint32 line, uint32 col)
{
	const ChunksIndexAddressIndexLocator *index = chunks_packs_view_View_index(view, space);

	if (index == NULL)
		std_option_unwrap_failed(file, line, col);
	return index;
}

/*
 * chunks_packs_tests_got asserts at a line of a test's Rust file that a
 * view's index of a tablespace finds an address at a place, or nowhere when
 * `at` is NULL, with the assertion's and the two unwraps' columns.
 */
void
chunks_packs_tests_got(const char *file, const ChunksPacksViewView *view, HostIoIo *io, uint32 space,
					   const ChunksAddressAddress *address, const ChunksTrailerLocator *at, uint32 line,
					   const uint32 *cols)
{
	const ChunksIndexAddressIndexLocator *index = chunks_packs_tests_index(file, view, space, line, cols[1]);
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;
	StdOptionRef want = {at != NULL, STD_ITEM(at, chunks_trailer_Locator_Debug_fmt)};

	if (!chunks_index_AddressIndexLocator_get(index, HOST_IO_IO_READ_DOORS(io), address, &has, &found, &error))
		chunks_packs_tests_unwrapped(file, &error, line, cols[2]);
	if (has != (at != NULL) || (has && !chunks_trailer_Locator_PartialEq_eq(&found, at)))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(file, line, cols[0], "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
}

/*
 * chunks_packs_tests_runs returns the runs a view's index of a tablespace
 * holds, which unwraps at a line and column of a test's Rust file.
 */
StdArcInner *const *
chunks_packs_tests_runs(const char *file, const ChunksPacksViewView *view, uint32 space, uint32 line, uint32 col,
						size_t *result_len)
{
	return chunks_index_AddressIndexLocator_runs(chunks_packs_tests_index(file, view, space, line, col), result_len);
}

/*
 * chunks_packs_tests_run returns a run a view's index of pg_default's runs
 * holds, by its place, which unwraps and is indexed at a line and columns of
 * a test's Rust file.
 */
StdArcInner *
chunks_packs_tests_run(const char *file, const ChunksPacksViewView *view, size_t place, uint32 line,
					   uint32 unwrap_col, uint32 index_col)
{
	size_t		runs_len;
	StdArcInner *const *runs = chunks_packs_tests_runs(file, view, 0, line, unwrap_col, &runs_len);

	std_index_check(place, runs_len, file, line, index_col);
	return runs[place];
}

/* chunks_packs_tests_refreshed brings a view up to date with a record, which unwraps at a line and column. */
void
chunks_packs_tests_refreshed(const char *file, ChunksPacksViewView *view, HostIoIo *io, HostSharedRecord record,
							 uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_view_View_refresh(view, HOST_IO_IO_TAKES_SHORT(io), record, &error))
		chunks_packs_tests_unwrapped(file, &error, line, col);
}

/*
 * chunks_packs_tests_installed installs lists' snapshot under a record's
 * state lock, which unwraps at a line and column, taking the deferred adds.
 */
void
chunks_packs_tests_installed(const char *file, HostIoIo *io, HostSharedRecord record,
							 const ChunksPacksFixturesLists *lists, StdVec *deferred, uint32 line, uint32 col)
{
	HostSharedGuard state;
	HostError	error;

	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX, &state,
								 &error))
		chunks_packs_tests_host_unwrapped(file, &error, line, col);
	chunks_packs_fixtures_install(&state, record, lists, deferred);
}

/*
 * chunks_packs_tests_appended appends a record, which it frees, to a list
 * file through its tail, in place, which unwraps at a line and column.
 */
void
chunks_packs_tests_appended(const char *file, HostIoIo *io, ChunksFileTornTail *torn, StdVec *record, uint32 line,
							uint32 col)
{
	ChunksErrorError error;

	if (!chunks_file_append(HOST_IO_IO_IO_DOORS(io), torn, record->ptr, record->len, torn, &error))
		chunks_packs_tests_unwrapped(file, &error, line, col);
	std_vec_free_Checked(record, 1, 1);
}

/* chunks_packs_tests_made makes a pack record, which unwraps at a line and column. */
HostSharedOwned
chunks_packs_tests_made(const char *file, HostIoIo *io, uint32 line, uint32 col)
{
	HostSharedOwned owned;
	HostError	error;

	if (!host_shared_make(&CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, io, &owned, &error))
		chunks_packs_tests_host_unwrapped(file, &error, line, col);
	return owned;
}

/* chunks_packs_tests_no_deferred returns an empty Vec<TableEntry>. */
StdVec
chunks_packs_tests_no_deferred(void)
{
	StdVec		none;

	std_vec_new(&none, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	return none;
}

/*
 * chunks_packs_tests_runs_at returns a table's tail of a tablespace's runs
 * file, which panics as a BTreeMap's index of a missing key does.
 */
const ChunksPacksTail *
chunks_packs_tests_runs_at(const ChunksPacksTable *table, uint32 space)
{
	static const char missing[] = "no entry found for key";
	const ChunksPacksTail *tail = chunks_packs_table_runs_get(&table->runs, space);

	if (tail == NULL)
		std_option_expect_failed(missing, sizeof(missing) - 1, STD_BTREE_DIR "map.rs", 2653, 23);
	return tail;
}

/* chunks_packs_tests_failed returns the host error of an actor's failure with a message, which it takes. */
HostError
chunks_packs_tests_failed(StdString *message)
{
	return host_Error_fork((const uint8 *) "XX000", message);
}

/* chunks_packs_tests_failed_text returns the host error of an actor's failure with a literal message. */
HostError
chunks_packs_tests_failed_text(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return chunks_packs_tests_failed(&message);
}

/* chunks_packs_tests_raised returns the host error of an actor's failure with a chunks error's text, which it takes. */
HostError
chunks_packs_tests_raised(ChunksErrorError *error)
{
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	chunks_error_Error_drop(error);
	return chunks_packs_tests_failed(&message);
}

/* chunks_packs_tests_returned reports whether a run's every actor ended returning Ok. */
bool
chunks_packs_tests_returned(const SimcOutcome *outcome)
{
	if (outcome->tag != SIMC_OUTCOME_ENDED)
		return false;
	for (size_t k = 0, k_len = outcome->u.ended.len; k < k_len; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(&outcome->u.ended, simc_TupleStringEnding_t, k).f1;

		if (ending->tag != SIMC_ENDING_RETURNED || ending->is_err)
			return false;
	}
	return true;
}

/*
 * chunks_packs_tests_ended asserts at a line and column that a run's every
 * actor ended returning Ok, with the outcome's Debug as the message.
 */
void
chunks_packs_tests_ended(const char *file, const SimcReport *report, uint32 line, uint32 col)
{
	if (!chunks_packs_tests_returned(&report->outcome))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, line, col, &arguments);
	}
}

/* chunks_packs_tests_Result_Debug_fmt is a Result's derived Debug. */
bool
chunks_packs_tests_Result_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksTestsResult *result = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, result->ok ? "Ok" : "Err", result->ok ? 2 : 3);

	std_DebugTuple_field(&b, result->value.value, result->value.fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_tests_reading returns a reader of the shared state, which unwraps at a line and column. */
HostSharedReader
chunks_packs_tests_reading(const char *file, HostIoIo *io, uint32 line, uint32 col)
{
	HostSharedReader reading;
	HostError	host;

	if (!host_shared_read(io, &reading, &host))
		chunks_packs_tests_host_unwrapped(file, &host, line, col);
	return reading;
}

/* chunks_packs_tests_opened opens a repository's packs by its id, whose NonZeroU32 and open unwrap at a line and columns. */
void
chunks_packs_tests_opened(const char *file, HostIoIo *io, uint32 id, uint32 line, uint32 id_col, uint32 open_col,
						  ChunksPacksBackendPacks *result)
{
	ChunksErrorError error;

	if (id == 0)
		std_option_unwrap_failed(file, line, id_col);
	if (!chunks_packs_open_Packs_open(io, id, HOST_PG_DEFAULT, result, &error))
		chunks_packs_tests_unwrapped(file, &error, line, open_col);
}

/*
 * chunks_packs_tests_numbers_eq asserts at a line that a repository's
 * packs list the numbers given, with a message or none, which unwraps at a
 * column.
 */
void
chunks_packs_tests_numbers_eq(const char *file, const ChunksPacksBackendPacks *packs, HostIoIo *io, const HostSharedReader *reading,
							  const uint64 *want, size_t want_len, const StdFmtArguments *message, uint32 line,
							  uint32 assert_col, uint32 unwrap_col)
{
	StdVec		numbers;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_numbers(packs, io, reading, &numbers, &error))
		chunks_packs_tests_unwrapped(file, &error, line, unwrap_col);
	if (numbers.len != want_len || (want_len != 0 && memcmp(numbers.ptr, want, want_len * sizeof(uint64)) != 0))
	{
		StdVec		wanted;

		std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(uint64), _Alignof(uint64));
		std_assert_failed(file, line, assert_col, "==", &numbers, chunks_packs_tests_Numbers_Debug_fmt, &wanted,
						  chunks_packs_tests_Numbers_Debug_fmt, message);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_tests_found asserts at a line and column that a repository's packs find an address at a place, which unwraps at another column. */
void
chunks_packs_tests_found(const char *file, const ChunksPacksBackendPacks *packs, HostIoIo *io, const HostSharedReader *reading,
						 const ChunksAddressAddress *address, const ChunksTrailerLocator *at, uint32 line, uint32 assert_col,
						 uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_backend_Packs_find(packs, io, reading, address, &has, &found, &error))
		chunks_packs_tests_unwrapped(file, &error, line, unwrap_col);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&found, at))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(at, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(file, line, assert_col, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
}

/* chunks_packs_tests_freed claims the record at a location and frees it, whose claim and option unwrap at a line and columns. */
void
chunks_packs_tests_freed(const char *file, HostIoIo *io, uint64 location, uint32 line, uint32 claim_col, uint32 some_col)
{
	HostSharedOwned owned;
	HostError	host;
	bool		has;

	if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &owned,
						   &host))
		chunks_packs_tests_host_unwrapped(file, &host, line, claim_col);
	if (!has)
		std_option_unwrap_failed(file, line, some_col);
	host_shared_free(&owned);
}

/* chunks_packs_tests_writer returns a writer of a repository's packs in pg_default, which unwraps at a line and column. */
ChunksPackPackWriterInDir
chunks_packs_tests_writer(const char *file, const ChunksPacksBackendPacks *packs, HostIoIo *io, const HostSharedReader *reading,
						  uint32 line, uint32 col)
{
	ChunksPackPackWriterInDir writer;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_writer(packs, io, reading, 0, &writer, &error))
		chunks_packs_tests_unwrapped(file, &error, line, col);
	return writer;
}

/* chunks_packs_tests_Ending_pair_Debug_fmt is an actor's (String, Ending)'s Debug. */
static bool
chunks_packs_tests_Ending_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleStringEnding_t *pair = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &pair->f0, std_String_Debug_fmt);
	std_DebugTuple_field(&b, &pair->f1, simc_Ending_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_tests_Endings_Debug_fmt is a Vec<(String, Ending)>'s Debug. */
bool
chunks_packs_tests_Endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *endings = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = endings->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(endings, simc_TupleStringEnding_t, k),
							chunks_packs_tests_Ending_pair_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * chunks_packs_tests_terminated asserts that a run ended, panicking with its
 * outcome at a line and column otherwise, and that its first actor was
 * terminated, at another line, with the endings as the message.
 */
void
chunks_packs_tests_terminated(const char *file, const SimcReport *report, uint32 else_line, uint32 else_col,
							  uint32 line, uint32 index_col, uint32 col)
{
	const StdVec *endings = &report->outcome.u.ended;
	SimcEnding	terminated = {SIMC_ENDING_TERMINATED};

	if (report->outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report->outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, else_line, else_col, &arguments);
	}
	std_index_check(0, endings->len, file, line, index_col);
	if (!simc_Ending_PartialEq_eq(&STD_VEC_AT(endings, simc_TupleStringEnding_t, 0).f1, &terminated))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{endings, chunks_packs_tests_Endings_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(file, line, col, "==", &STD_VEC_AT(endings, simc_TupleStringEnding_t, 0).f1,
						  simc_Ending_Debug_fmt, &terminated, simc_Ending_Debug_fmt, &arguments);
	}
}

#endif							/* DOLTGRES_TEST_CHUNKS */
