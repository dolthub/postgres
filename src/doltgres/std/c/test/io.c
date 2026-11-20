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
 * The tests of std's io::Error forms (the port text's section 7): each
 * io::ErrorKind's Display and the Display, kind, and raw_os_error of an Os,
 * a Simple, and a SimpleMessage error, against Rust 1.99.1's own results,
 * which a Rust program printed into this table: it prints each kind's
 * to_string(), in StdErrorKind's order, and is built by rustc 1.99.1 with
 * RUSTC_BOOTSTRAP=1 for the five kinds still unstable there.
 */
#include "postgres.h"

#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* IoKindCase is an io::ErrorKind and Rust's text of its Display. */
typedef struct IoKindCase
{
	StdErrorKind kind;
	const char *expected;
} IoKindCase;

/* IO_KIND_CASES are every io::ErrorKind, in StdErrorKind's order, with Rust's Display of it. */
static const IoKindCase IO_KIND_CASES[] = {
	{STD_ERROR_KIND_NOT_FOUND, "entity not found"},
	{STD_ERROR_KIND_PERMISSION_DENIED, "permission denied"},
	{STD_ERROR_KIND_CONNECTION_REFUSED, "connection refused"},
	{STD_ERROR_KIND_CONNECTION_RESET, "connection reset"},
	{STD_ERROR_KIND_HOST_UNREACHABLE, "host unreachable"},
	{STD_ERROR_KIND_NETWORK_UNREACHABLE, "network unreachable"},
	{STD_ERROR_KIND_CONNECTION_ABORTED, "connection aborted"},
	{STD_ERROR_KIND_NOT_CONNECTED, "not connected"},
	{STD_ERROR_KIND_ADDR_IN_USE, "address in use"},
	{STD_ERROR_KIND_ADDR_NOT_AVAILABLE, "address not available"},
	{STD_ERROR_KIND_NETWORK_DOWN, "network down"},
	{STD_ERROR_KIND_BROKEN_PIPE, "broken pipe"},
	{STD_ERROR_KIND_ALREADY_EXISTS, "entity already exists"},
	{STD_ERROR_KIND_WOULD_BLOCK, "operation would block"},
	{STD_ERROR_KIND_NOT_A_DIRECTORY, "not a directory"},
	{STD_ERROR_KIND_IS_A_DIRECTORY, "is a directory"},
	{STD_ERROR_KIND_DIRECTORY_NOT_EMPTY, "directory not empty"},
	{STD_ERROR_KIND_READ_ONLY_FILESYSTEM, "read-only filesystem or storage medium"},
	{STD_ERROR_KIND_FILESYSTEM_LOOP, "filesystem loop or indirection limit (e.g. symlink loop)"},
	{STD_ERROR_KIND_STALE_NETWORK_FILE_HANDLE, "stale network file handle"},
	{STD_ERROR_KIND_INVALID_INPUT, "invalid input parameter"},
	{STD_ERROR_KIND_INVALID_DATA, "invalid data"},
	{STD_ERROR_KIND_TIMED_OUT, "timed out"},
	{STD_ERROR_KIND_WRITE_ZERO, "write zero"},
	{STD_ERROR_KIND_STORAGE_FULL, "no storage space"},
	{STD_ERROR_KIND_NOT_SEEKABLE, "seek on unseekable file"},
	{STD_ERROR_KIND_QUOTA_EXCEEDED, "quota exceeded"},
	{STD_ERROR_KIND_FILE_TOO_LARGE, "file too large"},
	{STD_ERROR_KIND_RESOURCE_BUSY, "resource busy"},
	{STD_ERROR_KIND_EXECUTABLE_FILE_BUSY, "executable file busy"},
	{STD_ERROR_KIND_DEADLOCK, "deadlock"},
	{STD_ERROR_KIND_CROSSES_DEVICES, "cross-device link or rename"},
	{STD_ERROR_KIND_TOO_MANY_LINKS, "too many links"},
	{STD_ERROR_KIND_INVALID_FILENAME, "invalid filename"},
	{STD_ERROR_KIND_ARGUMENT_LIST_TOO_LONG, "argument list too long"},
	{STD_ERROR_KIND_INTERRUPTED, "operation interrupted"},
	{STD_ERROR_KIND_UNSUPPORTED, "unsupported"},
	{STD_ERROR_KIND_UNEXPECTED_EOF, "unexpected end of file"},
	{STD_ERROR_KIND_OUT_OF_MEMORY, "out of memory"},
	{STD_ERROR_KIND_IN_PROGRESS, "in progress"},
	{STD_ERROR_KIND_TOO_MANY_OPEN_FILES, "too many open files"},
	{STD_ERROR_KIND_INPUT_OUTPUT_ERROR, "input/output error"},
	{STD_ERROR_KIND_OTHER, "other error"},
	{STD_ERROR_KIND_UNCATEGORIZED, "uncategorized error"},
};

