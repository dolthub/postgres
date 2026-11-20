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
 * What the store's tests share: a repository made as a restart leaves it,
 * and the forms of the reports the simulation took.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef DOLTGRES_TEST_STORE

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/fixtures.h"

#define RUST_FILE "store" STD_SEP "src" STD_SEP "fixtures.rs"

/* STORE_FIXTURES_UNWRAP is Result::unwrap's panic text. */
#define STORE_FIXTURES_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* store_fixtures_id returns a test's repository id, which must not be zero. */
uint32
store_fixtures_id(uint32 number)
{
	if (number == 0)
		std_option_unwrap_failed(RUST_FILE, 25, 29);
	return number;
}

/* store_fixtures_base is made's gathering: a base of one storage and an identity tree's pair. */
static bool
store_fixtures_base(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	RepoGathered gathered = {.tag = REPO_GATHERED_BASE};

	(void) captures;
	(void) io;
	(void) read;
	(void) error;
	std_vec_with_capacity_Checked(result, 2, sizeof(RepoGathered), _Alignof(RepoGathered));
	gathered.u.base.storage.tablespace = HOST_PG_DEFAULT;
	gathered.u.base.storage.relation = 1;
	std_slice_to_vec_Checked(&gathered.u.base.record.bytes, (const uint8 *) "base", 4, 1, 1);
	std_vec_new(&gathered.u.base.record.addresses, sizeof(chunks_TupleU32Locator_t),
				_Alignof(chunks_TupleU32Locator_t));
	gathered.u.base.folded = 5;
	std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	gathered.tag = REPO_GATHERED_PAIR;
	std_slice_to_vec_Checked(&gathered.u.pair.key, (const uint8 *) "pair", 4, 1, 1);
	gathered.u.pair.has_value = true;
	std_slice_to_vec_Checked(&gathered.u.pair.value, (const uint8 *) "1", 1, 1, 1);
	std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	return true;
}

