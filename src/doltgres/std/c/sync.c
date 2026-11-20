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
 * The C forms of Rust's Once, Mutex, RwLock, and Arc (the port text's
 * section 9), ported from std/src/sync at Rust 1.99.1 for one thread at a
 * time: a lock is its holders' counts, a take Rust would wait on waits,
 * and a writer's guard dropped while unwinding poisons its lock, as Rust's
 * poison flag does.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_ONCE_FILE is std's sync/once.rs, where Once's closures take their f. */
#define STD_ONCE_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sync" STD_SEP "once.rs"

/* STD_ONCE_LOCK_FILE is std's sync/once_lock.rs, where try_insert's unwrap reports. */
#define STD_ONCE_LOCK_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sync" STD_SEP "once_lock.rs"

/* STD_RWLOCK_FILE is std's sync/poison/rwlock.rs, where RwLockReadGuard::new's check reports. */
#define STD_RWLOCK_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sync" STD_SEP "poison" STD_SEP "rwlock.rs"

/* STD_ARC_MAX_REFCOUNT is Arc's MAX_REFCOUNT, isize::MAX: a clone past it aborts. */
#define STD_ARC_MAX_REFCOUNT ((size_t) PG_INT64_MAX)

/* StdOnceTake is a Once closure's captures: its f, taken when it runs, as Option::take takes it. */
typedef struct StdOnceTake
{
	void		(*f) (void *);
	void	   *captures;
	bool		some;
} StdOnceTake;

/*
 * std_once_call is the futex Once's call in section 9's form: the closure
 * run by the first call (and by one after a poisoning panic when poisoning
 * is ignored), a poisoned Once's panic otherwise, and every call while an
 * initialization runs waiting, as std's does, even one from inside it; its
 * panics report its caller's location. A panic in the closure poisons it.
 */
static void
std_once_call(StdOnce *self, bool ignore_poisoning, void (*f) (StdOnceTake *), StdOnceTake *take,
			  const char *file, uint32 line, uint32 col)
{
	StdUnwind	unwind;

	for (;;)
	{
		switch (self->state)
		{
			case STD_ONCE_COMPLETE:
				return;
			case STD_ONCE_POISONED:
				if (!ignore_poisoning)
				{
					static const char text[] = "Once instance has previously been poisoned";

					std_panic(file, line, col, text, sizeof(text) - 1);
				}
				pg_fallthrough;
			case STD_ONCE_INCOMPLETE:
				self->state = STD_ONCE_RUNNING;
				STD_UNWIND_PUSH(&unwind);
				if (sigsetjmp(unwind.jump, 0) == 0)
				{
					f(take);
					std_unwind_pop(&unwind);
				}
				else
				{
					self->state = STD_ONCE_POISONED;
					std_unwind_resume(&unwind);
				}
				self->state = STD_ONCE_COMPLETE;
				return;
			default:
				if (!(self->state == STD_ONCE_RUNNING))
				{
					static const char text[] = "assertion failed: state == RUNNING";

					std_panic(file, line, col, text, sizeof(text) - 1);
				}
				pg_usleep(1000L);
				break;
		}
	}
}

/* std_once_call_once_closure_0 is call_once's closure: f.take().unwrap()(), at once.rs's location. */
static void
std_once_call_once_closure_0(StdOnceTake *take)
{
	if (!take->some)
		std_option_unwrap_failed(STD_ONCE_FILE, 166, 50);
	take->some = false;
	take->f(take->captures);
}

/*
 * std_once_call_once is Once::call_once: the closure run the first time, and
 * nothing after; a panic in it poisons the Once, and a later call panics at
 * its caller's location.
 */
void
std_once_call_once(StdOnce *self, void (*f) (void *), void *captures, const char *file,
				   uint32 line, uint32 col)
{
	StdOnceTake take = {f, captures, true};

	if (self->state == STD_ONCE_COMPLETE)
		return;
	std_once_call(self, false, std_once_call_once_closure_0, &take, file, line, col);
}

