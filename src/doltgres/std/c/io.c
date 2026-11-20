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
 * std's sys/io/error at Rust 1.99.1): an error of the operating system's code,
 * its kind, the operating system's message for it, and their Debug, for the
 * panics of std's cvt(..).unwrap() where a system call fails.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <errno.h>
#ifndef WIN32
#include <limits.h>
#include <unistd.h>
#endif

#include "doltgres/std/atomic.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_ERROR_KIND_NAMES are io::ErrorKind's variants' names, which its derived Debug writes. */
static const char *const STD_ERROR_KIND_NAMES[] = {
	"NotFound", "PermissionDenied", "ConnectionRefused", "ConnectionReset", "HostUnreachable",
	"NetworkUnreachable", "ConnectionAborted", "NotConnected", "AddrInUse", "AddrNotAvailable", "NetworkDown",
	"BrokenPipe", "AlreadyExists", "WouldBlock", "NotADirectory", "IsADirectory", "DirectoryNotEmpty",
	"ReadOnlyFilesystem", "FilesystemLoop", "StaleNetworkFileHandle", "InvalidInput", "InvalidData", "TimedOut",
	"WriteZero", "StorageFull", "NotSeekable", "QuotaExceeded", "FileTooLarge", "ResourceBusy", "ExecutableFileBusy",
	"Deadlock", "CrossesDevices", "TooManyLinks", "InvalidFilename", "ArgumentListTooLong", "Interrupted",
	"Unsupported", "UnexpectedEof", "OutOfMemory", "InProgress", "TooManyOpenFiles", "InputOutputError", "Other",
	"Uncategorized",
};

StaticAssertDecl(lengthof(STD_ERROR_KIND_NAMES) == STD_ERROR_KIND_UNCATEGORIZED + 1, "an ErrorKind name for each kind");

