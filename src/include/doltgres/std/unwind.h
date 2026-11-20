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
 * The port's panics and unwinding (the port text's section 6): a panic keeps
 * its message as pg's panic hook keeps it and jumps to the innermost landing
 * pad, which runs the drops Rust's landing pad runs and resumes to the next,
 * until the entry's frame ends the unwind.
 */
#ifndef DOLTGRES_STD_UNWIND_H
#define DOLTGRES_STD_UNWIND_H

#include <setjmp.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifndef FRONTEND
#include "port/atomics.h"
#endif

/* STD_SEP is the separator Rust's Location::file() uses on the platform. */
#ifdef WIN32
#define STD_SEP "\\"
#else
#define STD_SEP "/"
#endif

/* StdPayload is what a panic unwinds with: its payload box's type. */
typedef enum StdPayload
{
	/* STD_PAYLOAD_STR is a literal message's Box<&str>. */
	STD_PAYLOAD_STR,
	/* STD_PAYLOAD_STRING is a formatted message's Box<String>. */
	STD_PAYLOAD_STRING,
#ifndef FRONTEND

	/*
	 * STD_PAYLOAD_OUT_OF_MEMORY is a failed allocation's zero-sized
	 * OutOfMemory, pg's.
	 */
	STD_PAYLOAD_OUT_OF_MEMORY,
#endif
} StdPayload;

/* StdPayloadStr is the &str a literal message's box holds. */
typedef struct StdPayloadStr
{
	const char *ptr;
	size_t		len;
} StdPayloadStr;

/*
 * StdUnwind is a frame of the unwind stack: a landing pad's jump buffer,
 * the frame outside it, and whether a panic has landed on it, so its pad
 * is running the drops unwinding runs.
 */
typedef struct StdUnwind
{
	sigjmp_buf	jump;
	struct StdUnwind *prev;
	bool		landed;

	/*
	 * catches is whether the frame's pad catches, an entry's or
	 * catch_unwind's, rather than resuming.
	 */
	bool		catches;

	/*
	 * payload, box, and exception are the panic a landed pad runs its drops
	 * for, which its resume carries on, so a panic caught inside the pad
	 * leaves it as Rust's separate exception objects do.
	 */
	StdPayload	payload;
	void	   *box;
	void	   *exception;
} StdUnwind;

/*
 * StdLocation is core::panic::Location: a call site's file, with the
 * platform's separator, its line, and its column, as #[track_caller]'s
 * Location::caller gives it; each site's is a static constant.
 */
typedef struct StdLocation
{
	const char *file;
	uint32		line;
	uint32		col;
} StdLocation;

/* std_unwind_top is the innermost landing pad of the backend. */
extern StdUnwind *std_unwind_top;

/* std_panic_count is LOCAL_PANIC_COUNT's count, the backend's panics in flight: a panic raises it, a catch lowers it. */
extern size_t std_panic_count;

/* std_payload is the type of the payload of the panic in flight. */
extern StdPayload std_payload;

/* std_payload_box is the payload box of the panic in flight, NULL for out of memory's zero-sized one. */
extern void *std_payload_box;

#ifndef WIN32
/* std_exception is the exception record of the panic in flight, which gcc.rs boxes. */
extern void *std_exception;
#endif

/* std_local_in_panic_hook is panic_count's LOCAL_PANIC_COUNT in_panic_hook: whether the backend runs the panic hook. */
extern bool std_local_in_panic_hook;

/* StdCaught is the payload a catch took: its type and its box, which the catcher drops with std_Caught_drop. */
typedef struct StdCaught
{
	StdPayload	payload;
	void	   *box;
} StdCaught;

/*
 * STD_UNWIND_PUSH pushes a landing pad's frame. It is a statement of its
 * own; the region follows under `if (sigsetjmp(unwind.jump, 0) == 0)`, a
 * context C allows sigsetjmp in, which must run in the pad's frame.
 */
#define STD_UNWIND_PUSH(unwind) \
	((unwind)->prev = std_unwind_top, (unwind)->landed = false, (unwind)->catches = false, std_unwind_top = (unwind))

/* STD_UNWIND_PUSH_CATCHING pushes the frame of a pad that catches the unwind, an entry's or catch_unwind's. */
#define STD_UNWIND_PUSH_CATCHING(unwind) (STD_UNWIND_PUSH(unwind), (unwind)->catches = true)

/*
 * STD_NOUNWIND_PUSH pushes the frame of a region Rust compiles as a nounwind
 * function (an unsafe precondition's check, panic_nounwind_fmt), whose
 * landing pad is the terminate edge rustc gives it: a panic out of the
 * region is panic_cannot_unwind. std_unwind_pop ends the region.
 */
#define STD_NOUNWIND_PUSH(unwind) \
	do { \
		STD_UNWIND_PUSH(unwind); \
		if (sigsetjmp((unwind)->jump, 0) != 0) \
			std_panic_cannot_unwind(); \
	} while (0)

