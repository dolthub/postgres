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
 * The port's panics and unwinding (the port text's section 6), in place of
 * Rust's panic = "unwind": a panic raises the panic count, runs the panic
 * hook (the one set_hook set, or std's default), boxes its payload and,
 * off Windows, the
 * unwinder's exception record through the allocator door where std 1.99.1
 * and panic_unwind allocate them, and jumps to the innermost landing pad,
 * so the drops Rust's unwinding runs run in the same order; a catch frees
 * the exception record, lowers the count, and hands the payload over.
 * Postgres' own error machinery never crosses a port frame.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef FRONTEND
#include <stdatomic.h>
#endif
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifndef WIN32
/*
 * STD_EXCEPTION_SIZE is the size of panic_unwind's gcc.rs Exception on
 * 64-bit Linux and macOS: an _Unwind_Exception of two words and two private
 * words, the canary, and the boxed cause.
 */
#define STD_EXCEPTION_SIZE 56
#endif

/* STD_PANICKING_FILE is core's panicking.rs, where its non-unwinding panics of the runtime report. */
#define STD_PANICKING_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "panicking.rs"

/* STD_PANICKING_STD_FILE is std's panicking.rs, where the panic handler's own checks report. */
#define STD_PANICKING_STD_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "panicking.rs"

#ifndef WIN32
pg_noreturn static void std_panic_in_cleanup(void);
#endif
static bool std_Location_Display_fmt(const void *value, StdFormatter *f);

/* std_unwind_top is the innermost landing pad of the backend. */
StdUnwind  *std_unwind_top = NULL;

/* std_panic_count is the backend's count of panics in flight, std's panic_count: a panic raises it, a catch lowers it. */
size_t		std_panic_count = 0;

/* std_payload is the type of the payload of the panic in flight. */
StdPayload	std_payload = STD_PAYLOAD_STR;

/* std_payload_box is the payload box of the panic in flight, NULL for out of memory's zero-sized one. */
void	   *std_payload_box = NULL;

#ifndef WIN32
/* std_exception is the exception record of the panic in flight, which gcc.rs boxes. */
void	   *std_exception = NULL;
#endif

/* std_process_abort is process::abort: Windows' fail-fast FAST_FAIL_FATAL_APP_EXIT, elsewhere abort(). */
void
std_process_abort(void)
{
#ifdef WIN32
	__fastfail(FAST_FAIL_FATAL_APP_EXIT);
#else
	abort();
#endif
}

/* std_unwind_pop pops a landing pad its region left without a panic, after the region's drops. */
void
std_unwind_pop(StdUnwind *unwind)
{
	Assert(std_unwind_top == unwind);
	std_unwind_top = unwind->prev;
}

/*
 * std_unwind_jump lands the unwind on the innermost frame, marking it
 * landed and keeping the panic its pad runs for. An unwind that reaches a
 * frame whose pad is running its drops (the panic came out of one of them,
 * through the frames inside it, whose pads ran) is core's panic_in_cleanup,
 * which a cleanup's terminate edge calls; on Windows std_unwind_begin has
 * aborted such a panic before it unwound, so no unwind reaches one. Every entry and test runs under a frame, so one is always
 * there to reach.
 */
pg_noreturn static void
std_unwind_jump(void)
{
	Assert(std_unwind_top != NULL);
#ifdef WIN32
	Assert(!std_unwind_top->landed);
#else
	if (std_unwind_top->landed)
		std_panic_in_cleanup();
#endif
	std_unwind_top->landed = true;
	std_unwind_top->payload = std_payload;
	std_unwind_top->box = std_payload_box;
#ifndef WIN32
	std_unwind_top->exception = std_exception;
#endif
	siglongjmp(std_unwind_top->jump, 1);
}

/*
 * std_unwind_resume goes on unwinding from a landing pad that ran its drops,
 * to the next pad out, with the panic it landed with.
 */
void
std_unwind_resume(StdUnwind *unwind)
{
	std_payload = unwind->payload;
	std_payload_box = unwind->box;
#ifndef WIN32
	std_exception = unwind->exception;
#endif
	std_unwind_top = unwind->prev;
	std_unwind_jump();
}

/*
 * std_unwind_begin starts unwinding with a payload box, after the panic's
 * hook: off Windows it boxes the exception record, as gcc.rs does. On
 * Windows, a panic whose frames up to the nearest one that catches hold a
 * pad running its drops is process::abort, before any frame unwinds: the
 * second exception's search finds the cleanup funclet in flight, and the C++
 * runtime terminates, failing fast, with no frame's cleanup run (a probe of
 * rustc 1.99.1 on MSVC: the inner drop of the panicking drop never runs).
 */
void
std_unwind_begin(StdPayload payload, void *box)
{
#ifdef WIN32
	for (StdUnwind *frame = std_unwind_top; frame != NULL && !frame->catches; frame = frame->prev)
	{
		if (frame->landed)
			std_process_abort();
	}
#endif
	std_payload = payload;
	std_payload_box = box;
#ifndef WIN32
	std_exception = std_alloc(STD_EXCEPTION_SIZE, MAXIMUM_ALIGNOF);
#endif
	std_unwind_jump();
}

/* STD_BACKTRACE_NONE is default_hook's None: neither a backtrace nor the note. */
#define STD_BACKTRACE_NONE 0

/* STD_BACKTRACE_MAX_NB_FRAMES is MAX_NB_FRAMES, the frames a short backtrace walks. */
#define STD_BACKTRACE_MAX_NB_FRAMES 100

/* std_backtrace_should_capture is SHOULD_CAPTURE: the style RUST_BACKTRACE gave, or 0 before it is read. */
#ifndef FRONTEND
static pg_atomic_uint32 std_backtrace_should_capture = {0};
#else
static atomic_uchar std_backtrace_should_capture = 0;
#endif

/* std_panic_first_panic is default_hook's FIRST_PANIC: whether no panic has printed the RUST_BACKTRACE note. */
#ifndef FRONTEND
static pg_atomic_uint32 std_panic_first_panic = {1};
#else
static atomic_bool std_panic_first_panic = true;
#endif

/* std_panic_first_panic_swap is FIRST_PANIC.swap(false, Relaxed): whether no panic had printed the note before. */
static bool
std_panic_first_panic_swap(void)
{
#ifndef FRONTEND
	return pg_atomic_exchange_u32(&std_panic_first_panic, 0) != 0;
#else
	return atomic_exchange_explicit(&std_panic_first_panic, false, memory_order_relaxed);
#endif
}

/*
 * std_panic_get_backtrace_style is panic::get_backtrace_style: the style
 * RUST_BACKTRACE asks for, read once ("0" off, "full" full, any other value
 * short, unset off).
 */
int
std_panic_get_backtrace_style(void)
{
#ifndef FRONTEND
	unsigned char current = (unsigned char) pg_atomic_read_u32(&std_backtrace_should_capture);
#else
	unsigned char current = atomic_load_explicit(&std_backtrace_should_capture, memory_order_relaxed);
#endif
	const char *x;
	unsigned char format;

	if (current >= STD_BACKTRACE_SHORT && current <= STD_BACKTRACE_OFF)
		return current;
	x = getenv("RUST_BACKTRACE");
	if (x == NULL || strcmp(x, "0") == 0)
		format = STD_BACKTRACE_OFF;
	else if (strcmp(x, "full") == 0)
		format = STD_BACKTRACE_FULL;
	else
		format = STD_BACKTRACE_SHORT;
	current = 0;
#ifndef FRONTEND
	{
		uint32		expected = current;

		if (pg_atomic_compare_exchange_u32(&std_backtrace_should_capture, &expected, format))
			return format;
		current = (unsigned char) expected;
	}
#else
	if (atomic_compare_exchange_strong_explicit(&std_backtrace_should_capture, &current, format,
												memory_order_relaxed, memory_order_relaxed))
		return format;
#endif
	return current;
}

/*
 * std_backtrace_print is the backtrace lock's print: std's "stack backtrace:"
 * and its frames, here the C's own from backtrace() and backtrace_symbols()
 * where the platform has them, as Postgres' backtrace_functions takes them;
 * a short backtrace prints frames only between std's short-backtrace
 * markers, which no C frame names, then its note.
 */
void
std_backtrace_print(StdIoWrite *err, int style)
{
	static const char header[] = "stack backtrace:\n";
	static const char note[] = "note: Some details are omitted, run with `RUST_BACKTRACE=full` for a verbose backtrace.\n";

	if (!err->write_all(err->self, header, sizeof(header) - 1))
		return;
#ifdef HAVE_BACKTRACE_SYMBOLS
	if (style == STD_BACKTRACE_FULL)
	{
		void	   *frames[STD_BACKTRACE_MAX_NB_FRAMES + 1];
		int			nframes = backtrace(frames, lengthof(frames));
		char	  **symbols = backtrace_symbols(frames, nframes);

		for (int idx = 0; idx < nframes; idx++)
		{
			char		ip[24];
			char		line[64];
			const char *name = symbols != NULL ? symbols[idx] : "<unknown>";
			int			len;

			snprintf(ip, sizeof(ip), "0x%zx", (size_t) (uintptr_t) frames[idx]);
			len = snprintf(line, sizeof(line), "%4d: %18s - ", idx, ip);
			if (!err->write_all(err->self, line, (size_t) len) ||
				!err->write_all(err->self, name, strlen(name)) ||
				!err->write_all(err->self, "\n", 1))
				break;
		}
		free(symbols);
	}
#endif
	if (style == STD_BACKTRACE_SHORT)
		(void) err->write_all(err->self, note, sizeof(note) - 1);
}

/* std_thread_current_os_id is thread::current_os_id: the operating system's id of the thread. */
static uint64
std_thread_current_os_id(void)
{
#if defined(WIN32)
	return (uint64) GetCurrentThreadId();
#elif defined(__APPLE__)
	uint64		id = 0;

	(void) pthread_threadid_np(NULL, &id);
	return id;
#else
	return (uint64) syscall(SYS_gettid);
#endif
}

/*
 * std_default_hook_closure_0 is default_hook's write: the thread's name and
 * id, the location with its column, and the message, through a 512-byte
 * buffer when they fit; then the backtrace the style asks for, or on the
 * first panic with backtraces off the note on RUST_BACKTRACE, and neither
 * when the panic forces no backtrace. The process's one thread is std's main
 * thread, named "main".
 */
static void
std_default_hook_closure_0(StdIoWrite *err, int backtrace, const StdLocation *location, const char *msg,
						   size_t msg_len)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("\nthread '"), STD_FMT_ARG(0), STD_FMT_LIT("' ("),
		STD_FMT_ARG(1), STD_FMT_LIT(") panicked at "), STD_FMT_ARG(2), STD_FMT_LIT(":\n"), STD_FMT_ARG(3),
	STD_FMT_LIT("\n")};
	StdStr		name = {"main", 4};
	uint64		tid;
	StdStr		message = {msg, msg_len};
	StdFmtArg	args[4];
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	uint8		buffer[512] = {0};
	StdIoCursor cursor = {buffer, sizeof(buffer), 0};

	tid = std_thread_current_os_id();
	args[0] = (StdFmtArg)
	{
		&name, std_str_Display_fmt
	};
	args[1] = (StdFmtArg)
	{
		&tid, std_u64_Display_fmt
	};
	args[2] = (StdFmtArg)
	{
		location, std_Location_Display_fmt
	};
	args[3] = (StdFmtArg)
	{
		&message, std_str_Display_fmt
	};
	if (std_io_default_write_fmt(&cursor, std_IoCursor_write_all, &arguments))
	{
		size_t		pos = (size_t) cursor.pos;

		std_range_check(0, pos, sizeof(buffer), STD_PANICKING_STD_FILE, 278, 46);
		(void) err->write_all(err->self, (const char *) buffer, pos);
	}
	else
		(void) std_io_default_write_fmt(err->self, err->write_all, &arguments);
	if (backtrace == STD_BACKTRACE_SHORT || backtrace == STD_BACKTRACE_FULL)
		std_backtrace_print(err, backtrace);
	else if (backtrace == STD_BACKTRACE_OFF &&
			 std_panic_first_panic_swap())
	{
		static const StdFmtPiece note[] = {
		STD_FMT_LIT("note: run with `RUST_BACKTRACE=1` environment variable to display a backtrace\n")};
		StdFmtArguments note_arguments = {note, lengthof(note), NULL};

		(void) std_io_default_write_fmt(err->self, err->write_all, &note_arguments);
	}
}

