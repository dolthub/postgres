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
 * The storage crates' locks over a backend's own state. A lock under which
 * no file I/O runs borrows its taker's Held for as long as its guard lives,
 * so no file door compiles under it, and a lock held across I/O is taken only
 * with the whole Io, which its guard lends back, so it is never taken under
 * one that does no I/O (L1). A backend runs one thread, so a lock found held
 * was taken by its own taker further up, a re-entry, which is refused as the
 * bug it is rather than waited for; state shared between backends takes
 * shared memory's locks instead. A lock a panic poisoned holds state the
 * panic may have left half changed, so the next ordinary take rebuilds it, to
 * its default, which its owner fills again lazily (B5), and a drop's take,
 * which must not rebuild, gives nothing (H2). The mutexes are reader-writer
 * locks taken only exclusively, since std's mutex allocates its lock at the
 * first take on macOS, which a drop's take must not, and its reader-writer
 * lock allocates on no platform.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "lock.rs"

/* HostLockDeclared shows where a lock is declared, its file with / between folders on every platform, so a refusal reads the same everywhere. */
typedef struct HostLockDeclared
{
	const StdLocation *f0;
} HostLockDeclared;

/* Chars::next's checked instance, which Declared's Display iterates with, as host's codegen of it checks. */
#define STD_STR_SCOPE static
#define STD_STR_DECLARE
#define STD_STR_DEFINE
#define STD_STR_UB_CHECKS 1
#define STD_STR_SUFFIX Checked
#include "doltgres/std/str_template.h"

/* host_lock_Declared_Display_fmt writes the declaration's file, its separators as /, then its line and column. */
static bool
host_lock_Declared_Display_fmt(const void *value, StdFormatter *f)
{
	const HostLockDeclared *self = (const HostLockDeclared *) value;
	StdChars	chars = std_str_chars(self->f0->file, strlen(self->f0->file));
	uint32		c;
	static const StdFmtPiece pieces[] = {
		STD_FMT_LIT(":"), STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1),
	};
	StdFmtArg	args[] = {{&self->f0->line, std_u32_Display_fmt}, {&self->f0->col, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	while (std_str_Chars_next_Checked(&chars, &c))
	{
		if (!std_fmt_write_char(f, c == '\\' ? '/' : c))
			return false;
	}
	return std_fmt_write_fmt_Checked(f, &arguments);
}

/*
 * host_lock_held_already fails the take of a lock found held, naming the lock
 * by where it is declared: in the server its own backend holds it, a
 * re-entry, and under the simulation's actors another actor may, which is
 * state shared between backends outside shared memory.
 */
void
host_lock_held_already(const StdLocation *at)
{
	HostLockDeclared at_2 = {at};
	StdFmtArg	args[] = {{&at_2, host_lock_Declared_Display_fmt}};

	if (!host_acting())
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("a backend took the storage lock declared at "), STD_FMT_ARG(0),
			STD_FMT_LIT(", which it already holds"),
		};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 51, 18, &arguments);
	}
	else
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("an actor took the storage lock declared at "), STD_FMT_ARG(0),
			STD_FMT_LIT(", which it or another actor holds: state shared between backends outside shared "
						"memory, or a re-entry"),
		};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 52, 17, &arguments);
	}
}

/*
 * host_lock_shared returns a reader's guard, refusing a lock its backend
 * holds exclusively; its taker rebuilt the state of a poisoned one under a
 * write first, so a poisoned result here is a new poisoning's, already
 * rebuilt.
 */
static StdRwLockReadGuard
host_lock_shared(StdTryLockTag tag, StdRwLockReadGuard *result, const StdLocation *at)
{
	switch (tag)
	{
		case STD_TRY_LOCK_OK:
			return *result;
		case STD_TRY_LOCK_WOULD_BLOCK:
			host_lock_held_already(at);
		case STD_TRY_LOCK_POISONED:
			return *result;
	}
	pg_unreachable();
}

/*
 * host_lock_IoMutex_lock_in_drop takes the mutex for a drop, giving nothing
 * when a frame the drop unwinds through holds it or a panic poisoned it.
 */
bool
host_lock_IoMutex_lock_in_drop(HostLockIoMutex *self, HostIoInDrop *in_drop,
							   StdRwLockWriteGuard *result)
{
	StdRwLockWriteGuard guard;

	switch (std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &guard))
	{
		case STD_TRY_LOCK_OK:
			*result = guard;
			return true;
		case STD_TRY_LOCK_POISONED:
			std_RwLockWriteGuard_drop(&guard);
			return false;
		case STD_TRY_LOCK_WOULD_BLOCK:
			return false;
	}
	pg_unreachable();
}

