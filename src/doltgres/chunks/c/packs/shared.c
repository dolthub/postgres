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
 * The C port of chunks' packs/shared.rs: a repository's pack record, its
 * pool, its parts, the snapshot every view is brought up to date from, and
 * the evening of the repositories' manifests.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "shared.rs"

/* chunks_packs_shared_given returns the highest number given to a pack writer, of a record's Reserved. */
uint64
chunks_packs_shared_given(uint64 reserved)
{
	return STD_SHR_FOLDED(u64, reserved, 16);
}

/* chunks_packs_shared_headroom returns how many more numbers the file's bound lets writers be given, of a record's Reserved. */
uint64
chunks_packs_shared_headroom(uint64 reserved)
{
	return reserved & 0xffff;
}

/* chunks_packs_shared_taken returns a record's Reserved once one more number is given under the same bound. */
uint64
chunks_packs_shared_taken(uint64 reserved)
{
	return STD_SUB(u64, STD_ADD(u64, reserved, UINT64CONST(1) << 16, 321, 5), 1, 321, 5);
}

/* chunks_packs_shared_bound returns the bound no pack writer's number passes, of a record's Reserved. */
uint64
chunks_packs_shared_bound(uint64 reserved)
{
	return STD_ADD(u64, chunks_packs_shared_given(reserved), chunks_packs_shared_headroom(reserved), 311, 5);
}

/* chunks_packs_shared_reserved returns a record's Reserved of a bound and the highest number given. */
uint64
chunks_packs_shared_reserved(uint64 bound, uint64 given)
{
	return STD_SHL(u64, given, 16, 317, 5) | STD_SUB(u64, bound, given, 317, 18);
}

/* chunks_packs_shared_set stores a value in an atomic of a pack record that changes only under a lock its caller holds. */
void
chunks_packs_shared_set(HostSharedRecord record, size_t atomic, uint64 value)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

	exchange.u.exchange.f0 = host_shared_Record_atomic(record, atomic, load);
	exchange.u.exchange.f1 = value;
	host_shared_Record_atomic(record, atomic, exchange);
}

/* chunks_packs_shared_load loads an atomic of a pack record. */
uint64
chunks_packs_shared_load(HostSharedRecord record, size_t atomic)
{
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(record, atomic, load);
}

/* chunks_packs_shared_add adds to an atomic of a pack record, wrapping, and returns its value before. */
uint64
chunks_packs_shared_add(HostSharedRecord record, size_t atomic, uint64 value)
{
	HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = value};

	return host_shared_Record_atomic(record, atomic, add);
}

/* chunks_packs_shared_ListTail_write appends the tail as the snapshot holds it. */
static void
chunks_packs_shared_ListTail_write(const ChunksPacksSharedListTail *self, StdVec *out)
{
	uint8		bytes[CHUNKS_PACKS_TAIL];
	uint8		appended[8];

	chunks_packs_Tail_bytes(&self->tail, bytes);
	std_vec_extend_from_slice_Checked(out, bytes, sizeof(bytes), 1, 1);
	std_u64_to_le_bytes(self->appended, appended);
	std_vec_extend_from_slice_Checked(out, appended, sizeof(appended), 1, 1);
}

/* chunks_packs_shared_ListTail_read returns the tail the snapshot holds in CHUNKS_PACKS_SHARED_LIST_TAIL bytes. */
static ChunksPacksSharedListTail
chunks_packs_shared_ListTail_read(const uint8 *bytes)
{
	ChunksPacksSharedListTail tail;

	tail.tail = chunks_packs_Tail_read(bytes);
	tail.appended = chunks_packs_long(bytes, CHUNKS_PACKS_SHARED_LIST_TAIL, CHUNKS_PACKS_TAIL);
	return tail;
}