struct StdFmtArguments;
struct StdFormatter;

extern void std_unwind_pop(StdUnwind *unwind);
pg_noreturn extern void std_unwind_resume(StdUnwind *unwind);
pg_noreturn extern void std_unwind_begin(StdPayload payload, void *box);
pg_noreturn extern void std_process_abort(void);
pg_noreturn extern void std_panic_cannot_unwind(void);

/*
 * std_intrinsics_abort is intrinsics::abort, which rustc lowers to a trap
 * instruction (ud2 on x86-64, brk on arm64), so the process dies of the
 * trap's signal or exception rather than abort()'s.
 */
pg_noreturn static inline void
std_intrinsics_abort(void)
{
#ifdef _MSC_VER
	__ud2();
	pg_unreachable();
#else
	__builtin_trap();
#endif
}

/* The backtrace styles, BacktraceStyle's as_u8 codes; 0 is SHOULD_CAPTURE before RUST_BACKTRACE is read. */
#define STD_BACKTRACE_SHORT 1
#define STD_BACKTRACE_FULL 2
#define STD_BACKTRACE_OFF 3

struct StdIoWrite;

/*
 * StdPanicHookInfo is PanicHookInfo: the payload as payload.get() gives it,
 * a &str (StdPayloadStr) or a String, its location, and whether the panic
 * can unwind and forces no backtrace.
 */
typedef struct StdPanicHookInfo
{
	StdPayload	payload;
	const void *value;
	const StdLocation *location;
	bool		can_unwind;
	bool		force_no_backtrace;
} StdPanicHookInfo;

extern int	std_panic_get_backtrace_style(void);
extern void std_backtrace_print(struct StdIoWrite *err, int style);
extern void std_panic_set_hook(void (*hook) (const StdPanicHookInfo *info));
extern bool std_PanicHookInfo_payload_as_str(const StdPanicHookInfo *self, const char **text, size_t *len);
pg_noreturn extern void std_panic(const char *file, uint32 line, uint32 col,
								  const char *text, size_t len);
pg_noreturn extern void std_panic_nounwind(const char *file, uint32 line, uint32 col, const char *text, size_t len);
pg_noreturn extern void std_panic_fmt(const char *file, uint32 line, uint32 col,
									  const struct StdFmtArguments *args);
pg_noreturn extern void std_option_expect_failed(const char *msg, size_t msg_len, const char *file,
												 uint32 line, uint32 col);
pg_noreturn extern void std_option_unwrap_failed(const char *file, uint32 line, uint32 col);
pg_noreturn extern void std_result_unwrap_failed(const char *msg, size_t msg_len, const void *error,
												 bool (*error_fmt) (const void *, struct StdFormatter *),
												 const char *file, uint32 line, uint32 col);
extern StdCaught std_unwind_end(StdUnwind *unwind);
extern void std_Caught_drop_Checked(StdCaught *self);
extern bool std_Caught_downcast_string_Checked(const StdCaught *self, const char **text, size_t *len);
extern void std_Caught_drop_string_Checked(StdCaught *self);
extern bool std_catch_unwind(void (*body) (void *), void *captures, StdCaught *caught);
extern void std_landing(void *frame, void (*body) (void *frame), void (*pad) (void *frame));
extern bool std_Caught_downcast_str(const StdCaught *self, const char **text, size_t *len);
extern bool std_Caught_downcast_string(const StdCaught *self, const char **text, size_t *len);
extern void std_Caught_drop(StdCaught *self);

extern bool std_thread_panicking(void);
pg_noreturn extern void std_resume_unwind(StdPayload payload, void *box);

/* STD_PANIC panics with a literal message at a line and column of the file's Rust source. */
#define STD_PANIC(line, col, literal) \
	std_panic(RUST_FILE, (line), (col), (literal), sizeof(literal) - 1)

/* STD_OPTION_EXPECT_FAILED is Option::expect's panic on None with a literal message at a line and column of the file's Rust source. */
#define STD_OPTION_EXPECT_FAILED(line, col, literal) \
	std_option_expect_failed((literal), sizeof(literal) - 1, RUST_FILE, (line), (col))

/* STD_RESULT_EXPECT_FAILED is Result::expect's panic on Err with a literal message and the error's Debug at a line and column. */
#define STD_RESULT_EXPECT_FAILED(line, col, literal, error, error_fmt) \
	std_result_unwrap_failed((literal), sizeof(literal) - 1, (error), (error_fmt), RUST_FILE, (line), (col))

/* STD_OPTION_UNWRAP_FAILED is Option::unwrap's panic on None at a line and column of the file's Rust source. */
#define STD_OPTION_UNWRAP_FAILED(line, col) std_option_unwrap_failed(RUST_FILE, (line), (col))

#endif							/* DOLTGRES_STD_UNWIND_H */