/*
 * std_default_hook is std's default_hook, the hook until set_hook sets
 * another: its write into the output capture a test set, under the capture's
 * lock, or else to stderr; a backtrace of the style the panic asks for, full
 * from the second panic in flight, none when it forces none.
 */
static void
std_default_hook(const StdPanicHookInfo *info)
{
	int			backtrace = info->force_no_backtrace ? STD_BACKTRACE_NONE :
		std_panic_count >= 2 ? STD_BACKTRACE_FULL : std_panic_get_backtrace_style();
	const StdLocation *location = info->location;
	const char *msg;
	size_t		msg_len;
	StdOutputCapture *local;

	if (!std_PanicHookInfo_payload_as_str(info, &msg, &msg_len))
	{
		msg = "Box<dyn Any>";
		msg_len = 12;
	}
	local = std_io_try_set_output_capture(NULL);
	if (local != NULL)
	{
		StdRwLockWriteGuard guard;
		StdIoWrite	err;

		(void) std_rw_lock_write(&local->lock, &local->buf, &guard);
		err = std_VecU8_as_write(&local->buf);
		std_default_hook_closure_0(&err, backtrace, location, msg, msg_len);
		std_RwLockWriteGuard_drop(&guard);
		(void) std_io_try_set_output_capture(local);
	}
	else
	{
		StdStderr	out = std_Stderr_new();
		StdIoWrite	err = std_Stderr_as_write(&out);

		std_default_hook_closure_0(&err, backtrace, location, msg, msg_len);
	}
}

