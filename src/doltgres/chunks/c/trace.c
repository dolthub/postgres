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
 * The storage protocol's transitions, recorded in order for each repository
 * under the trace feature, which the verification plan's trace check
 * replays through the protocol model; each event is emitted inside the lock
 * or at the ordering point that makes its transition visible.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "trace.rs"

/* trace_string_eq is whether two strings hold the same bytes. */
static bool
trace_string_eq(const StdString *a, const StdString *b)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0);
}

/* trace_locator_eq is Locator's equality over pointers. */
static bool
trace_locator_eq(const void *a, const void *b)
{
	return chunks_trailer_Locator_PartialEq_eq(a, b);
}

/* trace_pack_id_eq is PackId's equality over pointers. */
static bool
trace_pack_id_eq(const void *a, const void *b)
{
	return chunks_pack_PackId_PartialEq_eq(a, b);
}

/* trace_u64_pack_id_eq is (u64, PackId)'s equality over pointers. */
static bool
trace_u64_pack_id_eq(const void *a, const void *b)
{
	const chunks_TupleU64PackId_t *x = a;
	const chunks_TupleU64PackId_t *y = b;

	return x->f0 == y->f0 && chunks_pack_PackId_PartialEq_eq(&x->f1, &y->f1);
}

/* trace_locator_pair_eq is (Locator, Locator)'s equality over pointers. */
static bool
trace_locator_pair_eq(const void *a, const void *b)
{
	const chunks_TupleLocatorLocator_t *x = a;
	const chunks_TupleLocatorLocator_t *y = b;

	return chunks_trailer_Locator_PartialEq_eq(&x->f0, &y->f0) &&
		chunks_trailer_Locator_PartialEq_eq(&x->f1, &y->f1);
}

/* trace_u64_eq is u64's equality over pointers. */
static bool
trace_u64_eq(const void *a, const void *b)
{
	return *(const uint64 *) a == *(const uint64 *) b;
}

/* trace_u32_eq is u32's equality over pointers. */
static bool
trace_u32_eq(const void *a, const void *b)
{
	return *(const uint32 *) a == *(const uint32 *) b;
}

/* trace_u64_pair_eq is (u64, u64)'s equality over pointers. */
static bool
trace_u64_pair_eq(const void *a, const void *b)
{
	const std_TupleU64U64_t *x = a;
	const std_TupleU64U64_t *y = b;

	return x->f0 == y->f0 && x->f1 == y->f1;
}

/* chunks_trace_Source_PartialEq_eq is Source's derived equality: the same variant with the same number. */
bool
chunks_trace_Source_PartialEq_eq(const ChunksTraceSource *self, const ChunksTraceSource *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case CHUNKS_TRACE_SOURCE_WRITER:
			return self->u.writer == other->u.writer;
		case CHUNKS_TRACE_SOURCE_SNAPSHOT:
			return self->u.snapshot == other->u.snapshot;
		case CHUNKS_TRACE_SOURCE_CACHED:
			return self->u.cached == other->u.cached;
	}
	pg_unreachable();
}

