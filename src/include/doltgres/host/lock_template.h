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
 * The template of host's lock.rs generics that hold a type parameter by
 * value (the port text's section 2): taken at T, each lock's new and
 * Default at T, Locked at a guard G, and Parts at a capability I. An
 * instance defines the parameters of the parts it needs and includes this
 * file; a part is generated only when its own parameters are defined, and
 * every parameter is undefined at the end.
 *
 * HOST_LOCK_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * HOST_LOCK_DECLARE generates the instance's types and declarations, and
 * HOST_LOCK_DEFINE its definitions, which its declarations precede.
 *
 * taken at T: HOST_LOCK_TAKEN_TS spells T, HOST_LOCK_TAKEN_T is T (or
 * HOST_LOCK_TAKEN_UNIT is defined for ()), HOST_LOCK_TAKEN_DEFAULT(place)
 * writes T's default there (and takes no argument for ()),
 * HOST_LOCK_TAKEN_DROP(place), when T's drop does something, drops the
 * value there, and HOST_LOCK_TAKEN_TYPE names T's HostLockType.
 *
 * A lock at T: one of HOST_LOCK_IO_MUTEX, HOST_LOCK_IO_RW_LOCK,
 * HOST_LOCK_LONG_MUTEX, and HOST_LOCK_SHORT_MUTEX is defined,
 * HOST_LOCK_TS spells T, HOST_LOCK_T is T (or HOST_LOCK_UNIT is defined for
 * ()), and HOST_LOCK_DEFAULT(place), when Default is instantiated, writes
 * T's default there.
 *
 * Locked at G: HOST_LOCK_LOCKED_GS spells G, HOST_LOCK_LOCKED_G is G's C
 * form, HOST_LOCK_LOCKED_G_DROP is G's drop, and HOST_LOCK_LOCKED_DEREF_MUT
 * is defined when G is DerefMut.
 *
 * Parts at I: HOST_LOCK_PARTS_IS spells I, and HOST_LOCK_PARTS_I is the C
 * form the capability's field holds.
 */

/* HOST_LOCK_CAT pastes two tokens after expanding them. */
#define HOST_LOCK_CAT_(a, b) a##b
#define HOST_LOCK_CAT(a, b) HOST_LOCK_CAT_(a, b)

/* HOST_LOCK_DEF is HOST_LOCK_SCOPE's storage class for a definition: static, or none for extern. */
#define HOST_LOCK_DEF_static static
#define HOST_LOCK_DEF_extern
#define HOST_LOCK_DEF HOST_LOCK_CAT(HOST_LOCK_DEF_, HOST_LOCK_SCOPE)

/*
 * HOST_LOCK_EXTERN declares an object of an extern instance, which a static
 * instance's definition, coming after its declarations, needs no declaration
 * of.
 */
#define HOST_LOCK_EXTERN_static(decl)
#define HOST_LOCK_EXTERN_extern(decl) extern decl;
#define HOST_LOCK_EXTERN(decl) HOST_LOCK_CAT(HOST_LOCK_EXTERN_, HOST_LOCK_SCOPE)(decl)

#ifdef HOST_LOCK_TAKEN_TS

/* HOST_LOCK_TAKEN_NAME is taken's instance at T: G is T's write guard. */
#define HOST_LOCK_TAKEN_NAME \
	HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_taken_RwLockWriteGuard, HOST_LOCK_TAKEN_TS), HOST_LOCK_TAKEN_TS)

#ifdef HOST_LOCK_DECLARE
HOST_LOCK_SCOPE StdRwLockWriteGuard HOST_LOCK_TAKEN_NAME(StdTryLockTag tag, StdRwLockWriteGuard *result,
														 void (*clear) (void *), void *clear_captures,
														 void (*clear_drop) (void *), const StdLocation *at);

HOST_LOCK_EXTERN(const HostLockType HOST_LOCK_TAKEN_TYPE)
#endif

#ifdef HOST_LOCK_DEFINE

/*
 * taken returns a lock's guard, refusing a lock its backend already holds,
 * and rebuilding the state of one a panic poisoned to its default once
 * `clear` clears the poison. The default is stored even should the old
 * value's drop panic, as Rust's drop-and-replace does, and the guard and
 * clear are dropped should any of it panic.
 */
