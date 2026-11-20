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
 * The doltgres crate's entries of the fork's tuple table slot (entries.rs'
 * doltgres_slot_init to doltgres_slot_copy_minimal_tuple, with deformed and
 * overflow_chunk) and doltgres_table_slot_callbacks, which names its kind.
 */
#ifndef FRONTEND
#include "postgres.h"
#include "doltgres/access/slot.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/host/lib.h"
#include "doltgres/pg/lib.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/unwind.h"
#include "pg_calls.h"

/* doltgres_entries_doltgres_slot_init is the fork's slot's init: Postgres zeroed the slot's fields, which is their start. */
bool
doltgres_entries_doltgres_slot_init(const PgCaps *caps, PgSlot *slot, PgPgError *error)
{
	return true;
}

/* doltgres_entries_doltgres_slot_release is the fork's slot's release: it frees the slot's buffer. */
bool
doltgres_entries_doltgres_slot_release(const PgCleanup *caps, PgSlot *slot, PgPgError *error)
{
	pg_calls_slot_release(slot);
	return true;
}

/* doltgres_entries_doltgres_slot_clear is the fork's slot's clear: the slot holds no row after it, and keeps its buffer. */
bool
doltgres_entries_doltgres_slot_clear(const PgCleanup *caps, PgSlot *slot, PgPgError *error)
{
	pg_calls_slot_clear(slot);
	return true;
}

/*
 * doltgres_entries_overflow_chunk returns the bytes of the overflow chunk at
 * an address that a stored row of the session's database names, read
 * through the verified read door from its repository's working state's
 * delta, else its packs.
 */
static bool
doltgres_entries_overflow_chunk(const PgCaps *caps, const uint8 address[32], StdVec *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));

	return doltgres_entries_blob_bytes(&io, address, result, error);
}