/*
 * The panic count, std's panic_count: GLOBAL_PANIC_COUNT, the process's
 * count with ALWAYS_ABORT_FLAG, and LOCAL_PANIC_COUNT, the thread's count
 * and whether its panic hook is running, a static of the backend's one
 * thread (std-cuts.tsv).
 */
#define STD_ALWAYS_ABORT_FLAG ((size_t) 1 << 63)
#ifndef FRONTEND
static pg_atomic_uint64 std_global_panic_count = {0};
#else
static atomic_size_t std_global_panic_count = 0;
#endif
bool		std_local_in_panic_hook = false;

/* StdMustAbort is panic_count's MustAbort: why a panic must abort rather than unwind. */
typedef enum StdMustAbort
{
	STD_MUST_ABORT_NONE,
	STD_MUST_ABORT_ALWAYS_ABORT,
	STD_MUST_ABORT_PANIC_IN_HOOK,
} StdMustAbort;

/* std_global_panic_count_fetch_add is GLOBAL_PANIC_COUNT.fetch_add(n, Relaxed). */
static size_t
std_global_panic_count_fetch_add(size_t n)
{
#ifndef FRONTEND
	return (size_t) pg_atomic_fetch_add_u64(&std_global_panic_count, (int64) n);
#else
	return atomic_fetch_add_explicit(&std_global_panic_count, n, memory_order_relaxed);
#endif
}

