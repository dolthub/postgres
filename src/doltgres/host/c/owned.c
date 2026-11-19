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
 * Owned state across calls (design item 13): a backend's records of a kind,
 * each named to Postgres by a handle, its slot and a 64-bit generation,
 * which a call checks before it lends the record, and remembered by the
 * resource owner current when it was made, whose release drops the record
 * unless its end came first.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/owned.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "owned.rs"

/* A table's process value sits where HOST_LOCAL_PROCESS finds its BackendLocal's. */
StaticAssertDecl(offsetof(HostOwnedTable, records_process) == MAXALIGN(sizeof(HostLocalBackendLocal)),
				 "a table's process value follows its BackendLocal");

/*
 * host_owned_Records_Default_default returns no records, the next
 * generation being one; it takes R's size and its slot's alignment.
 */
void
host_owned_Records_Default_default(HostOwnedRecords *result, size_t size, size_t align)
{
	std_vec_new(&result->slots, HOST_OWNED_ELEMENT(size), align);
	result->next = 1;
}

/* host_owned_Records_drop is Records' drop glue: each live slot's record, then the slots. */
void
host_owned_Records_drop(HostOwnedRecords *self, size_t size, size_t align, void (*drop) (void *))
{
	size_t		element = HOST_OWNED_ELEMENT(size);

	for (size_t i = 0, i_len = self->slots.len; i < i_len; i++)
	{
		HostOwnedSlotOption *at = (HostOwnedSlotOption *) ((char *) self->slots.ptr + i * element);

		if (at->has && at->slot.has_record && drop != NULL)
			drop(HOST_OWNED_RECORD(at));
	}
	std_vec_free_Checked(&self->slots, element, align);
}

/*
 * host_owned_stale returns the error of a handle that names no live record of
 * its kind: one ended, never made, or forged.
 */
static HostError
host_owned_stale(void)
{
	static const char text[] = "a handle names no live record of its kind";
	StdString	message;

	std_string_from_Checked(&message, text, sizeof(text) - 1);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* host_owned_datum returns what a resource owner remembers of a record: its slot and its kind's number. */
uint64
host_owned_datum(const HostOwnedOwnedOps *r, uint32 slot)
{
	uint64		tmp_1 = (uint64) slot;
	uint64		tmp_2 = STD_SHL_FOLDED(u64, (uint64) r->KIND, 32);

	return tmp_1 | tmp_2;
}

/* host_owned_live returns the slot of the live record a handle names, or the error of a stale handle. */
bool
host_owned_live(HostOwnedRecords *records, const HostOwnedOwnedOps *r, HostOwnedHandle handle,
				HostOwnedSlotOption **result, HostError *error)
{
	HostOwnedSlotOption *found = host_owned_found(records, r, handle);

	if (found == NULL)
	{
		*error = host_owned_stale();
		return false;
	}
	*result = found;
	return true;
}

/* host_owned_found returns the slot of the live record a handle names, or nothing for a stale handle. */
HostOwnedSlotOption *
host_owned_found(HostOwnedRecords *records, const HostOwnedOwnedOps *r, HostOwnedHandle handle)
{
	HostOwnedSlotOption *slot;

	if (!((size_t) handle.slot < records->slots.len))
		return NULL;
	slot = ((HostOwnedSlotOption *) STD_VEC_AT_SIZE(&(records)->slots, handle.slot, HOST_OWNED_ELEMENT(r->size)));
	if (!slot->has)
		return NULL;
	return slot->slot.generation == handle.generation ? slot : NULL;
}

/*
 * host_owned_remembered returns the kind's number and the slot a resource
 * owner's datum of an owned record carries, for the owner's release, which
 * finds the kind's table by its number.
 */
void
host_owned_remembered(uint64 datum, uint8 *out_0, uint32 *out_1)
{
	*out_0 = (uint8) (STD_SHR_FOLDED(u64, datum, 32) & 0xff);
	*out_1 = (uint32) (datum & (uint64) PG_UINT32_MAX);
}