HOST_LOCK_DEF StdRwLockWriteGuard
HOST_LOCK_TAKEN_NAME(StdTryLockTag tag, StdRwLockWriteGuard *result, void (*clear) (void *), void *clear_captures,
					 void (*clear_drop) (void *), const StdLocation *at)
{
	volatile StdRwLockWriteGuard guard;
#ifndef HOST_LOCK_TAKEN_UNIT
	volatile	HOST_LOCK_TAKEN_T tmp;
#endif
	volatile bool tmp_live = false;
	StdUnwind	unwind;

	switch (tag)
	{
		case STD_TRY_LOCK_OK:
			if (clear_drop != NULL)
				clear_drop(clear_captures);
			return *result;
		case STD_TRY_LOCK_WOULD_BLOCK:
			STD_UNWIND_PUSH(&unwind);
			if (sigsetjmp(unwind.jump, 0) == 0)
				host_lock_held_already(at);
			if (clear_drop != NULL)
				clear_drop(clear_captures);
			std_unwind_resume(&unwind);
		case STD_TRY_LOCK_POISONED:
			break;
	}
	guard = *result;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
#ifndef HOST_LOCK_TAKEN_UNIT
		HOST_LOCK_TAKEN_DEFAULT((HOST_LOCK_TAKEN_T *) & tmp);
#else
		HOST_LOCK_TAKEN_DEFAULT();
#endif
		tmp_live = true;
#ifdef HOST_LOCK_TAKEN_DROP
		HOST_LOCK_TAKEN_DROP(((StdRwLockWriteGuard *) &guard)->data);
#endif
		tmp_live = false;
#ifndef HOST_LOCK_TAKEN_UNIT
		*(HOST_LOCK_TAKEN_T *) ((StdRwLockWriteGuard *) &guard)->data = *(HOST_LOCK_TAKEN_T *) & tmp;
#endif
		clear(clear_captures);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_live)
		{
			tmp_live = false;
#ifndef HOST_LOCK_TAKEN_UNIT
			*(HOST_LOCK_TAKEN_T *) ((StdRwLockWriteGuard *) &guard)->data = *(HOST_LOCK_TAKEN_T *) & tmp;
#endif
		}
		std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &guard);
		if (clear_drop != NULL)
			clear_drop(clear_captures);
		std_unwind_resume(&unwind);
	}
	return *(StdRwLockWriteGuard *) &guard;
}

/* HOST_LOCK_TAKEN_TYPE is T's table for the locks' takes, its taken. */
HOST_LOCK_DEF const HostLockType HOST_LOCK_TAKEN_TYPE = {HOST_LOCK_TAKEN_NAME};

#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TAKEN_NAME
#endif							/* HOST_LOCK_TAKEN_TS */

#ifdef HOST_LOCK_IO_MUTEX

/* HOST_LOCK_TYPE is IoMutex<T>, and HOST_LOCK_NAME names its methods. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockIoMutex, HOST_LOCK_TS)
#define HOST_LOCK_NAME(name) HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_IoMutex, HOST_LOCK_TS), _##name)

#ifdef HOST_LOCK_DECLARE

/* IoMutex<T> is a mutex under which no file I/O runs: its header, then its value. */
typedef struct HOST_LOCK_TYPE
{
	HostLockIoMutex header;
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;
#endif
}			HOST_LOCK_TYPE;

#ifndef HOST_LOCK_UNIT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller);
#else
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller);
#endif
#ifdef HOST_LOCK_DEFAULT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller);
#endif

#endif							/* HOST_LOCK_DECLARE */

#ifdef HOST_LOCK_DEFINE

/* new returns a mutex holding the value, declared at its caller. */
#ifndef HOST_LOCK_UNIT
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller)
#else
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller)
#endif
{
	std_rw_lock_new(&result->header.lock);
	result->header.at = caller;
#ifndef HOST_LOCK_UNIT
	result->value = value;
#endif
}

#ifdef HOST_LOCK_DEFAULT

/* Default_default is IoMutex's Default: new of T's default, declared at its caller. */
HOST_LOCK_DEF void
HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller)
{
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;

	HOST_LOCK_DEFAULT(&value);
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, value, caller);
#else
	HOST_LOCK_DEFAULT();
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, caller);
#endif
}