/* std_ErrorKind_Debug_fmt is io::ErrorKind's derived Debug: its variant's name. */
bool
std_ErrorKind_Debug_fmt(const void *value, StdFormatter *f)
{
	const char *name = STD_ERROR_KIND_NAMES[*(const StdErrorKind *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* The Repr's tags, repr_bitpacked.rs's. */
#define STD_IO_TAG_MASK ((uintptr_t) 0x3)
#define STD_IO_TAG_SIMPLE_MESSAGE ((uintptr_t) 0x0)
#define STD_IO_TAG_CUSTOM ((uintptr_t) 0x1)
#define STD_IO_TAG_OS ((uintptr_t) 0x2)
#define STD_IO_TAG_SIMPLE ((uintptr_t) 0x3)

/* STD_IO_REPR_BITPACKED_FILE is core's io/error/repr_bitpacked.rs, where decode_repr's unreachable arm reports. */
#define STD_IO_REPR_BITPACKED_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "io" STD_SEP "error" \
	STD_SEP "repr_bitpacked.rs"

/* StdIoOsFunctions is core's OsFunctions: the functions an OS error code's message, kind, and interruption take. */
typedef struct StdIoOsFunctions
{
	bool		(*format_os_error) (int32 code, StdFormatter *f);
	StdErrorKind (*decode_error_kind) (int32 code);
	bool		(*is_interrupted) (int32 code);
} StdIoOsFunctions;

static bool std_io_format_os_error(int32 code, StdFormatter *f);
static bool std_sys_is_interrupted(int32 errno_);

/* std_io_OsFunctions_DEFAULT_format_os_error is OsFunctions::DEFAULT's format_os_error, which writes nothing. */
static bool
std_io_OsFunctions_DEFAULT_format_os_error(int32 code, StdFormatter *f)
{
	return true;
}

/* std_io_OsFunctions_DEFAULT_decode_error_kind is OsFunctions::DEFAULT's decode_error_kind: Uncategorized. */
static StdErrorKind
std_io_OsFunctions_DEFAULT_decode_error_kind(int32 code)
{
	return STD_ERROR_KIND_UNCATEGORIZED;
}

/* std_io_OsFunctions_DEFAULT_is_interrupted is OsFunctions::DEFAULT's is_interrupted: false. */
static bool
std_io_OsFunctions_DEFAULT_is_interrupted(int32 code)
{
	return false;
}

/* STD_IO_OS_FUNCTIONS_DEFAULT is OsFunctions::DEFAULT, the functions before std installs its own. */
static const StdIoOsFunctions STD_IO_OS_FUNCTIONS_DEFAULT = {
	std_io_OsFunctions_DEFAULT_format_os_error,
	std_io_OsFunctions_DEFAULT_decode_error_kind,
	std_io_OsFunctions_DEFAULT_is_interrupted,
};

/* STD_IO_FUNCTIONS is from_raw_os_error's FUNCTIONS, the platform's functions std installs. */
static const StdIoOsFunctions STD_IO_FUNCTIONS = {
	std_io_format_os_error,
	std_sys_decode_error_kind,
	std_sys_is_interrupted,
};

/* std_io_os_functions is OS_FUNCTIONS, the installed functions' address, an AtomicPtr loaded and stored Relaxed. */
static StdAtomicPtr std_io_os_functions = STD_ATOMIC_PTR_INIT(&STD_IO_OS_FUNCTIONS_DEFAULT);

/* std_io_get_os_functions is get_os_functions: the installed functions. */
static const StdIoOsFunctions *
std_io_get_os_functions(void)
{
	return std_AtomicPtr_load_relaxed(&std_io_os_functions);
}

/* std_io_set_functions_inner is set_functions' cold store of the functions. */
static void
std_io_set_functions_inner(const StdIoOsFunctions *f)
{
	std_AtomicPtr_store_relaxed(&std_io_os_functions, (void *) f);
}

/* std_io_set_functions is set_functions: the functions installed unless they are already. */
static void
std_io_set_functions(const StdIoOsFunctions *f)
{
	if (std_AtomicPtr_load_relaxed(&std_io_os_functions) != f)
		std_io_set_functions_inner(f);
}

/* std_io_os_format_os_error is os_functions' format_os_error: the installed one's message for a code. */
static bool
std_io_os_format_os_error(int32 errno_, StdFormatter *f)
{
	return std_io_get_os_functions()->format_os_error(errno_, f);
}

/* std_io_os_decode_error_kind is os_functions' decode_error_kind: the installed one's kind of a code. */
static StdErrorKind
std_io_os_decode_error_kind(int32 errno_)
{
	return std_io_get_os_functions()->decode_error_kind(errno_);
}

/* std_io_os_is_interrupted is os_functions' is_interrupted: whether the installed one says a code is EINTR's. */
static bool
std_io_os_is_interrupted(int32 errno_)
{
	return std_io_get_os_functions()->is_interrupted(errno_);
}

/*
 * std_io_Error_from_raw_os_error_with_functions is
 * io::Error::from_raw_os_error_with_functions: the functions installed, then
 * Repr::new_os, the code above TAG_OS.
 */
static StdIoError
std_io_Error_from_raw_os_error_with_functions(int32 code, const StdIoOsFunctions *functions)
{
	std_io_set_functions(functions);
	return (StdIoError)
	{
		((uintptr_t) (intptr_t) code << 32) | STD_IO_TAG_OS
	};
}

/* std_io_Error_from_raw_os_error is io::Error::from_raw_os_error, std's: with std's FUNCTIONS. */
StdIoError
std_io_Error_from_raw_os_error(int32 code)
{
	return std_io_Error_from_raw_os_error_with_functions(code, &STD_IO_FUNCTIONS);
}

/* std_io_Error_From_ErrorKind_from is io::Error's From<ErrorKind>, Repr::new_simple: the kind above TAG_SIMPLE. */
StdIoError
std_io_Error_From_ErrorKind_from(StdErrorKind kind)
{
	return (StdIoError)
	{
		((uintptr_t) kind << 32) | STD_IO_TAG_SIMPLE
	};
}

/* std_io_Error_last_os_error is io::Error::last_os_error: the error of the code the operating system last set. */
StdIoError
std_io_Error_last_os_error(void)
{
#ifdef WIN32
	return std_io_Error_from_raw_os_error((int32) GetLastError());
#else
	return std_io_Error_from_raw_os_error(errno);
#endif
}

/*
 * std_io_Error_data is Repr::data, decode_repr: the form the tag says, the
 * tag's other values being unreachable!().
 */
StdIoErrorData
std_io_Error_data(const StdIoError *self)
{
	uintptr_t	bits = self->repr;
	StdIoErrorData data;

	switch (bits & STD_IO_TAG_MASK)
	{
		case STD_IO_TAG_OS:
			data.tag = STD_IO_ERROR_DATA_OS;
			data.u.os = (int32) ((int64) bits >> 32);
			return data;
		case STD_IO_TAG_SIMPLE:
			data.tag = STD_IO_ERROR_DATA_SIMPLE;
			data.u.simple = (StdErrorKind) (uint32) (bits >> 32);
			return data;
		case STD_IO_TAG_SIMPLE_MESSAGE:
			data.tag = STD_IO_ERROR_DATA_SIMPLE_MESSAGE;
			data.u.simple_message = (const StdSimpleMessage *) bits;
			return data;
		case STD_IO_TAG_CUSTOM:
			data.tag = STD_IO_ERROR_DATA_CUSTOM;
			data.u.custom = (const StdIoErrorCustom *) (bits - STD_IO_TAG_CUSTOM);
			return data;
		default:
			{
				static const char text[] = "internal error: entered unreachable code";

				std_panic(STD_IO_REPR_BITPACKED_FILE, 284, 13, text, sizeof(text) - 1);
			}
	}
}

/*
 * std_io_Error_is_interrupted is io::Error::is_interrupted: for an OS code
 * the installed is_interrupted, and otherwise whether the kind is
 * Interrupted.
 */
bool
std_io_Error_is_interrupted(const StdIoError *self)
{
	StdIoErrorData data = std_io_Error_data(self);

	switch (data.tag)
	{
		case STD_IO_ERROR_DATA_OS:
			return std_io_os_is_interrupted(data.u.os);
		case STD_IO_ERROR_DATA_CUSTOM:
			return data.u.custom->kind == STD_ERROR_KIND_INTERRUPTED;
		case STD_IO_ERROR_DATA_SIMPLE:
			return data.u.simple == STD_ERROR_KIND_INTERRUPTED;
		case STD_IO_ERROR_DATA_SIMPLE_MESSAGE:
			return data.u.simple_message->kind == STD_ERROR_KIND_INTERRUPTED;
	}
	pg_unreachable();
}

#ifdef WIN32

/* STD_FACILITY_NT_BIT is FACILITY_NT_BIT, the bit of an NTSTATUS encoded as an HRESULT. */
#define STD_FACILITY_NT_BIT 0x10000000

/* std_sys_decode_error_kind is Windows' decode_error_kind: the kind of an error code, its own first, then Winsock's. */
StdErrorKind
std_sys_decode_error_kind(int32 errno_)
{
	switch ((uint32) errno_)
	{
		case ERROR_ACCESS_DENIED:
			return STD_ERROR_KIND_PERMISSION_DENIED;
		case ERROR_ALREADY_EXISTS:
			return STD_ERROR_KIND_ALREADY_EXISTS;
		case ERROR_FILE_EXISTS:
			return STD_ERROR_KIND_ALREADY_EXISTS;
		case ERROR_BROKEN_PIPE:
			return STD_ERROR_KIND_BROKEN_PIPE;
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_INVALID_DRIVE:
		case ERROR_BAD_NETPATH:
		case ERROR_BAD_NET_NAME:
			return STD_ERROR_KIND_NOT_FOUND;
		case ERROR_NO_DATA:
			return STD_ERROR_KIND_BROKEN_PIPE;
		case ERROR_INVALID_NAME:
		case ERROR_BAD_PATHNAME:
			return STD_ERROR_KIND_INVALID_FILENAME;
		case ERROR_INVALID_PARAMETER:
			return STD_ERROR_KIND_INVALID_INPUT;
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return STD_ERROR_KIND_OUT_OF_MEMORY;
		case ERROR_SEM_TIMEOUT:
		case WAIT_TIMEOUT:
		case ERROR_DRIVER_CANCEL_TIMEOUT:
		case ERROR_OPERATION_ABORTED:
		case ERROR_SERVICE_REQUEST_TIMEOUT:
		case ERROR_COUNTER_TIMEOUT:
		case ERROR_TIMEOUT:
		case ERROR_RESOURCE_CALL_TIMED_OUT:
		case ERROR_CTX_MODEM_RESPONSE_TIMEOUT:
		case ERROR_CTX_CLIENT_QUERY_TIMEOUT:
		case FRS_ERR_SYSVOL_POPULATE_TIMEOUT:
		case ERROR_DS_TIMELIMIT_EXCEEDED:
		case DNS_ERROR_RECORD_TIMED_OUT:
		case ERROR_IPSEC_IKE_TIMED_OUT:
		case ERROR_RUNLEVEL_SWITCH_TIMEOUT:
		case ERROR_RUNLEVEL_SWITCH_AGENT_TIMEOUT:
			return STD_ERROR_KIND_TIMED_OUT;
		case ERROR_CALL_NOT_IMPLEMENTED:
			return STD_ERROR_KIND_UNSUPPORTED;
		case ERROR_HOST_UNREACHABLE:
			return STD_ERROR_KIND_HOST_UNREACHABLE;
		case ERROR_NETWORK_UNREACHABLE:
			return STD_ERROR_KIND_NETWORK_UNREACHABLE;
		case ERROR_DIRECTORY:
			return STD_ERROR_KIND_NOT_A_DIRECTORY;
		case ERROR_DIRECTORY_NOT_SUPPORTED:
			return STD_ERROR_KIND_IS_A_DIRECTORY;
		case ERROR_DIR_NOT_EMPTY:
			return STD_ERROR_KIND_DIRECTORY_NOT_EMPTY;
		case ERROR_WRITE_PROTECT:
			return STD_ERROR_KIND_READ_ONLY_FILESYSTEM;
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL:
			return STD_ERROR_KIND_STORAGE_FULL;
		case ERROR_SEEK_ON_DEVICE:
			return STD_ERROR_KIND_NOT_SEEKABLE;
		case ERROR_DISK_QUOTA_EXCEEDED:
			return STD_ERROR_KIND_QUOTA_EXCEEDED;
		case ERROR_FILE_TOO_LARGE:
			return STD_ERROR_KIND_FILE_TOO_LARGE;
		case ERROR_BUSY:
			return STD_ERROR_KIND_RESOURCE_BUSY;
		case ERROR_POSSIBLE_DEADLOCK:
			return STD_ERROR_KIND_DEADLOCK;
		case ERROR_NOT_SAME_DEVICE:
			return STD_ERROR_KIND_CROSSES_DEVICES;
		case ERROR_TOO_MANY_LINKS:
			return STD_ERROR_KIND_TOO_MANY_LINKS;
		case ERROR_TOO_MANY_OPEN_FILES:
			return STD_ERROR_KIND_TOO_MANY_OPEN_FILES;
		case ERROR_FILENAME_EXCED_RANGE:
			return STD_ERROR_KIND_INVALID_FILENAME;
		case ERROR_CANT_RESOLVE_FILENAME:
			return STD_ERROR_KIND_FILESYSTEM_LOOP;
		case ERROR_IO_DEVICE:
			return STD_ERROR_KIND_INPUT_OUTPUT_ERROR;
		case ERROR_NEGATIVE_SEEK:
			return STD_ERROR_KIND_INVALID_INPUT;
		default:
			break;
	}
	switch (errno_)
	{
		case WSAEACCES:
			return STD_ERROR_KIND_PERMISSION_DENIED;
		case WSAEADDRINUSE:
			return STD_ERROR_KIND_ADDR_IN_USE;
		case WSAEADDRNOTAVAIL:
			return STD_ERROR_KIND_ADDR_NOT_AVAILABLE;
		case WSAECONNABORTED:
			return STD_ERROR_KIND_CONNECTION_ABORTED;
		case WSAECONNREFUSED:
			return STD_ERROR_KIND_CONNECTION_REFUSED;
		case WSAECONNRESET:
			return STD_ERROR_KIND_CONNECTION_RESET;
		case WSAEINVAL:
			return STD_ERROR_KIND_INVALID_INPUT;
		case WSAENOTCONN:
			return STD_ERROR_KIND_NOT_CONNECTED;
		case WSAEWOULDBLOCK:
			return STD_ERROR_KIND_WOULD_BLOCK;
		case WSAETIMEDOUT:
			return STD_ERROR_KIND_TIMED_OUT;
		case WSAEHOSTUNREACH:
			return STD_ERROR_KIND_HOST_UNREACHABLE;
		case WSAENETDOWN:
			return STD_ERROR_KIND_NETWORK_DOWN;
		case WSAENETUNREACH:
			return STD_ERROR_KIND_NETWORK_UNREACHABLE;
		case WSAEDQUOT:
			return STD_ERROR_KIND_QUOTA_EXCEEDED;
		case WSAEMFILE:
			return STD_ERROR_KIND_TOO_MANY_OPEN_FILES;
		case WSAESHUTDOWN:
			return STD_ERROR_KIND_BROKEN_PIPE;
		default:
			return STD_ERROR_KIND_UNCATEGORIZED;
	}
}

/*
 * std_String_from_utf16 is String::from_utf16: a String of the capacity of
 * the units, each char decoded and pushed, or false at a lone surrogate,
 * whose String it frees.
 */
static bool
std_String_from_utf16(const WCHAR *v, size_t v_len, StdString *result)
{
	volatile StdString ret;
	volatile bool ret_live = false;
	bool		ok = true;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		i = 0;

		std_vec_with_capacity_Unchecked((StdString *) &ret, v_len, 1, 1);
		ret_live = true;
		while (i < v_len)
		{
			uint32		u = v[i++];
			uint32		c;

			if (u < 0xD800 || u > 0xDFFF)
				c = u;
			else if (u >= 0xDC00 || i == v_len || v[i] < 0xDC00 || v[i] > 0xDFFF)
			{
				ok = false;
				break;
			}
			else
				c = 0x10000 + ((u - 0xD800) << 10) + ((uint32) v[i++] - 0xDC00);
			std_string_push_Unchecked((StdString *) &ret, c);
		}
		if (!ok)
		{
			ret_live = false;
			std_vec_free_Unchecked((StdString *) &ret, 1, 1);
		}
		else
			ret_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (ret_live)
			std_vec_free_Unchecked((StdString *) &ret, 1, 1);
		std_unwind_resume(&unwind);
	}
	if (ok)
		*result = *(StdString *) &ret;
	return ok;
}

/* STD_SYS_IO_ERROR_WINDOWS_FILE is std's sys/io/error/windows.rs, where error_string's checks report. */
#define STD_SYS_IO_ERROR_WINDOWS_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "io" \
	STD_SEP "error" STD_SEP "windows.rs"

/*
 * std_sys_error_string is Windows' error_string: the system's message for an
 * error code, an NTSTATUS's from ntdll, without the CRLF FormatMessageW ends
 * it with, or a message of the code when the system gives none.
 */
void
std_sys_error_string(int32 errnum, StdString *result)
{
	WCHAR		buf[2048];
	HMODULE		module = NULL;
	DWORD		flags = 0;
	size_t		res;

	if ((errnum & STD_FACILITY_NT_BIT) != 0)
	{
		module = GetModuleHandleW(L"NTDLL.DLL");
		if (module != NULL)
		{
			errnum ^= STD_FACILITY_NT_BIT;
			flags = FORMAT_MESSAGE_FROM_HMODULE;
		}
	}
	res = (size_t) FormatMessageW(flags | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, module,
								  (DWORD) errnum, 0, buf, lengthof(buf), NULL);
	if (res == 0)
	{
		int32		fm_err = (int32) GetLastError();
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("OS Error "), STD_FMT_ARG(0), STD_FMT_LIT(" (FormatMessageW() returned error "),
			STD_FMT_ARG(1), STD_FMT_LIT(")"),
		};
		StdFmtArg	args[] = {{&errnum, std_i32_Display_fmt}, {&fm_err, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Unchecked(result, &arguments);
		return;
	}
	std_range_to_check(res, lengthof(buf), STD_SYS_IO_ERROR_WINDOWS_FILE, 137, 38);
	if (std_String_from_utf16(buf, res, result))
	{
		size_t		len = result->len;
		const uint8 *bytes = result->ptr;

		while (len > 0 && (bytes[len - 1] == ' ' || bytes[len - 1] == '\t' || bytes[len - 1] == '\n' ||
						   bytes[len - 1] == '\f' || bytes[len - 1] == '\r'))
			len--;
		std_string_truncate(result, len, STD_SYS_IO_ERROR_WINDOWS_FILE, 141, 21);
		return;
	}
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("OS Error "), STD_FMT_ARG(0), STD_FMT_LIT(" (FormatMessageW() returned invalid UTF-16)"),
		};
		StdFmtArg	args[] = {{&errnum, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Unchecked(result, &arguments);
	}
}

/* std_sys_is_interrupted is Windows' is_interrupted, which no code is. */
static bool
std_sys_is_interrupted(int32 errno_)
{
	return false;
}

#else

/* Postgres' port.h makes strerror_r its own; std calls the C library's. */
#undef strerror_r

#if defined(__linux__)
extern int	__xpg_strerror_r(int errnum, char *buf, size_t buflen);

/* std_strerror_r is the XSI strerror_r std links, glibc's __xpg_strerror_r. */
#define std_strerror_r __xpg_strerror_r
#else
/* std_strerror_r is the XSI strerror_r std links. */
#define std_strerror_r strerror_r
#endif

/*
 * std_sys_decode_error_kind is Unix's decode_error_kind: the kind of an errno,
 * by the first of its arms that matches it.
 */
StdErrorKind
std_sys_decode_error_kind(int32 errno_)
{
	int			e = errno_;

	if (e == E2BIG)
		return STD_ERROR_KIND_ARGUMENT_LIST_TOO_LONG;
	if (e == EADDRINUSE)
		return STD_ERROR_KIND_ADDR_IN_USE;
	if (e == EADDRNOTAVAIL)
		return STD_ERROR_KIND_ADDR_NOT_AVAILABLE;
	if (e == EBUSY)
		return STD_ERROR_KIND_RESOURCE_BUSY;
	if (e == ECONNABORTED)
		return STD_ERROR_KIND_CONNECTION_ABORTED;
	if (e == ECONNREFUSED)
		return STD_ERROR_KIND_CONNECTION_REFUSED;
	if (e == ECONNRESET)
		return STD_ERROR_KIND_CONNECTION_RESET;
	if (e == EDEADLK)
		return STD_ERROR_KIND_DEADLOCK;
	if (e == EDQUOT)
		return STD_ERROR_KIND_QUOTA_EXCEEDED;
	if (e == EEXIST)
		return STD_ERROR_KIND_ALREADY_EXISTS;
	if (e == EFBIG)
		return STD_ERROR_KIND_FILE_TOO_LARGE;
	if (e == EHOSTUNREACH)
		return STD_ERROR_KIND_HOST_UNREACHABLE;
	if (e == EINTR)
		return STD_ERROR_KIND_INTERRUPTED;
	if (e == EINVAL)
		return STD_ERROR_KIND_INVALID_INPUT;
	if (e == EISDIR)
		return STD_ERROR_KIND_IS_A_DIRECTORY;
	if (e == ELOOP)
		return STD_ERROR_KIND_FILESYSTEM_LOOP;
	if (e == ENOENT)
		return STD_ERROR_KIND_NOT_FOUND;
	if (e == ENOMEM)
		return STD_ERROR_KIND_OUT_OF_MEMORY;
	if (e == ENOSPC)
		return STD_ERROR_KIND_STORAGE_FULL;
	if (e == ENOSYS)
		return STD_ERROR_KIND_UNSUPPORTED;
	if (e == EMLINK)
		return STD_ERROR_KIND_TOO_MANY_LINKS;
	if (e == ENAMETOOLONG)
		return STD_ERROR_KIND_INVALID_FILENAME;
	if (e == ENETDOWN)
		return STD_ERROR_KIND_NETWORK_DOWN;
	if (e == ENETUNREACH)
		return STD_ERROR_KIND_NETWORK_UNREACHABLE;
	if (e == ENOTCONN)
		return STD_ERROR_KIND_NOT_CONNECTED;
	if (e == ENOTDIR)
		return STD_ERROR_KIND_NOT_A_DIRECTORY;
	if (e == ENOTEMPTY)
		return STD_ERROR_KIND_DIRECTORY_NOT_EMPTY;
	if (e == EPIPE)
		return STD_ERROR_KIND_BROKEN_PIPE;
	if (e == EROFS)
		return STD_ERROR_KIND_READ_ONLY_FILESYSTEM;
	if (e == ESPIPE)
		return STD_ERROR_KIND_NOT_SEEKABLE;
	if (e == ESTALE)
		return STD_ERROR_KIND_STALE_NETWORK_FILE_HANDLE;
	if (e == ETIMEDOUT)
		return STD_ERROR_KIND_TIMED_OUT;
	if (e == ETXTBSY)
		return STD_ERROR_KIND_EXECUTABLE_FILE_BUSY;
	if (e == EXDEV)
		return STD_ERROR_KIND_CROSSES_DEVICES;
	if (e == EINPROGRESS)
		return STD_ERROR_KIND_IN_PROGRESS;
	if (e == EMFILE || e == ENFILE)
		return STD_ERROR_KIND_TOO_MANY_OPEN_FILES;
	if (e == EOPNOTSUPP)
		return STD_ERROR_KIND_UNSUPPORTED;
	if (e == EIO)
		return STD_ERROR_KIND_INPUT_OUTPUT_ERROR;
	if (e == EACCES || e == EPERM)
		return STD_ERROR_KIND_PERMISSION_DENIED;
	if (e == EAGAIN || e == EWOULDBLOCK)
		return STD_ERROR_KIND_WOULD_BLOCK;
	return STD_ERROR_KIND_UNCATEGORIZED;
}

/*
 * std_sys_error_string is Unix's error_string: strerror_r's message for an
 * errno in a buffer of 128 bytes, read as UTF-8 with what isn't replaced,
 * panicking when strerror_r fails.
 */
void
std_sys_error_string(int32 errno_, StdString *result)
{
	char		buf[128] = {0};
	StdCowStr	lossy;

	if (std_strerror_r(errno_, buf, sizeof(buf)) < 0)
	{
		static const char text[] = "strerror_r failure";

		std_panic(STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "io" STD_SEP "error" STD_SEP
				  "unix.rs", 219, 13, text, sizeof(text) - 1);
	}
	std_string_from_utf8_lossy((const uint8 *) buf, strlen(buf), &lossy);
	if (lossy.tag == STD_COW_STR_OWNED)
		*result = lossy.u.owned;
	else
		std_string_from_Unchecked(result, lossy.u.borrowed.ptr, lossy.u.borrowed.len);
}

/* std_sys_is_interrupted is Unix's is_interrupted: whether a code is EINTR. */
static bool
std_sys_is_interrupted(int32 errno_)
{
	return errno_ == EINTR;
}

#endif

/*
 * std_io_format_os_error is std's format_os_error, the OsFunctions entry
 * std installs: the operating system's message for an error code written
 * whole, its String freed after.
 */
static bool
std_io_format_os_error(int32 code, StdFormatter *f)
{
	volatile StdString tmp_1;
	volatile bool tmp_1_live = false;
	bool		ok;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_sys_error_string(code, (StdString *) &tmp_1);
		tmp_1_live = true;
		ok = std_fmt_write_str(f, (const char *) ((StdString *) &tmp_1)->ptr, ((StdString *) &tmp_1)->len);
		tmp_1_live = false;
		std_vec_free_Unchecked((StdString *) &tmp_1, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Unchecked((StdString *) &tmp_1, 1, 1);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* std_io_Repr_Debug_fmt_closure_0_0 is the inner from_fn of Repr's Debug: format_os_error of the code it captures. */
static bool
std_io_Repr_Debug_fmt_closure_0_0(const void *captures, StdFormatter *f)
{
	return std_io_os_format_os_error(*(const int32 *) captures, f);
}

/* std_io_Repr_Debug_fmt_closure_0 is the message's from_fn of Repr's Debug: the message between quotes, unescaped. */
static bool
std_io_Repr_Debug_fmt_closure_0(const void *captures, StdFormatter *f)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("\""), STD_FMT_ARG(0), STD_FMT_LIT("\"")};
	StdFmtArg	args[] = {{captures, std_io_Repr_Debug_fmt_closure_0_0}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	return std_fmt_write_fmt_Unchecked(f, &arguments);
}

/*
 * std_io_Error_Debug_fmt is io::Error's Debug, its Repr's: for an Os error
 * the struct Os of its code, its kind, and its message, and for a
 * SimpleMessage the struct Error of its kind and its message.
 */
bool
std_io_Error_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdIoError *self = (const StdIoError *) value;
	StdIoErrorData data = std_io_Error_data(self);

	switch (data.tag)
	{
		case STD_IO_ERROR_DATA_OS:
			{
				int32		code = data.u.os;
				StdErrorKind kind;
				StdDebugStruct b = std_fmt_debug_struct(f, "Os", 2);

				std_DebugStruct_field(&b, "code", 4, &code, std_i32_Debug_fmt);
				kind = std_io_os_decode_error_kind(code);
				std_DebugStruct_field(&b, "kind", 4, &kind, std_ErrorKind_Debug_fmt);
				std_DebugStruct_field(&b, "message", 7, &code, std_io_Repr_Debug_fmt_closure_0);
				return std_DebugStruct_finish(&b);
			}
		case STD_IO_ERROR_DATA_CUSTOM:
			{
				StdDebugStruct b = std_fmt_debug_struct(f, "Custom", 6);

				std_DebugStruct_field(&b, "kind", 4, &data.u.custom->kind, std_ErrorKind_Debug_fmt);
				std_DebugStruct_field(&b, "error", 5, data.u.custom->error, data.u.custom->error_debug);
				return std_DebugStruct_finish(&b);
			}
		case STD_IO_ERROR_DATA_SIMPLE:
			{
				StdDebugTuple b = std_fmt_debug_tuple(f, "Kind", 4);

				std_DebugTuple_field(&b, &data.u.simple, std_ErrorKind_Debug_fmt);
				return std_DebugTuple_finish(&b);
			}
		case STD_IO_ERROR_DATA_SIMPLE_MESSAGE:
			{
				StdStr		message = {data.u.simple_message->message, data.u.simple_message->message_len};
				StdDebugStruct b = std_fmt_debug_struct(f, "Error", 5);

				std_DebugStruct_field(&b, "kind", 4, &data.u.simple_message->kind, std_ErrorKind_Debug_fmt);
				std_DebugStruct_field(&b, "message", 7, &message, std_str_Debug_fmt);
				return std_DebugStruct_finish(&b);
			}
	}
	pg_unreachable();
}

/*
 * STD_ERROR_KIND_DESCRIPTIONS is io::ErrorKind::as_str: each kind's
 * description, in StdErrorKind's order.
 */
static const char *const STD_ERROR_KIND_DESCRIPTIONS[] = {
	"entity not found", "permission denied", "connection refused", "connection reset", "host unreachable",
	"network unreachable", "connection aborted", "not connected", "address in use", "address not available",
	"network down", "broken pipe", "entity already exists", "operation would block", "not a directory",
	"is a directory", "directory not empty", "read-only filesystem or storage medium",
	"filesystem loop or indirection limit (e.g. symlink loop)", "stale network file handle",
	"invalid input parameter", "invalid data", "timed out", "write zero", "no storage space",
	"seek on unseekable file", "quota exceeded", "file too large", "resource busy", "executable file busy",
	"deadlock", "cross-device link or rename", "too many links", "invalid filename", "argument list too long",
	"operation interrupted", "unsupported", "unexpected end of file", "out of memory", "in progress",
	"too many open files", "input/output error", "other error", "uncategorized error",
};

StaticAssertDecl(lengthof(STD_ERROR_KIND_DESCRIPTIONS) == STD_ERROR_KIND_UNCATEGORIZED + 1,
				 "an ErrorKind description for each kind");

/* std_ErrorKind_Display_fmt is io::ErrorKind's Display: its description. */
bool
std_ErrorKind_Display_fmt(const void *value, StdFormatter *f)
{
	const char *description = STD_ERROR_KIND_DESCRIPTIONS[*(const StdErrorKind *) value];

	return std_fmt_write_str(f, description, strlen(description));
}

/* std_io_Error_raw_os_error is io::Error::raw_os_error: an OS error's code, and none for any other. */
bool
std_io_Error_raw_os_error(const StdIoError *self, int32 *result)
{
	StdIoErrorData data = std_io_Error_data(self);

	if (data.tag != STD_IO_ERROR_DATA_OS)
		return false;
	*result = data.u.os;
	return true;
}

/* std_io_Error_kind is io::Error::kind: for an OS code the installed decode_error_kind, and otherwise its kind. */
StdErrorKind
std_io_Error_kind(const StdIoError *self)
{
	StdIoErrorData data = std_io_Error_data(self);

	switch (data.tag)
	{
		case STD_IO_ERROR_DATA_OS:
			return std_io_os_decode_error_kind(data.u.os);
		case STD_IO_ERROR_DATA_CUSTOM:
			return data.u.custom->kind;
		case STD_IO_ERROR_DATA_SIMPLE:
			return data.u.simple;
		case STD_IO_ERROR_DATA_SIMPLE_MESSAGE:
			return data.u.simple_message->kind;
	}
	pg_unreachable();
}

/* std_io_Error_Display_fmt_closure_0 is the detail's from_fn of io::Error's Display: the installed message for the code. */
static bool
std_io_Error_Display_fmt_closure_0(const void *captures, StdFormatter *f)
{
	return std_io_os_format_os_error(*(const int32 *) captures, f);
}

/*
 * std_io_Error_Display_fmt is io::Error's Display: for an Os error its
 * message and its code, for a Custom its error's Display, for a Simple its
 * kind's description, and for a SimpleMessage its message.
 */
bool
std_io_Error_Display_fmt(const void *value, StdFormatter *f)
{
	StdIoErrorData data = std_io_Error_data(value);

	switch (data.tag)
	{
		case STD_IO_ERROR_DATA_OS:
			{
				static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" (os error "), STD_FMT_ARG(1),
				STD_FMT_LIT(")")};
				StdFmtArg	args[] = {{&data.u.os, std_io_Error_Display_fmt_closure_0}, {&data.u.os, std_i32_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				return std_fmt_write_fmt_Unchecked(f, &arguments);
			}
		case STD_IO_ERROR_DATA_CUSTOM:
			return data.u.custom->error_display(data.u.custom->error, f);
		case STD_IO_ERROR_DATA_SIMPLE:
			return std_ErrorKind_Display_fmt(&data.u.simple, f);
		case STD_IO_ERROR_DATA_SIMPLE_MESSAGE:
			{
				StdStr		message = {data.u.simple_message->message, data.u.simple_message->message_len};

				return std_str_Display_fmt(&message, f);
			}
	}
	pg_unreachable();
}

/*
 * std_io_Error_from_static_message is io::Error::from_static_message, which
 * const_error! expands to, Repr::new_simple_message: the static message's
 * address.
 */
StdIoError
std_io_Error_from_static_message(const StdSimpleMessage *m)
{
	return (StdIoError)
	{
		(uintptr_t) m
	};
}

/* STD_IO_WRITE_FILE and STD_IO_CURSOR_FILE are core's io/write.rs and io/cursor.rs, where io::Write's checks report. */
#define STD_IO_WRITE_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "io" STD_SEP "write.rs"
#define STD_IO_CURSOR_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "io" STD_SEP "cursor.rs"

#ifdef WIN32
/* STDIO_WINDOWS_FILE is std's sys/stdio/windows.rs, where a console write's checks report. */
#define STDIO_WINDOWS_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "stdio" STD_SEP "windows.rs"

/* STD_STDIO_MAX_BUFFER_SIZE is MAX_BUFFER_SIZE: a console write converts at most half of it in bytes. */
#define STD_STDIO_MAX_BUFFER_SIZE 8192

/* STD_STDIO_NOT_UTF8 is the const_error! of a console write's error for bytes that aren't UTF-8. */
static const StdSimpleMessage STD_STDIO_NOT_UTF8 = {STD_ERROR_KIND_INVALID_DATA,
	"Windows stdio in console mode does not support writing non-UTF-8 byte sequences",
sizeof("Windows stdio in console mode does not support writing non-UTF-8 byte sequences") - 1};

/* std_stdio_get_handle is get_handle: a standard handle, or the error that it has none. */
static bool
std_stdio_get_handle(DWORD handle_id, HANDLE *handle, StdIoError *error)
{
	*handle = GetStdHandle(handle_id);
	if (*handle == INVALID_HANDLE_VALUE)
	{
		*error = std_io_Error_last_os_error();
		return false;
	}
	if (*handle == NULL)
	{
		*error = std_io_Error_from_raw_os_error(ERROR_INVALID_HANDLE);
		return false;
	}
	return true;
}

/* std_stdio_is_console is is_console: whether a handle is a console's. */
static bool
std_stdio_is_console(HANDLE handle)
{
	DWORD		mode = 0;

	return GetConsoleMode(handle, &mode) != 0;
}

/* std_stdio_write_u16s is write_u16s: UTF-16 to the console, the code units written. */
static bool
std_stdio_write_u16s(HANDLE handle, const WCHAR *data, size_t len, size_t *written, StdIoError *error)
{
	DWORD		done = 0;

	if (WriteConsoleW(handle, data, (DWORD) len, &done, NULL) == 0)
	{
		*error = std_io_Error_last_os_error();
		return false;
	}
	*written = done;
	return true;
}

/*
 * std_stdio_write_valid_utf8_to_console is write_valid_utf8_to_console:
 * valid UTF-8, at most a buffer's worth, to the console as UTF-16, and the
 * UTF-8 bytes the code units written stand for.
 */
static bool
std_stdio_write_valid_utf8_to_console(HANDLE handle, const char *utf8, size_t utf8_len, size_t *result,
									  StdIoError *error)
{
	WCHAR		utf16[STD_STDIO_MAX_BUFFER_SIZE / 2];
	size_t		cut = std_str_floor_char_boundary(utf8, utf8_len, lengthof(utf16));
	int			converted;
	size_t		written;

	std_str_index_range(utf8, utf8_len, 0, cut, STDIO_WINDOWS_FILE, 194, 21);
	converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int) cut, utf16, (int) lengthof(utf16));
	if (converted == 0)
	{
		static const char text[] = "Unexpected error in MultiByteToWideChar";

		std_panic(STDIO_WINDOWS_FILE, 207, 9, text, sizeof(text) - 1);
	}
	std_range_to_check((size_t) converted, lengthof(utf16), STDIO_WINDOWS_FILE, 210, 14);
	if (!std_stdio_write_u16s(handle, utf16, (size_t) converted, &written, error))
		return false;
	if (written == (size_t) converted)
		*result = cut;
	else
	{
		size_t		count = 0;

		std_index_check(written, (size_t) converted, STDIO_WINDOWS_FILE, 224, 41);
		if (utf16[written] >= 0xDC00 && utf16[written] <= 0xDFFF)
		{
			size_t		one;
			StdIoError	ignored;

			std_range_check(written, written + 1, (size_t) converted, STDIO_WINDOWS_FILE, 228, 46);
			(void) std_stdio_write_u16s(handle, utf16 + written, 1, &one, &ignored);
			written += 1;
		}
		std_range_to_check(written, (size_t) converted, STDIO_WINDOWS_FILE, 233, 24);
		for (size_t i = 0; i < written; i++)
		{
			WCHAR		ch = utf16[i];

			if (ch <= 0x007F)
				count += 1;
			else if (ch <= 0x07FF)
				count += 2;
			else if (ch >= 0xDC00 && ch <= 0xDFFF)
				count += 1;
			else
				count += 3;
		}
		*result = count;
	}
	return true;
}

/*
 * std_stdio_write_console_utf16 is write_console_utf16: UTF-8 to the console
 * as UTF-16, keeping a sequence a write cuts short for the next write.
 */
static bool
std_stdio_write_console_utf16(const uint8 *data, size_t data_len, StdIncompleteUtf8 *incomplete_utf8, HANDLE handle,
							  size_t *result, StdIoError *error)
{
	size_t		len;
	StdUtf8Error utf8_error;
	size_t		valid;

	if (incomplete_utf8->len > 0)
	{
		size_t		char_width;
		size_t		width;
		size_t		written;

		if (!(incomplete_utf8->len < 4))
		{
			static const char text[] = "Unexpected number of bytes for incomplete UTF-8 codepoint.";

			std_panic(STDIO_WINDOWS_FILE, 125, 9, text, sizeof(text) - 1);
		}
		std_index_check(0, data_len, STDIO_WINDOWS_FILE, 129, 12);
		if (data[0] >> 6 != 0x2)
		{
			incomplete_utf8->len = 0;
			*error = std_io_Error_from_static_message(&STD_STDIO_NOT_UTF8);
			return false;
		}
		std_index_check(0, data_len, STDIO_WINDOWS_FILE, 137, 63);
		std_index_check(incomplete_utf8->len, 4, STDIO_WINDOWS_FILE, 137, 9);
		incomplete_utf8->bytes[incomplete_utf8->len] = data[0];
		incomplete_utf8->len += 1;
		char_width = std_str_utf8_char_width(incomplete_utf8->bytes[0]);
		if ((size_t) incomplete_utf8->len < char_width)
		{
			*result = 1;
			return true;
		}
		std_range_to_check(incomplete_utf8->len, 4, STDIO_WINDOWS_FILE, 144, 54);
		width = incomplete_utf8->len;
		incomplete_utf8->len = 0;
		if (!std_str_from_utf8(incomplete_utf8->bytes, width, &utf8_error))
		{
			*error = std_io_Error_from_static_message(&STD_STDIO_NOT_UTF8);
			return false;
		}
		if (char_width != width)
			std_assert_failed(STDIO_WINDOWS_FILE, 148, 17, "==", &char_width, std_usize_Debug_fmt, &width,
							  std_usize_Debug_fmt, NULL);
		if (!std_stdio_write_valid_utf8_to_console(handle, (const char *) incomplete_utf8->bytes, width, &written,
												   error))
			return false;
		if (written != width)
			std_assert_failed(STDIO_WINDOWS_FILE, 150, 17, "==", &written, std_usize_Debug_fmt, &width,
							  std_usize_Debug_fmt, NULL);
		*result = 1;
		return true;
	}
	len = std_min_usize(data_len, STD_STDIO_MAX_BUFFER_SIZE / 2);
	std_range_to_check(len, data_len, STDIO_WINDOWS_FILE, 169, 42);
	if (std_str_from_utf8(data, len, &utf8_error))
		valid = len;
	else if (utf8_error.valid_up_to == 0)
	{
		size_t		first_byte_char_width;

		std_index_check(0, data_len, STDIO_WINDOWS_FILE, 172, 57);
		first_byte_char_width = std_str_utf8_char_width(data[0]);
		if (first_byte_char_width > 1 && data_len < first_byte_char_width)
		{
			std_index_check(0, data_len, STDIO_WINDOWS_FILE, 174, 44);
			incomplete_utf8->bytes[0] = data[0];
			incomplete_utf8->len = 1;
			*result = 1;
			return true;
		}
		*error = std_io_Error_from_static_message(&STD_STDIO_NOT_UTF8);
		return false;
	}
	else
	{
		StdUtf8Error again;

		std_range_to_check(utf8_error.valid_up_to, data_len, STDIO_WINDOWS_FILE, 184, 39);
		if (!std_str_from_utf8(data, utf8_error.valid_up_to, &again))
			std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
									 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &again,
									 std_Utf8Error_Debug_fmt, STDIO_WINDOWS_FILE, 184, 60);
		valid = utf8_error.valid_up_to;
	}
	return std_stdio_write_valid_utf8_to_console(handle, (const char *) data, valid, result, error);
}
#endif

