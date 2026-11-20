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
 * Backend-local state (design items 6 and 14): a static whose value each
 * backend keeps for itself. In the server a backend is a process, so its
 * value is the static's own; the simulation runs several backends as actors
 * in one process, and the host names the running actor, whose value is its
 * own.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/std/unwind.h"

/* BTreeMap<u64, Arc<T>>, a BackendLocal's map of actors, one instance for every T, whose table its Arc's drop and default take. */
#define STD_BTREE_SCOPE static
#define STD_BTREE_DECLARE
#define STD_BTREE_DEFINE
#define STD_BTREE_KS U64
#define STD_BTREE_K uint64
#define STD_BTREE_VS Arc
#define STD_BTREE_V StdArcInner *
#define STD_BTREE_LAYOUT U64_ARC
#define STD_BTREE_CMP(a, b) std_u64_Ord_cmp((a), (b))
#define STD_BTREE_KEY_DROP(k) ((void) (k))
#define STD_BTREE_VAL_DROP(v) std_arc_drop(*(v), t->size, t->align, t->drop)
#define STD_BTREE_VAL_DEFAULT(v) (*(v) = std_arc_default(t))
#define STD_BTREE_TABLES , const StdDefaultType *t
#define STD_BTREE_TABLE_ARGS , t
#include "doltgres/std/btree_template.h"

/* host_local_Local_Deref_deref is the backend's value. */
const void *
host_local_Local_Deref_deref(const HostLocalLocal *self, const StdDefaultType *t)
{
	switch (self->tag)
	{
		case HOST_LOCAL_LOCAL_PROCESS:
			return self->u.process;
		case HOST_LOCAL_LOCAL_ACTOR:
			return STD_ARC_VALUE(self->u.actor, t->align);
	}
	pg_unreachable();
}

/* host_local_Local_drop is a backend's value's drop glue: an actor's Arc's drop. */
void
host_local_Local_drop(HostLocalLocal *self, const StdDefaultType *t)
{
	switch (self->tag)
	{
		case HOST_LOCAL_LOCAL_PROCESS:
			break;
		case HOST_LOCAL_LOCAL_ACTOR:
			std_arc_drop(self->u.actor, t->size, t->align, t->drop);
			break;
	}
}

/*
 * host_local_BackendLocal_get returns the running backend's value. An actor's
 * value is cloned out of the map, since it must outlive the map's lock, whose
 * read guard held across the value's use would deadlock a nested get that
 * inserts.
 */
HostLocalLocal
host_local_BackendLocal_get(HostLocalBackendLocal *self, const StdDefaultType *t)
{
	HostHost	host = host_current();
	uint64		actor;
	StdArcInner *volatile found = NULL;
	StdArcInner *volatile value = NULL;
	HostLocalLocal result;
	StdUnwind	unwind;

	if (!host.ops->actor(host.self, &actor))
	{
		void	   *process = HOST_LOCAL_PROCESS(self);

		std_once_lock_get_or_init(&self->process, process, std_default_call, (void *) t);
		result.tag = HOST_LOCAL_LOCAL_PROCESS;
		result.u.process = process;
		return result;
	}
	{
		volatile StdRwLockReadGuard tmp_1;
		volatile bool tmp_1_live;

		(void) std_rw_lock_read_Checked(&self->actors_lock, &self->actors, (StdRwLockReadGuard *) &tmp_1);
		tmp_1_live = true;
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			StdArcInner *const *at = std_btree_BTreeMapU64Arc_get(&self->actors, &actor);

			if (at != NULL)
				found = std_arc_clone(*at);
			tmp_1_live = false;
			std_RwLockReadGuard_drop((StdRwLockReadGuard *) &tmp_1);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (found != NULL)
				std_arc_drop(found, t->size, t->align, t->drop);
			if (tmp_1_live)
				std_RwLockReadGuard_drop((StdRwLockReadGuard *) &tmp_1);
			std_unwind_resume(&unwind);
		}
	}
	if (found != NULL)
		value = found;
	else
	{
		volatile StdRwLockWriteGuard tmp_2;
		volatile bool tmp_2_live;

		(void) std_rw_lock_write(&self->actors_lock, &self->actors, (StdRwLockWriteGuard *) &tmp_2);
		tmp_2_live = true;
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			StdBTreeEntryU64Arc tmp_3 = std_btree_BTreeMapU64Arc_entry(&self->actors, &actor);

			value = std_arc_clone(*std_btree_EntryU64Arc_or_default(&tmp_3, t));
			tmp_2_live = false;
			std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &tmp_2);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (value != NULL)
				std_arc_drop(value, t->size, t->align, t->drop);
			if (tmp_2_live)
				std_RwLockWriteGuard_drop((StdRwLockWriteGuard *) &tmp_2);
			std_unwind_resume(&unwind);
		}
	}
	result.tag = HOST_LOCAL_LOCAL_ACTOR;
	result.u.actor = value;
	return result;
}

/*
 * host_local_BackendLocal_existing returns the running backend's value if it
 * made one, without making it, which takes no lock that waits and allocates
 * nothing, for a release that runs inside an abort: an actor's map, which no
 * actor writes while another runs, is read only if no writer holds it.
 */
bool
host_local_BackendLocal_existing(HostLocalBackendLocal *self, HostLocalLocal *result)
{
	uint64		actor;
	volatile StdRwLockReadGuard actors;
	volatile bool actors_live;
	StdArcInner *const *at;
	StdArcInner *value;
	StdUnwind	unwind;

	if (!(std_once_lock_get(&HOST_HOST) && HOST_HOST_VALUE.ops->actor(HOST_HOST_VALUE.self, &actor)))
	{
		if (!std_once_lock_get(&self->process))
			return false;
		result->tag = HOST_LOCAL_LOCAL_PROCESS;
		result->u.process = HOST_LOCAL_PROCESS(self);
		return true;
	}
	switch (std_rw_lock_try_read(&self->actors_lock, &self->actors, (StdRwLockReadGuard *) &actors))
	{
		case STD_TRY_LOCK_OK:
			break;
		case STD_TRY_LOCK_POISONED:
			std_RwLockReadGuard_drop((StdRwLockReadGuard *) &actors);
			return false;
		case STD_TRY_LOCK_WOULD_BLOCK:
			return false;
	}
	actors_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		at = std_btree_BTreeMapU64Arc_get(&self->actors, &actor);
		if (at == NULL)
		{
			actors_live = false;
			std_RwLockReadGuard_drop((StdRwLockReadGuard *) &actors);
			std_unwind_pop(&unwind);
			return false;
		}
		value = std_arc_clone(*at);
		actors_live = false;
		std_RwLockReadGuard_drop((StdRwLockReadGuard *) &actors);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (actors_live)
			std_RwLockReadGuard_drop((StdRwLockReadGuard *) &actors);
		std_unwind_resume(&unwind);
	}
	result->tag = HOST_LOCAL_LOCAL_ACTOR;
	result->u.actor = value;
	return true;
}