/* host_lock_IoMutex_lock_closure_0 is lock's || self.lock.clear_poison(), its capture the lock. */
static void
host_lock_IoMutex_lock_closure_0(void *captures)
{
	std_rw_lock_clear_poison((StdRwLock *) captures);
}

/* host_lock_IoMutex_lock takes the mutex, lending the taker's Held to the guard. */
HostLockLockedRwLockWriteGuard
host_lock_IoMutex_lock(HostLockIoMutex *self, const HostLockType *t, HostIoHeld *held)
{
	StdRwLockWriteGuard guard;
	StdTryLockTag tag;

	host_yield_point();
	tag = std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &guard);
	return (HostLockLockedRwLockWriteGuard)
	{
		t->taken(tag, &guard, host_lock_IoMutex_lock_closure_0, &self->lock, NULL, self->at), held
	};
}

/* host_lock_IoRwLock_read_closure_0 is read's || self.lock.clear_poison(), its capture the lock. */
static void
host_lock_IoRwLock_read_closure_0(void *captures)
{
	std_rw_lock_clear_poison((StdRwLock *) captures);
}

/*
 * host_lock_IoRwLock_read takes the lock shared, lending the taker's Held to
 * the guard. A poisoned lock is rebuilt under a write, since a shared guard
 * can't change its state.
 */
HostLockLockedRwLockReadGuard
host_lock_IoRwLock_read(HostLockIoRwLock *self, const HostLockType *t, HostIoHeld *held)
{
	StdRwLockReadGuard guard;
	StdTryLockTag tag;

	host_yield_point();
	if (std_rw_lock_is_poisoned(&self->lock))
	{
		StdRwLockWriteGuard write;
		StdRwLockWriteGuard tmp_1;

		tag = std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &write);
		tmp_1 = t->taken(tag, &write, host_lock_IoRwLock_read_closure_0, &self->lock, NULL, self->at);
		std_RwLockWriteGuard_drop(&tmp_1);
	}
	tag = std_rw_lock_try_read(&self->lock, HOST_LOCK_VALUE(self), &guard);
	return (HostLockLockedRwLockReadGuard)
	{
		host_lock_shared(tag, &guard, self->at), held
	};
}

/* host_lock_IoRwLock_write_closure_0 is write's || self.lock.clear_poison(), its capture the lock. */
static void
host_lock_IoRwLock_write_closure_0(void *captures)
{
	std_rw_lock_clear_poison((StdRwLock *) captures);
}

/* host_lock_IoRwLock_write takes the lock exclusively, lending the taker's Held to the guard. */
HostLockLockedRwLockWriteGuard
host_lock_IoRwLock_write(HostLockIoRwLock *self, const HostLockType *t, HostIoHeld *held)
{
	StdRwLockWriteGuard guard;
	StdTryLockTag tag;

	host_yield_point();
	tag = std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &guard);
	return (HostLockLockedRwLockWriteGuard)
	{
		t->taken(tag, &guard, host_lock_IoRwLock_write_closure_0, &self->lock, NULL, self->at), held
	};
}

/* Locked<RwLockWriteGuard<T>>'s methods and drop, which lock.h declares. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_LOCKED_GS RwLockWriteGuard
#define HOST_LOCK_LOCKED_G StdRwLockWriteGuard
#define HOST_LOCK_LOCKED_G_DROP std_RwLockWriteGuard_drop
#define HOST_LOCK_LOCKED_DEREF_MUT
#include "doltgres/host/lock_template.h"

/* Locked<RwLockReadGuard<T>>'s methods and drop, which lock.h declares. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_LOCKED_GS RwLockReadGuard
#define HOST_LOCK_LOCKED_G StdRwLockReadGuard
#define HOST_LOCK_LOCKED_G_DROP std_RwLockReadGuard_drop
#include "doltgres/host/lock_template.h"

/* host_lock_LongMutex_lock_closure_0 is lock's || self.lock.clear_poison(), its capture the lock. */
static void
host_lock_LongMutex_lock_closure_0(void *captures)
{
	std_rw_lock_clear_poison((StdRwLock *) captures);
}

/* host_lock_LongMutex_lock takes the mutex, lending the taker's Io, or its registration's view of one, to the guard. */
HostLockLending
host_lock_LongMutex_lock(HostLockLongMutex *self, const HostLockType *t, HostIoTakesShort io)
{
	StdRwLockWriteGuard guard;
	StdTryLockTag tag;

	host_yield_point();
	tag = std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &guard);
	return (HostLockLending)
	{
		t->taken(tag, &guard, host_lock_LongMutex_lock_closure_0, &self->lock, NULL, self->at), io
	};
}

