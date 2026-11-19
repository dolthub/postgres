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
 * Backend-local state (host's local.rs, design items 6 and 14): a static
 * whose value each backend keeps for itself. The process's one backend's
 * value follows the static's header at HOST_LOCAL_PROCESS, as the Rust's
 * OnceLock<T> holds it; the simulation's actors' values are Arcs in a map.
 */
#ifndef DOLTGRES_HOST_LOCAL_H
#define DOLTGRES_HOST_LOCAL_H

#include "doltgres/std/any.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/sync.h"

/*
 * HostLocalBackendLocal is a value each backend keeps for itself, its type's
 * default until the backend first changes it: the process's one backend's,
 * whose value follows, and the actors' by the actor the host names, a map
 * from u64 to Arc<T> under a lock.
 */
typedef struct HostLocalBackendLocal
{
	StdOnceLock process;
	StdRwLock	actors_lock;
	StdBTreeMap actors;
} HostLocalBackendLocal;

/* HOST_LOCAL_PROCESS is the process's one backend's value, which follows the header. */
#define HOST_LOCAL_PROCESS(local) ((void *) ((char *) (local) + MAXALIGN(sizeof(HostLocalBackendLocal))))

/* HOST_LOCAL_BACKEND_LOCAL_NEW is BackendLocal::new, for a static's initializer. */
#define HOST_LOCAL_BACKEND_LOCAL_NEW {STD_ONCE_LOCK_NEW, STD_RW_LOCK_NEW, STD_BTREE_MAP_NEW}

/* HostLocalLocalTag is Local's variants. */
typedef enum HostLocalLocalTag
{
	HOST_LOCAL_LOCAL_PROCESS,
	HOST_LOCAL_LOCAL_ACTOR,
} HostLocalLocalTag;

/* HostLocalLocal is a backend's value of a BackendLocal: the process's one backend's, or an actor's Arc. */
typedef struct HostLocalLocal
{
	HostLocalLocalTag tag;
	union
	{
		const void *process;
		StdArcInner *actor;
	}			u;
} HostLocalLocal;

extern const void *host_local_Local_Deref_deref(const HostLocalLocal *self, const StdDefaultType *t);
extern void host_local_Local_drop(HostLocalLocal *self, const StdDefaultType *t);
extern HostLocalLocal host_local_BackendLocal_get(HostLocalBackendLocal *self,
												  const StdDefaultType *t);
extern bool host_local_BackendLocal_existing(HostLocalBackendLocal *self, HostLocalLocal *result);

#endif							/* DOLTGRES_HOST_LOCAL_H */
