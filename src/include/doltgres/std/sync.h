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
 * The C forms of Rust's OnceLock, Once, Mutex, RwLock, and Arc (the port
 * text's section 9). A backend runs one thread, and the simulation's actors
 * run one at a time, handing over only inside a host call, so a lock is its
 * holders' counts, and a take that would wait in Rust, which only a bug can
 * reach in a backend, is an internal error. A write guard's drop poisons its
 * lock when a panic began unwinding after its take, as Rust's does.
 */
#ifndef DOLTGRES_STD_SYNC_H
#define DOLTGRES_STD_SYNC_H

#include "doltgres/std/any.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/unwind.h"


/* StdOnceState is Once's states. */
typedef enum StdOnceState
{
	STD_ONCE_INCOMPLETE,
	STD_ONCE_RUNNING,
	STD_ONCE_POISONED,
	STD_ONCE_COMPLETE,
} StdOnceState;

/* StdOnce is a Once. */
typedef struct StdOnce
{
	StdOnceState state;
} StdOnce;

/* StdOnceLock is a OnceLock's state: the Once that sets its value, which is beside it. */
typedef struct StdOnceLock
{
	StdOnce		once;
} StdOnceLock;

/* STD_ONCE_LOCK_NEW is OnceLock::new's state. */
#define STD_ONCE_LOCK_NEW {{STD_ONCE_INCOMPLETE}}

/*
 * StdRwLock is a RwLock's or a Mutex's state: its readers, its writer, and
 * whether it is poisoned, std's poison::Flag.
 */
typedef struct StdRwLock
{
	size_t		readers;
	bool		writer;
	StdAtomicBool poisoned;
} StdRwLock;

/* STD_RW_LOCK_NEW is RwLock::new's and Mutex::new's state, a static's initializer. */
#define STD_RW_LOCK_NEW {0, false, STD_ATOMIC_BOOL_INIT(false)}

/* StdTryLockTag is TryLockResult's forms: Ok, Err(Poisoned) with the guard, and Err(WouldBlock). */
typedef enum StdTryLockTag
{
	STD_TRY_LOCK_OK,
	STD_TRY_LOCK_POISONED,
	STD_TRY_LOCK_WOULD_BLOCK,
} StdTryLockTag;

/* StdRwLockReadGuard is a read guard: the value it lends and its lock. */
typedef struct StdRwLockReadGuard
{
	const void *data;
	StdRwLock  *inner_lock;
} StdRwLockReadGuard;

/*
 * StdRwLockWriteGuard is a write or mutex guard: its lock, the value it
 * lends, and whether a panic was unwinding when it was taken, poison::Guard's
 * panicking.
 */
typedef struct StdRwLockWriteGuard
{
	StdRwLock  *lock;
	void	   *data;
	bool		panicking;
} StdRwLockWriteGuard;

/* StdArcInner is an Arc's allocation's header, ArcInner's counts; the value follows. */
typedef struct StdArcInner
{
	StdAtomicUsize strong;
	StdAtomicUsize weak;
} StdArcInner;

/* StdArcSlice is an Arc<[T]>: its allocation and its length, as Box<[T]> is a pointer and a length. */
typedef struct StdArcSlice
{
	StdArcInner *ptr;
	size_t		len;
} StdArcSlice;

/*
 * STD_ARC_DATA_OFFSET is where ArcInner's data starts for a T of an
 * alignment: after its two counts, at T's alignment, as repr(C) places it.
 */
#define STD_ARC_DATA_OFFSET(align) TYPEALIGN((align), sizeof(StdArcInner))

/* STD_ARC_VALUE is the value of a T of an alignment an Arc's allocation holds. */
#define STD_ARC_VALUE(inner, align) ((void *) ((char *) (inner) + STD_ARC_DATA_OFFSET(align)))

extern void std_once_call_once_force(StdOnce *self, void (*f) (void *), void *captures);
extern bool std_once_lock_get(const StdOnceLock *self);
extern void std_once_lock_get_or_init(StdOnceLock *self, void *slot,
									  void (*f) (void *captures, void *result), void *f_captures);
extern bool std_once_lock_set(StdOnceLock *self, void *slot, const void *value, size_t size);
extern void std_once_call_once(StdOnce *self, void (*f) (void *), void *captures,
							   const char *file, uint32 line, uint32 col);
extern bool std_rw_lock_read_Checked(StdRwLock *self, const void *data, StdRwLockReadGuard *guard);
extern bool std_rw_lock_read_Unchecked(StdRwLock *self, const void *data, StdRwLockReadGuard *guard);
extern bool std_rw_lock_write(StdRwLock *self, void *data, StdRwLockWriteGuard *guard);
extern StdTryLockTag std_rw_lock_try_read(StdRwLock *self, const void *data,
										  StdRwLockReadGuard *guard);
extern StdTryLockTag std_rw_lock_try_write(StdRwLock *self, void *data,
										   StdRwLockWriteGuard *guard);
extern void std_RwLockReadGuard_drop(StdRwLockReadGuard *self);
extern void std_RwLockWriteGuard_drop(StdRwLockWriteGuard *self);
extern void std_rw_lock_new(StdRwLock *self);
extern bool std_rw_lock_is_poisoned(StdRwLock *self);
extern void std_rw_lock_clear_poison(StdRwLock *self);
extern StdArcInner *std_arc_new(const void *value, size_t size, size_t align);
extern StdArcInner *std_arc_new_dropping(const void *value, size_t size, size_t align,
										 void (*drop) (void *value));
extern const StdLocation STD_ARC_DEFAULT_SITE;

extern StdArcInner *std_arc_default(const StdDefaultType *t);
extern StdArcInner *std_arc_clone(StdArcInner *self);
extern StdArcSlice std_arc_from_slice(const void *v, size_t len, size_t size, size_t align);
extern void *std_arc_Deref_deref(StdArcInner *self, size_t align);
extern void std_arc_drop(StdArcInner *self, size_t size, size_t align, void (*drop) (void *));

#ifdef DOLTGRES_TEST_HOST
extern void test_std_sync_tests_an_arc_whose_allocation_fails_drops_its_value(void);
extern void test_std_sync_tests_an_over_aligned_arc_value_stays_aligned(void);
#endif

#endif							/* DOLTGRES_STD_SYNC_H */