/* host_lock_Lending_io lends the guard's capability on, for file I/O or a lock under it. */
HostIoTakesShort
host_lock_Lending_io(HostLockLending *self)
{
	return self->io;
}

/*
 * host_lock_Lending_split returns the locked value and the guard's capability
 * together, to change the one with I/O the other does.
 */
HostLockPartsTakesShort
host_lock_Lending_split(HostLockLending *self)
{
	return (HostLockPartsTakesShort)
	{
		self->guard.data, self->io
	};
}

/* host_lock_Lending_Deref_deref is the locked value. */
const void *
host_lock_Lending_Deref_deref(const HostLockLending *self)
{
	return self->guard.data;
}

/* host_lock_Lending_DerefMut_deref_mut is the locked value. */
void *
host_lock_Lending_DerefMut_deref_mut(HostLockLending *self)
{
	return self->guard.data;
}

/* host_lock_Lending_drop is the guard's drop glue, its lock's write guard's. */
void
host_lock_Lending_drop(HostLockLending *self)
{
	std_RwLockWriteGuard_drop(&self->guard);
}

/* host_lock_ShortMutex_lock_closure_0 is lock's || self.lock.clear_poison(), its capture the lock. */
static void
host_lock_ShortMutex_lock_closure_0(void *captures)
{
	std_rw_lock_clear_poison((StdRwLock *) captures);
}

/*
 * host_lock_ShortMutex_lock takes the mutex, lending a view of the taker's Io
 * that does file I/O for the step to the guard.
 */
HostLockShort
host_lock_ShortMutex_lock(HostLockShortMutex *self, const HostLockType *t,
						  HostIoTakesShort io)
{
	StdRwLockWriteGuard guard;
	StdTryLockTag tag;
	StdRwLockWriteGuard taken;

	host_yield_point();
	tag = std_rw_lock_try_write(&self->lock, HOST_LOCK_VALUE(self), &guard);
	taken = t->taken(tag, &guard, host_lock_ShortMutex_lock_closure_0, &self->lock, NULL, self->at);
	return (HostLockShort)
	{
		taken, host_io_ShortIo_new(io)
	};
}

/* host_lock_Short_io lends the guard's view on, for the step's file I/O or a lock under which no I/O runs. */
HostIoShortIo *
host_lock_Short_io(HostLockShort *self)
{
	return &self->io;
}

/*
 * host_lock_Short_split returns the locked value and the guard's view
 * together, to change the one with the I/O the other does.
 */
HostLockPartsShortIo
host_lock_Short_split(HostLockShort *self)
{
	return (HostLockPartsShortIo)
	{
		self->guard.data, &self->io
	};
}

/* host_lock_Short_Deref_deref is the locked value. */
const void *
host_lock_Short_Deref_deref(const HostLockShort *self)
{
	return self->guard.data;
}

/* host_lock_Short_DerefMut_deref_mut is the locked value. */
void *
host_lock_Short_DerefMut_deref_mut(HostLockShort *self)
{
	return self->guard.data;
}

/* host_lock_Short_drop is the guard's drop glue, its lock's write guard's. */
void
host_lock_Short_drop(HostLockShort *self)
{
	std_RwLockWriteGuard_drop(&self->guard);
}

#ifdef DOLTGRES_TEST_HOST

/* host_lock_tests_I32_Default_default is i32's Default, zero. */
static void
host_lock_tests_I32_Default_default(void *result, const StdLocation *caller)
{
	*(int32 *) result = 0;
}

/* host_lock_tests_Tuple_Default_default is ()'s Default, which makes nothing. */
static void
host_lock_tests_Tuple_Default_default(void)
{
}

/* host_lock_tests_VecU8_Default_default is Vec<u8>'s Default, an empty vector. */
static void
host_lock_tests_VecU8_Default_default(void *result, const StdLocation *caller)
{
	std_vec_new((StdVec *) result, 1, 1);
}

/* host_lock_tests_VecU8_drop is Vec<u8>'s drop glue, which frees its buffer. */
static void
host_lock_tests_VecU8_drop(void *value)
{
	std_vec_free_Checked((StdVec *) value, 1, 1);
}

