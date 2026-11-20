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
 * The template of host's owned.rs generics that hold a type parameter by
 * value (the port text's section 2): Table<R>'s instance at a kind's record
 * type R, with the glue its instantiation needs (R's Records' drop, the
 * IoMutex<Records<R>> instance through lock_template.h, the Arc its table
 * keeps, release::<R>, and the kind's table of Owned), and Table<R>::with<T>'s instance at a call's result T. An
 * instance defines its parameters and includes this file; every parameter
 * is undefined at the end.
 *
 * HOST_OWNED_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * HOST_OWNED_DECLARE generates the instance's types and declarations, and
 * HOST_OWNED_DEFINE its definitions, which its declarations precede.
 *
 * Table<R>: HOST_OWNED_PREFIX is the kind's prefix, which the glue's names
 * take, HOST_OWNED_RS spells R, HOST_OWNED_R is R, HOST_OWNED_R_DROP is R's
 * drop glue (NULL for a record whose drop does nothing), HOST_OWNED_KIND is
 * the kind's number, and HOST_OWNED_TABLE_FN returns the kind's static
 * Table.
 *
 * with<T>: HOST_OWNED_WITH_TS spells T besides the Table's parameters,
 * HOST_OWNED_WITH_T is T (or HOST_OWNED_WITH_UNIT is defined for ()), and
 * HOST_OWNED_WITH_T_DROP(place), when T's drop does something, drops the
 * call's result there.
 */

/* HOST_OWNED_CAT pastes two tokens after expanding them. */
#define HOST_OWNED_CAT_(a, b) a##b
#define HOST_OWNED_CAT(a, b) HOST_OWNED_CAT_(a, b)

/* HOST_OWNED_DEF is HOST_OWNED_SCOPE's storage class for a definition: static, or none for extern. */
#define HOST_OWNED_DEF_static static
#define HOST_OWNED_DEF_extern
#define HOST_OWNED_DEF HOST_OWNED_CAT(HOST_OWNED_DEF_, HOST_OWNED_SCOPE)

/*
 * HOST_OWNED_EXTERN declares an object of an extern instance, which a
 * static instance's definition, coming after its declarations, needs no
 * declaration of.
 */
#define HOST_OWNED_EXTERN_static(decl)
#define HOST_OWNED_EXTERN_extern(decl) extern decl;
#define HOST_OWNED_EXTERN(decl) HOST_OWNED_CAT(HOST_OWNED_EXTERN_, HOST_OWNED_SCOPE)(decl)

/*
 * HOST_OWNED_SLOT is a slot of R's records, HOST_OWNED_NAME names Table<R>'s
 * methods, HOST_OWNED_KIND_NAME the kind's glue, and HOST_OWNED_RECORDS_TS
 * spells Records<R>, with HOST_OWNED_MUTEX_TYPE and HOST_OWNED_MUTEX_DEFAULT
 * its IoMutex and that IoMutex's Default.
 */
#define HOST_OWNED_SLOT HOST_OWNED_CAT(HostOwnedOptionSlot, HOST_OWNED_RS)
#define HOST_OWNED_NAME(name) HOST_OWNED_CAT(HOST_OWNED_CAT(host_owned_Table, HOST_OWNED_RS), _##name)
#define HOST_OWNED_KIND_NAME(name) HOST_OWNED_CAT(HOST_OWNED_PREFIX, _##name)
#define HOST_OWNED_RECORDS_TS HOST_OWNED_CAT(Records, HOST_OWNED_RS)
#define HOST_OWNED_MUTEX_TYPE HOST_OWNED_CAT(HostLockIoMutex, HOST_OWNED_RECORDS_TS)
#define HOST_OWNED_MUTEX_DEFAULT HOST_OWNED_CAT(HOST_OWNED_CAT(host_lock_IoMutex, HOST_OWNED_RECORDS_TS), _Default_default)

#ifndef HOST_OWNED_WITH_TS
#ifdef HOST_OWNED_DECLARE

/* HOST_OWNED_SLOT is a slot of R's records, its record where HOST_OWNED_RECORD finds it. */
typedef struct HOST_OWNED_SLOT
{
	HostOwnedSlotOption head;
	HOST_OWNED_R record;
}			HOST_OWNED_SLOT;

StaticAssertDecl(offsetof(HOST_OWNED_SLOT, record) == MAXALIGN(sizeof(HostOwnedSlotOption)) &&
				 sizeof(HOST_OWNED_SLOT) == HOST_OWNED_ELEMENT(sizeof(HOST_OWNED_R)),
				 "a slot's record sits where HOST_OWNED_RECORD finds it");

HOST_OWNED_SCOPE bool HOST_OWNED_NAME(make) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record,
											 HostOwnedHandle *result, HostError *error);
HOST_OWNED_SCOPE bool HOST_OWNED_NAME(make_in_transaction) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record,
															HostOwnedHandle *result, HostError *error);