/* chunks_trace_Event_PartialEq_eq is Event's derived equality: the same variant with equal fields. */
bool
chunks_trace_Event_PartialEq_eq(const ChunksTraceEvent *self, const ChunksTraceEvent *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case CHUNKS_TRACE_EVENT_WRITER_OPENED:
			return self->u.writer_opened.writer == other->u.writer_opened.writer &&
				self->u.writer_opened.pack == other->u.writer_opened.pack;
		case CHUNKS_TRACE_EVENT_PACK_OPENED:
			return self->u.pack_opened.pack == other->u.pack_opened.pack;
		case CHUNKS_TRACE_EVENT_CHUNK_WRITTEN:
			return chunks_address_Address_PartialEq_eq(&self->u.chunk_written.address,
													   &other->u.chunk_written.address) &&
				chunks_trailer_Locator_PartialEq_eq(&self->u.chunk_written.locator,
													&other->u.chunk_written.locator) &&
				self->u.chunk_written.has_names == other->u.chunk_written.has_names &&
				(!self->u.chunk_written.has_names ||
				 std_vec_PartialEq_eq(&self->u.chunk_written.names, &other->u.chunk_written.names,
									  sizeof(ChunksTrailerLocator), trace_locator_eq));
		case CHUNKS_TRACE_EVENT_PACK_SYNCED:
			return self->u.pack_synced.pack == other->u.pack_synced.pack;
		case CHUNKS_TRACE_EVENT_PACK_ADDED:
			return self->u.pack_added.pack == other->u.pack_added.pack;
		case CHUNKS_TRACE_EVENT_PACK_ABANDONED:
			return self->u.pack_abandoned.pack == other->u.pack_abandoned.pack;
		case CHUNKS_TRACE_EVENT_REGISTERED:
			return self->u.registered.writer == other->u.registered.writer &&
				chunks_trailer_Locator_PartialEq_eq(&self->u.registered.root, &other->u.registered.root);
		case CHUNKS_TRACE_EVENT_RELEASED:
			return self->u.released.writer == other->u.released.writer;
		case CHUNKS_TRACE_EVENT_REF_STORED:
			return trace_string_eq(&self->u.ref_stored.name, &other->u.ref_stored.name) &&
				chunks_trailer_Locator_PartialEq_eq(&self->u.ref_stored.root, &other->u.ref_stored.root) &&
				chunks_trace_Source_PartialEq_eq(&self->u.ref_stored.from, &other->u.ref_stored.from);
		case CHUNKS_TRACE_EVENT_REF_CLEARED:
			return trace_string_eq(&self->u.ref_cleared.name, &other->u.ref_cleared.name);
		case CHUNKS_TRACE_EVENT_SNAPSHOT_READ:
			return self->u.snapshot_read.csn == other->u.snapshot_read.csn &&
				trace_string_eq(&self->u.snapshot_read.name, &other->u.snapshot_read.name);
		case CHUNKS_TRACE_EVENT_SNAPSHOT_ENDED:
			return self->u.snapshot_ended.csn == other->u.snapshot_ended.csn;
		case CHUNKS_TRACE_EVENT_CACHED:
			return self->u.cached.writer == other->u.cached.writer &&
				self->u.cached.csn == other->u.cached.csn &&
				trace_string_eq(&self->u.cached.name, &other->u.cached.name);
		case CHUNKS_TRACE_EVENT_CAPTURED:
			return self->u.captured.full == other->u.captured.full;
		case CHUNKS_TRACE_EVENT_RETIRED:
			return std_vec_PartialEq_eq(&self->u.retired.packs, &other->u.retired.packs,
										sizeof(chunks_TupleU64PackId_t), trace_u64_pack_id_eq);
		case CHUNKS_TRACE_EVENT_COPIED:
			return self->u.copied.from == other->u.copied.from && self->u.copied.to == other->u.copied.to;
		case CHUNKS_TRACE_EVENT_FORWARDED:
			return std_vec_PartialEq_eq(&self->u.forwarded.moves, &other->u.forwarded.moves,
										sizeof(chunks_TupleLocatorLocator_t), trace_locator_pair_eq);
		case CHUNKS_TRACE_EVENT_SETTLED:
			return std_vec_PartialEq_eq(&self->u.settled.packs, &other->u.settled.packs, sizeof(ChunksPackPackId),
										trace_pack_id_eq);
		case CHUNKS_TRACE_EVENT_RESTORED:
			return self->u.restored.from == other->u.restored.from && self->u.restored.to == other->u.restored.to;
		case CHUNKS_TRACE_EVENT_RETIRED_QUEUED:
			return std_vec_PartialEq_eq(&self->u.retired_queued.packs, &other->u.retired_queued.packs, sizeof(uint64),
										trace_u64_eq);
		case CHUNKS_TRACE_EVENT_RELISTED:
			return self->u.relisted.pack == other->u.relisted.pack &&
				chunks_pack_PackId_PartialEq_eq(&self->u.relisted.id, &other->u.relisted.id);
		case CHUNKS_TRACE_EVENT_PACK_DELETED:
			return chunks_pack_PackId_PartialEq_eq(&self->u.pack_deleted.pack, &other->u.pack_deleted.pack);
		case CHUNKS_TRACE_EVENT_FORWARDS_DROPPED:
			return std_vec_PartialEq_eq(&self->u.forwards_dropped.entries, &other->u.forwards_dropped.entries,
										sizeof(std_TupleU64U64_t), trace_u64_pair_eq);
		case CHUNKS_TRACE_EVENT_LIST_RECORDED:
			return self->u.list_recorded.pack == other->u.list_recorded.pack;
		case CHUNKS_TRACE_EVENT_PACKS_SETTLED:
			return self->u.packs_settled.removed == other->u.packs_settled.removed &&
				std_vec_PartialEq_eq(&self->u.packs_settled.moved, &other->u.packs_settled.moved, sizeof(uint32),
									 trace_u32_eq) &&
				self->u.packs_settled.mended == other->u.packs_settled.mended &&
				self->u.packs_settled.rebuilt == other->u.packs_settled.rebuilt &&
				std_vec_PartialEq_eq(&self->u.packs_settled.unlisted, &other->u.packs_settled.unlisted, sizeof(uint64),
									 trace_u64_eq);
		case CHUNKS_TRACE_EVENT_WRITER_STORED:
			return self->u.writer_stored.lowest == other->u.writer_stored.lowest;
		case CHUNKS_TRACE_EVENT_ROOT_REGISTERED:
			return self->u.root_registered.slot == other->u.root_registered.slot &&
				chunks_trailer_Locator_PartialEq_eq(&self->u.root_registered.root, &other->u.root_registered.root);
		case CHUNKS_TRACE_EVENT_ROOT_RELEASED:
			return self->u.root_released.slot == other->u.root_released.slot;
		case CHUNKS_TRACE_EVENT_MARKING_BEGAN:
			return self->u.marking_began.newest == other->u.marking_began.newest;
		case CHUNKS_TRACE_EVENT_HIT_RECORDED:
			return chunks_address_Address_PartialEq_eq(&self->u.hit_recorded.address,
													   &other->u.hit_recorded.address) &&
				chunks_trailer_Locator_PartialEq_eq(&self->u.hit_recorded.locator, &other->u.hit_recorded.locator);
		case CHUNKS_TRACE_EVENT_VECTOR_NAMED:
			return self->u.vector_named.pack == other->u.vector_named.pack &&
				self->u.vector_named.generation == other->u.vector_named.generation;
		case CHUNKS_TRACE_EVENT_RESERVED:
			return self->u.reserved.entries == other->u.reserved.entries;
		case CHUNKS_TRACE_EVENT_UNRESERVED:
			return self->u.unreserved.entries == other->u.unreserved.entries;
		case CHUNKS_TRACE_EVENT_FILE_QUEUED:
			return chunks_packs_single_Queued_PartialEq_eq(&self->u.file_queued.file, &other->u.file_queued.file) &&
				chunks_pack_PackId_PartialEq_eq(&self->u.file_queued.id, &other->u.file_queued.id) &&
				self->u.file_queued.epoch == other->u.file_queued.epoch;
		case CHUNKS_TRACE_EVENT_FILE_REMOVED:
			return chunks_packs_single_Queued_PartialEq_eq(&self->u.file_removed.file, &other->u.file_removed.file) &&
				chunks_pack_PackId_PartialEq_eq(&self->u.file_removed.id, &other->u.file_removed.id);
		case CHUNKS_TRACE_EVENT_RELOCATED:
		case CHUNKS_TRACE_EVENT_COLLECTION_ENDED:
		case CHUNKS_TRACE_EVENT_BACKUP_STARTED:
		case CHUNKS_TRACE_EVENT_BACKUP_ENDED:
		case CHUNKS_TRACE_EVENT_RECORD_MADE:
		case CHUNKS_TRACE_EVENT_RECORD_RECLAIMED:
		case CHUNKS_TRACE_EVENT_RECORD_DROPPED:
		case CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED:
		case CHUNKS_TRACE_EVENT_WRITER_CLEARED:
		case CHUNKS_TRACE_EVENT_MARKING_ENDED:
		case CHUNKS_TRACE_EVENT_MARKING_RETIRED:
		case CHUNKS_TRACE_EVENT_MARKING_CLEARED:
		case CHUNKS_TRACE_EVENT_CHECKPOINT_BEGAN:
		case CHUNKS_TRACE_EVENT_LISTS_COMPACTED:
		case CHUNKS_TRACE_EVENT_CHECKPOINT_ENDED:
		case CHUNKS_TRACE_EVENT_CRASHED:
			return true;
	}
	pg_unreachable();
}