/* std_once_call_once_force_closure_0 is call_once_force's closure: f.take().unwrap()(p), at once.rs's location. */
static void
std_once_call_once_force_closure_0(StdOnceTake *take)
{
	if (!take->some)
		std_option_unwrap_failed(STD_ONCE_FILE, 226, 49);
	take->some = false;
	take->f(take->captures);
}

/*
 * std_once_call_once_force is Once::call_once_force: the closure run the
 * first time, even after a poisoning panic, and nothing after it completes.
 */
void
std_once_call_once_force(StdOnce *self, void (*f) (void *), void *captures)
{
	StdOnceTake take = {f, captures, true};

	if (self->state == STD_ONCE_COMPLETE)
		return;
	std_once_call(self, true, std_once_call_once_force_closure_0, &take, STD_ONCE_FILE, 226, 20);
}

/* std_once_lock_get is OnceLock::get: whether the value beside the lock is set. */
bool
std_once_lock_get(const StdOnceLock *self)
{
	return self->once.state == STD_ONCE_COMPLETE;
}

/* StdOnceLockInitializeClosure0 is initialize's closure's captures: the value's maker and its slot. */
typedef struct StdOnceLockInitializeClosure0
{
	void		(*f) (void *captures, void *result);
	void	   *f_captures;
	void	   *slot;
} StdOnceLockInitializeClosure0;

/* std_once_lock_initialize_closure_0 makes the value into its slot, as initialize's call_once_force closure does. */
static void
std_once_lock_initialize_closure_0(void *captures)
{
	StdOnceLockInitializeClosure0 *c = captures;

	c->f(c->f_captures, c->slot);
}

/*
 * std_once_lock_get_or_init is OnceLock::get_or_init: the value made into
 * the slot by the closure unless the lock has one; a panic in the closure
 * leaves it empty.
 */
void
std_once_lock_get_or_init(StdOnceLock *self, void *slot, void (*f) (void *captures, void *result),
						  void *f_captures)
{
	StdOnceLockInitializeClosure0 captures;

	if (std_once_lock_get(self))
		return;
	captures.f = f;
	captures.f_captures = f_captures;
	captures.slot = slot;
	std_once_call_once_force(&self->once, std_once_lock_initialize_closure_0, &captures);
}

/* StdOnceLockTryInsertClosure0 is try_insert's closure's captures: the value, its size, and whether it was taken. */
typedef struct StdOnceLockTryInsertClosure0
{
	const void *value;
	size_t		size;
	bool		taken;
} StdOnceLockTryInsertClosure0;

/*
 * std_once_lock_try_insert_closure_0 takes the value into the slot, as
 * value.take().unwrap() does, panicking if it was taken already.
 */
static void
std_once_lock_try_insert_closure_0(void *captures, void *result)
{
	StdOnceLockTryInsertClosure0 *c = captures;

	if (c->taken)
		std_option_unwrap_failed(STD_ONCE_LOCK_FILE, 309, 52);
	memcpy(result, c->value, c->size);
	c->taken = true;
}

/* std_once_lock_set is OnceLock::set: the value stored unless the lock has one, true when it was. */
bool
std_once_lock_set(StdOnceLock *self, void *slot, const void *value, size_t size)
{
	StdOnceLockTryInsertClosure0 captures = {value, size, false};

	std_once_lock_get_or_init(self, slot, std_once_lock_try_insert_closure_0, &captures);
	return captures.taken;
}

/*
 * std_rw_lock_read_Unchecked is RwLock::read as std's own code runs it: the
 * lock taken shared and its guard, true unless it is poisoned, which the
 * guard is given either way. It waits while a writer holds the lock, as
 * std's does, even this backend.
 */
bool
std_rw_lock_read_Unchecked(StdRwLock *self, const void *data, StdRwLockReadGuard *guard)
{
	bool		poisoned;

	while (self->writer)
		pg_usleep(1000L);
	self->readers++;
	poisoned = std_AtomicBool_load_relaxed(&self->poisoned);
	*guard = (StdRwLockReadGuard)
	{
		data, self
	};
	return !poisoned;
}