#endif							/* HOST_LOCK_DEFAULT */
#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TYPE
#undef HOST_LOCK_NAME
#endif							/* HOST_LOCK_IO_MUTEX */

#ifdef HOST_LOCK_IO_RW_LOCK

/* HOST_LOCK_TYPE is IoRwLock<T>, and HOST_LOCK_NAME names its methods. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockIoRwLock, HOST_LOCK_TS)
#define HOST_LOCK_NAME(name) HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_IoRwLock, HOST_LOCK_TS), _##name)

#ifdef HOST_LOCK_DECLARE

/* IoRwLock<T> is a reader-writer lock under which no file I/O runs: its header, then its value. */
typedef struct HOST_LOCK_TYPE
{
	HostLockIoRwLock header;
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;
#endif
}			HOST_LOCK_TYPE;

#ifndef HOST_LOCK_UNIT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller);
#else
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller);
#endif
#ifdef HOST_LOCK_DEFAULT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller);
#endif

#endif							/* HOST_LOCK_DECLARE */

#ifdef HOST_LOCK_DEFINE

/* new returns a reader-writer lock holding the value, declared at its caller. */
#ifndef HOST_LOCK_UNIT
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller)
#else
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller)
#endif
{
	std_rw_lock_new(&result->header.lock);
	result->header.at = caller;
#ifndef HOST_LOCK_UNIT
	result->value = value;
#endif
}

#ifdef HOST_LOCK_DEFAULT

/* Default_default is IoRwLock's Default: new of T's default, declared at its caller. */
HOST_LOCK_DEF void
HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller)
{
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;

	HOST_LOCK_DEFAULT(&value);
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, value, caller);
#else
	HOST_LOCK_DEFAULT();
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, caller);
#endif
}

#endif							/* HOST_LOCK_DEFAULT */
#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TYPE
#undef HOST_LOCK_NAME
#endif							/* HOST_LOCK_IO_RW_LOCK */

#ifdef HOST_LOCK_LOCKED_GS

/* HOST_LOCK_TYPE is Locked<G>, and HOST_LOCK_NAME names its methods. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockLocked, HOST_LOCK_LOCKED_GS)
#define HOST_LOCK_NAME(name) HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_Locked, HOST_LOCK_LOCKED_GS), _##name)

#ifdef HOST_LOCK_DECLARE

/*
 * Locked<G> is the guard of a lock under which no file I/O runs, which
 * holds its taker's Held until it drops.
 */
typedef struct HOST_LOCK_TYPE
{
	/* guard is the lock's own guard. */
	HOST_LOCK_LOCKED_G guard;
	/* held is the taker's lock scope, which no file door takes. */
	HostIoHeld *held;
}			HOST_LOCK_TYPE;

HOST_LOCK_SCOPE HostIoHeld *HOST_LOCK_NAME(held) (HOST_LOCK_TYPE * self);
#ifdef HOST_LOCK_LOCKED_DEREF_MUT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(split) (HOST_LOCK_TYPE * self, void **out_0, HostIoHeld **out_1);
#endif
HOST_LOCK_SCOPE void HOST_LOCK_NAME(split_ref) (HOST_LOCK_TYPE * self, const void **out_0, HostIoHeld **out_1);
HOST_LOCK_SCOPE const void *HOST_LOCK_NAME(Deref_deref) (const HOST_LOCK_TYPE * self);
#ifdef HOST_LOCK_LOCKED_DEREF_MUT
HOST_LOCK_SCOPE void *HOST_LOCK_NAME(DerefMut_deref_mut) (HOST_LOCK_TYPE * self);
#endif
HOST_LOCK_SCOPE void HOST_LOCK_NAME(drop) (HOST_LOCK_TYPE * self);

#endif							/* HOST_LOCK_DECLARE */

#ifdef HOST_LOCK_DEFINE

/* held lends the guard's Held on, to take a further lock under it. */
HOST_LOCK_DEF HostIoHeld *
HOST_LOCK_NAME(held) (HOST_LOCK_TYPE * self)
{
	return self->held;
}