HOST_OWNED_SCOPE bool HOST_OWNED_NAME(made) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record, uint64 owner,
											 HostOwnedHandle *result, HostError *error);
HOST_OWNED_SCOPE bool HOST_OWNED_NAME(end) (HostOwnedTable *self, HostIoIo *io, HostOwnedHandle handle,
											HostError *error);
HOST_OWNED_SCOPE void HOST_OWNED_NAME(released) (HostOwnedTable *self, uint32 slot);
HOST_OWNED_SCOPE void HOST_OWNED_KIND_NAME(records_drop) (void *value);
HOST_OWNED_SCOPE void HOST_OWNED_KIND_NAME(mutex_drop) (void *value);
HOST_OWNED_SCOPE void HOST_OWNED_KIND_NAME(arc_default) (void *result, const StdLocation *caller);
HOST_OWNED_SCOPE void HOST_OWNED_KIND_NAME(release) (uint32 slot);
HOST_OWNED_SCOPE void HOST_OWNED_KIND_NAME(arc_drop) (void *value);

HOST_OWNED_EXTERN(const StdDefaultType HOST_OWNED_KIND_NAME(MUTEX))
HOST_OWNED_EXTERN(const StdDefaultType HOST_OWNED_KIND_NAME(ARC))
HOST_OWNED_EXTERN(const HostOwnedOwnedOps HOST_OWNED_KIND_NAME(OWNED_OPS))
HOST_OWNED_EXTERN(const HostLockType HOST_OWNED_KIND_NAME(RECORDS_LOCK))

#endif							/* HOST_OWNED_DECLARE */
#ifdef HOST_OWNED_DEFINE

/*
 * The kind's glue: R's Records' drop, the IoMutex<Records<R>> instance with
 * its new, Default, and taken, the Arc<IoMutex<Records<R>>> its table keeps,
 * and the kind's table of Owned.
 */