/*
 * std_rw_lock_read_Checked is RwLock::read as the crates' code runs it:
 * RwLockReadGuard::new's NonNull::new_unchecked checked once the lock is
 * taken.
 */
bool
std_rw_lock_read_Checked(StdRwLock *self, const void *data, StdRwLockReadGuard *guard)
{
	bool		ok = std_rw_lock_read_Unchecked(self, data, guard);

	std_NonNull_new_unchecked_check(data, STD_RWLOCK_FILE, 742, 28);
	return ok;
}

/*
 * std_rw_lock_write is RwLock::write and Mutex::lock: the lock taken
 * exclusively and its guard, true unless it is poisoned. It waits while
 * anyone holds the lock, as std's does, even this backend.
 */
bool
std_rw_lock_write(StdRwLock *self, void *data, StdRwLockWriteGuard *guard)
{
	while (self->writer || self->readers > 0)
		pg_usleep(1000L);
	self->writer = true;
	*guard = (StdRwLockWriteGuard)
	{
		self, data, std_thread_panicking()
	};
	return !std_AtomicBool_load_relaxed(&self->poisoned);
}

/* std_rw_lock_try_read is RwLock::try_read: WouldBlock while a writer holds it, and Poisoned with the guard. */
StdTryLockTag
std_rw_lock_try_read(StdRwLock *self, const void *data, StdRwLockReadGuard *guard)
{
	bool		poisoned;

	if (self->writer)
		return STD_TRY_LOCK_WOULD_BLOCK;
	self->readers++;
	poisoned = std_AtomicBool_load_relaxed(&self->poisoned);
	std_NonNull_new_unchecked_check(data, STD_RWLOCK_FILE, 742, 28);
	*guard = (StdRwLockReadGuard)
	{
		data, self
	};
	return poisoned ? STD_TRY_LOCK_POISONED : STD_TRY_LOCK_OK;
}

/*
 * std_rw_lock_try_write is RwLock::try_write and Mutex::try_lock:
 * WouldBlock while anyone holds it, and Poisoned with the guard.
 */
StdTryLockTag
std_rw_lock_try_write(StdRwLock *self, void *data, StdRwLockWriteGuard *guard)
{
	if (self->writer || self->readers > 0)
		return STD_TRY_LOCK_WOULD_BLOCK;
	self->writer = true;
	*guard = (StdRwLockWriteGuard)
	{
		self, data, std_thread_panicking()
	};
	return std_AtomicBool_load_relaxed(&self->poisoned) ? STD_TRY_LOCK_POISONED : STD_TRY_LOCK_OK;
}

/* std_RwLockReadGuard_drop is a read guard's drop, which never poisons. */
void
std_RwLockReadGuard_drop(StdRwLockReadGuard *self)
{
	self->inner_lock->readers--;
}

/*
 * std_RwLockWriteGuard_drop is a write or mutex guard's drop, which poisons
 * the lock when a panic is unwinding that wasn't when the guard was taken.
 */
void
std_RwLockWriteGuard_drop(StdRwLockWriteGuard *self)
{
	if (!self->panicking && std_thread_panicking())
		std_AtomicBool_store_relaxed(&self->lock->poisoned, true);
	self->lock->writer = false;
}

/* std_rw_lock_new is RwLock::new and Mutex::new made at run time: unlocked and unpoisoned. */
void
std_rw_lock_new(StdRwLock *self)
{
	self->readers = 0;
	self->writer = false;
	std_AtomicBool_new(&self->poisoned, false);
}

/* std_rw_lock_is_poisoned is is_poisoned. */
bool
std_rw_lock_is_poisoned(StdRwLock *self)
{
	return std_AtomicBool_load_relaxed(&self->poisoned);
}