/* std_global_panic_count_load is GLOBAL_PANIC_COUNT.load(Relaxed). */
static size_t
std_global_panic_count_load(void)
{
#ifndef FRONTEND
	return (size_t) pg_atomic_read_u64(&std_global_panic_count);
#else
	return atomic_load_explicit(&std_global_panic_count, memory_order_relaxed);
#endif
}

/*
 * std_panic_count_increase is panic_count::increase: the panic counted, the
 * thread's hook marked running when it will run, or why it must abort.
 */
static StdMustAbort
std_panic_count_increase(bool run_panic_hook)
{
	size_t		global_count = std_global_panic_count_fetch_add(1);

	if ((global_count & STD_ALWAYS_ABORT_FLAG) != 0)
		return STD_MUST_ABORT_ALWAYS_ABORT;
	if (std_local_in_panic_hook)
		return STD_MUST_ABORT_PANIC_IN_HOOK;
	std_panic_count += 1;
	std_local_in_panic_hook = run_panic_hook;
	return STD_MUST_ABORT_NONE;
}

/* std_panic_count_finished_panic_hook is panic_count::finished_panic_hook. */
static void
std_panic_count_finished_panic_hook(void)
{
	std_local_in_panic_hook = false;
}

/* std_panic_count_decrease is panic_count::decrease: a caught panic uncounted. */
static void
std_panic_count_decrease(void)
{
#ifndef FRONTEND
	(void) pg_atomic_fetch_sub_u64(&std_global_panic_count, 1);
#else
	(void) atomic_fetch_sub_explicit(&std_global_panic_count, 1, memory_order_relaxed);
#endif
	std_panic_count -= 1;
	std_local_in_panic_hook = false;
}

/* std_thread_panicking is thread::panicking: panic_count::count_is_zero's fast path, then the thread's count. */
bool
std_thread_panicking(void)
{
	if ((std_global_panic_count_load() & ~STD_ALWAYS_ABORT_FLAG) == 0)
		return false;
	return std_panic_count != 0;
}

/*
 * StdPanicPayload is the panic handler's payload: StaticStrPayload's literal,
 * or FormatStringPayload's message and the String its first read formats.
 */
typedef struct StdPanicPayload
{
	bool		is_str;
	const char *str;
	size_t		str_len;
	const StdFmtArguments *inner;
	bool		has_string;
	StdString	string;
} StdPanicPayload;

/* std_FormatStringPayload_fill is FormatStringPayload's fill: the message formatted once, into a String::new(). */
static StdString *
std_FormatStringPayload_fill(StdPanicPayload *self)
{
	if (!self->has_string)
	{
		std_vec_new(&self->string, 1, 1);
		(void) std_fmt_write(&STD_STRING_WRITE_OPS, &self->string, self->inner);
		self->has_string = true;
	}
	return &self->string;
}

/* std_PanicPayload_as_str is the payload's as_str: the literal, or the message's own as_str. */
static bool
std_PanicPayload_as_str(StdPanicPayload *self, const char **text, size_t *len)
{
	if (self->is_str)
	{
		*text = self->str;
		*len = self->str_len;
		return true;
	}
	return std_fmt_as_str_Unchecked(self->inner, text, len);
}