/* taken at i32, its table HOST_LOCK_TESTS_I32_LOCK_TYPE, and IoMutex<i32> with its new. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS I32
#define HOST_LOCK_TAKEN_T int32
#define HOST_LOCK_TAKEN_DEFAULT(place) host_lock_tests_I32_Default_default((place), NULL)
#define HOST_LOCK_TAKEN_TYPE HOST_LOCK_TESTS_I32_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS I32
#define HOST_LOCK_T int32
#include "doltgres/host/lock_template.h"

/* taken at (), its table HOST_LOCK_TESTS_TUPLE_LOCK_TYPE, and IoMutex<()> with its new. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Tuple
#define HOST_LOCK_TAKEN_UNIT
#define HOST_LOCK_TAKEN_DEFAULT() host_lock_tests_Tuple_Default_default()
#define HOST_LOCK_TAKEN_TYPE HOST_LOCK_TESTS_TUPLE_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Tuple
#define HOST_LOCK_UNIT
#include "doltgres/host/lock_template.h"

/* IoRwLock<()> with its new. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_IO_RW_LOCK
#define HOST_LOCK_TS Tuple
#define HOST_LOCK_UNIT
#include "doltgres/host/lock_template.h"

/* taken at Vec<u8>, its table HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, and IoMutex<Vec<u8>> with its new. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecU8
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) host_lock_tests_VecU8_Default_default((place), NULL)
#define HOST_LOCK_TAKEN_DROP(place) host_lock_tests_VecU8_drop(place)
#define HOST_LOCK_TAKEN_TYPE HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecU8
#define HOST_LOCK_T StdVec
#include "doltgres/host/lock_template.h"

/* host_lock_tests_with_held runs a test's body with a lock scope of its own. */
static void
host_lock_tests_with_held(void (*body) (void *captures, HostIoHeld *held, void *result),
						  void *captures, void *result)
{
	HostIoHeld	held = host_io_Held_testing();

	body(captures, &held, result);
}

/* TestHostLockTestsAGuardGivesTheValueAndLendsTheHeldClosure0 is closure_0's captures: the two mutexes. */
typedef struct TestHostLockTestsAGuardGivesTheValueAndLendsTheHeldClosure0
{
	HostLockIoMutexI32 *outer;
	HostLockIoMutexI32 *inner;
} TestHostLockTestsAGuardGivesTheValueAndLendsTheHeldClosure0;

/*
 * test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_0
 * adds the inner mutex's value to the outer's, under the outer's guard.
 */
static void
test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_0(void *captures, HostIoHeld *held,
																		  void *result)
{
	TestHostLockTestsAGuardGivesTheValueAndLendsTheHeldClosure0 *c = captures;
	volatile HostLockLockedRwLockWriteGuard guard;
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	volatile bool tmp_1_live = false;
	StdUnwind	unwind;

	guard = host_lock_IoMutex_lock(&c->outer->header, &HOST_LOCK_TESTS_I32_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		void	   *value;
		HostIoHeld *held_2;

		host_lock_LockedRwLockWriteGuard_split((HostLockLockedRwLockWriteGuard *) &guard, &value, &held_2);
		tmp_1 = host_lock_IoMutex_lock(&c->inner->header, &HOST_LOCK_TESTS_I32_LOCK_TYPE, held_2);
		tmp_1_live = true;
		*(int32 *) value = STD_ADD(i32, *(int32 *) value,
								   *(const int32 *) host_lock_LockedRwLockWriteGuard_Deref_deref((HostLockLockedRwLockWriteGuard *) &tmp_1),
								   373, 13);
		tmp_1_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &guard);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &guard);
}

/* test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_1 returns the outer mutex's value. */
static void
test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_1(void *captures, HostIoHeld *held,
																		  void *result)
{
	HostLockIoMutexI32 *outer = captures;
	HostLockLockedRwLockWriteGuard tmp_1 = host_lock_IoMutex_lock(&outer->header, &HOST_LOCK_TESTS_I32_LOCK_TYPE, held);

	*(int32 *) result = *(const int32 *) host_lock_LockedRwLockWriteGuard_Deref_deref(&tmp_1);
	host_lock_LockedRwLockWriteGuard_drop(&tmp_1);
}

/*
 * test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held: a lock
 * under which no I/O runs gives its value through its guard, and lends the
 * taker's Held on.
 */
void
test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held(void)
{
	static const StdLocation outer_at = {RUST_FILE, 369, 31};
	static const StdLocation inner_at = {RUST_FILE, 369, 48};
	HostLockIoMutexI32 outer;
	HostLockIoMutexI32 inner;
	int32		one = 1;
	int32		two = 2;
	TestHostLockTestsAGuardGivesTheValueAndLendsTheHeldClosure0 captures = {&outer, &inner};
	int32		got;
	int32		expected = 3;

	host_lock_IoMutexI32_new(&outer, one, &outer_at);
	host_lock_IoMutexI32_new(&inner, two, &inner_at);
	host_lock_tests_with_held(test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_0,
							  &captures, NULL);
	host_lock_tests_with_held(test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held_closure_1,
							  &outer, &got);
	STD_ASSERT_EQ(375, 9, got == expected, &got, std_i32_Debug_fmt, &expected, std_i32_Debug_fmt);
}