/* chunks_packs_shared_Snapshot_Default_default is Snapshot's Default: both tails at zero and nothing deferred. */
void
chunks_packs_shared_Snapshot_Default_default(ChunksPacksSharedSnapshot *result)
{
	memset(result, 0, sizeof(*result));
	std_vec_new(&result->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
}

/* chunks_packs_shared_Snapshot_drop is Snapshot's drop glue: its deferred adds'. */
void
chunks_packs_shared_Snapshot_drop(ChunksPacksSharedSnapshot *self)
{
	std_vec_free_Checked(&self->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
}

/* chunks_packs_shared_Snapshot_bytes returns the snapshot as the record's blob holds it. */
void
chunks_packs_shared_Snapshot_bytes(const ChunksPacksSharedSnapshot *self, StdVec *result)
{
	uint8		count[4];

	std_vec_with_capacity_Checked(result,
								  STD_ADD(usize, CHUNKS_PACKS_SHARED_HEAD,
										  STD_MUL(usize, self->deferred.len, CHUNKS_PACKS_ENTRY, 375, 63), 375, 46),
								  1, 1);
	chunks_packs_shared_ListTail_write(&self->table, result);
	chunks_packs_shared_ListTail_write(&self->forwarding, result);
	std_u32_to_le_bytes((uint32) self->deferred.len, count);
	std_vec_extend_from_slice_Checked(result, count, sizeof(count), 1, 1);
	for (size_t k = 0, k_len = self->deferred.len; k < k_len; k++)
	{
		uint8		entry[CHUNKS_PACKS_ENTRY];

		chunks_packs_entry_bytes(&STD_VEC_AT(&self->deferred, ChunksPacksTableEntry, k), entry);
		std_vec_extend_from_slice_Checked(result, entry, sizeof(entry), 1, 1);
	}
}

/* chunks_packs_shared_Snapshot_read returns the snapshot the record's blob holds, refusing bytes no writer makes. */
bool
chunks_packs_shared_Snapshot_read(const uint8 *bytes, size_t bytes_len, ChunksPacksSharedSnapshot *result,
								  ChunksErrorError *error)
{
	static const char text[] = "a pack record's snapshot is corrupt";
	size_t		count;
	const uint8 *entries;
	size_t		entries_len;

	if (bytes_len < CHUNKS_PACKS_SHARED_HEAD)
	{
		*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
		return false;
	}
	count = (size_t) chunks_packs_word(bytes, CHUNKS_PACKS_SHARED_HEAD, CHUNKS_PACKS_SHARED_HEAD - 4);
	entries = STD_PTR_AT(bytes, CHUNKS_PACKS_SHARED_HEAD, 1);
	entries_len = std_wrapping_sub_usize(bytes_len, CHUNKS_PACKS_SHARED_HEAD);
	if (count > CHUNKS_PACKS_SHARED_DEFERRED + 1 || entries_len != count * CHUNKS_PACKS_ENTRY)
	{
		*error = chunks_error_Error_corrupt_RefStr(text, sizeof(text) - 1);
		return false;
	}
	result->table = chunks_packs_shared_ListTail_read(bytes);
	result->forwarding = chunks_packs_shared_ListTail_read(STD_PTR_AT(bytes, CHUNKS_PACKS_SHARED_LIST_TAIL, 1));
	std_vec_with_capacity_Checked(&result->deferred, count, sizeof(ChunksPacksTableEntry),
								  _Alignof(ChunksPacksTableEntry));
	for (size_t k = 0; k < count; k++)
	{
		ChunksPacksTableEntry entry = chunks_packs_entry_of(STD_PTR_AT(entries, k * CHUNKS_PACKS_ENTRY, 1));

		std_vec_push_Checked(&result->deferred, &entry, sizeof(entry), _Alignof(ChunksPacksTableEntry));
	}
	return true;
}

/* chunks_packs_shared_ListTail_PartialEq_eq is ListTail's derived PartialEq. */
bool
chunks_packs_shared_ListTail_PartialEq_eq(const ChunksPacksSharedListTail *self,
										  const ChunksPacksSharedListTail *other)
{
	return chunks_packs_Tail_PartialEq_eq(&self->tail, &other->tail) && self->appended == other->appended;
}

/* chunks_packs_shared_ListTail_Debug_fmt is ListTail's derived Debug. */
bool
chunks_packs_shared_ListTail_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksSharedListTail *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "ListTail", strlen("ListTail"));

	std_DebugStruct_field(&s, "tail", strlen("tail"), &self->tail, chunks_packs_Tail_Debug_fmt);
	std_DebugStruct_field(&s, "appended", strlen("appended"), &self->appended, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_shared_TableEntry_Debug_fmt is a TableEntry's Debug, a (u64, u32, PackId) tuple's. */
static bool
chunks_packs_shared_TableEntry_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksTableEntry *self = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &self->f0, std_u64_Debug_fmt);
	std_DebugTuple_field(&b, &self->f1, std_u32_Debug_fmt);
	std_DebugTuple_field(&b, &self->f2, chunks_pack_PackId_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_shared_entries_Debug_fmt is the Debug of a Vec<TableEntry>: a list of (u64, u32, PackId) tuples. */
static bool
chunks_packs_shared_entries_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		std_DebugList_entry(&builder, &STD_VEC_AT(self, ChunksPacksTableEntry, k), chunks_packs_shared_TableEntry_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* chunks_packs_shared_Snapshot_Debug_fmt is Snapshot's derived Debug. */
bool
chunks_packs_shared_Snapshot_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksSharedSnapshot *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Snapshot", strlen("Snapshot"));

	std_DebugStruct_field(&s, "table", strlen("table"), &self->table, chunks_packs_shared_ListTail_Debug_fmt);
	std_DebugStruct_field(&s, "forwarding", strlen("forwarding"), &self->forwarding,
						  chunks_packs_shared_ListTail_Debug_fmt);
	std_DebugStruct_field(&s, "deferred", strlen("deferred"), &self->deferred, chunks_packs_shared_entries_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_shared_Snapshot_PartialEq_eq is Snapshot's derived PartialEq. */
bool
chunks_packs_shared_Snapshot_PartialEq_eq(const ChunksPacksSharedSnapshot *self,
										  const ChunksPacksSharedSnapshot *other)
{
	if (!chunks_packs_shared_ListTail_PartialEq_eq(&self->table, &other->table) ||
		!chunks_packs_shared_ListTail_PartialEq_eq(&self->forwarding, &other->forwarding) ||
		self->deferred.len != other->deferred.len)
		return false;
	for (size_t k = 0, k_len = self->deferred.len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *a = &STD_VEC_AT(&self->deferred, ChunksPacksTableEntry, k);
		const ChunksPacksTableEntry *b = &STD_VEC_AT(&other->deferred, ChunksPacksTableEntry, k);

		if (a->f0 != b->f0 || a->f1 != b->f1 || !chunks_pack_PackId_PartialEq_eq(&a->f2, &b->f2))
			return false;
	}
	return true;
}

/*
 * chunks_packs_shared_raised returns the error a reclaim's make raises for a
 * failure of chunks: a host's as it is, damaged data as Postgres' corrupted
 * data, and anything else as an internal error. It takes the error.
 */
HostError
chunks_packs_shared_raised(ChunksErrorError *error)
{
	HostError	raised;
	StdString	message;

	if (error->tag == CHUNKS_ERROR_ERROR_HOST)
		return error->u.host;
	std_to_string(error, chunks_error_Error_Display_fmt, &message);
	raised = host_Error_fork((const uint8 *) (error->tag == CHUNKS_ERROR_ERROR_CORRUPT ||
											  error->tag == CHUNKS_ERROR_ERROR_DAMAGED ? "XX001" : "XX000"),
							 &message);
	chunks_error_Error_drop(error);
	return raised;
}

/* chunks_packs_shared_reclaimed is RecordReclaimed's event. */
static void
chunks_packs_shared_reclaimed(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_RECORD_RECLAIMED;
}

/* chunks_packs_shared_freed frees a record a reclaim is looking at if no process opened it since, taking the look. */
static bool
chunks_packs_shared_freed(HostSharedReclaim *look)
{
	uint64		repository = chunks_packs_shared_load(host_shared_Reclaim_record(look),
													  CHUNKS_PACKS_SHARED_REPOSITORY_INDEX);
	bool		freed = host_shared_Reclaim_free(look);

	if (freed)
		chunks_trace_emit(repository, chunks_packs_shared_reclaimed, NULL, NULL);
	return freed;
}

/*
 * chunks_packs_shared_defers reports whether a record a reclaim is looking
 * at owes the repository's files a write: its bound is ahead of the highest
 * number given, its manifest's generations may differ, or its snapshot holds
 * a deferred add.
 */
static bool
chunks_packs_shared_defers(const HostSharedReclaim *look, HostIoIo *io, bool *result, HostError *error)
{
	HostSharedRecord record = host_shared_Reclaim_record(look);
	uint64		numbers = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	uint64		differs = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_DIFFERS_INDEX);
	HostSharedGuard state;
	StdVec		bytes;
	ChunksPacksSharedSnapshot snapshot;
	ChunksErrorError failed;

	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX,
										&state, error))
		return false;
	bytes = host_shared_Guard_read_blob(&state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB);
	host_shared_Guard_drop(&state);
	if (chunks_packs_shared_bound(numbers) != chunks_packs_shared_given(numbers) || differs != 0)
	{
		std_vec_free_Checked(&bytes, 1, 1);
		*result = true;
		return true;
	}
	if (!chunks_packs_shared_Snapshot_read(bytes.ptr, bytes.len, &snapshot, &failed))
	{
		std_vec_free_Checked(&bytes, 1, 1);
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	std_vec_free_Checked(&bytes, 1, 1);
	*result = snapshot.deferred.len != 0;
	chunks_packs_shared_Snapshot_drop(&snapshot);
	return true;
}

/*
 * chunks_packs_shared_PackState_reclaim_frame_0_t is the reclaim's state
 * across its pad: the sweep's pairs, the locations that defer, the look it
 * holds, and the packs and reader of a record it writes for.
 */
typedef struct chunks_packs_shared_PackState_reclaim_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	HostError  *error;
	bool		ok;
	StdVec		swept;
	bool		swept_live;
	StdVec		deferring;
	bool		deferring_live;
	HostSharedLook look;
	bool		look_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} chunks_packs_shared_PackState_reclaim_frame_0_t;

/* chunks_packs_shared_PackState_reclaim_pad_0 is the reclaim's pad: what it holds, newest first. */
static void
chunks_packs_shared_PackState_reclaim_pad_0(void *frame_)
{
	chunks_packs_shared_PackState_reclaim_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->look_live)
	{
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
	}
	if (frame->deferring_live)
	{
		frame->deferring_live = false;
		std_vec_free_Checked(&frame->deferring, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* chunks_packs_shared_PackState_reclaim_look takes the reclaim's look at a location. */
static bool
chunks_packs_shared_PackState_reclaim_look(chunks_packs_shared_PackState_reclaim_frame_0_t *frame, uint64 location)
{
	if (!host_shared_reclaim(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location,
							 &frame->look, frame->error))
		return false;
	frame->look_live = true;
	return true;
}

/* chunks_packs_shared_PackState_reclaim_written writes what a held record owes its files, as one of its holders. */
static bool
chunks_packs_shared_PackState_reclaim_written(chunks_packs_shared_PackState_reclaim_frame_0_t *frame, uint64 location)
{
	bool		has_held;
	HostSharedHeld held;
	ChunksErrorError failed;
	ChunksPacksBackendPacks packs;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location,
						  &has_held, &held, frame->error))
		return false;
	if (!has_held)
		return true;
	if (!chunks_packs_backend_Packs_over(frame->io, &held, location, &packs, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		return false;
	}
	frame->packs = packs;
	frame->packs_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		return false;
	frame->reading_live = true;
	if (!chunks_packs_backend_Packs_unreserve(&frame->packs, frame->io, &frame->reading, &failed) ||
		!chunks_packs_backend_Packs_even_manifest(&frame->packs, frame->io, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		return false;
	}
	frame->packs_live = false;
	if (!chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		return false;
	}
	frame->reading_live = false;
	host_shared_Reader_drop(&frame->reading);
	return true;
}

/*
 * chunks_packs_shared_PackState_reclaim_body_0 is the reclaim's body: a
 * record that owes no write freed first, and then those that do, each
 * written for and looked at again.
 */
static void
chunks_packs_shared_PackState_reclaim_body_0(void *frame_)
{
	chunks_packs_shared_PackState_reclaim_frame_0_t *frame = frame_;
	bool		undecided = false;
	bool		defers;

	frame->ok = false;
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &frame->swept, frame->error))
		goto done;
	frame->swept_live = true;
	std_vec_new(&frame->deferring, sizeof(uint64), _Alignof(uint64));
	frame->deferring_live = true;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		uint64		location = STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k).f0;

		if (!chunks_packs_shared_PackState_reclaim_look(frame, location))
			goto done;
		if (frame->look.tag == HOST_SHARED_LOOK_MINE)
		{
			if (!chunks_packs_shared_defers(&frame->look.u.mine, frame->io, &defers, frame->error))
				goto done;
			if (defers)
				std_vec_push_Checked(&frame->deferring, &location, sizeof(location), _Alignof(uint64));
			else
			{
				if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-chunks-reclaim-looked", frame->error))
					goto done;
				frame->look_live = false;
				if (chunks_packs_shared_freed(&frame->look.u.mine))
				{
					*frame->result = true;
					frame->ok = true;
					goto done;
				}
				continue;
			}
		}
		else if (frame->look.tag == HOST_SHARED_LOOK_THEIRS)
			undecided = true;
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
	}
	for (size_t k = 0, k_len = frame->deferring.len; k < k_len; k++)
	{
		uint64		location = STD_VEC_AT(&frame->deferring, uint64, k);

		if (!chunks_packs_shared_PackState_reclaim_written(frame, location) ||
			!chunks_packs_shared_PackState_reclaim_look(frame, location))
			goto done;
		if (frame->look.tag == HOST_SHARED_LOOK_MINE)
		{
			if (!chunks_packs_shared_defers(&frame->look.u.mine, frame->io, &defers, frame->error))
				goto done;
			if (!defers)
			{
				frame->look_live = false;
				if (chunks_packs_shared_freed(&frame->look.u.mine))
				{
					*frame->result = true;
					frame->ok = true;
					goto done;
				}
				continue;
			}
		}
		else if (frame->look.tag == HOST_SHARED_LOOK_THEIRS)
			undecided = true;
		frame->look_live = false;
		host_shared_Look_drop(&frame->look);
	}
	*frame->result = undecided;
	frame->ok = true;
