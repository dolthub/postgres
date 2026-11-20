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
 * The frontend build's binding: its wrappers' error, the forms of the
 * callbacks its entry hands the check, and the runner of an entry, which
 * turns a failure or a panic into a message in the main's buffer, so that
 * nothing exits under the check.
 */
#include "postgres_fe.h"

#include "doltgres/pg/frontend.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "fe_pg_calls.h"

#ifdef DOLTGRES_TEST_PG
#include "doltgres/std/io.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fe.h"
#include "doltgres/std/test_fs.h"
#endif

/* pg_frontend_FeError_text returns the error's text, as strerror gives it. */
void
pg_frontend_FeError_text(PgFrontendFeError self, StdString *result)
{
	uint8		bytes[256];
	int64		len;
	PgFrontendFeError failed;

	if (pg_frontend_calls_error_text(self.f0, bytes, sizeof(bytes), &len, &failed))
	{
		StdCowStr	lossy;

		std_string_from_utf8_lossy(bytes, (size_t) Max(len, 0), &lossy);
		std_CowStr_into_owned(&lossy, result);
	}
	else
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("error "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&self.f0, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(result, &arguments);
	}
}

/* pg_frontend_Line_report hands the main a line of a kind: 0 found, 1 settled, 2 a note. */
void
pg_frontend_Line_report(const PgFrontendLine *self, int32 kind, const char *line)
{
	if (self->f0 != NULL)
		self->f0(kind, line);
}

/*
 * pg_frontend_Kept_hand gives the main a range: its repository, where it
 * starts, whether a start at the oldest segment held stands in for a first
 * segment that isn't kept, and whether a gap in it is a finding.
 */
void
pg_frontend_Kept_hand(const PgFrontendKept *self, uint32 repository, uint64 from, bool oldest, bool found)
{
	if (self->f0 != NULL)
		self->f0(repository, from, oldest, found);
}

/* pg_frontend_run_t is a frontend entry's run, which catch_unwind runs: the body with its arguments, and its outcome. */
typedef struct pg_frontend_run_t
{
	PgFrontendBody body;
	const char *datadir;
	PgFrontendLine line;
	PgFrontendKept kept;
	bool		ok;
	StdString	message;
} pg_frontend_run_t;

/* pg_frontend_run_body runs a frontend entry's body. */
static void
pg_frontend_run_body(void *captures)
{
	pg_frontend_run_t *run = captures;

	run->ok = run->body(run->datadir, &run->line, &run->kept, &run->message);
}

/*
 * pg_frontend_run_frontend runs a frontend entry's body with the main's data
 * directory and callbacks, writes a failure's or a panic's message into the
 * main's buffer, and returns whether the body succeeded.
 */
bool
pg_frontend_run_frontend(const char *datadir, DgLine line, DgKept kept, DgBuffer failure, PgFrontendBody body)
{
	pg_frontend_run_t run = {.body = body, .datadir = datadir, .line = {line}, .kept = {kept}};
	StdCaught	caught;
	StdString	message;

	if (std_catch_unwind(pg_frontend_run_body, &run, &caught))
	{
		if (run.ok)
			return true;
		message = run.message;
	}
	else
	{
		StdStr		shown = {NULL, 0};

		if (caught.payload == STD_PAYLOAD_STR)
		{
			const StdPayloadStr *s = caught.box;

			shown.ptr = s->ptr;
			shown.len = s->len;
		}
		else if (caught.payload == STD_PAYLOAD_STRING)
		{
			const StdString *s = caught.box;

			shown.ptr = s->ptr;
			shown.len = s->len;
		}
		if (shown.ptr != NULL)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("internal error: "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&shown, std_str_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&message, &arguments);
		}
		else
			std_string_from_Checked(&message, "internal error", strlen("internal error"));
		std_Caught_drop(&caught);
	}
	if (failure.data != NULL && failure.len > 0)
	{
		size_t		len = Min(message.len, failure.len - 1);

		memcpy(failure.data, message.ptr, len);
		failure.data[len] = '\0';
	}
	std_String_drop(&message);
	return false;
}