/* std_Stderr_new is Stderr::new: on Windows, no sequence left incomplete. */
StdStderr
std_Stderr_new(void)
{
	StdStderr	self = {0};

	return self;
}

/*
 * std_Stderr_write is Stderr's io::Write::write: on Windows, the console as
 * UTF-16 unless it reads UTF-8, else the handle's bytes as they are (std's
 * NtWriteFile is WriteFile, the same synchronous write of a standard handle);
 * off Windows, a write to file descriptor 2 of at most READ_LIMIT bytes.
 */
static bool
std_Stderr_write(StdStderr *self, const uint8 *buf, size_t len, size_t *written, StdIoError *error)
{
#ifdef WIN32
	HANDLE		handle;
	DWORD		done = 0;

	if (len == 0)
	{
		*written = 0;
		return true;
	}
	if (!std_stdio_get_handle(STD_ERROR_HANDLE, &handle, error))
		return false;
	if (std_stdio_is_console(handle) && GetConsoleOutputCP() != CP_UTF8)
		return std_stdio_write_console_utf16(buf, len, &self->incomplete_utf8, handle, written, error);
	if (WriteFile(handle, buf, (DWORD) std_min_usize(len, PG_UINT32_MAX), &done, NULL) == 0)
	{
		*error = std_io_Error_last_os_error();
		return false;
	}
	*written = done;
	return true;
#else
#ifdef __APPLE__
	ssize_t		ret = write(STDERR_FILENO, buf, std_min_usize(len, (size_t) INT_MAX - 1));
#else
	ssize_t		ret = write(STDERR_FILENO, buf, std_min_usize(len, (size_t) SSIZE_MAX));
#endif

	if (ret == -1)
	{
		*error = std_io_Error_last_os_error();
		return false;
	}
	*written = (size_t) ret;
	return true;
#endif
}