done:
	chunks_packs_shared_PackState_reclaim_pad_0(frame);
}

/*
 * chunks_packs_shared_PackState_reclaim frees a pack record no process holds
 * that owes the repository's files no write, or else writes for one that does
 * and frees it, answering whether the make should look again: only when it
 * freed a record or met one another reclaim was looking at.
 */
static bool
chunks_packs_shared_PackState_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	chunks_packs_shared_PackState_reclaim_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, chunks_packs_shared_PackState_reclaim_body_0, chunks_packs_shared_PackState_reclaim_pad_0);
	return frame.ok;
}

/* chunks_packs_shared_PackState_count returns three records for each process, the most a backend holds open at once. */
static size_t
chunks_packs_shared_PackState_count(const HostSharedLimits *limits)
{
	return STD_MUL(usize, 3, limits->processes, 49, 9);
}

/* CHUNKS_PACKS_SHARED_PACK_STATE_BLOBS is PackState's blobs: the snapshot. */
static const size_t CHUNKS_PACKS_SHARED_PACK_STATE_BLOBS[] = {CHUNKS_PACKS_SHARED_SNAPSHOT};

/* CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS is PackState's Pool: a repository's pack record. */
const HostSharedPoolOps CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x6368756e6b730001), .locks = 4, .flags = 2, .atomics = 7,
	.blobs = CHUNKS_PACKS_SHARED_PACK_STATE_BLOBS, .blobs_len = lengthof(CHUNKS_PACKS_SHARED_PACK_STATE_BLOBS)},
	.name = {"pack", 4},
	.count = chunks_packs_shared_PackState_count,
	.reclaim = chunks_packs_shared_PackState_reclaim,
};