/* pg_frontend_FeError_Debug_fmt is FeError's derived Debug: FeError(errno). */
bool
pg_frontend_FeError_Debug_fmt(const void *value, StdFormatter *f)
{
	const PgFrontendFeError *error = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "FeError", 7);

	std_DebugTuple_field(&b, &error->f0, std_i32_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

#ifdef DOLTGRES_TEST_PG

#define RUST_FILE "pg" STD_SEP "src" STD_SEP "fe_tests.rs"

/* PG_FRONTEND_TESTS_UNWRAP is Result::unwrap's panic text. */
#define PG_FRONTEND_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* pg_tests_joined returns a path joined to a name, NUL-ended, as Path::join makes it. */
static char *
pg_tests_joined(const char *dir, size_t dir_len, const char *name)
{
	StdPathBuf	joined;
	char	   *path;

	std_path_join(dir, dir_len, name, strlen(name), &joined);
	path = pnstrdup(joined.bytes.ptr, joined.bytes.len);
	std_vec_free_Checked(&joined.bytes, 1, 1);
	return path;
}

/* pg_tests_c_path returns a path as the wrappers take it, which must be UTF-8, as to_str's unwrap requires. */
static const char *
pg_tests_c_path(const char *path)
{
	StdUtf8Error invalid;

	if (!std_str_from_utf8((const uint8 *) path, strlen(path), &invalid))
		std_option_unwrap_failed(RUST_FILE, 33, 32);
	return path;
}

/* pg_tests_folder makes a test's own empty folder by its name and returns its path, NUL-ended as the wrappers take it. */
static char *
pg_tests_folder(const char *name)
{
	StdPathBuf	temp = std_env_temp_dir();
	char	   *own = psprintf("doltgres-fe-%u-%s", std_process_id(), name);
	char	   *dir = pg_tests_joined(temp.bytes.ptr, temp.bytes.len, own);
	StdIoError	error;

	(void) std_fs_remove_dir_all(dir, strlen(dir), &error);
	if (!std_fs_create_dir_all(dir, strlen(dir), &error))
		STD_RESULT_EXPECT_FAILED(27, 35, PG_FRONTEND_TESTS_UNWRAP, &error, std_io_Error_Debug_fmt);
	pfree(own);
	std_vec_free_Checked(&temp.bytes, 1, 1);
	return dir;
}

/* pg_frontend_tests_written writes a file's bytes, which unwraps at a line and column. */
static void
pg_frontend_tests_written(const char *path, const char *bytes, uint32 line, uint32 col)
{
	StdIoError	error;

	if (!std_fs_write(path, strlen(path), (const uint8 *) bytes, strlen(bytes), &error))
		STD_RESULT_EXPECT_FAILED(line, col, PG_FRONTEND_TESTS_UNWRAP, &error, std_io_Error_Debug_fmt);
}

/* pg_frontend_tests_ok asserts a wrapper's success and returns its value, which unwraps at a line and column. */
static int64
pg_frontend_tests_ok(bool ok, const int64 *value, const PgFrontendFeError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, PG_FRONTEND_TESTS_UNWRAP, error, pg_frontend_FeError_Debug_fmt);
	return *value;
}

/* pg_frontend_tests_eq asserts at a line that a wrapper's value is the one given. */
static void
pg_frontend_tests_eq(int64 got, int64 want, uint32 line)
{
	STD_ASSERT_EQ(line, 5, got == want, &got, std_i64_Debug_fmt, &want, std_i64_Debug_fmt);
}

/* test_pg_tests_a_file_opens_reads_and_closes: a file opens to read, reports its size, reads back whole from an offset, and closes. */
void
test_pg_tests_a_file_opens_reads_and_closes(void)
{
	char	   *dir = pg_tests_folder("file");
	char	   *path = pg_tests_joined(dir, strlen(dir), "held");
	PgFrontendFeError error;
	uint8		read[4];
	int64		value;
	int32		file;
	StdBytes	got = {read, sizeof(read)};
	StdBytes	want = {(const uint8 *) "rsio", 4};

	pg_frontend_tests_written(path, "versioned", 41, 41);
	value = pg_frontend_tests_ok(pg_frontend_calls_open(pg_tests_c_path(path), &value, &error), &value, &error, 42, 51);
	if (value < PG_INT32_MIN || value > PG_INT32_MAX)
		STD_RESULT_EXPECT_FAILED(42, 61, PG_FRONTEND_TESTS_UNWRAP, STD_DANGLING, std_TryFromIntError_Debug_fmt);
	file = (int32) value;
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_size(file, &value, &error), &value, &error, 43, 27),
						 9, 43);
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_pread(file, read, sizeof(read), 2, &value, &error),
											  &value, &error, 45, 42), 4, 45);
	STD_ASSERT_EQ(46, 5, memcmp(read, "rsio", 4) == 0, &got, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_close(file, &value, &error), &value, &error, 47, 28),
						 0, 47);
	pfree(path);
	pfree(dir);
}