/* std_rw_lock_clear_poison is clear_poison. */
void
std_rw_lock_clear_poison(StdRwLock *self)
{
	std_AtomicBool_store_relaxed(&self->poisoned, false);
}

/*
 * std_sync_arc_layout is ArcInner<T>'s layout, repr(C, align(2)): its two
 * counts, then T at T's alignment, the whole padded to the largest of the
 * alignments, which it returns with the size.
 */
static size_t
std_sync_arc_layout(size_t size, size_t align, size_t *result_align)
{
	size_t		arc_align = std_max_usize(std_max_usize(_Alignof(StdArcInner), align), 2);

	*result_align = arc_align;
	return TYPEALIGN(arc_align, STD_ARC_DATA_OFFSET(align) + size);
}

/* std_arc_new is Arc::new: one allocation of counts of one and the value moved in. */
StdArcInner *
std_arc_new(const void *value, size_t size, size_t align)
{
	size_t		arc_align;
	size_t		arc_size = std_sync_arc_layout(size, align, &arc_align);
	StdArcInner *inner = std_alloc(arc_size, arc_align);

	std_AtomicUsize_new(&inner->strong, 1);
	std_AtomicUsize_new(&inner->weak, 1);
	if (size > 0)
		memcpy(STD_ARC_VALUE(inner, align), value, size);
	return inner;
}

/*
 * StdArcNewFrame is std_arc_new_dropping's objects: the value moved in, its
 * layout and drop glue, and the Arc made.
 */
typedef struct StdArcNewFrame
{
	const void *value;
	size_t		size;
	size_t		align;
	void		(*drop) (void *value);
	StdArcInner *result;
} StdArcNewFrame;

/* std_arc_new_dropping_body makes the Arc. */
static void
std_arc_new_dropping_body(void *frame_)
{
	StdArcNewFrame *frame = frame_;

	frame->result = std_arc_new(frame->value, frame->size, frame->align);
}

/* std_arc_new_dropping_pad drops the value, which no Arc holds yet, when the allocation unwinds. */
static void
std_arc_new_dropping_pad(void *frame_)
{
	StdArcNewFrame *frame = frame_;

	frame->drop((void *) frame->value);
}

/*
 * std_arc_new_dropping is Arc::new of a value with drop glue: the value
 * moved into a new Arc, or dropped as Rust drops it when the allocation
 * unwinds.
 */
StdArcInner *
std_arc_new_dropping(const void *value, size_t size, size_t align, void (*drop) (void *value))
{
	StdArcNewFrame frame = {.value = value, .size = size, .align = align, .drop = drop};

	std_landing(&frame, std_arc_new_dropping_body, std_arc_new_dropping_pad);
	return frame.result;
}

/*
 * STD_ARC_DEFAULT_SITE is alloc/src/sync.rs's T::default() in Arc::default,
 * the caller a #[track_caller] Default of T reports when an Arc makes it.
 */
const StdLocation STD_ARC_DEFAULT_SITE = {
	STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "sync.rs", 3799, 31
};

/*
 * std_arc_default is Arc's Default: the block allocated first, as
 * box_new_uninit does, then T's default made in it, the block's Box dropped
 * should that panic.
 */
StdArcInner *
std_arc_default(const StdDefaultType *t)
{
	size_t		arc_align;
	size_t		arc_size = std_sync_arc_layout(t->size, t->align, &arc_align);
	StdArcInner *volatile inner = std_box_new_uninit_layout_Checked(arc_size, arc_align);
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);

	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		t->default_(STD_ARC_VALUE(inner, t->align), t->arc_caller);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_Layout_for_value_raw_check(arc_size, arc_align);
		std_free(inner, arc_size, arc_align);
		std_unwind_resume(&unwind);
	}
	std_AtomicUsize_new(&inner->strong, 1);
	std_AtomicUsize_new(&inner->weak, 1);
	return inner;
}

/*
 * std_arc_clone is Arc::clone: one more strong count on the same allocation,
 * aborting past MAX_REFCOUNT.
 */