HOST_OWNED_DEF void
HOST_OWNED_KIND_NAME(records_drop) (void *value)
{
	host_owned_Records_drop((HostOwnedRecords *) value, sizeof(HOST_OWNED_R), _Alignof(HOST_OWNED_SLOT), HOST_OWNED_R_DROP);
}
#define HOST_LOCK_SCOPE HOST_OWNED_SCOPE
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS HOST_OWNED_RECORDS_TS
#define HOST_LOCK_TAKEN_T HostOwnedRecords
#define HOST_LOCK_TAKEN_DEFAULT(place) host_owned_Records_Default_default((place), sizeof(HOST_OWNED_R), _Alignof(HOST_OWNED_SLOT))
#define HOST_LOCK_TAKEN_DROP(place) host_owned_Records_drop((place), sizeof(HOST_OWNED_R), _Alignof(HOST_OWNED_SLOT), HOST_OWNED_R_DROP)
#define HOST_LOCK_TAKEN_TYPE HOST_OWNED_KIND_NAME(RECORDS_LOCK)
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS HOST_OWNED_RECORDS_TS
#define HOST_LOCK_T HostOwnedRecords
#define HOST_LOCK_DEFAULT(place) host_owned_Records_Default_default((place), sizeof(HOST_OWNED_R), _Alignof(HOST_OWNED_SLOT))
#include "doltgres/host/lock_template.h"
HOST_OWNED_DEF void
HOST_OWNED_KIND_NAME(mutex_drop) (void *value)
{
	HOST_OWNED_KIND_NAME(records_drop) (HOST_LOCK_VALUE((HostLockIoMutex *) value));
}
HOST_OWNED_DEF const StdDefaultType
HOST_OWNED_KIND_NAME(MUTEX) =
{
	sizeof(HOST_OWNED_MUTEX_TYPE), _Alignof(HOST_OWNED_MUTEX_TYPE), HOST_OWNED_MUTEX_DEFAULT,
		HOST_OWNED_KIND_NAME(mutex_drop), &STD_ARC_DEFAULT_SITE
};
HOST_OWNED_DEF void
HOST_OWNED_KIND_NAME(arc_default) (void *result, const StdLocation *caller)
{
	*(StdArcInner **) result = std_arc_default(&HOST_OWNED_KIND_NAME(MUTEX));
}
HOST_OWNED_DEF void
HOST_OWNED_KIND_NAME(arc_drop) (void *value)
{
	std_arc_drop(*(StdArcInner **) value, HOST_OWNED_KIND_NAME(MUTEX).size, HOST_OWNED_KIND_NAME(MUTEX).align,
				 HOST_OWNED_KIND_NAME(MUTEX).drop);
}
HOST_OWNED_DEF const StdDefaultType
HOST_OWNED_KIND_NAME(ARC) =
{
	sizeof(StdArcInner *), _Alignof(StdArcInner *), HOST_OWNED_KIND_NAME(arc_default),
		HOST_OWNED_KIND_NAME(arc_drop)
};
HOST_OWNED_DEF const HostOwnedOwnedOps
HOST_OWNED_KIND_NAME(OWNED_OPS) =
{
	HOST_OWNED_KIND, HOST_OWNED_TABLE_FN, sizeof(HOST_OWNED_R), HOST_OWNED_R_DROP,
		&HOST_OWNED_KIND_NAME(RECORDS_LOCK), HOST_OWNED_KIND_NAME(release)
};