/* std_PanicPayload_Display_fmt is the payload's Display: the literal, the String once filled, or the message. */
static bool
std_PanicPayload_Display_fmt(const void *value, StdFormatter *f)
{
	const StdPanicPayload *self = value;

	if (self->is_str)
		return std_fmt_write_str(f, self->str, self->str_len);
	if (self->has_string)
		return std_fmt_write_str(f, self->string.ptr, self->string.len);
	return std_fmt_write_fmt_Unchecked(f, self->inner);
}

/*
 * std_PanicPayload_take_box is the payload's take_box: a Box<&str> of the
 * literal, or a Box<String> of the filled String, taken, as mem::take leaves
 * String::new().
 */
static void *
std_PanicPayload_take_box(StdPanicPayload *self, StdPayload *payload)
{
	if (self->is_str)
	{
		StdPayloadStr *box = std_alloc(sizeof(StdPayloadStr), _Alignof(StdPayloadStr));

		box->ptr = self->str;
		box->len = self->str_len;
		*payload = STD_PAYLOAD_STR;
		return box;
	}
	else
	{
		StdString	contents = *std_FormatStringPayload_fill(self);
		StdString  *box;

		std_vec_new(&self->string, 1, 1);
		box = std_alloc(sizeof(StdString), _Alignof(StdString));
		*box = contents;
		*payload = STD_PAYLOAD_STRING;
		return box;
	}
}

/* StdHook is panicking.rs's Hook: the default hook, or a custom one, which takes the hook's PanicHookInfo. */
typedef struct StdHook
{
	bool		custom;
	void		(*hook) (const StdPanicHookInfo *info);
} StdHook;

/* STD_HOOK is HOOK, behind its lock: Hook::Default until set_hook sets another. */
static StdRwLock std_hook_lock = STD_RW_LOCK_NEW;
static StdHook STD_HOOK = {false, NULL};

/*
 * std_panic_set_hook is set_hook: a panic from a panicking thread, else
 * HOOK replaced under its write lock, the old hook dropped after it, which
 * for a function is nothing.
 */
void
std_panic_set_hook(void (*hook) (const StdPanicHookInfo *info))
{
	StdRwLockWriteGuard guard;

	if (std_thread_panicking())
	{
		static const char text[] = "cannot modify the panic hook from a panicking thread";

		std_panic(STD_PANICKING_STD_FILE, 144, 9, text, sizeof(text) - 1);
	}
	(void) std_rw_lock_write(&std_hook_lock, &STD_HOOK, &guard);
	STD_HOOK.custom = true;
	STD_HOOK.hook = hook;
	std_RwLockWriteGuard_drop(&guard);
}

/*
 * std_PanicHookInfo_payload_as_str is PanicHookInfo::payload_as_str: the
 * payload's text when it is a &str or a String.
 */
bool
std_PanicHookInfo_payload_as_str(const StdPanicHookInfo *self, const char **text, size_t *len)
{
	if (self->payload == STD_PAYLOAD_STR)
	{
		const StdPayloadStr *s = self->value;

		*text = s->ptr;
		*len = s->len;
		return true;
	}
	if (self->payload == STD_PAYLOAD_STRING)
	{
		const StdString *s = self->value;

		*text = s->ptr;
		*len = s->len;
		return true;
	}
	return false;
}