StdArcInner *
std_arc_clone(StdArcInner *self)
{
	size_t		old_size = std_AtomicUsize_fetch_add_relaxed(&self->strong, 1);

	if (old_size > STD_ARC_MAX_REFCOUNT)
		std_intrinsics_abort();
	return self;
}

/* std_arc_Deref_deref is Arc's Deref: the value of a T of an alignment, after the allocation's counts. */
void *
std_arc_Deref_deref(StdArcInner *self, size_t align)
{
	return STD_ARC_VALUE(self, align);
}

/*
 * std_arc_Weak_drop is Weak's drop: nothing for a dangling one, else the
 * weak count down, freeing the block at zero.
 */
static void
std_arc_Weak_drop(StdArcInner *self, size_t size, size_t align)
{
	size_t		arc_align;
	size_t		arc_size;

	if ((uintptr_t) self == UINTPTR_MAX)
		return;
	if (std_AtomicUsize_fetch_sub_release(&self->weak, 1) == 1)
	{
		std_atomic_fence_acquire();
		arc_size = std_sync_arc_layout(size, align, &arc_align);
		std_Layout_for_value_raw_check(arc_size, arc_align);
		std_free(self, arc_size, arc_align);
	}
}

/*
 * std_arc_drop_slow is Arc's drop_slow: the value dropped, then the weak
 * count the strong ones held, also when the value's drop panics.
 */
static void
std_arc_drop_slow(StdArcInner *self, size_t size, size_t align, void (*drop) (void *))
{
	StdUnwind	unwind;

	if (drop != NULL)
	{
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			drop(STD_ARC_VALUE(self, align));
			std_unwind_pop(&unwind);
		}
		else
		{
			std_arc_Weak_drop(self, size, align);
			std_unwind_resume(&unwind);
		}
	}
	std_arc_Weak_drop(self, size, align);
}

/*
 * std_arc_drop is Arc's drop: the strong count down, and at zero, after an
 * acquire fence, drop_slow.
 */
void
std_arc_drop(StdArcInner *self, size_t size, size_t align, void (*drop) (void *))
{
	if (std_AtomicUsize_fetch_sub_release(&self->strong, 1) != 1)
		return;
	std_atomic_fence_acquire();
	std_arc_drop_slow(self, size, align, drop);
}

/*
 * std_arc_into_inner is Arc::into_inner: the strong count down, and when it
 * was the last, after an acquire fence, the value moved out to result and the
 * weak count the strong ones held dropped; it reports whether it moved the
 * value.
 */
bool
std_arc_into_inner(StdArcInner *self, size_t size, size_t align, void *result)
{
	if (std_AtomicUsize_fetch_sub_release(&self->strong, 1) != 1)
		return false;
	std_atomic_fence_acquire();
	memcpy(result, STD_ARC_VALUE(self, align), size);
	std_arc_Weak_drop(self, size, align);
	return true;
}

#ifdef DOLTGRES_TEST_HOST

/*
 * test_std_sync_tests_an_over_aligned_arc_value_stays_aligned checks an Arc
 * of a 64-byte-aligned value: its block and its value are at 64, the value
 * after the counts at its alignment, as ArcInner's repr(C) places it.
 */
void
test_std_sync_tests_an_over_aligned_arc_value_stays_aligned(void)
{
	StdTestsAligned64 value = {{7}};
	StdArcInner *arc = std_arc_new(&value, sizeof(StdTestsAligned64), _Alignof(StdTestsAligned64));

	if ((uintptr_t) arc % 64 != 0)
		std_panic(__FILE__, __LINE__, 0, "an Arc's block is not aligned to 64", sizeof("an Arc's block is not aligned to 64") - 1);
	if ((char *) STD_ARC_VALUE(arc, 64) - (char *) arc != 64)
		std_panic(__FILE__, __LINE__, 0, "an Arc's value is not after its counts at 64", sizeof("an Arc's value is not after its counts at 64") - 1);
	if (((StdTestsAligned64 *) STD_ARC_VALUE(arc, 64))->bytes[0] != 7)
		std_panic(__FILE__, __LINE__, 0, "an Arc's value changed", sizeof("an Arc's value changed") - 1);
	std_arc_drop(arc, sizeof(StdTestsAligned64), _Alignof(StdTestsAligned64), NULL);
}

