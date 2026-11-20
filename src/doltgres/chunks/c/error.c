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
 * Errors of reading and writing chunks. The invariant!, contract!, and twin!
 * macros expand in place where they are used (the port text's section 11).
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/error.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/std/alloc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/unwind.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "error.rs"

/* chunks_error_Error_FromError_from makes a seam's error chunks' error, so a door's failure passes up through ?. */
ChunksErrorError
chunks_error_Error_FromError_from(HostError *error)
{
	ChunksErrorError made;

	made.tag = CHUNKS_ERROR_ERROR_HOST;
	made.u.host = *error;
	return made;
}

/*
 * describe_closure_0 is describe's shown: a path's text, lossy, with '/'
 * between its parts on every platform.
 */
static void
chunks_error_Action_describe_closure_0(const char *path, size_t path_len,
									   StdString *result)
{
	StdCowStr	lossy;
	StdStr text;

	std_path_to_string_lossy(path, path_len, &lossy);
	if (lossy.tag == STD_COW_STR_OWNED)
	{
		for (size_t i = 0, i_len = lossy.u.owned.len; i < i_len; i++)
			if (((char *) lossy.u.owned.ptr)[i] == '\\')
				((char *) lossy.u.owned.ptr)[i] = '/';
		*result = lossy.u.owned;
		return;
	}
	text = std_cow_str_deref(&lossy);

	std_vec_with_capacity_Checked(result, text.len, 1, 1);
	for (size_t i = 0, i_len = text.len; i < i_len; i++)
	{
		char		b = text.ptr[i] == '\\' ? '/' : text.ptr[i];

		((char *) result->ptr)[i] = b;
	}
	result->len = text.len;
}

/*
 * ChunksErrorActionDescribeFrame is describe's objects: the path shown and a
 * rename's target shown, each freed by the frame while it is live.
 */
typedef struct ChunksErrorActionDescribeFrame
{
	const ChunksErrorAction *self;
	const char *path;
	size_t		path_len;
	StdString  *result;
	StdString	shown;
	bool		shown_live;
	StdString	to;
	bool		to_live;
} ChunksErrorActionDescribeFrame;

/* chunks_error_Action_describe_body writes the action's description of the frame's path. */
static void
chunks_error_Action_describe_body(void *frame_)
{
	ChunksErrorActionDescribeFrame *frame = frame_;
	const ChunksErrorAction *self = frame->self;
	const char *verb = NULL;

	chunks_error_Action_describe_closure_0(frame->path, frame->path_len, &frame->shown);
	frame->shown_live = true;
	switch (self->tag)
	{
		case CHUNKS_ERROR_ACTION_OPEN:
			verb = "open file \"";
			break;
		case CHUNKS_ERROR_ACTION_CREATE:
			verb = "create file \"";
			break;
		case CHUNKS_ERROR_ACTION_READ:
			verb = "read file \"";
			break;
		case CHUNKS_ERROR_ACTION_WRITE:
			verb = "write to file \"";
			break;
		case CHUNKS_ERROR_ACTION_EXTEND:
			verb = "extend file \"";
			break;
		case CHUNKS_ERROR_ACTION_SYNC:
			verb = "fsync file \"";
			break;
		case CHUNKS_ERROR_ACTION_REMOVE:
			verb = "remove file \"";
			break;
		case CHUNKS_ERROR_ACTION_TRUNCATE:
			verb = "truncate file \"";
			break;
		case CHUNKS_ERROR_ACTION_STAT:
			verb = "stat file \"";
			break;
		case CHUNKS_ERROR_ACTION_CREATE_DIR:
			verb = "create directory \"";
			break;
		case CHUNKS_ERROR_ACTION_READ_DIR:
			verb = "read directory \"";
			break;
		case CHUNKS_ERROR_ACTION_REMOVE_DIR:
			verb = "remove directory \"";
			break;
		case CHUNKS_ERROR_ACTION_RENAME:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("rename file \""), STD_FMT_ARG(0),
				STD_FMT_LIT("\" to \""), STD_FMT_ARG(1), STD_FMT_LIT("\"")};
				StdFmtArg	args[2];
				StdFmtArguments arguments = {pieces, 5, args};

				chunks_error_Action_describe_closure_0(self->u.rename.bytes.ptr,
													   self->u.rename.bytes.len, &frame->to);
				frame->to_live = true;
				args[0] = (StdFmtArg)
				{
					&frame->shown, std_String_Display_fmt
				};
				args[1] = (StdFmtArg)
				{
					&frame->to, std_String_Display_fmt
				};
				std_fmt_format_Checked(frame->result, &arguments);
				frame->to_live = false;
				std_vec_free_Checked(&frame->to, 1, 1);
				frame->shown_live = false;
				std_vec_free_Checked(&frame->shown, 1, 1);
				return;
			}
	}
	{
		StdFmtPiece pieces[] = {{verb, strlen(verb), 0, {0}}, STD_FMT_ARG(0), STD_FMT_LIT("\"")};
		StdFmtArg	args[] = {{&frame->shown, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(frame->result, &arguments);
	}
	frame->shown_live = false;
	std_vec_free_Checked(&frame->shown, 1, 1);
}

/* chunks_error_Action_describe_pad frees what an unwinding describe still owns: the shown paths. */
static void
chunks_error_Action_describe_pad(void *frame_)
{
	ChunksErrorActionDescribeFrame *frame = frame_;

	if (frame->to_live)
		std_vec_free_Checked(&frame->to, 1, 1);
	if (frame->shown_live)
		std_vec_free_Checked(&frame->shown, 1, 1);
}

/*
 * chunks_error_Action_describe returns what the action did to a path as
 * Postgres' messages say it after "could not", the path with '/' between its
 * parts on every platform.
 */
void
chunks_error_Action_describe(const ChunksErrorAction *self, const char *path,
							 size_t path_len, StdString *result)
{
	ChunksErrorActionDescribeFrame frame = {.self = self, .path = path, .path_len = path_len, .result = result};

	std_landing(&frame, chunks_error_Action_describe_body, chunks_error_Action_describe_pad);
}

/* chunks_error_Error_corrupt returns an error for stored data that does not have the expected format. */
ChunksErrorError
chunks_error_Error_corrupt(StdString *message)
{
	ChunksErrorError made;

	made.tag = CHUNKS_ERROR_ERROR_CORRUPT;
	made.u.corrupt = *message;
	return made;
}

/*
 * chunks_error_Error_corrupt_RefStr is corrupt's &str instance: an error for
 * stored data that does not have the expected format, its message made a
 * String in place.
 */
ChunksErrorError
chunks_error_Error_corrupt_RefStr(const char *message, size_t message_len)
{
	ChunksErrorError made;

	made.tag = CHUNKS_ERROR_ERROR_CORRUPT;
	std_string_from_Checked(&made.u.corrupt, message, message_len);
	return made;
}

/* chunks_error_Error_refused returns an error for a request the store's state refuses. */
ChunksErrorError
chunks_error_Error_refused(StdString *message)
{
	ChunksErrorError made;

	made.tag = CHUNKS_ERROR_ERROR_REFUSED;
	made.u.refused = *message;
	return made;
}

/*
 * chunks_error_Error_into_finding gives the failure up as a line of a check of
 * a stopped server's files, which reports what it finds rather than raising
 * it; it takes the error.
 */
void
chunks_error_Error_into_finding(ChunksErrorError *self, StdString *result)
{
	std_to_string(self, chunks_error_Error_Display_fmt, result);
	chunks_error_Error_drop(self);
}

/*
 * chunks_error_Error_own returns the failure when it is one of chunks' own,
 * and gives a host's error or an outside one back as the error, which can
 * only be returned; it takes the error.
 */
bool
chunks_error_Error_own(ChunksErrorError *self, ChunksErrorOwn *result,
					   ChunksErrorError *error)
{
	switch (self->tag)
	{
		case CHUNKS_ERROR_ERROR_CORRUPT:
			result->tag = CHUNKS_ERROR_OWN_CORRUPT;
			result->u.corrupt = self->u.corrupt;
			return true;
		case CHUNKS_ERROR_ERROR_DAMAGED:
			result->tag = CHUNKS_ERROR_OWN_DAMAGED;
			result->u.damaged.path = self->u.damaged.path;
			result->u.damaged.offset = self->u.damaged.offset;
			result->u.damaged.message = self->u.damaged.message;
			return true;
		case CHUNKS_ERROR_ERROR_MISSING:
			result->tag = CHUNKS_ERROR_OWN_MISSING;
			result->u.missing.f0 = self->u.missing.f0;
			result->u.missing.has_f1 = self->u.missing.has_f1;
			result->u.missing.f1 = self->u.missing.f1;
			return true;
		case CHUNKS_ERROR_ERROR_REFUSED:
			result->tag = CHUNKS_ERROR_OWN_REFUSED;
			result->u.refused = self->u.refused;
			return true;
		case CHUNKS_ERROR_ERROR_INVARIANT:
			result->tag = CHUNKS_ERROR_OWN_INVARIANT;
			result->u.invariant = self->u.invariant;
			return true;
		case CHUNKS_ERROR_ERROR_HOST:
		case CHUNKS_ERROR_ERROR_EXTERNAL:
			*error = *self;
			return false;
	}
	pg_unreachable();
}

/*
 * chunks_error_Error_reading says what a missing chunk was read for, unless
 * the error already says or is another; it takes the error.
 */
ChunksErrorError
chunks_error_Error_reading(ChunksErrorError *self, ChunksErrorReading reading)
{
	if (self->tag == CHUNKS_ERROR_ERROR_MISSING && !self->u.missing.has_f1)
	{
		ChunksErrorError made;

		made.tag = CHUNKS_ERROR_ERROR_MISSING;
		made.u.missing.f0 = self->u.missing.f0;
		made.u.missing.has_f1 = true;
		made.u.missing.f1 = reading;
		return made;
	}
	return *self;
}

/* chunks_error_Error_Display_fmt is Error's Display. */
bool
chunks_error_Error_Display_fmt(const void *value, StdFormatter *f)
{
	const ChunksErrorError *self = (const ChunksErrorError *) value;

	switch (self->tag)
	{
		case CHUNKS_ERROR_ERROR_HOST:
			{
				StdStr		message = host_Error_message(&self->u.host);

				if (message.ptr != NULL)
				{
					StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
					StdFmtArg	args[] = {{&message, std_str_Display_fmt}};
					StdFmtArguments arguments = {pieces, 1, args};

					return std_fmt_write_fmt_Checked(f, &arguments);
				}
				else
				{
					StdCowStr	sqlstate;
					StdStr		shown;
					char		copy[15];
					StdFmtPiece pieces[] = {STD_FMT_LIT("Postgres' error "), STD_FMT_ARG(0)};
					StdFmtArg	args[1];
					StdFmtArguments arguments = {pieces, 2, args};

					std_string_from_utf8_lossy(host_Error_sqlstate(&self->u.host), 5, &sqlstate);
					shown = std_cow_str_deref(&sqlstate);
					memcpy(copy, shown.ptr, shown.len);
					shown.ptr = copy;
					std_cow_str_drop(&sqlstate);
					args[0].value = &shown;
					args[0].fmt = std_str_Display_fmt;
					return std_fmt_write_fmt_Checked(f, &arguments);
				}
			}
		case CHUNKS_ERROR_ERROR_CORRUPT:
		case CHUNKS_ERROR_ERROR_REFUSED:
		case CHUNKS_ERROR_ERROR_INVARIANT:
			{
				const StdString *message = self->tag == CHUNKS_ERROR_ERROR_CORRUPT ? &self->u.corrupt :
					self->tag == CHUNKS_ERROR_ERROR_REFUSED ? &self->u.refused : &self->u.invariant;
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{message, std_String_Display_fmt}};
				StdFmtArguments arguments = {pieces, 1, args};

				return std_fmt_write_fmt_Checked(f, &arguments);
			}
		case CHUNKS_ERROR_ERROR_DAMAGED:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&self->u.damaged.message, std_String_Display_fmt}};
				StdFmtArguments arguments = {pieces, 1, args};

				return std_fmt_write_fmt_Checked(f, &arguments);
			}
		case CHUNKS_ERROR_ERROR_MISSING:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" is missing")};
				StdFmtArg	args[] = {{&self->u.missing.f0, chunks_address_Address_Display_fmt}};
				StdFmtArguments arguments = {pieces, 3, args};

				return std_fmt_write_fmt_Checked(f, &arguments);
			}
		case CHUNKS_ERROR_ERROR_EXTERNAL:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&self->u.external, std_dyn_error_Display_fmt}};
				StdFmtArguments arguments = {pieces, 1, args};

				return std_fmt_write_fmt_Checked(f, &arguments);
			}
	}
	pg_unreachable();
}