HOST_OWNED_DEF bool
HOST_OWNED_NAME(make) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record, HostOwnedHandle *result,
					   HostError *error)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	volatile bool record_live = true;
	uint64		owner;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostHost	host = host_current();
		HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

		if (!host.ops->owner_enlarge(host.self, &raise, &owner, error))
		{
			record_live = false;
			if (drop_fn != NULL)
				drop_fn(&record);
			std_unwind_pop(&unwind);
			return false;
		}
		record_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (record_live && drop_fn != NULL)
			drop_fn(&record);
		std_unwind_resume(&unwind);
	}
	return HOST_OWNED_NAME(made) (self, io, record, owner, result, error);
}
HOST_OWNED_DEF bool
HOST_OWNED_NAME(make_in_transaction) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record,
									  HostOwnedHandle *result, HostError *error)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	volatile bool record_live = true;
	uint64		owner;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostHost	host = host_current();
		HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

		if (!host.ops->owner_top_enlarge(host.self, &raise, &owner, error))
		{
			record_live = false;
			if (drop_fn != NULL)
				drop_fn(&record);
			std_unwind_pop(&unwind);
			return false;
		}
		record_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (record_live && drop_fn != NULL)
			drop_fn(&record);
		std_unwind_resume(&unwind);
	}
	return HOST_OWNED_NAME(made) (self, io, record, owner, result, error);
}
HOST_OWNED_DEF bool
HOST_OWNED_NAME(made) (HostOwnedTable *self, HostIoIo *io, HOST_OWNED_R record, uint64 owner,
					   HostOwnedHandle *result, HostError *error)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	StdArcInner *volatile table = NULL;
	volatile bool record_live = true;
	volatile bool records_live = false;
	volatile HostLockLockedRwLockWriteGuard records;
	HostOwnedHandle handle;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostHost	host = host_current();
		HostLocalLocal tmp_1;
		HostIoHeld *held;
		StdAny		raise;
		HostOwnedRecords *value;
		bool		has_free = false;
		size_t		free = 0;
		size_t		index;
		uint32		slot;
		uint64		generation;
		uint64		datum;
		HOST_OWNED_SLOT live;
		StdArcInner *dropped;

		tmp_1 = host_local_BackendLocal_get(&self->records, &HOST_OWNED_KIND_NAME(ARC));
		table = std_arc_clone(*(StdArcInner *const *) host_local_Local_Deref_deref(&tmp_1, &HOST_OWNED_KIND_NAME(ARC)));
		host_local_Local_drop(&tmp_1, &HOST_OWNED_KIND_NAME(ARC));
		host_io_Io_Sealed_parts(io, &held, &raise);
		records = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HOST_OWNED_MUTEX_TYPE)), &HOST_OWNED_KIND_NAME(RECORDS_LOCK), held);
		records_live = true;
		value = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &records);
		if (!std_vec_try_reserve(&value->slots, 1, sizeof(HOST_OWNED_SLOT), _Alignof(HOST_OWNED_SLOT)))
		{
			*error = host_Error_out_of_memory();
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			record_live = false;
			if (drop_fn != NULL)
				drop_fn(&record);
			std_unwind_pop(&unwind);
			return false;
		}
		for (size_t i = 0; i < value->slots.len; i++)
		{
			if (!STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, i).head.has)
			{
				has_free = true;
				free = i;
				break;
			}
		}
		index = has_free ? free : value->slots.len;
		if (index > PG_UINT32_MAX)
		{
			*error = host_Error_out_of_memory();
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			record_live = false;
			if (drop_fn != NULL)
				drop_fn(&record);
			std_unwind_pop(&unwind);
			return false;
		}
		slot = (uint32) index;
		generation = value->next;
		value->next = std_add_u64(value->next, 1, HOST_OWNED_RUST_FILE, 123, 13);
		live.head.has = true;
		live.head.slot.generation = generation;
		live.head.slot.owner = owner;
		live.head.slot.has_record = true;
		live.record = record;
		record_live = false;
		if (has_free)
		{
			std_index_check(index, value->slots.len, HOST_OWNED_RUST_FILE, 126, 41);
			STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, index) = live;
		}
		else
			std_vec_push_Checked(&value->slots, &live, sizeof(HOST_OWNED_SLOT), _Alignof(HOST_OWNED_SLOT));
		handle.slot = slot;
		handle.generation = generation;
		records_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		datum = host_owned_datum(&HOST_OWNED_KIND_NAME(OWNED_OPS), handle.slot);
		host.ops->owned_remember(host.self, owner, datum, HOST_OWNED_KIND_NAME(release));
		dropped = table;
		table = NULL;
		HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (records_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		}
		if (record_live && drop_fn != NULL)
			drop_fn(&record);
		std_unwind_resume(&unwind);
	}
	*result = handle;
	return true;
}

#endif							/* HOST_OWNED_DEFINE */
#endif							/* !HOST_OWNED_WITH_TS */

#ifdef HOST_OWNED_WITH_TS

/* HOST_OWNED_WITH_NAME is Table<R>::with's instance at T. */
#define HOST_OWNED_WITH_NAME HOST_OWNED_CAT(HOST_OWNED_NAME(with), HOST_OWNED_WITH_TS)

#ifdef HOST_OWNED_DECLARE
HOST_OWNED_SCOPE bool HOST_OWNED_WITH_NAME(HostOwnedTable *self, HostIoIo *io, HostOwnedHandle handle,
										   bool (*call) (void *captures, HostIoIo *io, HOST_OWNED_R * record,
#ifndef HOST_OWNED_WITH_UNIT
														 HOST_OWNED_WITH_T * result,
#endif
														 HostError *error),
										   void *call_captures, void (*call_drop) (void *),
#ifndef HOST_OWNED_WITH_UNIT
										   HOST_OWNED_WITH_T * result,
#endif
										   HostError *error);
#endif							/* HOST_OWNED_DECLARE */

#ifdef HOST_OWNED_DEFINE