/* std_sync_tests_DROPS counts the drops of the failed Arc case's value. */
static uint64 std_sync_tests_DROPS;

/* std_sync_tests_counted_drop is the failed Arc case's value's drop glue, which counts. */
static void
std_sync_tests_counted_drop(void *value)
{
	std_sync_tests_DROPS++;
}

/* std_sync_tests_oom is the failed Arc case's allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
std_sync_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* std_sync_tests_OOM_HOOK is the failed Arc case's hook. */
static const StdAllocErrorHook std_sync_tests_OOM_HOOK = {std_sync_tests_oom};

/* std_sync_tests_arc_new is the failed Arc case's body: an Arc of the value, whose allocation fails. */
static void
std_sync_tests_arc_new(void *captures)
{
	StdArcInner *arc;

	test_std_alloc_tests_fail_allocation(1);
	arc = std_arc_new_dropping(captures, sizeof(uint64), _Alignof(uint64), std_sync_tests_counted_drop);
	test_std_alloc_tests_fail_allocation(0);
	std_arc_drop(arc, sizeof(uint64), _Alignof(uint64), std_sync_tests_counted_drop);
}

/*
 * test_std_sync_tests_an_arc_whose_allocation_fails_drops_its_value makes an
 * Arc of a value with drop glue whose allocation fails: the panic unwinds and
 * the value is dropped once, as Rust 1.99.1's Arc::new drops it under a
 * panicking alloc-error hook.
 */
void
test_std_sync_tests_an_arc_whose_allocation_fails_drops_its_value(void)
{
	uint64		value = 7;
	StdCaught	caught;
	bool		ok;

	std_sync_tests_DROPS = 0;
	std_alloc_set_alloc_error_hook(&std_sync_tests_OOM_HOOK);
	ok = std_catch_unwind(std_sync_tests_arc_new, &value, &caught);
	test_std_alloc_tests_fail_allocation(0);
	std_alloc_set_alloc_error_hook(NULL);
	if (ok)
		std_panic(__FILE__, __LINE__, 0, "the Arc's allocation didn't fail", sizeof("the Arc's allocation didn't fail") - 1);
	std_Caught_drop(&caught);
	if (std_sync_tests_DROPS != 1)
		std_panic(__FILE__, __LINE__, 0, "the value wasn't dropped once", sizeof("the value wasn't dropped once") - 1);
}

/*
 * test_std_sync_tests_an_arcs_into_inner_takes_its_value_only_from_the_last_strong_arc
 * takes an Arc's value with into_inner: a clone's into_inner leaves the value
 * to the other Arc, whose into_inner moves it out, dropping nothing.
 */
void
test_std_sync_tests_an_arcs_into_inner_takes_its_value_only_from_the_last_strong_arc(void)
{
	uint64		value = 7;
	uint64		taken = 0;
	StdArcInner *arc;

	std_sync_tests_DROPS = 0;
	arc = std_arc_new_dropping(&value, sizeof(uint64), _Alignof(uint64), std_sync_tests_counted_drop);
	if (std_arc_into_inner(std_arc_clone(arc), sizeof(uint64), _Alignof(uint64), &taken))
		std_panic(__FILE__, __LINE__, 0, "a clone's into_inner took the value", sizeof("a clone's into_inner took the value") - 1);
	if (!std_arc_into_inner(arc, sizeof(uint64), _Alignof(uint64), &taken) || taken != 7)
		std_panic(__FILE__, __LINE__, 0, "the last Arc's into_inner didn't take the value", sizeof("the last Arc's into_inner didn't take the value") - 1);
	if (std_sync_tests_DROPS != 0)
		std_panic(__FILE__, __LINE__, 0, "into_inner dropped the value", sizeof("into_inner dropped the value") - 1);
}