#ifdef HOST_LOCK_LOCKED_DEREF_MUT

/*
 * split returns the locked value and the guard's Held together, to change
 * the one under a further lock the other takes.
 */
HOST_LOCK_DEF void
HOST_LOCK_NAME(split) (HOST_LOCK_TYPE * self, void **out_0, HostIoHeld **out_1)
{
	*out_0 = self->guard.data;
	*out_1 = self->held;
}

#endif							/* HOST_LOCK_LOCKED_DEREF_MUT */

/*
 * split_ref returns the locked value shared and the guard's Held together,
 * to read the one under a further lock the other takes.
 */
HOST_LOCK_DEF void
HOST_LOCK_NAME(split_ref) (HOST_LOCK_TYPE * self, const void **out_0, HostIoHeld **out_1)
{
	*out_0 = self->guard.data;
	*out_1 = self->held;
}

/* Deref_deref is the locked value. */
HOST_LOCK_DEF const void *
HOST_LOCK_NAME(Deref_deref) (const HOST_LOCK_TYPE * self)
{
	return self->guard.data;
}

#ifdef HOST_LOCK_LOCKED_DEREF_MUT

/* DerefMut_deref_mut is the locked value. */
HOST_LOCK_DEF void *
HOST_LOCK_NAME(DerefMut_deref_mut) (HOST_LOCK_TYPE * self)
{
	return self->guard.data;
}

#endif							/* HOST_LOCK_LOCKED_DEREF_MUT */

/* drop is Locked's drop glue, its guard's. */
HOST_LOCK_DEF void
HOST_LOCK_NAME(drop) (HOST_LOCK_TYPE * self)
{
	HOST_LOCK_LOCKED_G_DROP(&self->guard);
}

#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TYPE
#undef HOST_LOCK_NAME
#endif							/* HOST_LOCK_LOCKED_GS */

#ifdef HOST_LOCK_LONG_MUTEX

/* HOST_LOCK_TYPE is LongMutex<T>, and HOST_LOCK_NAME names its methods. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockLongMutex, HOST_LOCK_TS)
#define HOST_LOCK_NAME(name) HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_LongMutex, HOST_LOCK_TS), _##name)

#ifdef HOST_LOCK_DECLARE

/* LongMutex<T> is a mutex held across file I/O: its header, then its value. */
typedef struct HOST_LOCK_TYPE
{
	HostLockLongMutex header;
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;
#endif
}			HOST_LOCK_TYPE;

#ifndef HOST_LOCK_UNIT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller);
#else
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller);
#endif
#ifdef HOST_LOCK_DEFAULT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller);
#endif

#endif							/* HOST_LOCK_DECLARE */

#ifdef HOST_LOCK_DEFINE

/* new returns a mutex holding the value, declared at its caller. */
#ifndef HOST_LOCK_UNIT
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller)
#else
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller)
#endif
{
	std_rw_lock_new(&result->header.lock);
	result->header.at = caller;
#ifndef HOST_LOCK_UNIT
	result->value = value;
#endif
}

#ifdef HOST_LOCK_DEFAULT

/* Default_default is LongMutex's Default: new of T's default, declared at its caller. */
HOST_LOCK_DEF void
HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller)
{
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;

	HOST_LOCK_DEFAULT(&value);
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, value, caller);
#else
	HOST_LOCK_DEFAULT();
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, caller);
#endif
}

#endif							/* HOST_LOCK_DEFAULT */
#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TYPE
#undef HOST_LOCK_NAME
#endif							/* HOST_LOCK_LONG_MUTEX */

#ifdef HOST_LOCK_PARTS_IS

/* HOST_LOCK_TYPE is Parts<T, I>, its value reached through void *, which Rust's ?Sized T is. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockParts, HOST_LOCK_PARTS_IS)

#ifdef HOST_LOCK_DECLARE

/*
 * Parts<T, I> is a lender's guarded value and the capability it lends,
 * which split gives together, so that the one changes with the I/O the
 * other does.
 */
typedef struct HOST_LOCK_TYPE
{
	/* value is the guarded value. */
	void	   *value;
	/* io is the capability the guard lends. */
	HOST_LOCK_PARTS_I io;
}			HOST_LOCK_TYPE;