/*
 * host_lock_tests_assert_refused checks that a body panics with a backend's
 * refusal of a lock it holds, which names the lock's declaration with /
 * between folders on every platform.
 */
static void
host_lock_tests_assert_refused(void (*body) (void *), void *captures)
{
	StdCaught	payload;
	volatile bool payload_live = true;
	volatile bool message_live = false;
	const char *message;
	size_t		len;
	StdStr text;
	StdFmtArg	args[1];
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	static const char start[] = "a backend took the storage lock declared at ";
	static const char end[] = ", which it already holds";
	StdUnwind	unwind;

	if (std_catch_unwind(body, captures, &payload))
	{
		STD_RESULT_EXPECT_FAILED(381, 84, "the take is refused", NULL, std_unit_Debug_fmt);
	}
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		if (!std_Caught_downcast_string_Checked(&payload, &message, &len))
		{
			STD_RESULT_EXPECT_FAILED(382, 52, "the refusal's message is formatted", &payload, std_any_Debug_fmt);
		}
		payload_live = false;
		message_live = true;
		text.		ptr = message;
		text.		len = len;

		args[0].value = &text;
		args[0].fmt = std_str_Display_fmt;
		STD_CHECK_ALIGNED(payload.box, _Alignof(StdString), 383, 17);
		STD_CHECK_NULL_REF(payload.box, 383, 17);
		if (len >= sizeof(start) - 1)
			std_range_to_check(sizeof(start) - 1, len, STD_SLICE_MOD_FILE, 2635, 43);
		if (!(len >= sizeof(start) - 1 && memcmp(message, start, sizeof(start) - 1) == 0))
			std_panic_fmt(RUST_FILE, 383, 9, &arguments);
		STD_CHECK_ALIGNED(payload.box, _Alignof(StdString), 384, 17);
		STD_CHECK_NULL_REF(payload.box, 384, 17);
		if (!(len >= sizeof(end) - 1 && memcmp(message + len - (sizeof(end) - 1), end, sizeof(end) - 1) == 0))
			std_panic_fmt(RUST_FILE, 384, 9, &arguments);
		STD_CHECK_ALIGNED(payload.box, _Alignof(StdString), 385, 18);
		STD_CHECK_NULL_REF(payload.box, 385, 18);
		if (std_str_contains_char_Checked(message, len, '\\'))
			std_panic_fmt(RUST_FILE, 385, 9, &arguments);
		message_live = false;
		std_Caught_drop_string_Checked(&payload);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (message_live)
			std_Caught_drop_string_Checked(&payload);
		if (payload_live)
			std_Caught_drop_Checked(&payload);
		std_unwind_resume(&unwind);
	}
}

/* test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0_0 takes the mutex again and drops the guard. */
static void
test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0_0(void *captures, HostIoHeld *second,
																			void *result)
{
	HostLockIoMutexTuple *mutex = captures;
	HostLockLockedRwLockWriteGuard tmp_1 = host_lock_IoMutex_lock(&mutex->header, &HOST_LOCK_TESTS_TUPLE_LOCK_TYPE,
																  second);

	host_lock_LockedRwLockWriteGuard_drop(&tmp_1);
}

/*
 * test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0
 * takes the mutex, then takes it again under another scope while it holds
 * it.
 */
static void
test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0(void *captures, HostIoHeld *first,
																		  void *result)
{
	HostLockIoMutexTuple *mutex = captures;
	volatile HostLockLockedRwLockWriteGuard _first;
	StdUnwind	unwind;

	_first = host_lock_IoMutex_lock(&mutex->header, &HOST_LOCK_TESTS_TUPLE_LOCK_TYPE, first);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_lock_tests_with_held(test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0_0,
								  mutex, NULL);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &_first);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &_first);
}

/* test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0 runs the takes with a lock scope of their own. */
static void
test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0(void *captures)
{
	host_lock_tests_with_held(test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0_0,
							  captures, NULL);
}

/*
 * test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused: a backend's
 * second take of a mutex it holds is a re-entry, refused rather than waited
 * for.
 */