/* trace_list_fmt writes a vector's Debug: its elements of the size, each by its Debug, as a list. */
static bool
trace_list_fmt(const StdVec *v, size_t size, StdFmtFn fmt, StdFormatter *f)
{
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t i = 0; i < v->len; i++)
		std_DebugList_entry(&list, STD_VEC_AT_SIZE(v, i, size), fmt);
	return std_DebugList_finish(&list);
}

/* trace_locators_fmt is Vec<Locator>'s Debug. */
static bool
trace_locators_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(ChunksTrailerLocator), chunks_trailer_Locator_Debug_fmt, f);
}

/* trace_names_fmt is Option<Vec<Locator>>'s Debug, of a chunk written's names. */
static bool
trace_names_fmt(const void *value, StdFormatter *f)
{
	const ChunksTraceEvent *event = value;
	StdOptionRef option = {event->u.chunk_written.has_names, {&event->u.chunk_written.names, trace_locators_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* trace_u64_pack_id_fmt is (u64, PackId)'s Debug. */
static bool
trace_u64_pack_id_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleU64PackId_t *t = value;
	StdTupleRef tuple = {2, {{&t->f0, std_u64_Debug_fmt}, {&t->f1, chunks_pack_PackId_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* trace_u64_pack_ids_fmt is Vec<(u64, PackId)>'s Debug. */
static bool
trace_u64_pack_ids_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(chunks_TupleU64PackId_t), trace_u64_pack_id_fmt, f);
}

/* trace_locator_pair_fmt is (Locator, Locator)'s Debug. */
static bool
trace_locator_pair_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleLocatorLocator_t *t = value;
	StdTupleRef tuple = {2, {{&t->f0, chunks_trailer_Locator_Debug_fmt}, {&t->f1, chunks_trailer_Locator_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* trace_locator_pairs_fmt is Vec<(Locator, Locator)>'s Debug. */
static bool
trace_locator_pairs_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(chunks_TupleLocatorLocator_t), trace_locator_pair_fmt, f);
}

/* trace_pack_ids_fmt is Vec<PackId>'s Debug. */
static bool
trace_pack_ids_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(ChunksPackPackId), chunks_pack_PackId_Debug_fmt, f);
}

/* trace_u64s_fmt is Vec<u64>'s Debug. */
static bool
trace_u64s_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(uint64), std_u64_Debug_fmt, f);
}

/* trace_u32s_fmt is Vec<u32>'s Debug. */
static bool
trace_u32s_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(uint32), std_u32_Debug_fmt, f);
}