/* doltgres_entries_deformed_frame_t is deformed's state across its pad: the chunk's bytes, while live. */
typedef struct doltgres_entries_deformed_frame_t
{
	StdVec		bytes;
	bool		bytes_live;
	const PgCaps *caps;
	const PgSlot *slot;
	int32		natts;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_deformed_frame_t;

/* doltgres_entries_deformed_pad frees what deformed holds. */
static void
doltgres_entries_deformed_pad(void *frame_)
{
	doltgres_entries_deformed_frame_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->bytes_live = false;
}

/* doltgres_entries_deformed_body deforms the row, reading each overflow chunk the deform stops at. */
static void
doltgres_entries_deformed_body(void *frame_)
{
	doltgres_entries_deformed_frame_t *frame = frame_;
	AccessSlotChunk chunk;
	size_t		len;
	bool		stop;

	pg_calls_slot_tuple(frame->slot, &len);
	if (len == 0)
	{
		frame->ok = true;
		return;
	}
	if (!access_slot_getsomeattrs(frame->slot, frame->natts, &stop, &chunk, frame->error))
		return;
	while (stop)
	{
		AccessSlotChunk at = chunk;
		bool		overflowed;

		if (!doltgres_entries_overflow_chunk(frame->caps, at.address, &frame->bytes, frame->error))
			return;
		frame->bytes_live = true;
		overflowed = access_slot_overflowed(frame->slot, frame->natts, &at, frame->bytes.ptr, frame->bytes.len, &stop,
											&chunk, frame->error);
		std_vec_free_Checked(&frame->bytes, 1, 1);
		frame->bytes_live = false;
		if (!overflowed)
			return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_deformed deforms the stored row a slot holds as far as a
 * number of attributes, reading each overflow chunk the deform stops at
 * through the session's repository, its working state's delta first, and
 * nothing for the executor's values.
 */
static bool
doltgres_entries_deformed(const PgCaps *caps, const PgSlot *slot, int32 natts, PgPgError *error)
{
	doltgres_entries_deformed_frame_t frame = {.caps = caps, .slot = slot, .natts = natts, .error = error};

	std_landing(&frame, doltgres_entries_deformed_body, doltgres_entries_deformed_pad);
	doltgres_entries_deformed_pad(&frame);
	return frame.ok;
}

/* doltgres_entries_doltgres_slot_getsomeattrs is the fork's slot's getsomeattrs: it deforms the stored row as far as natts. */
bool
doltgres_entries_doltgres_slot_getsomeattrs(const PgCaps *caps, PgSlot *slot, int32 natts, PgPgError *error)
{
	return doltgres_entries_deformed(caps, slot, natts, error);
}

/* doltgres_entries_doltgres_slot_getsysattr is the fork's slot's getsysattr. */
bool
doltgres_entries_doltgres_slot_getsysattr(const PgCaps *caps, PgSlot *slot, int32 attnum, PgIsNull *isnull,
										  PgDatum *result, PgPgError *error)
{
	return access_slot_getsysattr(pg_Caps_may_raise(caps), slot, attnum, isnull, result, error);
}

/*
 * doltgres_entries_doltgres_slot_is_current_xact_tuple is the fork's slot's
 * is_current_xact_tuple: whether the current transaction or one of its live
 * subtransactions wrote the stored row it holds, asked of seam 1's
 * is_current on each call, so a rollback since the row was stored is seen;
 * for the executor's values a virtual slot's answer.
 */
bool
doltgres_entries_doltgres_slot_is_current_xact_tuple(const PgCaps *caps, PgSlot *slot, bool *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	uint64		writer = pg_calls_slot_writer(slot);
	HostXid		xid = {writer};
	HostIoIo	io;

	if (writer == 0)
		return pg_calls_slot_is_current(raise, slot, result, error);
	io = pghost_io(raise);
	*result = host_is_current(HOST_IO_IO_IO_DOORS(&io), xid);
	return true;
}

/*
 * doltgres_entries_doltgres_slot_materialize is the fork's slot's
 * materialize: a stored row is the slot's own already, and the executor's
 * values are copied into the slot's memory.
 */
bool
doltgres_entries_doltgres_slot_materialize(const PgCaps *caps, PgSlot *slot, PgPgError *error)
{
	return pg_calls_slot_materialize(pg_Caps_may_raise(caps), slot, error);
}

/* doltgres_entries_doltgres_slot_copyslot is the fork's slot's copyslot: the slot holds the source's row, in memory of its own. */
bool
doltgres_entries_doltgres_slot_copyslot(const PgCaps *caps, PgSlot *dstslot, PgSlot *srcslot, PgPgError *error)
{
	return access_slot_copy(pg_Caps_may_raise(caps), dstslot, srcslot, error);
}

/* doltgres_entries_doltgres_slot_copy_heap_tuple is the fork's slot's copy_heap_tuple. */
bool
doltgres_entries_doltgres_slot_copy_heap_tuple(const PgCaps *caps, PgSlot *slot, PgHeapTuple *result,
											   PgPgError *error)
{
	return doltgres_entries_deformed(caps, slot, pg_calls_slot_natts(slot), error) &&
		pg_calls_slot_heap_tuple(pg_Caps_may_raise(caps), slot, result, error);
}

/* doltgres_entries_doltgres_slot_copy_minimal_tuple is the fork's slot's copy_minimal_tuple. */
bool
doltgres_entries_doltgres_slot_copy_minimal_tuple(const PgCaps *caps, PgSlot *slot, size_t extra,
												  PgMinimalTuple *result, PgPgError *error)
{
	return doltgres_entries_deformed(caps, slot, pg_calls_slot_natts(slot), error) &&
		pg_calls_slot_minimal_tuple(pg_Caps_may_raise(caps), slot, extra, result, error);
}

/* doltgres_entries_doltgres_table_slot_callbacks names the fork's kind of tuple table slot, the table routine's slot_callbacks. */
bool
doltgres_entries_doltgres_table_slot_callbacks(const PgCaps *caps, const PgRel *rel, PgSlotOps *result,
											   PgPgError *error)
{
	*result = pg_calls_slot_ops();
	return true;
}
#endif							/* FRONTEND */