/*
 * std_Stderr_write_all is io::Write's write_all on Stderr: every byte
 * written, an interrupted write retried, and false for an error or a write
 * of none, as WRITE_ALL_EOF.
 */
bool
std_Stderr_write_all(StdStderr *self, const char *buf, size_t len)
{
	const uint8 *at = (const uint8 *) buf;

	while (len != 0)
	{
		size_t		n;
		StdIoError	error;

		if (std_Stderr_write(self, at, len, &n, &error))
		{
			if (n == 0)
				return false;
			std_range_from_check(n, len, STD_IO_WRITE_FILE, 220, 36);
			at += n;
			len -= n;
		}
		else if (!std_io_Error_is_interrupted(&error))
			return false;
	}
	return true;
}

/* StdIoAdapter is default_write_fmt's Adapter: the writer, its write_all, and whether that failed. */
typedef struct StdIoAdapter
{
	void	   *inner;
	bool		(*write_all) (void *inner, const char *buf, size_t len);
	bool		error;
} StdIoAdapter;

/* std_IoAdapter_Write_write_str is the Adapter's write_str: the writer's write_all, its error kept. */
static bool
std_IoAdapter_Write_write_str(void *self_, const char *s, size_t len)
{
	StdIoAdapter *self = self_;

	if (self->write_all(self->inner, s, len))
		return true;
	self->error = true;
	return false;
}