void
test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused(void)
{
	static const StdLocation at = {RUST_FILE, 391, 21};
	HostLockIoMutexTuple mutex;

	host_lock_IoMutexTuple_new(&mutex, &at);
	host_lock_tests_assert_refused(test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused_closure_0,
								   &mutex);
}

/* test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0_0 takes the lock exclusively and drops the guard. */
static void
test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0_0(void *captures, HostIoHeld *third,
																	 void *result)
{
	HostLockIoRwLockTuple *lock = captures;
	HostLockLockedRwLockWriteGuard tmp_1 = host_lock_IoRwLock_write(&lock->header, &HOST_LOCK_TESTS_TUPLE_LOCK_TYPE,
																	third);

	host_lock_LockedRwLockWriteGuard_drop(&tmp_1);
}

/*
 * test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0 reads
 * the lock again under a second scope, then writes it under a third.
 */
static void
test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0(void *captures, HostIoHeld *second,
																   void *result)
{
	HostLockIoRwLockTuple *lock = captures;
	volatile HostLockLockedRwLockReadGuard _second;
	StdUnwind	unwind;

	_second = host_lock_IoRwLock_read(&lock->header, &HOST_LOCK_TESTS_TUPLE_LOCK_TYPE, second);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_lock_tests_with_held(test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0_0, lock,
								  NULL);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockReadGuard_drop((HostLockLockedRwLockReadGuard *) &_second);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockReadGuard_drop((HostLockLockedRwLockReadGuard *) &_second);
}

/*
 * test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0 reads the
 * lock, then the reads and the write under further scopes.
 */
static void
test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0(void *captures, HostIoHeld *first,
																 void *result)
{
	HostLockIoRwLockTuple *lock = captures;
	volatile HostLockLockedRwLockReadGuard _first;
	StdUnwind	unwind;

	_first = host_lock_IoRwLock_read(&lock->header, &HOST_LOCK_TESTS_TUPLE_LOCK_TYPE, first);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		host_lock_tests_with_held(test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0_0, lock,
								  NULL);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockReadGuard_drop((HostLockLockedRwLockReadGuard *) &_first);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockReadGuard_drop((HostLockLockedRwLockReadGuard *) &_first);
}

/* test_host_lock_tests_a_write_under_a_read_is_refused_closure_0 runs the takes with a lock scope of their own. */
static void
test_host_lock_tests_a_write_under_a_read_is_refused_closure_0(void *captures)
{
	host_lock_tests_with_held(test_host_lock_tests_a_write_under_a_read_is_refused_closure_0_0, captures, NULL);
}

/*
 * test_host_lock_tests_a_write_under_a_read_is_refused: readers share a
 * reader-writer lock, and a writer under a reader is a re-entry, refused.
 */
void
test_host_lock_tests_a_write_under_a_read_is_refused(void)
{
	static const StdLocation at = {RUST_FILE, 403, 20};
	HostLockIoRwLockTuple lock;

	host_lock_IoRwLockTuple_new(&lock, &at);
	host_lock_tests_assert_refused(test_host_lock_tests_a_write_under_a_read_is_refused_closure_0, &lock);
}

/* HOST_LOCK_TESTS_STATIC_AT is where STATIC is declared. */
static const StdLocation HOST_LOCK_TESTS_STATIC_AT = {RUST_FILE, 416, 39};

/* HOST_LOCK_TESTS_STATIC is backend-local state behind a storage lock in a static, as a crate keeps it. */
static HostLockIoMutexVecU8 HOST_LOCK_TESTS_STATIC =
{
	HOST_LOCK_NEW(&HOST_LOCK_TESTS_STATIC_AT), STD_VEC_NEW
};