HOST_OWNED_DEF bool
HOST_OWNED_WITH_NAME(HostOwnedTable *self, HostIoIo *io, HostOwnedHandle handle,
					 bool (*call) (void *captures, HostIoIo *io, HOST_OWNED_R * record,

#ifndef HOST_OWNED_WITH_UNIT
								   HOST_OWNED_WITH_T * result,
#endif
								   HostError *error),
					 void *call_captures, void (*call_drop) (void *),

#ifndef HOST_OWNED_WITH_UNIT
					 HOST_OWNED_WITH_T * result,
#endif
					 HostError *error)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	StdArcInner *volatile table = NULL;
	volatile	HOST_OWNED_R record;
	volatile bool record_live = false;
	volatile bool ok = false;
	volatile bool result_live = false;
	volatile	HOST_OWNED_SLOT left;
	volatile bool left_live = false;
	volatile bool call_live = true;
	volatile bool records_live = false;
	volatile HostLockLockedRwLockWriteGuard records;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostLocalLocal tmp_1;
		HostIoHeld *held;
		StdAny		raise;
		HostOwnedRecords *value;
		HostOwnedSlotOption *slot;
		StdArcInner *dropped;

		tmp_1 = host_local_BackendLocal_get(&self->records, &HOST_OWNED_KIND_NAME(ARC));
		table = std_arc_clone(*(StdArcInner *const *) host_local_Local_Deref_deref(&tmp_1, &HOST_OWNED_KIND_NAME(ARC)));
		host_local_Local_drop(&tmp_1, &HOST_OWNED_KIND_NAME(ARC));
		host_io_Io_Sealed_parts(io, &held, &raise);
		records = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HOST_OWNED_MUTEX_TYPE)), &HOST_OWNED_KIND_NAME(RECORDS_LOCK), held);
		records_live = true;
		value = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &records);
		if (!host_owned_live(value, &HOST_OWNED_KIND_NAME(OWNED_OPS), handle, &slot, error))
		{
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			call_live = false;
			if (call_drop != NULL)
				call_drop(call_captures);
			std_unwind_pop(&unwind);
			return false;
		}
		if (!slot->slot.has_record)
		{
			static const char text[] = "a record of the fork's is in use by a call that comes back into it";
			StdString	message;

			std_string_from_Checked(&message, text, sizeof(text) - 1);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			call_live = false;
			if (call_drop != NULL)
				call_drop(call_captures);
			std_unwind_pop(&unwind);
			return false;
		}
		slot->slot.has_record = false;
		*(HOST_OWNED_R *) & record = *(HOST_OWNED_R *) HOST_OWNED_RECORD(slot);
		record_live = true;
		records_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		call_live = false;
		ok = call(call_captures, io, (HOST_OWNED_R *) & record,
#ifndef HOST_OWNED_WITH_UNIT
				  result,
#endif
				  error);
		result_live = true;
		host_io_Io_Sealed_parts(io, &held, &raise);
		records = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HOST_OWNED_MUTEX_TYPE)), &HOST_OWNED_KIND_NAME(RECORDS_LOCK), held);
		records_live = true;
		value = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &records);
		slot = host_owned_found(value, &HOST_OWNED_KIND_NAME(OWNED_OPS), handle);
		if (slot != NULL)
		{
			left_live = slot->slot.has_record;
			if (left_live)
				((HOST_OWNED_SLOT *) & left)->record = *(HOST_OWNED_R *) HOST_OWNED_RECORD(slot);
			*(HOST_OWNED_R *) HOST_OWNED_RECORD(slot) = *(HOST_OWNED_R *) & record;
			slot->slot.has_record = true;
		}
		else
		{
			left_live = true;
			((HOST_OWNED_SLOT *) & left)->record = *(HOST_OWNED_R *) & record;
		}
		record_live = false;
		records_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		if (left_live)
		{
			left_live = false;
			if (drop_fn != NULL)
				drop_fn(&((HOST_OWNED_SLOT *) & left)->record);
		}
		dropped = table;
		table = NULL;
		HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (records_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		if (left_live && drop_fn != NULL)
			drop_fn(&((HOST_OWNED_SLOT *) & left)->record);
		if (result_live)
		{
			if (ok)
			{
#ifdef HOST_OWNED_WITH_T_DROP
				HOST_OWNED_WITH_T_DROP(result);
#endif
			}
			else
				host_Error_drop(error);
		}
		if (record_live && drop_fn != NULL)
			drop_fn((HOST_OWNED_R *) & record);
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		}
		if (call_live && call_drop != NULL)
			call_drop(call_captures);
		std_unwind_resume(&unwind);
	}
	return ok;
}