/* std_Location_Display_fmt is Location's Display: the file, the line, and the column, colon-separated. */
static bool
std_Location_Display_fmt(const void *value, StdFormatter *f)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1), STD_FMT_LIT(":"),
	STD_FMT_ARG(2)};
	const StdLocation *self = value;
	StdStr		file = {self->file, strlen(self->file)};
	StdFmtArg	args[] = {{&file, std_str_Display_fmt}, {&self->line, std_u32_Display_fmt},
	{&self->col, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	return std_fmt_write_fmt_Unchecked(f, &arguments);
}

/*
 * std_panic_with_hook is std's panic_with_hook: the panic counted, an abort
 * when it must, HOOK's hook under its read lock (the default one only when
 * panic_output has a stream, which it always does), and then an abort for a
 * panic that can't unwind, or the unwind with the payload's box.
 */
pg_noreturn static void
std_panic_with_hook(StdPanicPayload *payload, const StdLocation *location, bool can_unwind,
					bool force_no_backtrace)
{
	StdMustAbort must_abort = std_panic_count_increase(true);
	StdRwLockReadGuard guard;
	StdPayloadStr literal;
	StdPanicHookInfo info;
	StdPayload	kind;
	void	   *box;

	if (must_abort != STD_MUST_ABORT_NONE)
	{
		StdStderr	out = std_Stderr_new();

		if (must_abort == STD_MUST_ABORT_PANIC_IN_HOOK)
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("panicked at "), STD_FMT_ARG(0), STD_FMT_LIT(":\n"),
			STD_FMT_ARG(1), STD_FMT_LIT("\nthread panicked while processing panic. aborting.\n")};
			StdStr		message = {"", 0};
			StdFmtArg	args[] = {{location, std_Location_Display_fmt}, {&message, std_str_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			if (!std_PanicPayload_as_str(payload, &message.ptr, &message.len))
				message.len = 0;
			(void) std_Stderr_write_fmt(&out, &arguments);
		}
		else
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("aborting due to panic at "), STD_FMT_ARG(0),
			STD_FMT_LIT(":\n"), STD_FMT_ARG(1), STD_FMT_LIT("\n")};
			StdFmtArg	args[] = {{location, std_Location_Display_fmt}, {payload, std_PanicPayload_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			(void) std_Stderr_write_fmt(&out, &arguments);
		}
		std_process_abort();
	}
	(void) std_rw_lock_read_Unchecked(&std_hook_lock, &STD_HOOK, &guard);
	if (payload->is_str)
	{
		literal.ptr = payload->str;
		literal.len = payload->str_len;
		info.payload = STD_PAYLOAD_STR;
		info.value = &literal;
	}
	else
	{
		info.payload = STD_PAYLOAD_STRING;
		info.value = std_FormatStringPayload_fill(payload);
	}
	info.location = location;
	info.can_unwind = can_unwind;
	info.force_no_backtrace = force_no_backtrace;
	if (STD_HOOK.custom)
		STD_HOOK.hook(&info);
	else
		std_default_hook(&info);
	std_RwLockReadGuard_drop(&guard);
	std_panic_count_finished_panic_hook();
	if (!can_unwind)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("thread caused non-unwinding panic. aborting.\n")};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};
		StdStderr	out = std_Stderr_new();

		(void) std_Stderr_write_fmt(&out, &arguments);
		std_process_abort();
	}
	box = std_PanicPayload_take_box(payload, &kind);
	std_unwind_begin(kind, box);
}

/*
 * std_begin_panic_handler is std's panic handler, rust_begin_unwind: the
 * PanicInfo's location, which is always there, and its message, a
 * StaticStrPayload when it is one literal, else a FormatStringPayload, to
 * panic_with_hook.
 */
pg_noreturn static void
std_begin_panic_handler(const StdFmtArguments *message, const StdLocation *location, bool can_unwind,
						bool force_no_backtrace)
{
	StdPanicPayload payload = {0};

	payload.is_str = std_fmt_as_str_Unchecked(message, &payload.str, &payload.str_len);
	payload.inner = message;
	std_panic_with_hook(&payload, location, can_unwind, force_no_backtrace);
}

/*
 * std_panic_fmt is core's panic_fmt: a PanicInfo of the message at its
 * caller's location, a panic that can unwind, to the panic handler.
 */
void
std_panic_fmt(const char *file, uint32 line, uint32 col, const struct StdFmtArguments *args)
{
	StdLocation location = {file, line, col};

	std_begin_panic_handler(args, &location, true, false);
}