/*
 * chunks_packs_shared_even_manifests_frame_0_t is even_manifests' state
 * across its pad: the two sweeps' pairs and the packs it evens.
 */
typedef struct chunks_packs_shared_even_manifests_frame_0_t
{
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	StdVec		named;
	bool		named_live;
	StdVec		differing;
	bool		differing_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
} chunks_packs_shared_even_manifests_frame_0_t;

/* chunks_packs_shared_even_manifests_pad_0 is even_manifests' pad: the packs, then the pairs. */
static void
chunks_packs_shared_even_manifests_pad_0(void *frame_)
{
	chunks_packs_shared_even_manifests_frame_0_t *frame = frame_;

	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->differing_live)
	{
		frame->differing_live = false;
		std_vec_free_Checked(&frame->differing, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->named_live)
	{
		frame->named_live = false;
		std_vec_free_Checked(&frame->named, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* chunks_packs_shared_even_manifests_body_0 is even_manifests' body: each marked record's manifest written once more. */
static void
chunks_packs_shared_even_manifests_body_0(void *frame_)
{
	chunks_packs_shared_even_manifests_frame_0_t *frame = frame_;
	HostError	host;

	frame->ok = false;
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &frame->named, &host))
		goto failed;
	frame->named_live = true;
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_DIFFERS_INDEX, &frame->differing, &host))
		goto failed;
	frame->differing_live = true;
	for (size_t k = 0, k_len = frame->differing.len; k < k_len; k++)
	{
		std_TupleU64U64_t pair = STD_VEC_AT(&frame->differing, std_TupleU64U64_t, k);
		bool		has_held;
		HostSharedHeld held;
		ChunksErrorError failed;
		bool		evened;

		if (pair.f1 == 0)
			continue;
		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, pair.f0,
							  &has_held, &held, &host))
			goto failed;
		if (!has_held)
			continue;
		evened = chunks_packs_backend_Packs_over(frame->io, &held, pair.f0, &frame->packs, &failed);
		if (evened)
		{
			frame->packs_live = true;
			evened = chunks_packs_backend_Packs_even_manifest(&frame->packs, frame->io, &failed);
			frame->packs_live = false;
			chunks_packs_backend_Packs_drop(&frame->packs);
		}
		if (!evened)
		{
			uint64		repository = 0;
			char		context[96];
			HostError	raised;

			for (size_t n = 0, n_len = frame->named.len; n < n_len; n++)
				if (STD_VEC_AT(&frame->named, std_TupleU64U64_t, n).f0 == pair.f0)
				{
					repository = STD_VEC_AT(&frame->named, std_TupleU64U64_t, n).f1;
					break;
				}
			raised = chunks_packs_shared_raised(&failed);
			snprintf(context, sizeof(context), "could not write the manifest of database " UINT64_FORMAT " once more",
					 repository);
			if (!host_warn(HOST_IO_IO_IO_DOORS(frame->io), &raised, context, &host))
				goto failed;
		}
	}
	frame->ok = true;
	chunks_packs_shared_even_manifests_pad_0(frame);
	return;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
	chunks_packs_shared_even_manifests_pad_0(frame);
}