/* chunks_error_Reading_Debug_fmt is Reading's derived Debug. */
bool
chunks_error_Reading_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksErrorReading *self = (const ChunksErrorReading *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Reading", 7);

	std_DebugStruct_field(&b, "db", 2, &self->db, std_u32_Debug_fmt);
	std_DebugStruct_field(&b, "branch", 6, &self->branch, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "tree", 4, &self->tree, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "work", 4, &self->work, std_str_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* chunks_error_Error_Debug_fmt is Error's derived Debug. */
bool
chunks_error_Error_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksErrorError *self = (const ChunksErrorError *) value;
	StdDebugTuple t;
	StdDebugStruct s;

	switch (self->tag)
	{
		case CHUNKS_ERROR_ERROR_HOST:
			t = std_fmt_debug_tuple(f, "Host", 4);
			std_DebugTuple_field(&t, &self->u.host, host_Error_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case CHUNKS_ERROR_ERROR_CORRUPT:
			t = std_fmt_debug_tuple(f, "Corrupt", 7);
			std_DebugTuple_field(&t, &self->u.corrupt, std_String_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case CHUNKS_ERROR_ERROR_DAMAGED:
			s = std_fmt_debug_struct(f, "Damaged", 7);
			std_DebugStruct_field(&s, "path", 4, &self->u.damaged.path, std_PathBuf_Debug_fmt);
			std_DebugStruct_field(&s, "offset", 6, &self->u.damaged.offset, std_u64_Debug_fmt);
			std_DebugStruct_field(&s, "message", 7, &self->u.damaged.message, std_String_Debug_fmt);
			return std_DebugStruct_finish(&s);
		case CHUNKS_ERROR_ERROR_MISSING:
			{
				StdOptionRef reading = {self->u.missing.has_f1,
				{&self->u.missing.f1, chunks_error_Reading_Debug_fmt}};

				t = std_fmt_debug_tuple(f, "Missing", 7);
				std_DebugTuple_field(&t, &self->u.missing.f0, chunks_address_Address_Debug_fmt);
				std_DebugTuple_field(&t, &reading, std_option_Debug_fmt);
				return std_DebugTuple_finish(&t);
			}
		case CHUNKS_ERROR_ERROR_REFUSED:
			t = std_fmt_debug_tuple(f, "Refused", 7);
			std_DebugTuple_field(&t, &self->u.refused, std_String_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case CHUNKS_ERROR_ERROR_EXTERNAL:
			t = std_fmt_debug_tuple(f, "External", 8);
			std_DebugTuple_field(&t, &self->u.external, std_dyn_error_Debug_fmt);
			return std_DebugTuple_finish(&t);
		case CHUNKS_ERROR_ERROR_INVARIANT:
			t = std_fmt_debug_tuple(f, "Invariant", 9);
			std_DebugTuple_field(&t, &self->u.invariant, std_String_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* chunks_error_Error_drop is Error's drop glue, by its variant. */
void
chunks_error_Error_drop(ChunksErrorError *self)
{
	switch (self->tag)
	{
		case CHUNKS_ERROR_ERROR_HOST:
			host_Error_drop(&self->u.host);
			break;
		case CHUNKS_ERROR_ERROR_CORRUPT:
			std_vec_free_Checked(&self->u.corrupt, 1, 1);
			break;
		case CHUNKS_ERROR_ERROR_DAMAGED:
			std_vec_free_Checked(&self->u.damaged.path.bytes, 1, 1);
			std_vec_free_Checked(&self->u.damaged.message, 1, 1);
			break;
		case CHUNKS_ERROR_ERROR_MISSING:
			break;
		case CHUNKS_ERROR_ERROR_REFUSED:
			std_vec_free_Checked(&self->u.refused, 1, 1);
			break;
		case CHUNKS_ERROR_ERROR_EXTERNAL:
			std_dyn_error_drop(&self->u.external);
			break;
		case CHUNKS_ERROR_ERROR_INVARIANT:
			std_vec_free_Checked(&self->u.invariant, 1, 1);
			break;
	}
}

/* chunks_error_Own_drop is Own's drop glue, by its variant. */
void
chunks_error_Own_drop(ChunksErrorOwn *self)
{
	switch (self->tag)
	{
		case CHUNKS_ERROR_OWN_CORRUPT:
			std_vec_free_Checked(&self->u.corrupt, 1, 1);
			break;
		case CHUNKS_ERROR_OWN_DAMAGED:
			std_vec_free_Checked(&self->u.damaged.path.bytes, 1, 1);
			std_vec_free_Checked(&self->u.damaged.message, 1, 1);
			break;
		case CHUNKS_ERROR_OWN_MISSING:
			break;
		case CHUNKS_ERROR_OWN_REFUSED:
			std_vec_free_Checked(&self->u.refused, 1, 1);
			break;
		case CHUNKS_ERROR_OWN_INVARIANT:
			std_vec_free_Checked(&self->u.invariant, 1, 1);
			break;
	}
}

/* chunks_error_Action_drop is Action's drop glue, which frees a rename's path. */
void
chunks_error_Action_drop(ChunksErrorAction *self)
{
	if (self->tag == CHUNKS_ERROR_ACTION_RENAME)
		std_vec_free_Checked(&self->u.rename.bytes, 1, 1);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_error_tests_oom is the leak cases' allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
chunks_error_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* CHUNKS_ERROR_TESTS_OOM_HOOK is the leak cases' hook. */
static const StdAllocErrorHook CHUNKS_ERROR_TESTS_OOM_HOOK = {chunks_error_tests_oom};

/*
 * chunks_error_tests_leaks_nothing runs a body with each of the global
 * allocator's allocations failing in turn, then each of its reallocations,
 * until the body runs whole, and checks that every failure gives back every
 * byte the body took, as Rust's unwinding drops what a failed allocation
 * leaves held; the test's output capture is taken away meanwhile, since the
 * panic hook's report would grow its buffer.
 */
static void
chunks_error_tests_leaks_nothing(void (*body) (void *captures))
{
	StdOutputCapture *capture = std_io_set_output_capture(NULL);

	for (int growth = 0; growth < 2; growth++)
		for (uint64 nth = 1;; nth++)
		{
			uint64		before = test_std_alloc_tests_in_use();
			StdCaught	caught;
			bool		ok;

			std_alloc_set_alloc_error_hook(&CHUNKS_ERROR_TESTS_OOM_HOOK);
			if (growth)
				test_std_alloc_tests_fail_reallocation(nth);
			else
				test_std_alloc_tests_fail_allocation(nth);
			ok = std_catch_unwind(body, NULL, &caught);
			test_std_alloc_tests_fail_allocation(0);
			test_std_alloc_tests_fail_reallocation(0);
			std_alloc_set_alloc_error_hook(NULL);
			if (!ok)
				std_Caught_drop(&caught);
			if (test_std_alloc_tests_in_use() != before)
				std_panic(__FILE__, __LINE__, 0, "a failed allocation leaked", sizeof("a failed allocation leaked") - 1);
			if (ok)
				break;
		}
	(void) std_io_set_output_capture(capture);
}

/* chunks_error_tests_describe describes an open of a path whose text is lossy, its last part not text. */
static void
chunks_error_tests_describe(void *captures)
{
#ifdef WIN32
	static const char path[] = {'a', (char) 0xED, (char) 0xA0, (char) 0x80};
#else
	static const char path[] = {'a', (char) 0xFF};
#endif
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_OPEN};
	StdString	described;

	chunks_error_Action_describe(&action, path, sizeof(path), &described);
	std_vec_free_Checked(&described, 1, 1);
}

/* chunks_error_tests_display formats a Postgres error whose SQLSTATE isn't UTF-8 with Error's Display. */
static void
chunks_error_tests_display(void *captures)
{
	static const uint8 sqlstate[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	StdAny		raised = {&STD_UNIT_TYPE_ID, STD_DANGLING};
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_HOST};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	shown;

	error.u.host = host_Error_postgres(sqlstate, &raised);
	std_fmt_format_Checked(&shown, &arguments);
	std_vec_free_Checked(&shown, 1, 1);
	chunks_error_Error_drop(&error);
}

/*
 * test_chunks_error_tests_a_failed_allocation_in_a_lossy_description_leaks_nothing
 * describes an action on a path whose text isn't all text, so its lossy
 * text is owned, with each allocation and reallocation failing in turn: no
 * failure leaks the lossy text.
 */
void
test_chunks_error_tests_a_failed_allocation_in_a_lossy_description_leaks_nothing(void)
{
	chunks_error_tests_leaks_nothing(chunks_error_tests_describe);
}

/*
 * test_chunks_error_tests_a_failed_allocation_in_a_lossy_sqlstate_leaks_nothing
 * shows a Postgres error whose SQLSTATE isn't UTF-8, so its lossy text is
 * owned, with each allocation and reallocation failing in turn: no failure
 * leaks the lossy text.
 */
void
test_chunks_error_tests_a_failed_allocation_in_a_lossy_sqlstate_leaks_nothing(void)
{
	chunks_error_tests_leaks_nothing(chunks_error_tests_display);
}

/* chunks_error_tests_str_eq is whether a String holds a text. */
static bool
chunks_error_tests_str_eq(const StdString *a, const char *b)
{
	return a->len == strlen(b) && memcmp(a->ptr, b, a->len) == 0;
}

/* chunks_error_tests_assert_eq is the tests' assert_eq! of a String and a text at a line. */
static void
chunks_error_tests_assert_eq(const StdString *left, const char *right, uint32 line, uint32 col)
{
	StdStr		want = {right, strlen(right)};

	if (!chunks_error_tests_str_eq(left, right))
		std_assert_failed(RUST_FILE, line, col, "==", left, std_String_Debug_fmt, &want, std_str_Debug_fmt, NULL);
}

/*
 * test_chunks_error_tests_file_actions_read_as_postgres_messages_do checks
 * that a file action reads as Postgres' messages say it after "could not",
 * with '/' between a path's parts.
 */
void
test_chunks_error_tests_file_actions_read_as_postgres_messages_do(void)
{
	ChunksErrorAction renamed = {.tag = CHUNKS_ERROR_ACTION_RENAME};
	ChunksErrorAction write = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	StdPathBuf	tmp;
	StdString	described;

	std_path_join("pg_stat", 7, "pgstat.stat", 11, &renamed.u.rename);
	std_path_join("pg_stat", 7, "pgstat.tmp", 10, &tmp);
	chunks_error_Action_describe(&renamed, tmp.bytes.ptr, tmp.bytes.len, &described);
	chunks_error_tests_assert_eq(&described, "rename file \"pg_stat/pgstat.tmp\" to \"pg_stat/pgstat.stat\"", 226,
								 9);
	std_vec_free_Checked(&described, 1, 1);
	std_vec_free_Checked(&tmp.bytes, 1, 1);
	chunks_error_Action_drop(&renamed);
	chunks_error_Action_describe(&write, "a", 1, &described);
	chunks_error_tests_assert_eq(&described, "write to file \"a\"", 231, 9);
	std_vec_free_Checked(&described, 1, 1);
}

/* test_chunks_error_tests_a_seams_failure_reads_as_its_message checks that a seam's failure reads as its message. */
void
test_chunks_error_tests_a_seams_failure_reads_as_its_message(void)
{
	static const char text[] = "could not open file \"missing.pack\": No such file";
	StdString	message;
	HostError	missing;
	ChunksErrorError error;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	shown;

	std_string_from_Checked(&message, text, sizeof(text) - 1);
	missing = host_Error_fork((const uint8 *) "58P01", &message);
	error = chunks_error_Error_FromError_from(&missing);
	std_fmt_format_Checked(&shown, &arguments);
	chunks_error_tests_assert_eq(&shown, text, 238, 9);
	std_vec_free_Checked(&shown, 1, 1);
	chunks_error_Error_drop(&error);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
