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
 * Owned state across calls (host's owned.rs, design item 13): a backend's
 * records of a kind, each named to Postgres by a handle, and remembered by
 * the resource owner current when it was made. A kind's code is one C
 * function for every R, which reads what it needs of R from the kind's
 * HostOwnedOwnedOps; owned_template.h, included in the implementer's file,
 * makes the functions and tables of R's instantiation that Rust's
 * monomorphization makes.
 */
#ifndef DOLTGRES_HOST_OWNED_H
#define DOLTGRES_HOST_OWNED_H

#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"

/*
 * HostOwnedHandle names a record to Postgres: its slot in its kind's table
 * and the generation that tells it from every other record the slot held,
 * which never repeats.
 */
typedef struct HostOwnedHandle
{
	uint32		slot;
	uint64		generation;
} HostOwnedHandle;

/* HostOwnedTable is a kind's records in each backend, a BackendLocal of an Arc<IoMutex<Records<R>>>. */
typedef struct HostOwnedTable
{
	HostLocalBackendLocal records;

	/*
	 * records_process is the BackendLocal's value for the process's one
	 * backend.
	 */
	StdArcInner *records_process;
} HostOwnedTable;

/* HOST_OWNED_TABLE_NEW is Table::new, for a static's initializer. */
#define HOST_OWNED_TABLE_NEW {HOST_LOCAL_BACKEND_LOCAL_NEW, NULL}

/*
 * HostOwnedRecords is a backend's records of one kind, in slots an end frees
 * and a later record reuses, and the generation its next record takes.
 * Nothing under its lock panics, so the lock is never poisoned.
 */
typedef struct HostOwnedRecords
{
	StdVec		slots;
	uint64		next;
} HostOwnedRecords;

/*
 * HostOwnedSlot is a live record's place in its table: its generation, the
 * resource owner that remembers it, and the record, which a call takes out
 * while it has it, and which follows at HOST_OWNED_RECORD.
 */
typedef struct HostOwnedSlot
{
	uint64		generation;
	uint64		owner;
	bool		has_record;
} HostOwnedSlot;

/* HostOwnedSlotOption is an element of a table's slots, an Option<Slot<R>>, whose record follows it. */
typedef struct HostOwnedSlotOption
{
	bool		has;
	HostOwnedSlot slot;
} HostOwnedSlotOption;

/*
 * HostOwnedOwnedOps is the Owned trait's table, its KIND and table, with
 * what the generic code needs of R beside them: R's size and drop, its
 * Records' lock type, and release::<R>.
 */
typedef struct HostOwnedOwnedOps
{
	uint8		KIND;
	HostOwnedTable *(*table) (void);
	size_t		size;
	void		(*drop) (void *record);
	const HostLockType *records_lock;
	void		(*release) (uint32 slot);
} HostOwnedOwnedOps;

/* HOST_OWNED_ELEMENT is the size of an element of a table's slots for a record of a size. */
#define HOST_OWNED_ELEMENT(size) (MAXALIGN(sizeof(HostOwnedSlotOption)) + MAXALIGN(size))

/* HOST_OWNED_RECORD is the record an element of a table's slots holds. */
#define HOST_OWNED_RECORD(element) ((void *) ((char *) (element) + MAXALIGN(sizeof(HostOwnedSlotOption))))

extern void host_owned_Records_Default_default(HostOwnedRecords *result, size_t size, size_t align);
extern void host_owned_Records_drop(HostOwnedRecords *self, size_t size, size_t align, void (*drop) (void *));
extern uint64 host_owned_datum(const HostOwnedOwnedOps *r, uint32 slot);
extern bool host_owned_live(HostOwnedRecords *records, const HostOwnedOwnedOps *r, HostOwnedHandle handle,
							HostOwnedSlotOption **result, HostError *error);
extern HostOwnedSlotOption *host_owned_found(HostOwnedRecords *records, const HostOwnedOwnedOps *r,
											 HostOwnedHandle handle);
extern void host_owned_remembered(uint64 datum, uint8 *out_0, uint32 *out_1);

/* HOST_OWNED_RUST_FILE is owned.rs, which the instances' panics name, as Rust's monomorphized code reports them. */
#define HOST_OWNED_RUST_FILE "host" STD_SEP "src" STD_SEP "owned.rs"

#endif							/* DOLTGRES_HOST_OWNED_H */