/*
 * chunks_packs_shared_even_manifests writes the newest manifest once more
 * for each repository whose pack record says its generations may differ,
 * reporting at WARNING, by its database, one whose manifest it can't write,
 * and leaving it marked, so one repository's damage never stops the
 * checkpoint.
 */
bool
chunks_packs_shared_even_manifests(HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_shared_even_manifests_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, chunks_packs_shared_even_manifests_body_0, chunks_packs_shared_even_manifests_pad_0);
	return frame.ok;
}

/* chunks_packs_shared_marked reports whether a repository's pack record says its manifest's generations may differ. */
bool
chunks_packs_shared_marked(HostIoIo *io, bool *result, ChunksErrorError *error)
{
	StdVec		differing;
	HostError	host;

	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_DIFFERS_INDEX, &differing, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	*result = false;
	for (size_t k = 0, k_len = differing.len; k < k_len; k++)
		if (STD_VEC_AT(&differing, std_TupleU64U64_t, k).f1 != 0)
		{
			*result = true;
			break;
		}
	std_vec_free_Checked(&differing, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	return true;
}

/* CHUNKS_PACKS_BARRIER_HOLDING is this process's exclusive hold of a repository's barrier, a capability with no value. */
ChunksPacksBarrierHolding CHUNKS_PACKS_BARRIER_HOLDING;

/* chunks_packs_barrier_held marks the barrier the caller holds exclusively as held, for the analysis alone. */
void
chunks_packs_barrier_held(void)
{
}

/* chunks_packs_barrier_left marks the barrier the caller is about to give back as no longer held. */
void
chunks_packs_barrier_left(void)
{
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_shared_tests_entry returns the table's entry of a pack numbered `number`. */
static ChunksPacksTableEntry
chunks_packs_shared_tests_entry(uint64 number)
{
	ChunksPacksTableEntry entry = {number, (uint32) number % 3};

	memset(entry.f2.f0, (uint8) number, sizeof(entry.f2.f0));
	return entry;
}

/* chunks_packs_shared_tests_entries returns the table's entries of the packs numbered `from` up to `to`, not `to`. */
static StdVec
chunks_packs_shared_tests_entries(uint64 from, uint64 to)
{
	StdVec		entries;

	std_vec_new(&entries, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	for (uint64 number = from; number < to; number++)
	{
		ChunksPacksTableEntry entry = chunks_packs_shared_tests_entry(number);

		std_vec_push_Checked(&entries, &entry, sizeof(entry), _Alignof(ChunksPacksTableEntry));
	}
	return entries;
}

/* chunks_packs_shared_tests_read_back reads a snapshot back from its bytes, which unwrap at a line and column. */
static void
chunks_packs_shared_tests_read_back(const StdVec *bytes, uint32 line, uint32 col, ChunksPacksSharedSnapshot *result)
{
	ChunksErrorError error;

	if (!chunks_packs_shared_Snapshot_read(bytes->ptr, bytes->len, result, &error))
		std_result_unwrap_failed("called `Result::unwrap()` on an `Err` value",
								 sizeof("called `Result::unwrap()` on an `Err` value") - 1, &error,
								 chunks_error_Error_Debug_fmt, RUST_FILE, line, col);
}

/*
 * test_chunks_packs_shared_tests_a_snapshot_read_back_is_the_snapshot_written_up_to_its_largest_size:
 * a snapshot read from its bytes is the snapshot written, with no deferred
 * add and with every one it can hold, which fills the blob's largest size
 * exactly.
 */
void
test_chunks_packs_shared_tests_a_snapshot_read_back_is_the_snapshot_written_up_to_its_largest_size(void)
{
	ChunksPacksSharedSnapshot empty = {{{40, 3}, 0}, {{8, 1}, 5}};
	ChunksPacksSharedSnapshot full;
	ChunksPacksSharedSnapshot got;
	StdVec		bytes;
	size_t		largest = CHUNKS_PACKS_SHARED_SNAPSHOT;
	size_t		sized = 1742;

	std_vec_new(&empty.deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_shared_Snapshot_bytes(&empty, &bytes);
	chunks_packs_shared_tests_read_back(&bytes, 409, 51, &got);
	std_vec_free_Checked(&bytes, 1, 1);
	STD_ASSERT_EQ(409, 9, chunks_packs_shared_Snapshot_PartialEq_eq(&got, &empty), &got,
				  chunks_packs_shared_Snapshot_Debug_fmt, &empty, chunks_packs_shared_Snapshot_Debug_fmt);
	chunks_packs_shared_Snapshot_drop(&got);
	full = empty;
	full.deferred = chunks_packs_shared_tests_entries(1, CHUNKS_PACKS_SHARED_DEFERRED + 2);
	chunks_packs_shared_Snapshot_bytes(&full, &bytes);
	STD_ASSERT_EQ(412, 9, bytes.len == largest, &bytes.len, std_usize_Debug_fmt, &largest, std_usize_Debug_fmt);
	STD_ASSERT_EQ(413, 9, largest == sized, &largest, std_usize_Debug_fmt, &sized, std_usize_Debug_fmt);
	chunks_packs_shared_tests_read_back(&bytes, 414, 43, &got);
	std_vec_free_Checked(&bytes, 1, 1);
	STD_ASSERT_EQ(414, 9, chunks_packs_shared_Snapshot_PartialEq_eq(&got, &full), &got,
				  chunks_packs_shared_Snapshot_Debug_fmt, &full, chunks_packs_shared_Snapshot_Debug_fmt);
	chunks_packs_shared_Snapshot_drop(&got);
	chunks_packs_shared_Snapshot_drop(&full);
}

/*
 * test_chunks_packs_shared_tests_bytes_that_are_no_snapshot_are_refused:
 * bytes that are no snapshot are refused as corrupt: an empty blob, one cut
 * short, one with bytes left over, and one that counts more deferred adds
 * than a snapshot holds.
 */
void
test_chunks_packs_shared_tests_bytes_that_are_no_snapshot_are_refused(void)
{
	ChunksPacksSharedSnapshot snapshot;
	StdVec		whole;
	StdVec		over;
	StdVec		cases[4];

	chunks_packs_shared_Snapshot_Default_default(&snapshot);
	snapshot.deferred = chunks_packs_shared_tests_entries(1, 3);
	chunks_packs_shared_Snapshot_bytes(&snapshot, &whole);
	chunks_packs_shared_Snapshot_drop(&snapshot);
	chunks_packs_shared_Snapshot_Default_default(&snapshot);
	snapshot.deferred = chunks_packs_shared_tests_entries(0, CHUNKS_PACKS_SHARED_DEFERRED + 2);
	chunks_packs_shared_Snapshot_bytes(&snapshot, &over);
	chunks_packs_shared_Snapshot_drop(&snapshot);
	std_vec_new(&cases[0], 1, 1);
	std_range_to_check(whole.len - 1, whole.len, RUST_FILE, 423, 33);
	std_slice_to_vec_Checked(&cases[1], whole.ptr, whole.len - 1, 1, 1);
	std_slice_to_vec_Checked(&cases[2], whole.ptr, whole.len, 1, 1);
	std_vec_u8_push_Checked(&cases[2], 0);
	cases[3] = over;
	for (size_t k = 0; k < lengthof(cases); k++)
	{
		ChunksPacksSharedSnapshot read;
		ChunksErrorError error;
		bool		corrupt;

		if (chunks_packs_shared_Snapshot_read(cases[k].ptr, cases[k].len, &read, &error))
		{
			chunks_packs_shared_Snapshot_drop(&read);
			corrupt = false;
		}
		else
		{
			corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
			chunks_error_Error_drop(&error);
		}
		if (!corrupt)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" bytes")};
			StdFmtArg	args[] = {{&cases[k].len, std_usize_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 424, 13, &arguments);
		}
		std_vec_free_Checked(&cases[k], 1, 1);
	}
	std_vec_free_Checked(&whole, 1, 1);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