#endif							/* DOLTGRES_TEST_HOST */

/*
 * std_AtomicBool_Default_default is AtomicBool's Default, new(false), made in
 * place, for generic code that takes a type's Default.
 */
void
std_AtomicBool_Default_default(void *result, const StdLocation *caller)
{
	std_AtomicBool_new(result, false);
}

/* STD_ATOMIC_BOOL_TYPE is AtomicBool's size, its Default, and its drop, for generic code that makes one. */
const StdDefaultType STD_ATOMIC_BOOL_TYPE = {sizeof(StdAtomicBool), _Alignof(StdAtomicBool),
std_AtomicBool_Default_default, NULL};

/*
 * std_AtomicU64_Default_default is AtomicU64's Default, new(0), made in
 * place, for generic code that takes a type's Default.
 */
void
std_AtomicU64_Default_default(void *result, const StdLocation *caller)
{
	std_AtomicU64_new(result, 0);
}

/* STD_ATOMIC_U64_TYPE is AtomicU64's size, its Default, and its drop, for generic code that makes one. */
const StdDefaultType STD_ATOMIC_U64_TYPE = {sizeof(StdAtomicU64), _Alignof(StdAtomicU64),
std_AtomicU64_Default_default, NULL};

/*
 * std_AtomicU32_Default_default is AtomicU32's Default, new(0), made in
 * place, for generic code that takes a type's Default.
 */
void
std_AtomicU32_Default_default(void *result, const StdLocation *caller)
{
	std_AtomicU32_new(result, 0);
}

/* STD_ATOMIC_U32_TYPE is AtomicU32's size, its Default, and its drop, for generic code that makes one. */
const StdDefaultType STD_ATOMIC_U32_TYPE = {sizeof(StdAtomicU32), _Alignof(StdAtomicU32),
std_AtomicU32_Default_default, NULL};

/*
 * std_arc_from_slice is Arc<[T]>'s From<&[T]> for a TrivialClone T,
 * copy_from_slice: allocate_for_slice's layout, Layout::array's unwrapped,
 * then ArcInner's after the counts, whose alignment extend's "capacity
 * overflow" checks the whole against, one allocation of counts of one, and
 * the slice's elements copied in.
 */
StdArcSlice
std_arc_from_slice(const void *v, size_t len, size_t size, size_t align)
{
	size_t		arc_align;
	size_t		arc_size;
	size_t		bytes;
	size_t		offset = STD_ARC_DATA_OFFSET(align);
	StdArcInner *inner;
	StdArcSlice result;

	if (size != 0 && len > STD_DIV_FOLDED(usize, std_wrapping_sub_usize((size_t) PG_INT64_MAX + 1, align), size))
	{
		static const char text[] = "called `Result::unwrap()` on an `Err` value";

		std_result_unwrap_failed(text, sizeof(text) - 1, STD_DANGLING, std_LayoutError_Debug_fmt,
								 STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "sync.rs", 2277, 41);
	}
	bytes = std_wrapping_mul_usize(size, len);
	arc_size = std_sync_arc_layout(bytes, align, &arc_align);
	if (std_wrapping_add_usize(offset, bytes) > std_wrapping_sub_usize((size_t) PG_INT64_MAX + 1, arc_align))
	{
		static const char text[] = "capacity overflow";

		std_panic(STD_RUST_LIBRARY STD_SEP "alloc" STD_SEP "src" STD_SEP "sync.rs", 416, 29, text, sizeof(text) - 1);
	}
	inner = std_alloc(arc_size, arc_align);
	std_AtomicUsize_new(&inner->strong, 1);
	std_AtomicUsize_new(&inner->weak, 1);
	if (bytes > 0)
		memcpy(STD_ARC_VALUE(inner, align), v, bytes);
	result.ptr = inner;
	result.len = len;
	return result;
}