/* pg_frontend_tests_Names_Debug_fmt is a Vec<&[u8]>'s Debug, a listing's names. */
static bool
pg_frontend_tests_Names_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *names = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(names, StdBytes, k), std_bytes_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* pg_frontend_tests_bytes_cmp is &[u8]'s Ord::cmp, qsort's comparator. */
static int
pg_frontend_tests_bytes_cmp(const void *a, const void *b)
{
	const StdBytes *x = a;
	const StdBytes *y = b;
	int			c = memcmp(x->ptr, y->ptr, Min(x->len, y->len));

	return c != 0 ? c : (x->len > y->len) - (x->len < y->len);
}

/*
 * test_pg_tests_a_directory_lists_its_names_and_asks_for_room: a directory
 * lists its names without . and .., each ending with a NUL, and asks for the
 * room a buffer too small lacks.
 */
void
test_pg_tests_a_directory_lists_its_names_and_asks_for_room(void)
{
	char	   *dir = pg_tests_folder("listed");
	char	   *a = pg_tests_joined(dir, strlen(dir), "a");
	char	   *bc = pg_tests_joined(dir, strlen(dir), "bc");
	char	   *absent = pg_tests_joined(dir, strlen(dir), "absent");
	uint8		names[64];
	uint8		small[2];
	PgFrontendFeError error;
	int64		value;
	size_t		len;
	size_t		start = 0;
	StdVec		listed;
	StdBytes	wanted[] = {{(const uint8 *) "a", 1}, {(const uint8 *) "bc", 2}};
	StdVec		want = {wanted, lengthof(wanted), lengthof(wanted)};
	bool		same;

	pg_frontend_tests_written(a, "", 56, 40);
	pg_frontend_tests_written(bc, "", 57, 41);
	len = (size_t) pg_frontend_tests_ok(pg_frontend_calls_list_dir(pg_tests_c_path(dir), names, sizeof(names), &value, &error), &value,
										&error, 59, 67);
	std_range_check(0, len, sizeof(names), RUST_FILE, 60, 34);
	std_vec_new(&listed, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t at = 0; at <= len; at++)
	{
		if (at < len && names[at] != 0)
			continue;
		if (at > start)
		{
			StdBytes	name = {names + start, at - start};

			std_vec_push_Checked(&listed, &name, sizeof(name), _Alignof(StdBytes));
		}
		start = at + 1;
	}
	qsort(listed.ptr, listed.len, sizeof(StdBytes), pg_frontend_tests_bytes_cmp);
	same = listed.len == want.len;
	for (size_t k = 0; same && k < listed.len; k++)
		same = pg_frontend_tests_bytes_cmp(&STD_VEC_AT(&listed, StdBytes, k), &wanted[k]) == 0;
	STD_ASSERT_EQ(62, 5, same, &listed, pg_frontend_tests_Names_Debug_fmt, &want, pg_frontend_tests_Names_Debug_fmt);
	std_vec_free_Checked(&listed, sizeof(StdBytes), _Alignof(StdBytes));
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_list_dir(pg_tests_c_path(dir), small, sizeof(small), &value, &error),
											  &value, &error, 63, 55), 5, 63);
	STD_ASSERT(64, 5, !pg_frontend_calls_list_dir(pg_tests_c_path(absent), names, sizeof(names), &value, &error),
			   "list_dir(&c_path(&dir.join(\"absent\")), &mut names).is_err()");
	pfree(absent);
	pfree(bc);
	pfree(a);
	pfree(dir);
}

/* test_pg_tests_a_missing_file_fails_with_its_errno_and_text: a missing file fails to open with an errno the wrapper says is a missing file's, whose text says so. */
void
test_pg_tests_a_missing_file_fails_with_its_errno_and_text(void)
{
	char	   *dir = pg_tests_folder("missing");
	char	   *absent = pg_tests_joined(dir, strlen(dir), "absent");
	PgFrontendFeError error;
	int64		value;
	StdString	shown;
	static const char wanted[] = "No such file";

	if (pg_frontend_calls_open(pg_tests_c_path(absent), &value, &error))
		STD_RESULT_EXPECT_FAILED(71, 76, "called `Result::unwrap_err()` on an `Ok` value", &value, std_i64_Debug_fmt);
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_is_missing(error.f0, &value, &error), &value, &error,
											  72, 35), 1, 72);
	pg_frontend_tests_eq(pg_frontend_tests_ok(pg_frontend_calls_is_missing(0, &value, &error), &value, &error, 73, 30),
						 0, 73);
	pg_frontend_FeError_text(error, &shown);
	if (!std_str_contains_str_Checked(shown.ptr, shown.len, wanted, strlen(wanted)))
	{
		StdString	again;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&again, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		pg_frontend_FeError_text(error, &again);
		std_panic_fmt(RUST_FILE, 74, 5, &arguments);
	}
	std_String_drop(&shown);
	pfree(absent);
	pfree(dir);
}