/* test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_0 pushes 1 onto STATIC. */
static void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_0(void *captures,
																				   HostIoHeld *held,
																				   void *result)
{
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	StdUnwind	unwind;

	tmp_1 = host_lock_IoMutex_lock(&HOST_LOCK_TESTS_STATIC.header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_u8_push_Checked(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &tmp_1), 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
}

/*
 * test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1_0
 * pushes 2 onto STATIC, takes it again, and panics while holding it.
 */
static void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1_0(void *captures,
																					 HostIoHeld *held,
																					 void *result)
{
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	volatile HostLockLockedRwLockWriteGuard guard;
	StdUnwind	unwind;

	tmp_1 = host_lock_IoMutex_lock(&HOST_LOCK_TESTS_STATIC.header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_u8_push_Checked(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &tmp_1), 2);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
	guard = host_lock_IoMutex_lock(&HOST_LOCK_TESTS_STATIC.header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const char text[] = "a step's bug while the lock is held";

		std_panic(RUST_FILE, 427, 17, text, sizeof(text) - 1);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &guard);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1
 * runs closure_1_0 with a lock scope of its own.
 */
static void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1(void *captures)
{
	host_lock_tests_with_held(test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1_0,
							  NULL, NULL);
}

/*
 * test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_2
 * checks STATIC was rebuilt empty and pushes 3 onto it.
 */
static void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_2(void *captures,
																				   HostIoHeld *held,
																				   void *result)
{
	volatile HostLockLockedRwLockWriteGuard state;
	StdUnwind	unwind;

	state = host_lock_IoMutex_lock(&HOST_LOCK_TESTS_STATIC.header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		STD_ASSERT(434, 13, ((const StdVec *) host_lock_LockedRwLockWriteGuard_Deref_deref((HostLockLockedRwLockWriteGuard *) &state))->len == 0,
				   "state.is_empty()");
		std_vec_u8_push_Checked(host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &state), 3);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &state);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &state);
}

/*
 * test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_3
 * returns a clone of STATIC's vector.
 */
static void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_3(void *captures,
																				   HostIoHeld *held,
																				   void *result)
{
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	StdUnwind	unwind;

	tmp_1 = host_lock_IoMutex_lock(&HOST_LOCK_TESTS_STATIC.header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_clone(result, host_lock_LockedRwLockWriteGuard_Deref_deref((HostLockLockedRwLockWriteGuard *) &tmp_1), 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
}

/*
 * test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take: a
 * panic while a static storage lock is held poisons it, and the next take
 * rebuilds its state to its default, which the owner fills again, rather than
 * refusing every later entry.
 */
void
test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take(void)
{
	StdCaught	panicked;
	bool		returned;
	volatile bool panicked_live;
	StdVec		tmp_1;
	volatile bool tmp_1_live = false;
	static const uint8 three[] = {3};
	StdUnwind	unwind;

	host_lock_tests_with_held(test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_0,
							  NULL, NULL);
	returned = std_catch_unwind(test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_1,
								NULL, &panicked);
	panicked_live = !returned;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdBytes	left;
		StdBytes	right = {three, 1};

		STD_ASSERT(430, 9, !returned, "panicked.is_err()");
		STD_ASSERT(431, 9, std_rw_lock_is_poisoned(&HOST_LOCK_TESTS_STATIC.header.lock), "STATIC.lock.is_poisoned()");
		host_lock_tests_with_held(test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_2,
								  NULL, NULL);
		host_lock_tests_with_held(test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take_closure_3,
								  NULL, &tmp_1);
		tmp_1_live = true;
		left.ptr = tmp_1.ptr;
		left.len = tmp_1.len;
		STD_ASSERT_EQ(437, 9, left.len == right.len && memcmp(left.ptr, right.ptr, right.len) == 0, &left,
					  std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt);
		tmp_1_live = false;
		std_vec_free_Checked(&tmp_1, 1, 1);
		if (panicked_live)
		{
			panicked_live = false;
			std_Caught_drop_Checked(&panicked);
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked(&tmp_1, 1, 1);
		if (panicked_live)
			std_Caught_drop_Checked(&panicked);
		std_unwind_resume(&unwind);
	}
}

/* HostLockTestsReleasing drops a root from a list a lock holds, the way a pin's drop does. */
typedef struct HostLockTestsReleasing
{
	HostLockIoMutexVecU8 *f0;
} HostLockTestsReleasing;

/* host_lock_tests_Releasing_drop drops a root from the list the lock holds, as a pin's drop does. */
static void
host_lock_tests_Releasing_drop(HostLockTestsReleasing *self)
{
	HostIoInDrop tmp_1 = host_io_InDrop_new();
	StdRwLockWriteGuard roots;

	if (host_lock_IoMutex_lock_in_drop(&self->f0->header, &tmp_1, &roots))
	{
		uint8		popped;

		(void) std_vec_pop(roots.data, &popped, 1);
		std_RwLockWriteGuard_drop(&roots);
	}
}

/*
 * test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_1
 * returns a clone of the lock's list.
 */
static void
test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_1(void *captures, HostIoHeld *held,
																			  void *result)
{
	HostLockIoMutexVecU8 *roots = captures;
	volatile HostLockLockedRwLockWriteGuard tmp_1;
	StdUnwind	unwind;

	tmp_1 = host_lock_IoMutex_lock(&roots->header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_clone(result, host_lock_LockedRwLockWriteGuard_Deref_deref((HostLockLockedRwLockWriteGuard *) &tmp_1), 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
		std_unwind_resume(&unwind);
	}
	host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &tmp_1);
}

/*
 * test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0_0
 * takes the lock, makes a Releasing under it, and panics.
 */
static void
test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0_0(void *captures, HostIoHeld *held,
																				void *result)
{
	HostLockIoMutexVecU8 *roots = captures;
	volatile HostLockLockedRwLockWriteGuard guard;
	HostLockTestsReleasing releasing;
	StdUnwind	unwind;

	guard = host_lock_IoMutex_lock(&roots->header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE, held);
	releasing.f0 = roots;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const char text[] = "a step's error while the lock is held";

		std_panic(RUST_FILE, 463, 17, text, sizeof(text) - 1);
	}
	else
	{
		host_lock_tests_Releasing_drop(&releasing);
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &guard);
		std_unwind_resume(&unwind);
	}
}