#endif							/* HOST_LOCK_DECLARE */

#undef HOST_LOCK_TYPE
#endif							/* HOST_LOCK_PARTS_IS */

#ifdef HOST_LOCK_SHORT_MUTEX

/* HOST_LOCK_TYPE is ShortMutex<T>, and HOST_LOCK_NAME names its methods. */
#define HOST_LOCK_TYPE HOST_LOCK_CAT(HostLockShortMutex, HOST_LOCK_TS)
#define HOST_LOCK_NAME(name) HOST_LOCK_CAT(HOST_LOCK_CAT(host_lock_ShortMutex, HOST_LOCK_TS), _##name)

#ifdef HOST_LOCK_DECLARE

/* ShortMutex<T> is a mutex held across one bounded step of file I/O: its header, then its value. */
typedef struct HOST_LOCK_TYPE
{
	HostLockShortMutex header;
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;
#endif
}			HOST_LOCK_TYPE;

#ifndef HOST_LOCK_UNIT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller);
#else
HOST_LOCK_SCOPE void HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller);
#endif
#ifdef HOST_LOCK_DEFAULT
HOST_LOCK_SCOPE void HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller);
#endif

#endif							/* HOST_LOCK_DECLARE */

#ifdef HOST_LOCK_DEFINE

/* new returns a mutex holding the value, declared at its caller. */
#ifndef HOST_LOCK_UNIT
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, HOST_LOCK_T value, const StdLocation *caller)
#else
HOST_LOCK_DEF void
HOST_LOCK_NAME(new) (HOST_LOCK_TYPE * result, const StdLocation *caller)
#endif
{
	std_rw_lock_new(&result->header.lock);
	result->header.at = caller;
#ifndef HOST_LOCK_UNIT
	result->value = value;
#endif
}

#ifdef HOST_LOCK_DEFAULT

/* Default_default is ShortMutex's Default: new of T's default, declared at its caller. */
HOST_LOCK_DEF void
HOST_LOCK_NAME(Default_default) (void *result, const StdLocation *caller)
{
#ifndef HOST_LOCK_UNIT
	HOST_LOCK_T value;

	HOST_LOCK_DEFAULT(&value);
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, value, caller);
#else
	HOST_LOCK_DEFAULT();
	HOST_LOCK_NAME(new) ((HOST_LOCK_TYPE *) result, caller);
#endif
}

#endif							/* HOST_LOCK_DEFAULT */
#endif							/* HOST_LOCK_DEFINE */

#undef HOST_LOCK_TYPE
#undef HOST_LOCK_NAME
#endif							/* HOST_LOCK_SHORT_MUTEX */

#undef HOST_LOCK_CAT_
#undef HOST_LOCK_CAT
#undef HOST_LOCK_DEF_static
#undef HOST_LOCK_DEF_extern
#undef HOST_LOCK_DEF
#undef HOST_LOCK_EXTERN_static
#undef HOST_LOCK_EXTERN_extern
#undef HOST_LOCK_EXTERN
#undef HOST_LOCK_SCOPE
#undef HOST_LOCK_DECLARE
#undef HOST_LOCK_DEFINE
#undef HOST_LOCK_TAKEN_TS
#undef HOST_LOCK_TAKEN_T
#undef HOST_LOCK_TAKEN_UNIT
#undef HOST_LOCK_TAKEN_DEFAULT
#undef HOST_LOCK_TAKEN_DROP
#undef HOST_LOCK_TAKEN_TYPE
#undef HOST_LOCK_IO_MUTEX
#undef HOST_LOCK_IO_RW_LOCK
#undef HOST_LOCK_LONG_MUTEX
#undef HOST_LOCK_SHORT_MUTEX
#undef HOST_LOCK_TS
#undef HOST_LOCK_T
#undef HOST_LOCK_UNIT
#undef HOST_LOCK_DEFAULT
#undef HOST_LOCK_LOCKED_GS
#undef HOST_LOCK_LOCKED_G
#undef HOST_LOCK_LOCKED_G_DROP
#undef HOST_LOCK_LOCKED_DEREF_MUT
#undef HOST_LOCK_PARTS_IS
#undef HOST_LOCK_PARTS_I