/* std_IoAdapter_Write_write_char is fmt::Write's default write_char: the char's UTF-8 through write_str. */
static bool
std_IoAdapter_Write_write_char(void *self, uint32 c)
{
	char		buf[4];

	return std_IoAdapter_Write_write_str(self, buf, std_fmt_encode_utf8(c, buf));
}

/* STD_IO_ADAPTER_WRITE_OPS is the Adapter's fmt::Write table. */
static const StdFmtWriteOps STD_IO_ADAPTER_WRITE_OPS = {std_IoAdapter_Write_write_str,
std_IoAdapter_Write_write_char};

/*
 * std_io_default_write_fmt is io::Write's write_fmt, which at opt-level 0
 * is default_write_fmt: the arguments through the Adapter, false for the
 * writer's error, and core's panic for a formatter's error the writer didn't
 * give.
 */
bool
std_io_default_write_fmt(void *self, bool (*write_all) (void *inner, const char *buf, size_t len),
						 const StdFmtArguments *args)
{
	StdIoAdapter output = {self, write_all, false};

	if (std_fmt_write(&STD_IO_ADAPTER_WRITE_OPS, &output, args))
		return true;
	if (output.error)
		return false;
	{
		static const char text[] = "a formatting trait implementation returned an error when the underlying "
			"stream did not";

		std_panic(STD_IO_WRITE_FILE, 411, 17, text, sizeof(text) - 1);
	}
}