/* store_fixtures_saved is made's after-write, which does nothing. */
static bool
store_fixtures_saved(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/*
 * store_fixtures_made makes a repository whose manifest was saved once after
 * its create, with a base and an identity tree's pair, so its two
 * generations differ, with redo points 10 and 20, and frees its record, as a
 * restart leaves it.
 */
void
store_fixtures_made(HostIoIo *io, uint32 number)
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedOwned owned;
	ChunksErrorError failed;
	HostError	error;
	uint64		location;
	bool		has;

	simc_checkpointed(10);
	if (!repo_Repo_create(io, store_fixtures_id(number), HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(32, 63, STORE_FIXTURES_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	simc_checkpointed(20);
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(34, 42, STORE_FIXTURES_UNWRAP, &error, host_Error_Debug_fmt);
	if (!repo_Repo_save(&repo, io, &reading, false, NULL, store_fixtures_base, NULL, store_fixtures_saved, NULL,
						&failed))
		STD_RESULT_EXPECT_FAILED(40, 62, STORE_FIXTURES_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	location = chunks_packs_backend_Packs_location(&repo.packs);
	if (!repo_Repo_close(&repo, io, &reading, &failed))
		STD_RESULT_EXPECT_FAILED(42, 30, STORE_FIXTURES_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &owned,
						   &error))
		STD_RESULT_EXPECT_FAILED(43, 71, STORE_FIXTURES_UNWRAP, &error, host_Error_Debug_fmt);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, 43, 80);
	host_shared_free(&owned);
	host_shared_Reader_drop(&reading);
}

/* store_fixtures_manifest returns the path of a repository's manifest. */
StdPathBuf
store_fixtures_manifest(uint32 number)
{
	StdPathBuf	dir = host_tablespace_dir(number, HOST_PG_DEFAULT);
	StdPathBuf	path;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return path;
}

/* store_tests_Sqlstate_Debug_fmt is a [u8; 5]'s Debug: its bytes as a list of numbers. */
bool
store_tests_Sqlstate_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint8 *code = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0; k < 5; k++)
		std_DebugList_entry(&l, &code[k], std_u8_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* store_tests_Logs_Debug_fmt is a Vec<String>'s Debug, the reports at LOG simc took. */
bool
store_tests_Logs_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *logs = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = logs->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(logs, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* store_tests_Warning_Debug_fmt is a ([u8; 5], String)'s Debug, a report at WARNING simc took. */
static bool
store_tests_Warning_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleSqlstateString_t *warning = value;
	StdTupleRef tuple = {2, {STD_ITEM(warning->f0, store_tests_Sqlstate_Debug_fmt),
	STD_ITEM(&warning->f1, std_String_Debug_fmt)}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* store_tests_Warnings_Debug_fmt is a Vec<([u8; 5], String)>'s Debug, the reports at WARNING simc took. */
bool
store_tests_Warnings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *warnings = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = warnings->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(warnings, simc_TupleSqlstateString_t, k), store_tests_Warning_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* store_tests_Logs_drop drops the reports at LOG simc took. */
void
store_tests_Logs_drop(StdVec *logs)
{
	std_vec_drop_Checked(logs, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* store_tests_Warnings_drop drops the reports at WARNING simc took, their messages first. */
void
store_tests_Warnings_drop(StdVec *warnings)
{
	for (size_t k = 0, k_len = warnings->len; k < k_len; k++)
		std_String_drop(&STD_VEC_AT(warnings, simc_TupleSqlstateString_t, k).f1);
	std_vec_free_Checked(warnings, sizeof(simc_TupleSqlstateString_t), _Alignof(simc_TupleSqlstateString_t));
}

/* store_tests_ended asserts at a line and column that a run's every actor ended returning Ok, with the outcome's Debug as the message. */
void
store_tests_ended(const char *file, const SimcReport *report, uint32 line, uint32 col)
{
	const SimcOutcome *outcome = &report->outcome;
	bool		returned = outcome->tag == SIMC_OUTCOME_ENDED;

	for (size_t k = 0, k_len = returned ? outcome->u.ended.len : 0; k < k_len; k++)
	{
		const SimcEnding *ending = &STD_VEC_AT(&outcome->u.ended, simc_TupleStringEnding_t, k).f1;

		returned = returned && ending->tag == SIMC_ENDING_RETURNED && !ending->is_err;
	}
	if (!returned)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, line, col, &arguments);
	}
}

/* store_tests_kept_eq asserts at a line that the simulation's last read of the kept WAL is the range given or none, with a literal message when one is given. */
void
store_tests_kept_eq(const char *file, bool has, HostLsn from, HostLsn to, uint32 line, const char *message)
{
	HostLsn		got_from = 0;
	HostLsn		got_to = 0;
	bool		got = simc_kept_wal_read(&got_from, &got_to);

	if (got != has || (has && (got_from != from || got_to != to)))
	{
		StdTupleRef got_range = {2, {STD_ITEM(&got_from, std_u64_Debug_fmt), STD_ITEM(&got_to, std_u64_Debug_fmt)}};
		StdTupleRef want_range = {2, {STD_ITEM(&from, std_u64_Debug_fmt), STD_ITEM(&to, std_u64_Debug_fmt)}};
		StdOptionRef left = {got, STD_ITEM(&got_range, std_tuple_Debug_fmt)};
		StdOptionRef right = {has, STD_ITEM(&want_range, std_tuple_Debug_fmt)};
		StdFmtPiece piece = {message, message == NULL ? 0 : strlen(message), 0, {0}};
		StdFmtArguments arguments = {&piece, 1, NULL};

		std_assert_failed(file, line, 9, "==", &left, std_option_Debug_fmt, &right, std_option_Debug_fmt,
						  message == NULL ? NULL : &arguments);
	}
}

/*
 * store_tests_warned_once asserts at a line that the simulation took one
 * report at WARNING, of the data-corrupted SQLSTATE and a message that holds
 * the text given, with the reports' Debug as the message, and drops them.
 */
void
store_tests_warned_once(const char *file, StdVec *warnings, const char *needle, uint32 line)
{
	const simc_TupleSqlstateString_t *warning = warnings->len == 1 ?
		&STD_VEC_AT(warnings, simc_TupleSqlstateString_t, 0) : NULL;

	if (warning == NULL || memcmp(warning->f0, "XX001", 5) != 0 ||
		!std_str_contains_str_Checked(warning->f1.ptr, warning->f1.len, needle, strlen(needle)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{warnings, store_tests_Warnings_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(file, line, 9, &arguments);
	}
	store_tests_Warnings_drop(warnings);
}

/* store_tests_raised_eq asserts at a line and column that an error, which it drops, has the SQLSTATE and the message given. */
void
store_tests_raised_eq(const char *file, HostError *error, const char *code, const char *message, size_t message_len,
					  uint32 line, uint32 col)
{
	StdStr		got = host_Error_message(error);
	StdStr		want = {message, message_len};
	const uint8 *sqlstate = host_Error_sqlstate(error);

	if (memcmp(sqlstate, code, 5) != 0 || got.ptr == NULL || got.len != want.len ||
		memcmp(got.ptr, want.ptr, want.len) != 0)
	{
		StdOptionRef got_message = {got.ptr != NULL, STD_ITEM(&got, std_str_Debug_fmt)};
		StdOptionRef want_message = {true, STD_ITEM(&want, std_str_Debug_fmt)};
		StdTupleRef left = {2, {STD_ITEM(sqlstate, store_tests_Sqlstate_Debug_fmt),
		STD_ITEM(&got_message, std_option_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(code, store_tests_Sqlstate_Debug_fmt),
		STD_ITEM(&want_message, std_option_Debug_fmt)}};

		std_assert_failed(file, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	host_Error_drop(error);
}

#endif							/* DOLTGRES_TEST_STORE */