/* test_pg_tests_the_version_directory_is_the_major_versions: the version directory is the major version's, as Postgres names a tablespace's. */
void
test_pg_tests_the_version_directory_is_the_major_versions(void)
{
	uint8		shown[64];
	PgFrontendFeError error;
	int64		value;
	size_t		len;

	len = (size_t) pg_frontend_tests_ok(pg_frontend_calls_version_directory(shown, sizeof(shown), &value, &error),
										&value, &error, 82, 60);
	if (len < 6 || memcmp(shown, "PG_19_", 6) != 0)
	{
		StdBytes	held = {shown, len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&held, std_bytes_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 83, 5, &arguments);
	}
}

/* test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_0 is a body that sees the main's path and succeeds. */
static bool
test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_0(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
																			  StdString *error)
{
	static const char want[] = "/datadir";

	(void) line;
	(void) kept;
	(void) error;
	STD_ASSERT_EQ(102, 43, strcmp(datadir, want) == 0, datadir, std_CStr_Debug_fmt, want, std_CStr_Debug_fmt);
	return true;
}

/* test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_1 is a body that fails with "refused". */
static bool
test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_1(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
																			  StdString *error)
{
	(void) datadir;
	(void) line;
	(void) kept;
	std_string_from_Checked(error, "refused", 7);
	return false;
}

/* test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_2 is a body that fails with a message longer than its buffer. */
static bool
test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_2(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
																			  StdString *error)
{
	(void) datadir;
	(void) line;
	(void) kept;
	std_string_from_Checked(error, "a longer message", 16);
	return false;
}

/* test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_3 is a body that panics with "broken". */
static bool
test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_3(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
																			  StdString *error)
{
	(void) datadir;
	(void) line;
	(void) kept;
	(void) error;
	std_panic(RUST_FILE, 105, 34, "broken", 6);
}

/* pg_tests_run asserts at a line what run_frontend gives for a body with the main's buffer of a size filled with 0xaa: whether it ran and the buffer's bytes up to its first NUL. */
static void
pg_tests_run(size_t size, PgFrontendBody body, bool ran, const char *bytes, size_t bytes_len, uint32 line)
{
	char	   *buffer = palloc(size);
	DgBuffer	failure = {buffer, size};
	bool		got_ran;
	size_t		end = 0;

	memset(buffer, 0xaa, size);
	got_ran = pg_frontend_run_frontend("/datadir", NULL, NULL, failure, body);
	while (end < size && buffer[end] != 0)
		end++;
	if (got_ran != ran || end != bytes_len || memcmp(buffer, bytes, bytes_len) != 0)
	{
		StdBytes	got_bytes = {(const uint8 *) buffer, end};
		StdBytes	want_bytes = {(const uint8 *) bytes, bytes_len};
		StdTupleRef left = {2, {STD_ITEM(&got_ran, std_bool_Debug_fmt), STD_ITEM(&got_bytes, std_bytes_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(&ran, std_bool_Debug_fmt), STD_ITEM(&want_bytes, std_bytes_Debug_fmt)}};

		std_assert_failed(RUST_FILE, line, 5, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	pfree(buffer);
}

/*
 * test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated: a body
 * that succeeds writes nothing; one that fails or panics writes its message,
 * a message longer than the buffer cut to it with its terminating NUL inside,
 * and the body sees the main's path.
 */
void
test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated(void)
{
	char		untouched[16];

	memset(untouched, 0xaa, sizeof(untouched));
	pg_tests_run(16, test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_0, true, untouched, sizeof(untouched), 102);
	pg_tests_run(16, test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_1, false, "refused", 7, 103);
	pg_tests_run(8, test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_2, false, "a longe", 7, 104);
	pg_tests_run(32, test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated_closure_3, false, "internal error: broken", 22, 105);
}

#endif							/* DOLTGRES_TEST_PG */