StaticAssertDecl(lengthof(IO_KIND_CASES) == STD_ERROR_KIND_UNCATEGORIZED + 1, "a case for each io::ErrorKind");

/* IO_STATIC_MESSAGE is a SimpleMessage's message, which Rust's Display of the error shows as it is. */
static const StdSimpleMessage IO_STATIC_MESSAGE = {STD_ERROR_KIND_OTHER, "a static message", 16};

/* io_tests_fail panics with a message, at the test's own place. */
static pg_noreturn void
io_tests_fail(const char *message)
{
	std_panic(__FILE__, __LINE__, 0, message, strlen(message));
}

/* io_tests_shown_is reports whether a value's Display is a text. */
static bool
io_tests_shown_is(const void *value, StdFmtFn fmt, const char *expected, size_t expected_len)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{value, fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	shown;
	bool		same;

	std_fmt_format_Checked(&shown, &arguments);
	same = shown.len == expected_len && memcmp(shown.ptr, expected, shown.len) == 0;
	std_vec_free_Checked(&shown, 1, 1);
	return same;
}

/* test_std_io_tests_error_forms_match_rust checks io::ErrorKind's Display and io::Error's Display, kind, and raw_os_error against Rust's. */
void
test_std_io_tests_error_forms_match_rust(void)
{
	StdIoError	error;
	StdString	detail;
	StdString	expected;
	int32		code;

	for (size_t i = 0; i < lengthof(IO_KIND_CASES); i++)
	{
		const IoKindCase *c = &IO_KIND_CASES[i];

		if (c->kind != (StdErrorKind) i)
			io_tests_fail("a case is out of StdErrorKind's order");
		if (!io_tests_shown_is(&c->kind, std_ErrorKind_Display_fmt, c->expected, strlen(c->expected)))
			io_tests_fail("an io::ErrorKind's Display differs from Rust's");
		error = std_io_Error_From_ErrorKind_from(c->kind);
		if (!io_tests_shown_is(&error, std_io_Error_Display_fmt, c->expected, strlen(c->expected)))
			io_tests_fail("a Simple error's Display differs from Rust's");
		if (std_io_Error_kind(&error) != c->kind || std_io_Error_raw_os_error(&error, &code))
			io_tests_fail("a Simple error's kind or raw_os_error differs from Rust's");
	}

	error = std_io_Error_from_static_message(&IO_STATIC_MESSAGE);
	if (!io_tests_shown_is(&error, std_io_Error_Display_fmt, "a static message", 16))
		io_tests_fail("a SimpleMessage error's Display differs from Rust's");
	if (std_io_Error_kind(&error) != STD_ERROR_KIND_OTHER || std_io_Error_raw_os_error(&error, &code))
		io_tests_fail("a SimpleMessage error's kind or raw_os_error differs from Rust's");

	error = std_io_Error_from_raw_os_error(2);
	if (std_io_Error_kind(&error) != STD_ERROR_KIND_NOT_FOUND || !std_io_Error_raw_os_error(&error, &code) ||
		code != 2)
		io_tests_fail("an Os error's kind or raw_os_error differs from Rust's");
	std_sys_error_string(2, &detail);
	std_string_from_Checked(&expected, detail.ptr, detail.len);
	std_string_push_str_Checked(&expected, " (os error 2)", strlen(" (os error 2)"));
	if (!io_tests_shown_is(&error, std_io_Error_Display_fmt, expected.ptr, expected.len))
		io_tests_fail("an Os error's Display isn't its message and its code, as Rust's is");
	std_String_drop(&expected);
	std_String_drop(&detail);
}