/* std_panic is core's panic of a literal: panic_fmt of Arguments::from_str. */
void
std_panic(const char *file, uint32 line, uint32 col, const char *text, size_t len)
{
	StdFmtPiece pieces[] = {{text,
			len,
			0,
	{0}}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

	std_panic_fmt(file, line, col, &arguments);
}

/*
 * std_panic_nounwind_fmt is core's panic_nounwind_fmt of a literal message,
 * a panic that can't unwind: a PanicInfo with can_unwind false to the panic
 * handler, whose panic_with_hook writes that it can't unwind and aborts.
 */
pg_noreturn static void
std_panic_nounwind_fmt(const char *file, uint32 line, uint32 col, const char *text, size_t len,
					   bool force_no_backtrace)
{
	StdFmtPiece pieces[] = {{text,
			len,
			0,
	{0}}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};
	StdLocation location = {file, line, col};
	StdUnwind	unwind;

	STD_NOUNWIND_PUSH(&unwind);
	std_begin_panic_handler(&arguments, &location, false, force_no_backtrace);
}

/* std_panic_nounwind is panic_nounwind_fmt of a literal message, its backtrace not forced off. */
void
std_panic_nounwind(const char *file, uint32 line, uint32 col, const char *text, size_t len)
{
	std_panic_nounwind_fmt(file, line, col, text, len, false);
}

#ifndef WIN32
/*
 * std_panic_in_cleanup is core's panic_in_cleanup: panic_nounwind_nobacktrace
 * of its message, at core's own location, with no backtrace.
 */
pg_noreturn static void
std_panic_in_cleanup(void)
{
	static const char text[] = "panic in a destructor during cleanup";

	std_panic_nounwind_fmt(STD_PANICKING_FILE, 233, 5, text, sizeof(text) - 1, true);
}
#endif

/* std_panic_cannot_unwind is core's panic_cannot_unwind: panic_nounwind of its message, at core's own location. */
void
std_panic_cannot_unwind(void)
{
	static const char text[] = "panic in a function that cannot unwind";

	std_panic_nounwind_fmt(STD_PANICKING_FILE, 225, 5, text, sizeof(text) - 1, false);
}

/*
 * std_resume_unwind is resume_unwind: the panic counted without the hook,
 * an abort when it must, then the unwind with the payload's box as it is.
 */
void
std_resume_unwind(StdPayload payload, void *box)
{
	StdMustAbort must_abort = std_panic_count_increase(false);

	if (must_abort != STD_MUST_ABORT_NONE)
	{
		static const StdFmtPiece in_hook[] = {STD_FMT_LIT("thread panicked while processing panic. aborting.\n")};
		static const StdFmtPiece always[] = {STD_FMT_LIT("aborting due to panic\n")};
		StdFmtArguments arguments = {must_abort == STD_MUST_ABORT_PANIC_IN_HOOK ? in_hook : always, 1, NULL};
		StdStderr	out = std_Stderr_new();

		(void) std_Stderr_write_fmt(&out, &arguments);
		std_process_abort();
	}
	std_unwind_begin(payload, box);
}

/* std_unwind_push_catching pushes the frame of a pad that catches the unwind, keeping the port heap it was pushed under. */
void
std_unwind_push_catching(StdUnwind *unwind)
{
	STD_UNWIND_PUSH(unwind);
	unwind->catches = true;
	unwind->heap = std_heap_current();
}

/*
 * std_unwind_end ends an unwind at the frame that catches it, an entry's or
 * catch_unwind's: it puts back the port heap the frame was pushed under,
 * frees the exception record, lowers the panic count, and returns the
 * payload, which the catcher drops.
 */
StdCaught
std_unwind_end(StdUnwind *unwind)
{
	StdCaught	caught;

	std_heap_left(unwind->heap);
	std_unwind_top = unwind->prev;
#ifndef WIN32
	std_free(std_exception, STD_EXCEPTION_SIZE, MAXIMUM_ALIGNOF);
	std_exception = NULL;
#endif
	std_panic_count_decrease();
	caught.payload = std_payload;
	caught.box = std_payload_box;
	std_payload_box = NULL;
	return caught;
}

/*
 * std_catch_unwind_frame is catch_unwind's catching frame over its caller's
 * StdUnwind, which the jump writes, so the jump changes no local of the
 * function holding sigsetjmp: whether the body returned.
 */
static bool
std_catch_unwind_frame(StdUnwind *unwind, void (*body) (void *), void *captures)
{
	STD_UNWIND_PUSH_CATCHING(unwind);
	if (sigsetjmp(unwind->jump, 0) == 0)
	{
		body(captures);
		std_unwind_pop(unwind);
		return true;
	}
	return false;
}

/*
 * std_catch_unwind is catch_unwind: it runs a body and reports whether it
 * returned, and when it panicked, the payload it caught.
 */
bool
std_catch_unwind(void (*body) (void *), void *captures, StdCaught *caught)
{
	StdUnwind	unwind;

	if (std_catch_unwind_frame(&unwind, body, captures))
		return true;
	*caught = std_unwind_end(&unwind);
	return false;
}

/*
 * std_landing_frame is a landing pad's frame over its caller's StdUnwind,
 * which the jump writes, so the jump changes no local of the function
 * holding sigsetjmp: the body, or on a landing the pad and the unwind going
 * on.
 */
static void
std_landing_frame(StdUnwind *unwind, void *frame, void (*body) (void *frame), void (*pad) (void *frame))
{
	STD_UNWIND_PUSH(unwind);
	if (sigsetjmp(unwind->jump, 0) == 0)
	{
		body(frame);
		std_unwind_pop(unwind);
	}
	else
	{
		pad(frame);
		std_unwind_resume(unwind);
	}
}

/*
 * std_landing is a Rust cleanup region (the port text's section 6): the body
 * over its frame, whose pad, should the body unwind, drops what the frame
 * holds live before the unwind goes on. The frame is the caller's, so its
 * objects keep their values across the jump.
 */
void
std_landing(void *frame, void (*body) (void *frame), void (*pad) (void *frame))
{
	StdUnwind	unwind;

	std_landing_frame(&unwind, frame, body, pad);
}

/* std_Caught_downcast_str is downcast_ref::<&str>: the text of a literal message's payload. */
bool
std_Caught_downcast_str(const StdCaught *self, const char **text, size_t *len)
{
	const StdPayloadStr *box = self->box;

	if (self->payload != STD_PAYLOAD_STR)
		return false;
	*text = box->ptr;
	*len = box->len;
	return true;
}

/* std_Caught_downcast_string is downcast_ref::<String>: the text of a formatted message's payload. */
bool
std_Caught_downcast_string(const StdCaught *self, const char **text, size_t *len)
{
	const StdString *box = self->box;

	if (self->payload != STD_PAYLOAD_STRING)
		return false;
	*text = box->ptr;
	*len = box->len;
	return true;
}

/* std_Caught_drop drops a caught payload: a String's buffer, then its box. */
void
std_Caught_drop(StdCaught *self)
{
	switch (self->payload)
	{
		case STD_PAYLOAD_STR:
			std_free(self->box, sizeof(StdPayloadStr), _Alignof(StdPayloadStr));
			break;
		case STD_PAYLOAD_STRING:
			std_vec_free_Unchecked((StdString *) self->box, 1, 1);
			std_free(self->box, sizeof(StdString), _Alignof(StdString));
			break;
#ifndef FRONTEND
		case STD_PAYLOAD_OUT_OF_MEMORY:
			break;
#endif
	}
}

/*
 * std_Caught_drop_Checked is Box<dyn Any + Send>'s drop as a crate's code
 * runs it: the payload's own drop through its vtable, std's, and then Box's
 * drop, whose Layout::for_value_raw is checked, freeing the box but a
 * zero-sized one.
 */
void
std_Caught_drop_Checked(StdCaught *self)
{
	switch (self->payload)
	{
		case STD_PAYLOAD_STR:
			std_Layout_for_value_raw_check(sizeof(StdPayloadStr), _Alignof(StdPayloadStr));
			std_free(self->box, sizeof(StdPayloadStr), _Alignof(StdPayloadStr));
			break;
		case STD_PAYLOAD_STRING:
			std_vec_free_Unchecked((StdString *) self->box, 1, 1);
			std_Layout_for_value_raw_check(sizeof(StdString), _Alignof(StdString));
			std_free(self->box, sizeof(StdString), _Alignof(StdString));
			break;
#ifndef FRONTEND
		case STD_PAYLOAD_OUT_OF_MEMORY:
			std_Layout_for_value_raw_check(0, 1);
			break;
#endif
	}
}

/*
 * std_Caught_downcast_string_Checked is Box<dyn Any + Send>::downcast::<String>
 * as a crate's code runs it: for a String payload, its text, the box taken as
 * a Box<String> by Box::from_raw_in, whose Unique::new_unchecked is checked.
 */
bool
std_Caught_downcast_string_Checked(const StdCaught *self, const char **text, size_t *len)
{
	if (!std_Caught_downcast_string(self, text, len))
		return false;
	std_NonNull_new_unchecked_check(self->box, STD_PTR_UNIQUE_FILE, 88, 36);
	return true;
}

/*
 * std_Caught_drop_string_Checked is the drop glue of the Box<String> a
 * downcast gave, as a crate's code runs it: the String's buffer, then Box's
 * drop, Layout::for_value_raw checked, freeing the box.
 */
void
std_Caught_drop_string_Checked(StdCaught *self)
{
	std_vec_free_Checked((StdString *) self->box, 1, 1);
	std_Layout_for_value_raw_check(sizeof(StdString), _Alignof(StdString));
	std_free(self->box, sizeof(StdString), _Alignof(StdString));
}

/*
 * std_option_expect_failed is core's expect_failed, Option::expect's panic
 * on None: panic_display of the message, formatted into the payload's
 * String, at the caller's location.
 */
void
std_option_expect_failed(const char *msg, size_t msg_len, const char *file, uint32 line, uint32 col)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdStr text = {msg, msg_len};
	StdFmtArg	args[] = {{&text, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(file, line, col, &arguments);
}

/* std_option_unwrap_failed is core's unwrap_failed, Option::unwrap's panic on None, at the caller's location. */
void
std_option_unwrap_failed(const char *file, uint32 line, uint32 col)
{
	static const char text[] = "called `Option::unwrap()` on a `None` value";

	std_panic(file, line, col, text, sizeof(text) - 1);
}

/*
 * std_result_unwrap_failed is core's Result unwrap_failed, the panic of
 * Result::expect and Result::unwrap on Err: the message and the error's
 * Debug, formatted into the payload's String, at the caller's location.
 */
void
std_result_unwrap_failed(const char *msg, size_t msg_len, const void *error, StdFmtFn error_fmt, const char *file,
						 uint32 line, uint32 col)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
	StdStr text = {msg, msg_len};
	StdFmtArg	args[] = {{&text, std_str_Display_fmt}, {error, error_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_panic_fmt(file, line, col, &arguments);
}