/* trace_u64_pair_fmt is (u64, u64)'s Debug. */
static bool
trace_u64_pair_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *t = value;
	StdTupleRef tuple = {2, {{&t->f0, std_u64_Debug_fmt}, {&t->f1, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* trace_u64_pairs_fmt is Vec<(u64, u64)>'s Debug. */
static bool
trace_u64_pairs_fmt(const void *value, StdFormatter *f)
{
	return trace_list_fmt(value, sizeof(std_TupleU64U64_t), trace_u64_pair_fmt, f);
}

/* chunks_trace_Source_Debug_fmt is Source's derived Debug: the variant's name and its number as a tuple. */
bool
chunks_trace_Source_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksTraceSource *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case CHUNKS_TRACE_SOURCE_WRITER:
			t = std_fmt_debug_tuple(f, "Writer", 6);
			std_DebugTuple_field(&t, &self->u.writer, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_SOURCE_SNAPSHOT:
			t = std_fmt_debug_tuple(f, "Snapshot", 8);
			std_DebugTuple_field(&t, &self->u.snapshot, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_SOURCE_CACHED:
			t = std_fmt_debug_tuple(f, "Cached", 6);
			std_DebugTuple_field(&t, &self->u.cached, std_u64_Debug_fmt);
			break;
		default:
			pg_unreachable();
	}
	return std_DebugTuple_finish(&t);
}

/* TRACE_STRUCT starts a variant's Debug as a struct of the name. */
#define TRACE_STRUCT(name) (s = std_fmt_debug_struct(f, name, sizeof(name) - 1))

/* TRACE_FIELD adds a field of the name, by its Debug, to the variant's Debug. */
#define TRACE_FIELD(name, value, fmt) std_DebugStruct_field(&s, name, sizeof(name) - 1, value, fmt)

/* TRACE_UNIT writes a fieldless variant's Debug, its name. */
#define TRACE_UNIT(name) return std_fmt_write_str(f, name, sizeof(name) - 1)

/* chunks_trace_Event_Debug_fmt is Event's derived Debug: a fieldless variant's name, or the variant as a struct. */
bool
chunks_trace_Event_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksTraceEvent *self = value;
	StdDebugStruct s;

	switch (self->tag)
	{
		case CHUNKS_TRACE_EVENT_WRITER_OPENED:
			TRACE_STRUCT("WriterOpened");
			TRACE_FIELD("writer", &self->u.writer_opened.writer, std_u64_Debug_fmt);
			TRACE_FIELD("pack", &self->u.writer_opened.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_PACK_OPENED:
			TRACE_STRUCT("PackOpened");
			TRACE_FIELD("pack", &self->u.pack_opened.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_CHUNK_WRITTEN:
			TRACE_STRUCT("ChunkWritten");
			TRACE_FIELD("address", &self->u.chunk_written.address, chunks_address_Address_Debug_fmt);
			TRACE_FIELD("locator", &self->u.chunk_written.locator, chunks_trailer_Locator_Debug_fmt);
			TRACE_FIELD("names", self, trace_names_fmt);
			break;
		case CHUNKS_TRACE_EVENT_PACK_SYNCED:
			TRACE_STRUCT("PackSynced");
			TRACE_FIELD("pack", &self->u.pack_synced.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_PACK_ADDED:
			TRACE_STRUCT("PackAdded");
			TRACE_FIELD("pack", &self->u.pack_added.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_PACK_ABANDONED:
			TRACE_STRUCT("PackAbandoned");
			TRACE_FIELD("pack", &self->u.pack_abandoned.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_REGISTERED:
			TRACE_STRUCT("Registered");
			TRACE_FIELD("writer", &self->u.registered.writer, std_u64_Debug_fmt);
			TRACE_FIELD("root", &self->u.registered.root, chunks_trailer_Locator_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RELEASED:
			TRACE_STRUCT("Released");
			TRACE_FIELD("writer", &self->u.released.writer, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_REF_STORED:
			TRACE_STRUCT("RefStored");
			TRACE_FIELD("name", &self->u.ref_stored.name, std_String_Debug_fmt);
			TRACE_FIELD("root", &self->u.ref_stored.root, chunks_trailer_Locator_Debug_fmt);
			TRACE_FIELD("from", &self->u.ref_stored.from, chunks_trace_Source_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_REF_CLEARED:
			TRACE_STRUCT("RefCleared");
			TRACE_FIELD("name", &self->u.ref_cleared.name, std_String_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_SNAPSHOT_READ:
			TRACE_STRUCT("SnapshotRead");
			TRACE_FIELD("csn", &self->u.snapshot_read.csn, std_u64_Debug_fmt);
			TRACE_FIELD("name", &self->u.snapshot_read.name, std_String_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_SNAPSHOT_ENDED:
			TRACE_STRUCT("SnapshotEnded");
			TRACE_FIELD("csn", &self->u.snapshot_ended.csn, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_CACHED:
			TRACE_STRUCT("Cached");
			TRACE_FIELD("writer", &self->u.cached.writer, std_u64_Debug_fmt);
			TRACE_FIELD("csn", &self->u.cached.csn, std_u64_Debug_fmt);
			TRACE_FIELD("name", &self->u.cached.name, std_String_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RELOCATED:
			TRACE_UNIT("Relocated");
		case CHUNKS_TRACE_EVENT_CAPTURED:
			TRACE_STRUCT("Captured");
			TRACE_FIELD("full", &self->u.captured.full, std_bool_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RETIRED:
			TRACE_STRUCT("Retired");
			TRACE_FIELD("packs", &self->u.retired.packs, trace_u64_pack_ids_fmt);
			break;
		case CHUNKS_TRACE_EVENT_COPIED:
			TRACE_STRUCT("Copied");
			TRACE_FIELD("from", &self->u.copied.from, std_u64_Debug_fmt);
			TRACE_FIELD("to", &self->u.copied.to, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_FORWARDED:
			TRACE_STRUCT("Forwarded");
			TRACE_FIELD("moves", &self->u.forwarded.moves, trace_locator_pairs_fmt);
			break;
		case CHUNKS_TRACE_EVENT_COLLECTION_ENDED:
			TRACE_UNIT("CollectionEnded");
		case CHUNKS_TRACE_EVENT_SETTLED:
			TRACE_STRUCT("Settled");
			TRACE_FIELD("packs", &self->u.settled.packs, trace_pack_ids_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RESTORED:
			TRACE_STRUCT("Restored");
			TRACE_FIELD("from", &self->u.restored.from, std_u64_Debug_fmt);
			TRACE_FIELD("to", &self->u.restored.to, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RETIRED_QUEUED:
			TRACE_STRUCT("RetiredQueued");
			TRACE_FIELD("packs", &self->u.retired_queued.packs, trace_u64s_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RELISTED:
			TRACE_STRUCT("Relisted");
			TRACE_FIELD("pack", &self->u.relisted.pack, std_u64_Debug_fmt);
			TRACE_FIELD("id", &self->u.relisted.id, chunks_pack_PackId_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_PACK_DELETED:
			TRACE_STRUCT("PackDeleted");
			TRACE_FIELD("pack", &self->u.pack_deleted.pack, chunks_pack_PackId_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_FORWARDS_DROPPED:
			TRACE_STRUCT("ForwardsDropped");
			TRACE_FIELD("entries", &self->u.forwards_dropped.entries, trace_u64_pairs_fmt);
			break;
		case CHUNKS_TRACE_EVENT_BACKUP_STARTED:
			TRACE_UNIT("BackupStarted");
		case CHUNKS_TRACE_EVENT_BACKUP_ENDED:
			TRACE_UNIT("BackupEnded");
		case CHUNKS_TRACE_EVENT_LIST_RECORDED:
			TRACE_STRUCT("ListRecorded");
			TRACE_FIELD("pack", &self->u.list_recorded.pack, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RECORD_MADE:
			TRACE_UNIT("RecordMade");
		case CHUNKS_TRACE_EVENT_RECORD_RECLAIMED:
			TRACE_UNIT("RecordReclaimed");
		case CHUNKS_TRACE_EVENT_RECORD_DROPPED:
			TRACE_UNIT("RecordDropped");
		case CHUNKS_TRACE_EVENT_PACKS_SETTLED:
			TRACE_STRUCT("PacksSettled");
			TRACE_FIELD("removed", &self->u.packs_settled.removed, std_usize_Debug_fmt);
			TRACE_FIELD("moved", &self->u.packs_settled.moved, trace_u32s_fmt);
			TRACE_FIELD("mended", &self->u.packs_settled.mended, std_usize_Debug_fmt);
			TRACE_FIELD("rebuilt", &self->u.packs_settled.rebuilt, std_usize_Debug_fmt);
			TRACE_FIELD("unlisted", &self->u.packs_settled.unlisted, trace_u64s_fmt);
			break;
		case CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED:
			TRACE_UNIT("RepositoryRemoved");
		case CHUNKS_TRACE_EVENT_WRITER_STORED:
			TRACE_STRUCT("WriterStored");
			TRACE_FIELD("lowest", &self->u.writer_stored.lowest, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_WRITER_CLEARED:
			TRACE_UNIT("WriterCleared");
		case CHUNKS_TRACE_EVENT_ROOT_REGISTERED:
			TRACE_STRUCT("RootRegistered");
			TRACE_FIELD("slot", &self->u.root_registered.slot, std_usize_Debug_fmt);
			TRACE_FIELD("root", &self->u.root_registered.root, chunks_trailer_Locator_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_ROOT_RELEASED:
			TRACE_STRUCT("RootReleased");
			TRACE_FIELD("slot", &self->u.root_released.slot, std_usize_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_MARKING_BEGAN:
			TRACE_STRUCT("MarkingBegan");
			TRACE_FIELD("newest", &self->u.marking_began.newest, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_MARKING_ENDED:
			TRACE_UNIT("MarkingEnded");
		case CHUNKS_TRACE_EVENT_MARKING_RETIRED:
			TRACE_UNIT("MarkingRetired");
		case CHUNKS_TRACE_EVENT_MARKING_CLEARED:
			TRACE_UNIT("MarkingCleared");
		case CHUNKS_TRACE_EVENT_HIT_RECORDED:
			TRACE_STRUCT("HitRecorded");
			TRACE_FIELD("address", &self->u.hit_recorded.address, chunks_address_Address_Debug_fmt);
			TRACE_FIELD("locator", &self->u.hit_recorded.locator, chunks_trailer_Locator_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_VECTOR_NAMED:
			TRACE_STRUCT("VectorNamed");
			TRACE_FIELD("pack", &self->u.vector_named.pack, std_u64_Debug_fmt);
			TRACE_FIELD("generation", &self->u.vector_named.generation, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_RESERVED:
			TRACE_STRUCT("Reserved");
			TRACE_FIELD("entries", &self->u.reserved.entries, std_usize_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_UNRESERVED:
			TRACE_STRUCT("Unreserved");
			TRACE_FIELD("entries", &self->u.unreserved.entries, std_usize_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_FILE_QUEUED:
			TRACE_STRUCT("FileQueued");
			TRACE_FIELD("file", &self->u.file_queued.file, chunks_packs_single_Queued_Debug_fmt);
			TRACE_FIELD("id", &self->u.file_queued.id, chunks_pack_PackId_Debug_fmt);
			TRACE_FIELD("epoch", &self->u.file_queued.epoch, std_u64_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_FILE_REMOVED:
			TRACE_STRUCT("FileRemoved");
			TRACE_FIELD("file", &self->u.file_removed.file, chunks_packs_single_Queued_Debug_fmt);
			TRACE_FIELD("id", &self->u.file_removed.id, chunks_pack_PackId_Debug_fmt);
			break;
		case CHUNKS_TRACE_EVENT_CHECKPOINT_BEGAN:
			TRACE_UNIT("CheckpointBegan");
		case CHUNKS_TRACE_EVENT_LISTS_COMPACTED:
			TRACE_UNIT("ListsCompacted");
		case CHUNKS_TRACE_EVENT_CHECKPOINT_ENDED:
			TRACE_UNIT("CheckpointEnded");
		case CHUNKS_TRACE_EVENT_CRASHED:
			TRACE_UNIT("Crashed");
		default:
			pg_unreachable();
	}
	return std_DebugStruct_finish(&s);
}

/* chunks_trace_Event_drop frees what an event owns: its names, strings, and lists. */
void
chunks_trace_Event_drop(ChunksTraceEvent *self)
{
	switch (self->tag)
	{
		case CHUNKS_TRACE_EVENT_CHUNK_WRITTEN:
			if (self->u.chunk_written.has_names)
				std_vec_free_Checked(&self->u.chunk_written.names, sizeof(ChunksTrailerLocator),
									 _Alignof(ChunksTrailerLocator));
			break;
		case CHUNKS_TRACE_EVENT_REF_STORED:
			std_String_drop(&self->u.ref_stored.name);
			break;
		case CHUNKS_TRACE_EVENT_REF_CLEARED:
			std_String_drop(&self->u.ref_cleared.name);
			break;
		case CHUNKS_TRACE_EVENT_SNAPSHOT_READ:
			std_String_drop(&self->u.snapshot_read.name);
			break;
		case CHUNKS_TRACE_EVENT_CACHED:
			std_String_drop(&self->u.cached.name);
			break;
		case CHUNKS_TRACE_EVENT_RETIRED:
			std_vec_free_Checked(&self->u.retired.packs, sizeof(chunks_TupleU64PackId_t),
								 _Alignof(chunks_TupleU64PackId_t));
			break;
		case CHUNKS_TRACE_EVENT_FORWARDED:
			std_vec_free_Checked(&self->u.forwarded.moves, sizeof(chunks_TupleLocatorLocator_t),
								 _Alignof(chunks_TupleLocatorLocator_t));
			break;
		case CHUNKS_TRACE_EVENT_SETTLED:
			std_vec_free_Checked(&self->u.settled.packs, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
			break;
		case CHUNKS_TRACE_EVENT_RETIRED_QUEUED:
			std_vec_free_Checked(&self->u.retired_queued.packs, sizeof(uint64), _Alignof(uint64));
			break;
		case CHUNKS_TRACE_EVENT_FORWARDS_DROPPED:
			std_vec_free_Checked(&self->u.forwards_dropped.entries, sizeof(std_TupleU64U64_t),
								 _Alignof(std_TupleU64U64_t));
			break;
		case CHUNKS_TRACE_EVENT_PACKS_SETTLED:
			std_vec_free_Checked(&self->u.packs_settled.moved, sizeof(uint32), _Alignof(uint32));
			std_vec_free_Checked(&self->u.packs_settled.unlisted, sizeof(uint64), _Alignof(uint64));
			break;
		default:
			break;
	}
}

#ifdef DOLTGRES_CHUNKS_TRACE

/* TraceRepository is one repository's events in order, by the repository's id. */
typedef struct TraceRepository
{
	uint64		repo;
	StdVec		events;
} TraceRepository;

/* trace_events is each repository's events, a TraceRepository each, in no order. */
static StdVec trace_events = STD_VEC_NEW;

/* TRACE_LEFT is how many drops' transitions can wait at once. */
#define TRACE_LEFT 1024

/*
 * trace_waiting is the drops' transitions not yet recorded, three words each:
 * the repository's id, the value, and the kind's number, zero once recorded.
 * Plain stores suffice, since the C runs one thread at a time: a backend's,
 * or sim's actors, which hand over only inside a host call.
 */
static uint64 trace_waiting[3 * TRACE_LEFT];

/* trace_leaving is how many transitions drops have left in trace_waiting. */
static size_t trace_leaving;

/* trace_recorded is how many of the transitions drops left were recorded. */
static size_t trace_recorded;

/* trace_repository returns the events of the repository, adding an empty list when it has none. */
static StdVec *
trace_repository(uint64 repo)
{
	TraceRepository added = {repo};

	for (size_t i = 0; i < trace_events.len; i++)
		if (STD_VEC_AT(&trace_events, TraceRepository, i).repo == repo)
			return &STD_VEC_AT(&trace_events, TraceRepository, i).events;
	std_vec_new(&added.events, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent));
	std_vec_push_Checked(&trace_events, &added, sizeof(added), _Alignof(TraceRepository));
	return &STD_VEC_AT(&trace_events, TraceRepository, trace_events.len - 1).events;
}

/* trace_record moves the drops' transitions that wait into the events, in the order the drops left them. */
static void
trace_record(void)
{
	while (trace_recorded < trace_leaving)
	{
		size_t		slot = trace_recorded % TRACE_LEFT * 3;
		ChunksTraceEvent event = {0};

		switch (trace_waiting[slot + 2])
		{
			case 1:
				event.tag = CHUNKS_TRACE_EVENT_WRITER_STORED;
				event.u.writer_stored.lowest = trace_waiting[slot + 1];
				break;
			case 2:
				event.tag = CHUNKS_TRACE_EVENT_WRITER_CLEARED;
				break;
			default:
				event.tag = CHUNKS_TRACE_EVENT_UNRESERVED;
				event.u.unreserved.entries = (size_t) trace_waiting[slot + 1];
				break;
		}
		trace_waiting[slot + 2] = 0;
		std_vec_push_Checked(trace_repository(trace_waiting[slot]), &event, sizeof(event),
							 _Alignof(ChunksTraceEvent));
		trace_recorded++;
	}
}

#endif							/* DOLTGRES_CHUNKS_TRACE */

/*
 * chunks_trace_emit_in_drop leaves a drop's transition of the repository
 * whose packs have the id for the next emit or take to record, with stores
 * alone.
 */
void
chunks_trace_emit_in_drop(uint64 repo, ChunksTraceDropped dropped)
{
#ifdef DOLTGRES_CHUNKS_TRACE
	size_t		at = trace_leaving++;
	size_t		slot = at % TRACE_LEFT * 3;
	uint64		kind;
	uint64		value;

	if (at - trace_recorded >= TRACE_LEFT)
	{
		static const char text[] = "more drops' transitions wait than the trace has room for";

		std_panic(RUST_FILE, 189, 9, text, sizeof(text) - 1);
	}
	switch (dropped.tag)
	{
		case CHUNKS_TRACE_DROPPED_WRITER_STORED:
			kind = 1;
			value = dropped.u.writer_stored;
			break;
		case CHUNKS_TRACE_DROPPED_WRITER_CLEARED:
			kind = 2;
			value = 0;
			break;
		case CHUNKS_TRACE_DROPPED_UNRESERVED:
			kind = 3;
			value = dropped.u.unreserved;
			break;
		default:
			pg_unreachable();
	}
	trace_waiting[slot] = repo;
	trace_waiting[slot + 1] = value;
	trace_waiting[slot + 2] = kind;
#endif
}

#ifdef DOLTGRES_CHUNKS_TRACE

/*
 * TraceEmitFrame is an emit's objects: the builder and the captures it is
 * called with, the captures the frame drops until the builder takes them,
 * and the event made, which it drops until the repository's list holds it.
 */
typedef struct TraceEmitFrame
{
	uint64		repo;
	bool		(*event) (void *captures, ChunksTraceEvent *result, ChunksErrorError *error);
	void	   *event_captures;
	void		(*event_drop) (void *);
	void	   *drop_captures;
	bool		captures_live;
	ChunksTraceEvent made;
	bool		made_live;
	ChunksErrorError *error;
	bool		ok;
} TraceEmitFrame;

/*
 * TraceInfallible is an emit's builder, which can't fail, and its captures,
 * as the captures of the fallible builder the frame calls.
 */
typedef struct TraceInfallible
{
	void		(*event) (void *captures, ChunksTraceEvent *result);
	void	   *captures;
} TraceInfallible;

/*
 * trace_emit_body records the drops' transitions, finds the repository's
 * list, then makes the event and pushes it there, as the Rust's lock, record,
 * and entry come before the event's making.
 */
static void
trace_emit_body(void *frame_)
{
	TraceEmitFrame *frame = frame_;
	StdVec	   *events;

	trace_record();
	events = trace_repository(frame->repo);
	frame->captures_live = false;
	if (!frame->event(frame->event_captures, &frame->made, frame->error))
		return;
	frame->made_live = true;
	std_vec_push_Checked(events, &frame->made, sizeof(frame->made), _Alignof(ChunksTraceEvent));
	frame->made_live = false;
	frame->ok = true;
}

/* trace_emit_pad drops what an unwinding emit still owns: the builder's captures, or the event made. */
static void
trace_emit_pad(void *frame_)
{
	TraceEmitFrame *frame = frame_;

	if (frame->captures_live && frame->event_drop != NULL)
		frame->event_drop(frame->drop_captures);
	if (frame->made_live)
		chunks_trace_Event_drop(&frame->made);
}

/* trace_infallible calls an emit's builder, which can't fail, as a fallible one. */
static bool
trace_infallible(void *captures, ChunksTraceEvent *result, ChunksErrorError *error)
{
	TraceInfallible *infallible = captures;

	infallible->event(infallible->captures, result);
	return true;
}

#endif							/* DOLTGRES_CHUNKS_TRACE */

/*
 * chunks_trace_emit records an event of the repository whose packs have the
 * id, building it only under the trace feature; without it the builder's
 * captures are dropped unused.
 */
void
chunks_trace_emit(uint64 repo, void (*event) (void *captures, ChunksTraceEvent *result),
				  void *event_captures, void (*event_drop) (void *))
{
#ifdef DOLTGRES_CHUNKS_TRACE
	TraceInfallible infallible = {event, event_captures};
	TraceEmitFrame frame = {.repo = repo, .event = trace_infallible, .event_captures = &infallible,
	.event_drop = event_drop, .drop_captures = event_captures, .captures_live = true};

	std_landing(&frame, trace_emit_body, trace_emit_pad);
#else
	if (event_drop != NULL)
		event_drop(event_captures);
#endif
}

#ifdef DOLTGRES_CHUNKS_TRACE

/* chunks_trace_take removes and returns the events of the repository whose packs have the id, in order. */
StdVec
chunks_trace_take(uint64 repo)
{
	StdVec		taken;

	trace_record();
	for (size_t i = 0; i < trace_events.len; i++)
	{
		TraceRepository *at = &STD_VEC_AT(&trace_events, TraceRepository, i);

		if (at->repo == repo)
		{
			taken = at->events;
			*at = STD_VEC_AT(&trace_events, TraceRepository, trace_events.len - 1);
			trace_events.len--;
			return taken;
		}
	}
	std_vec_new(&taken, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent));
	return taken;
}

#endif							/* DOLTGRES_CHUNKS_TRACE */

/*
 * chunks_trace_try_emit is chunks_trace_emit for an event whose making can
 * fail, which returns that failure.
 */
bool
chunks_trace_try_emit(uint64 repo, bool (*event) (void *captures, ChunksTraceEvent *result, ChunksErrorError *error),
					  void *event_captures, void (*event_drop) (void *), ChunksErrorError *error)
{
#ifdef DOLTGRES_CHUNKS_TRACE
	TraceEmitFrame frame = {.repo = repo, .event = event, .event_captures = event_captures,
	.event_drop = event_drop, .drop_captures = event_captures, .captures_live = true, .error = error};

	std_landing(&frame, trace_emit_body, trace_emit_pad);
	return frame.ok;
#else
	if (event_drop != NULL)
		event_drop(event_captures);
	return true;
#endif
}

/* trace_header_addresses counts the addresses a stored chunk's header names, for Stored::split. */
static bool
trace_header_addresses(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
					   ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		read;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &read, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/*
 * chunks_trace_named returns the locators a stored chunk's trailer names, or
 * none for bytes without a trailer.
 */
bool
chunks_trace_named(const uint8 *stored, size_t stored_len, bool *has_result, StdVec *result,
				   ChunksErrorError *error)
{
	ChunksTrailerStored split;
	ChunksErrorError failed;
	ChunksErrorOwn own;
	StdVec		addresses;
	size_t		count;

	if (chunks_trailer_Stored_split(stored, stored_len, trace_header_addresses, NULL, NULL, &split, &failed) &&
		chunks_header_addresses(split.hashed.ptr, split.hashed.len, &addresses, &failed))
	{
		count = addresses.len;
		std_vec_free_Checked(&addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
		std_vec_with_capacity_Checked(result, count, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		for (size_t i = 0; i < count; i++)
		{
			ChunksTrailerLocator locator = chunks_trailer_Stored_locator(&split, i);

			std_vec_push_Checked(result, &locator, sizeof(locator), _Alignof(ChunksTrailerLocator));
		}
		*has_result = true;
		return true;
	}
	if (!chunks_error_Error_own(&failed, &own, error))
		return false;
	chunks_error_Own_drop(&own);
	*has_result = false;
	return true;
}