#endif							/* HOST_OWNED_DEFINE */

#undef HOST_OWNED_WITH_NAME
#endif							/* HOST_OWNED_WITH_TS */

#ifndef HOST_OWNED_WITH_TS
#ifdef HOST_OWNED_DEFINE

HOST_OWNED_DEF bool
HOST_OWNED_NAME(end) (HostOwnedTable *self, HostIoIo *io, HostOwnedHandle handle, HostError *error)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	StdArcInner *volatile table = NULL;
	volatile	HOST_OWNED_SLOT record;
	volatile bool record_live = false;
	volatile bool records_live = false;
	volatile HostLockLockedRwLockWriteGuard records;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostLocalLocal tmp_1;
		HostIoHeld *held;
		StdAny		raise;
		HostOwnedRecords *value;
		HostOwnedSlotOption *slot;
		HOST_OWNED_SLOT *freed;
		HostHost	host;
		uint64		owner;
		uint64		datum;
		StdArcInner *dropped;

		tmp_1 = host_local_BackendLocal_get(&self->records, &HOST_OWNED_KIND_NAME(ARC));
		table = std_arc_clone(*(StdArcInner *const *) host_local_Local_Deref_deref(&tmp_1, &HOST_OWNED_KIND_NAME(ARC)));
		host_local_Local_drop(&tmp_1, &HOST_OWNED_KIND_NAME(ARC));
		host_io_Io_Sealed_parts(io, &held, &raise);
		records = host_lock_IoMutex_lock((HostLockIoMutex *) STD_ARC_VALUE(table, _Alignof(HOST_OWNED_MUTEX_TYPE)), &HOST_OWNED_KIND_NAME(RECORDS_LOCK), held);
		records_live = true;
		value = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut((HostLockLockedRwLockWriteGuard *) &records);
		if (!host_owned_live(value, &HOST_OWNED_KIND_NAME(OWNED_OPS), handle, &slot, error))
		{
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			std_unwind_pop(&unwind);
			return false;
		}
		if (!slot->slot.has_record)
		{
			static const char text[] = "a record of the fork's was ended by a call it lends to";
			StdString	message;

			std_string_from_Checked(&message, text, sizeof(text) - 1);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			records_live = false;
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
			dropped = table;
			table = NULL;
			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
			std_unwind_pop(&unwind);
			return false;
		}
		owner = slot->slot.owner;
		std_index_check(handle.slot, value->slots.len, HOST_OWNED_RUST_FILE, 178, 38);
		freed = &STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, handle.slot);
		*(HOST_OWNED_SLOT *) & record = *freed;
		freed->head.has = false;
		record_live = true;
		records_live = false;
		host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		host = host_current();
		datum = host_owned_datum(&HOST_OWNED_KIND_NAME(OWNED_OPS), handle.slot);
		host.ops->owned_forget(host.self, owner, datum);
		record_live = false;
		if (record.head.has && record.head.slot.has_record && drop_fn != NULL)
			drop_fn(&((HOST_OWNED_SLOT *) & record)->record);
		dropped = table;
		table = NULL;
		HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (records_live)
			host_lock_LockedRwLockWriteGuard_drop((HostLockLockedRwLockWriteGuard *) &records);
		if (record_live && record.head.has && record.head.slot.has_record && drop_fn != NULL)
			drop_fn(&((HOST_OWNED_SLOT *) & record)->record);
		if (table != NULL)
		{
			StdArcInner *dropped = table;

			HOST_OWNED_KIND_NAME(arc_drop) (&dropped);
		}
		std_unwind_resume(&unwind);
	}
	return true;
}
HOST_OWNED_DEF void
HOST_OWNED_NAME(released) (HostOwnedTable *self, uint32 slot)
{
	void		(*drop_fn) (void *) = HOST_OWNED_R_DROP;
	volatile HostLocalLocal table;
	volatile bool table_live;
	HostIoInDrop dropping;
	volatile StdRwLockWriteGuard records;
	volatile bool records_live = false;
	volatile	HOST_OWNED_SLOT freed;
	volatile bool freed_live = false;
	StdUnwind	unwind;

	if (!host_local_BackendLocal_existing(&self->records, (HostLocalLocal *) &table))
		return;
	table_live = true;
	dropping = host_io_InDrop_new();
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdArcInner *arc = *(StdArcInner *const *) host_local_Local_Deref_deref((HostLocalLocal *) &table, &HOST_OWNED_KIND_NAME(ARC));
		HostOwnedRecords *value;

		if (!host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) STD_ARC_VALUE(arc, _Alignof(HOST_OWNED_MUTEX_TYPE)), &dropping,
											(StdRwLockWriteGuard *) &records))
		{
			table_live = false;
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_OWNED_KIND_NAME(ARC));
			std_unwind_pop(&unwind);
			return;
		}
		records_live = true;
		value = (HostOwnedRecords *) records.data;
		if ((size_t) slot < value->slots.len && STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, slot).head.has)
		{
			*(HOST_OWNED_SLOT *) & freed = STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, slot);
			STD_VEC_AT(&value->slots, HOST_OWNED_SLOT, slot).head.has = false;
			freed_live = true;
		}
		records_live = false;
		std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &records);
		if (freed_live)
		{
			freed_live = false;
			if (freed.head.slot.has_record && drop_fn != NULL)
				drop_fn(&((HOST_OWNED_SLOT *) & freed)->record);
		}
		table_live = false;
		host_local_Local_drop((HostLocalLocal *) &table, &HOST_OWNED_KIND_NAME(ARC));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (records_live)
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &records);
		if (freed_live && freed.head.slot.has_record && drop_fn != NULL)
			drop_fn(&((HOST_OWNED_SLOT *) & freed)->record);
		if (table_live)
			host_local_Local_drop((HostLocalLocal *) &table, &HOST_OWNED_KIND_NAME(ARC));
		std_unwind_resume(&unwind);
	}
}

