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
 * The storage crates' locks over a backend's own state (host's lock.rs). A
 * lock's state is its header, and its value follows it at HOST_LOCK_VALUE,
 * as the Rust's RwLock<T> holds its value. A lock's new and Default, and
 * taken, hold T by value, so they are instances, which lock_template.h
 * generates in the instantiating file; a take reaches T's taken through T's
 * HostLockType. Locked and Parts hold their guard and capability by value,
 * so they are instances too, declared here by the template and defined in
 * lock.c.
 */
#ifndef DOLTGRES_HOST_LOCK_H
#define DOLTGRES_HOST_LOCK_H

#include "doltgres/host/io.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"

/* HostLockIoMutex is a mutex under which no file I/O runs; its value follows it. */
typedef struct HostLockIoMutex
{
	StdRwLock	lock;

	/*
	 * at is where the lock is declared, which a take that finds it held
	 * names.
	 */
	const StdLocation *at;
} HostLockIoMutex;

/* HostLockIoRwLock is a reader-writer lock under which no file I/O runs; its value follows it. */
typedef struct HostLockIoRwLock
{
	StdRwLock	lock;

	/*
	 * at is where the lock is declared, which a take that finds it held
	 * names.
	 */
	const StdLocation *at;
} HostLockIoRwLock;

/*
 * HostLockLongMutex is a mutex held across file I/O, which its taker takes
 * with the whole Io, so it is never taken under a lock that does no I/O; its
 * value follows it.
 */
typedef struct HostLockLongMutex
{
	StdRwLock	lock;

	/*
	 * at is where the lock is declared, which a take that finds it held
	 * names.
	 */
	const StdLocation *at;
} HostLockLongMutex;

/*
 * HostLockShortMutex is a mutex held across one bounded step of file I/O and
 * nothing else: its guard lends a ShortIo, which takes no other lender and
 * checks no interrupt; its value follows it.
 */
typedef struct HostLockShortMutex
{
	StdRwLock	lock;

	/*
	 * at is where the lock is declared, which a take that finds it held
	 * names.
	 */
	const StdLocation *at;
} HostLockShortMutex;

/*
 * HOST_LOCK_VALUE is the value a lock's header is followed by, which a
 * struct of the header and then the value places there for any value
 * aligned to at most MAXIMUM_ALIGNOF.
 */
#define HOST_LOCK_VALUE(lock) ((void *) ((char *) (lock) + MAXALIGN(sizeof(*(lock)))))

/* HOST_LOCK_NEW is a lock's const new, for a static's initializer, given its declaration's location. */
#define HOST_LOCK_NEW(at) {STD_RW_LOCK_NEW, (at)}

/*
 * HostLockLending is the guard of a lock held across file I/O, which lends
 * its taker's capability back to the holder.
 */
typedef struct HostLockLending
{
	StdRwLockWriteGuard guard;
	/* io is the taker's capability, which the guard lends back. */
	HostIoTakesShort io;
} HostLockLending;

/*
 * HostLockShort is the guard of a short lender, which lends the view of its
 * taker's Io that does its step's file I/O.
 */
typedef struct HostLockShort
{
	StdRwLockWriteGuard guard;
	/* io is the view of the taker's Io the guard lends. */
	HostIoShortIo io;
} HostLockShort;

/*
 * HostLockType is what the locks' one-function takes need of their value's
 * type T: taken's instance at T.
 */
typedef struct HostLockType
{
	StdRwLockWriteGuard (*taken) (StdTryLockTag tag, StdRwLockWriteGuard *result, void (*clear) (void *),
								  void *clear_captures, void (*clear_drop) (void *), const StdLocation *at);
} HostLockType;

/* Locked<RwLockWriteGuard<T>>, the guard of a write, which lock.c defines. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_LOCKED_GS RwLockWriteGuard
#define HOST_LOCK_LOCKED_G StdRwLockWriteGuard
#define HOST_LOCK_LOCKED_DEREF_MUT
#include "doltgres/host/lock_template.h"

/* Locked<RwLockReadGuard<T>>, the guard of a read, which lock.c defines. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_LOCKED_GS RwLockReadGuard
#define HOST_LOCK_LOCKED_G StdRwLockReadGuard
#include "doltgres/host/lock_template.h"

/* Parts<T, I> at a lender's capability, which a LongMutex's guard lends. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_PARTS_IS TakesShort
#define HOST_LOCK_PARTS_I HostIoTakesShort
#include "doltgres/host/lock_template.h"

/* Parts<T, ShortIo>, which a ShortMutex's guard lends. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_PARTS_IS ShortIo
#define HOST_LOCK_PARTS_I HostIoShortIo *
#include "doltgres/host/lock_template.h"

pg_noreturn extern void host_lock_held_already(const StdLocation *at);
extern bool host_lock_IoMutex_lock_in_drop(HostLockIoMutex *self, HostIoInDrop *in_drop,
										   StdRwLockWriteGuard *result);
extern HostLockLockedRwLockWriteGuard host_lock_IoMutex_lock(HostLockIoMutex *self,
															 const HostLockType *t,
															 HostIoHeld *held);
extern HostLockLockedRwLockReadGuard host_lock_IoRwLock_read(HostLockIoRwLock *self,
															 const HostLockType *t,
															 HostIoHeld *held);
extern HostLockLockedRwLockWriteGuard host_lock_IoRwLock_write(HostLockIoRwLock *self,
															   const HostLockType *t,
															   HostIoHeld *held);
extern HostLockLending host_lock_LongMutex_lock(HostLockLongMutex *self,
												const HostLockType *t, HostIoTakesShort io);
extern HostIoTakesShort host_lock_Lending_io(HostLockLending *self);
extern HostLockPartsTakesShort host_lock_Lending_split(HostLockLending *self);
extern const void *host_lock_Lending_Deref_deref(const HostLockLending *self);
extern void *host_lock_Lending_DerefMut_deref_mut(HostLockLending *self);
extern void host_lock_Lending_drop(HostLockLending *self);
extern HostLockShort host_lock_ShortMutex_lock(HostLockShortMutex *self,
											   const HostLockType *t, HostIoTakesShort io);
extern HostIoShortIo *host_lock_Short_io(HostLockShort *self);
extern HostLockPartsShortIo host_lock_Short_split(HostLockShort *self);
extern const void *host_lock_Short_Deref_deref(const HostLockShort *self);
extern void *host_lock_Short_DerefMut_deref_mut(HostLockShort *self);
extern void host_lock_Short_drop(HostLockShort *self);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_lock_tests_a_guard_gives_the_value_and_lends_the_held(void);
extern void test_host_lock_tests_a_second_take_of_a_held_mutex_is_refused(void);
extern void test_host_lock_tests_a_write_under_a_read_is_refused(void);
extern void test_host_lock_tests_a_lock_a_panic_poisoned_is_rebuilt_at_the_next_take(void);
extern void test_host_lock_tests_a_drop_under_an_unwind_through_its_lock_defers(void);
#endif

#endif							/* DOLTGRES_HOST_LOCK_H */