/* std_Stderr_write_all_inner is std_Stderr_write_all for the Adapter. */
static bool
std_Stderr_write_all_inner(void *self, const char *buf, size_t len)
{
	return std_Stderr_write_all((StdStderr *) self, buf, len);
}

/* std_Stderr_write_fmt is io::Write's write_fmt on Stderr. */
bool
std_Stderr_write_fmt(StdStderr *self, const StdFmtArguments *args)
{
	return std_io_default_write_fmt(self, std_Stderr_write_all_inner, args);
}

/* std_Stderr_as_write is Stderr as a `&mut dyn io::Write`. */
StdIoWrite
std_Stderr_as_write(StdStderr *self)
{
	StdIoWrite	w = {self, std_Stderr_write_all_inner};

	return w;
}

/* std_VecU8_Write_write_all is Vec<u8>'s io::Write write_all: the bytes appended. */
static bool
std_VecU8_Write_write_all(void *self, const char *buf, size_t len)
{
	std_vec_extend_from_slice_Unchecked((StdVec *) self, buf, len, 1, 1);
	return true;
}

/* std_VecU8_as_write is a Vec<u8> as a `&mut dyn io::Write`. */
StdIoWrite
std_VecU8_as_write(StdVec *self)
{
	StdIoWrite	w = {self, std_VecU8_Write_write_all};

	return w;
}