/*
 * test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0
 * runs closure_0_0 with a lock scope of its own.
 */
static void
test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0(void *captures)
{
	host_lock_tests_with_held(test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0_0,
							  captures, NULL);
}

/*
 * test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_2
 * reports whether the lock's list is empty.
 */
static void
test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_2(void *captures, HostIoHeld *held,
																			  void *result)
{
	HostLockIoMutexVecU8 *roots = captures;
	HostLockLockedRwLockWriteGuard tmp_1 = host_lock_IoMutex_lock(&roots->header, &HOST_LOCK_TESTS_VEC_U8_LOCK_TYPE,
																  held);

	*(bool *) result = ((const StdVec *) host_lock_LockedRwLockWriteGuard_Deref_deref(&tmp_1))->len == 0;
	host_lock_LockedRwLockWriteGuard_drop(&tmp_1);
}

/*
 * test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers: a drop
 * whose lock a frame it unwinds through holds gives up its work instead of
 * panicking during the unwind, which would abort the process: the unwind's
 * panic is the one caught, the drop gives nothing on the poisoned lock after
 * it either, and the next ordinary take rebuilds the state.
 */
void
test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers(void)
{
	static const StdLocation at = {RUST_FILE, 456, 21};
	static const uint8 one_two[] = {1, 2};
	static const uint8 one[] = {1};
	HostLockIoMutexVecU8 roots;
	volatile bool roots_live;
	StdVec		tmp_1;
	StdVec		tmp_2;
	volatile bool tmp_2_live = false;
	StdCaught	unwound;
	bool		returned;
	volatile bool unwound_live = false;
	bool		empty;
	StdUnwind	unwind;

	{
		void	   *boxed = std_box_new_uninit_layout_Checked(2, 1);

		STD_CHECK_ALIGNED(boxed, 1, 456, 34);
		STD_CHECK_NULL_DEREF(boxed, 456, 34);
		memcpy(boxed, one_two, 2);
		std_box_slice_into_vec_Checked(&tmp_1, boxed, 2, 1);
	}
	host_lock_IoMutexVecU8_new(&roots, tmp_1, &at);
	roots_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostLockTestsReleasing releasing = {&roots};
		StdBytes	left;
		StdBytes	right = {one, 1};

		host_lock_tests_Releasing_drop(&releasing);
		host_lock_tests_with_held(test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_1,
								  &roots, &tmp_2);
		tmp_2_live = true;
		left.ptr = tmp_2.ptr;
		left.len = tmp_2.len;
		STD_ASSERT_EQ(458, 9, left.len == right.len && memcmp(left.ptr, right.ptr, right.len) == 0, &left,
					  std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt);
		tmp_2_live = false;
		std_vec_free_Checked(&tmp_2, 1, 1);
		returned = std_catch_unwind(test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_0,
									&roots, &unwound);
		unwound_live = !returned;
		STD_ASSERT(466, 9, !returned, "unwound.is_err()");
		host_lock_tests_Releasing_drop(&releasing);
		host_lock_tests_with_held(test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers_closure_2,
								  &roots, &empty);
		STD_ASSERT(468, 9, empty, "with_held(|held| roots.lock(held).is_empty())");
		if (unwound_live)
		{
			unwound_live = false;
			std_Caught_drop_Checked(&unwound);
		}
		roots_live = false;
		std_vec_free_Checked(&roots.value, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_2_live)
			std_vec_free_Checked(&tmp_2, 1, 1);
		if (unwound_live)
			std_Caught_drop_Checked(&unwound);
		if (roots_live)
			std_vec_free_Checked(&roots.value, 1, 1);
		std_unwind_resume(&unwind);
	}
}

#endif							/* DOLTGRES_TEST_HOST */