HOST_OWNED_DEF void
HOST_OWNED_KIND_NAME(release) (uint32 slot)
{
	HOST_OWNED_NAME(released) (HOST_OWNED_TABLE_FN(), slot);
}

#endif							/* HOST_OWNED_DEFINE */
#endif							/* !HOST_OWNED_WITH_TS */

#undef HOST_OWNED_CAT_
#undef HOST_OWNED_CAT
#undef HOST_OWNED_DEF_static
#undef HOST_OWNED_DEF_extern
#undef HOST_OWNED_DEF
#undef HOST_OWNED_EXTERN_static
#undef HOST_OWNED_EXTERN_extern
#undef HOST_OWNED_EXTERN
#undef HOST_OWNED_SLOT
#undef HOST_OWNED_NAME
#undef HOST_OWNED_KIND_NAME
#undef HOST_OWNED_RECORDS_TS
#undef HOST_OWNED_MUTEX_TYPE
#undef HOST_OWNED_MUTEX_DEFAULT
#undef HOST_OWNED_SCOPE
#undef HOST_OWNED_DECLARE
#undef HOST_OWNED_DEFINE
#undef HOST_OWNED_PREFIX
#undef HOST_OWNED_RS
#undef HOST_OWNED_R
#undef HOST_OWNED_R_DROP
#undef HOST_OWNED_KIND
#undef HOST_OWNED_TABLE_FN
#undef HOST_OWNED_WITH_TS
#undef HOST_OWNED_WITH_T
#undef HOST_OWNED_WITH_UNIT
#undef HOST_OWNED_WITH_T_DROP