/*
 * std_io_OUTPUT_CAPTURE is OUTPUT_CAPTURE, a thread local of the backend's
 * one thread: the stream a test captures its output into, or none.
 */
static StdOutputCapture *std_io_OUTPUT_CAPTURE = NULL;

/* std_io_OUTPUT_CAPTURE_USED is OUTPUT_CAPTURE_USED: whether any capture was ever set. */
static StdAtomicBool std_io_OUTPUT_CAPTURE_USED = STD_ATOMIC_BOOL_INIT(false);

/*
 * std_io_try_set_output_capture is try_set_output_capture: the capture
 * replaced and the old one returned, none while none was ever set, whose
 * thread local is always there to reach.
 */
StdOutputCapture *
std_io_try_set_output_capture(StdOutputCapture *sink)
{
	StdOutputCapture *old;

	if (sink == NULL && !std_AtomicBool_load_relaxed(&std_io_OUTPUT_CAPTURE_USED))
		return NULL;
	std_AtomicBool_store_relaxed(&std_io_OUTPUT_CAPTURE_USED, true);
	old = std_io_OUTPUT_CAPTURE;
	std_io_OUTPUT_CAPTURE = sink;
	return old;
}

/* std_io_set_output_capture is set_output_capture: try_set_output_capture, whose access never fails. */
StdOutputCapture *
std_io_set_output_capture(StdOutputCapture *sink)
{
	return std_io_try_set_output_capture(sink);
}

