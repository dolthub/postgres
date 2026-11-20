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
 * The C forms of io::Error that the port reaches (core's io/error.rs and
 * std's sys/io/error at Rust 1.99.1), for the panics of std's
 * cvt(..).unwrap() where a system call fails.
 */
#ifndef DOLTGRES_STD_IO_H
#define DOLTGRES_STD_IO_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/sync.h"

/* StdErrorKind is io::ErrorKind, its variants in core's order. */
typedef enum StdErrorKind
{
	STD_ERROR_KIND_NOT_FOUND,
	STD_ERROR_KIND_PERMISSION_DENIED,
	STD_ERROR_KIND_CONNECTION_REFUSED,
	STD_ERROR_KIND_CONNECTION_RESET,
	STD_ERROR_KIND_HOST_UNREACHABLE,
	STD_ERROR_KIND_NETWORK_UNREACHABLE,
	STD_ERROR_KIND_CONNECTION_ABORTED,
	STD_ERROR_KIND_NOT_CONNECTED,
	STD_ERROR_KIND_ADDR_IN_USE,
	STD_ERROR_KIND_ADDR_NOT_AVAILABLE,
	STD_ERROR_KIND_NETWORK_DOWN,
	STD_ERROR_KIND_BROKEN_PIPE,
	STD_ERROR_KIND_ALREADY_EXISTS,
	STD_ERROR_KIND_WOULD_BLOCK,
	STD_ERROR_KIND_NOT_A_DIRECTORY,
	STD_ERROR_KIND_IS_A_DIRECTORY,
	STD_ERROR_KIND_DIRECTORY_NOT_EMPTY,
	STD_ERROR_KIND_READ_ONLY_FILESYSTEM,
	STD_ERROR_KIND_FILESYSTEM_LOOP,
	STD_ERROR_KIND_STALE_NETWORK_FILE_HANDLE,
	STD_ERROR_KIND_INVALID_INPUT,
	STD_ERROR_KIND_INVALID_DATA,
	STD_ERROR_KIND_TIMED_OUT,
	STD_ERROR_KIND_WRITE_ZERO,
	STD_ERROR_KIND_STORAGE_FULL,
	STD_ERROR_KIND_NOT_SEEKABLE,
	STD_ERROR_KIND_QUOTA_EXCEEDED,
	STD_ERROR_KIND_FILE_TOO_LARGE,
	STD_ERROR_KIND_RESOURCE_BUSY,
	STD_ERROR_KIND_EXECUTABLE_FILE_BUSY,
	STD_ERROR_KIND_DEADLOCK,
	STD_ERROR_KIND_CROSSES_DEVICES,
	STD_ERROR_KIND_TOO_MANY_LINKS,
	STD_ERROR_KIND_INVALID_FILENAME,
	STD_ERROR_KIND_ARGUMENT_LIST_TOO_LONG,
	STD_ERROR_KIND_INTERRUPTED,
	STD_ERROR_KIND_UNSUPPORTED,
	STD_ERROR_KIND_UNEXPECTED_EOF,
	STD_ERROR_KIND_OUT_OF_MEMORY,
	STD_ERROR_KIND_IN_PROGRESS,
	STD_ERROR_KIND_TOO_MANY_OPEN_FILES,
	STD_ERROR_KIND_INPUT_OUTPUT_ERROR,
	STD_ERROR_KIND_OTHER,
	STD_ERROR_KIND_UNCATEGORIZED,
} StdErrorKind;

/* StdSimpleMessage is io::SimpleMessage: a const_error!'s kind and its static message. */
typedef struct StdSimpleMessage
{
	StdErrorKind kind;
	const char *message;
	size_t		message_len;
} StdSimpleMessage;

/*
 * StdIoErrorCustom is io::Custom: a kind and a boxed error, its value, its
 * Debug, and its Display. The port makes none, but decode_repr's arm reads
 * one.
 */
typedef struct StdIoErrorCustom
{
	StdErrorKind kind;
	const void *error;
	bool		(*error_debug) (const void *value, struct StdFormatter *f);
	bool		(*error_display) (const void *value, struct StdFormatter *f);
} StdIoErrorCustom;

/*
 * StdIoError is io::Error, its Repr as repr_bitpacked.rs packs it on the
 * fork's 64-bit targets (no 32-bit target, where std's repr_unpacked.rs
 * would be the form, is built): a static SimpleMessage's address (tag 0b00),
 * a Custom's address tagged 0b01, an OS code above TAG_OS (0b10), or a
 * Simple kind above TAG_SIMPLE (0b11).
 */
typedef struct StdIoError
{
	uintptr_t	repr;
} StdIoError;

/* StdIoErrorDataTag is ErrorData's forms. */
typedef enum StdIoErrorDataTag
{
	STD_IO_ERROR_DATA_OS,
	STD_IO_ERROR_DATA_SIMPLE,
	STD_IO_ERROR_DATA_SIMPLE_MESSAGE,
	STD_IO_ERROR_DATA_CUSTOM,
} StdIoErrorDataTag;

/* StdIoErrorData is ErrorData: what decode_repr reads from a Repr. */
typedef struct StdIoErrorData
{
	StdIoErrorDataTag tag;
	union
	{
		int32		os;
		StdErrorKind simple;
		const StdSimpleMessage *simple_message;
		const StdIoErrorCustom *custom;
	}			u;
} StdIoErrorData;

extern bool std_ErrorKind_Debug_fmt(const void *value, StdFormatter *f);
extern StdIoError std_io_Error_last_os_error(void);
extern StdIoError std_io_Error_from_static_message(const StdSimpleMessage *m);
extern StdIoError std_io_Error_from_raw_os_error(int32 code);
extern StdIoError std_io_Error_From_ErrorKind_from(StdErrorKind kind);
extern StdIoErrorData std_io_Error_data(const StdIoError *self);
extern bool std_io_Error_is_interrupted(const StdIoError *self);
extern StdErrorKind std_sys_decode_error_kind(int32 errno_);
extern void std_sys_error_string(int32 errnum, StdString *result);
extern bool std_io_Error_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_ErrorKind_Display_fmt(const void *value, StdFormatter *f);
extern bool std_io_Error_raw_os_error(const StdIoError *self, int32 *result);
extern StdErrorKind std_io_Error_kind(const StdIoError *self);
extern bool std_io_Error_Display_fmt(const void *value, StdFormatter *f);

/* test_std_io_tests_error_forms_match_rust is the io::Error forms' test, which std's test module runs. */
extern void test_std_io_tests_error_forms_match_rust(void);

#ifdef WIN32
/* StdIncompleteUtf8 is the start of a UTF-8 sequence a console write has taken but not yet written. */
typedef struct StdIncompleteUtf8
{
	uint8		bytes[4];
	uint8		len;
} StdIncompleteUtf8;

/* StdStderr is sys::stdio's Stderr: on Windows, the sequence a console write left incomplete. */
typedef struct StdStderr
{
	StdIncompleteUtf8 incomplete_utf8;
} StdStderr;
#else
/* StdStderr is sys::stdio's Stderr, a unit struct off Windows. */
typedef struct StdStderr
{
	char		unused;
} StdStderr;
#endif

extern StdStderr std_Stderr_new(void);
extern bool std_Stderr_write_all(StdStderr *self, const char *buf, size_t len);
extern bool std_Stderr_write_fmt(StdStderr *self, const StdFmtArguments *args);

/* StdIoCursor is io::Cursor<&mut [u8]>: the slice and the position. */
typedef struct StdIoCursor
{
	uint8	   *inner;
	size_t		inner_len;
	uint64		pos;
} StdIoCursor;

extern bool std_io_default_write_fmt(void *self, bool (*write_all) (void *inner, const char *buf, size_t len),
									 const StdFmtArguments *args);

/* StdIoWrite is a `&mut dyn io::Write` the panic hooks write through: the writer and its write_all. */
typedef struct StdIoWrite
{
	void	   *self;
	bool		(*write_all) (void *self, const char *buf, size_t len);
} StdIoWrite;

/* StdOutputCapture is LocalStream, an Arc<Mutex<Vec<u8>>> of captured output: its lock and its bytes. */
typedef struct StdOutputCapture
{
	StdRwLock	lock;
	StdVec		buf;
} StdOutputCapture;

extern StdIoWrite std_Stderr_as_write(StdStderr *self);
extern StdIoWrite std_VecU8_as_write(StdVec *self);
extern StdOutputCapture *std_io_try_set_output_capture(StdOutputCapture *sink);
extern StdOutputCapture *std_io_set_output_capture(StdOutputCapture *sink);
extern bool std_io_print_to_buffer_if_capture_used(const StdFmtArguments *args);
extern bool std_IoCursor_write_all(void *self_, const char *buf, size_t len);

#endif							/* DOLTGRES_STD_IO_H */