/*
 * std_io_print_to_buffer_if_capture_used is print_to_buffer_if_capture_used:
 * the output written into the capture under its lock, the capture taken
 * out while it is written, true when there was one.
 */
bool
std_io_print_to_buffer_if_capture_used(const StdFmtArguments *args)
{
	StdOutputCapture *w;
	StdRwLockWriteGuard guard;

	if (!std_AtomicBool_load_relaxed(&std_io_OUTPUT_CAPTURE_USED))
		return false;
	w = std_io_OUTPUT_CAPTURE;
	std_io_OUTPUT_CAPTURE = NULL;
	if (w == NULL)
		return false;
	(void) std_rw_lock_write(&w->lock, &w->buf, &guard);
	(void) std_io_default_write_fmt(&w->buf, std_VecU8_Write_write_all, args);
	std_RwLockWriteGuard_drop(&guard);
	std_io_OUTPUT_CAPTURE = w;
	return true;
}

/*
 * std_io_slice_write is core::io's slice_write, Cursor<&mut [u8]>'s write:
 * as much of buf as the slice holds past the position, copied there.
 */
static size_t
std_io_slice_write(uint64 *pos_mut, uint8 *slice, size_t slice_len, const uint8 *buf, size_t buf_len)
{
	uint64		pos = Min(*pos_mut, (uint64) slice_len);
	uint8	   *dst;
	size_t		dst_len;
	size_t		amt;

	std_range_from_check((size_t) pos, slice_len, STD_IO_CURSOR_FILE, 305, 25);
	dst = slice + pos;
	dst_len = slice_len - (size_t) pos;
	amt = Min(buf_len, dst_len);
	std_range_to_check(amt, dst_len, STD_IO_CURSOR_FILE, 307, 8);
	std_range_to_check(amt, buf_len, STD_IO_CURSOR_FILE, 307, 36);
	std_slice_copy_from_slice_Unchecked(dst, amt, buf, amt, 1, 1, STD_IO_CURSOR_FILE, 307, 16);
	*pos_mut += amt;
	return amt;
}

/*
 * std_IoCursor_write_all is io::Write's write_all on a Cursor<&mut [u8]>:
 * each write of slice_write, false as WRITE_ALL_EOF when the slice is full.
 */
bool
std_IoCursor_write_all(void *self_, const char *buf, size_t len)
{
	StdIoCursor *self = self_;
	const uint8 *at = (const uint8 *) buf;

	while (len != 0)
	{
		size_t		n = std_io_slice_write(&self->pos, self->inner, self->inner_len, at, len);

		if (n == 0)
			return false;
		std_range_from_check(n, len, STD_IO_WRITE_FILE, 220, 36);
		at += n;
		len -= n;
	}
	return true;
}
