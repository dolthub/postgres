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
 * replays through the protocol model (chunks' trace.rs).
 */
#ifndef DOLTGRES_CHUNKS_TRACE_H
#define DOLTGRES_CHUNKS_TRACE_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"

/* ChunksTraceSourceTag is Source's variants: where a root a ref stores came from. */
typedef enum ChunksTraceSourceTag
{
	/* The root a tree writer wrote, by the writer's id. */
	CHUNKS_TRACE_SOURCE_WRITER,
	/* A name a snapshot read, by the snapshot's commit sequence number. */
	CHUNKS_TRACE_SOURCE_SNAPSHOT,

	/*
	 * A name a tree writer read into its own memory under its snapshot, by
	 * the writer's id.
	 */
	CHUNKS_TRACE_SOURCE_CACHED,
} ChunksTraceSourceTag;

/* ChunksTraceSource is Source: where a root a ref stores came from, which picks the model step the store replays as. */
typedef struct ChunksTraceSource
{
	ChunksTraceSourceTag tag;
	union
	{
		uint64		writer;
		uint64		snapshot;
		uint64		cached;
	}			u;
} ChunksTraceSource;

/* ChunksTraceEventTag is Event's variants. */
typedef enum ChunksTraceEventTag
{
	/*
	 * A tree writer began a pack, its first or the next one once the last
	 * filled.
	 */
	CHUNKS_TRACE_EVENT_WRITER_OPENED,
	/* A pack writer started a pack. */
	CHUNKS_TRACE_EVENT_PACK_OPENED,

	/*
	 * A chunk was written into a pack, with the locators its trailer names,
	 * or None for a record copied with its bytes as they were, which names
	 * what its source names, and for bytes without a trailer, which only
	 * chunks' own tests write.
	 */
	CHUNKS_TRACE_EVENT_CHUNK_WRITTEN,
	/* A pack's file and its name were made durable. */
	CHUNKS_TRACE_EVENT_PACK_SYNCED,

	/*
	 * A finished pack was added to the table, by its writer or by recovery
	 * replaying the WAL's add after Crashed.
	 */
	CHUNKS_TRACE_EVENT_PACK_ADDED,
	/* A pack writer's unfinished pack was removed. */
	CHUNKS_TRACE_EVENT_PACK_ABANDONED,
	/* A tree writer registered the root it will publish. */
	CHUNKS_TRACE_EVENT_REGISTERED,
	/* A tree writer's registration was dropped. */
	CHUNKS_TRACE_EVENT_RELEASED,
	/* A ref stored a root durably. */
	CHUNKS_TRACE_EVENT_REF_STORED,
	/* A ref was deleted. */
	CHUNKS_TRACE_EVENT_REF_CLEARED,
	/* A snapshot read a ref's root. */
	CHUNKS_TRACE_EVENT_SNAPSHOT_READ,
	/* A snapshot ended. */
	CHUNKS_TRACE_EVENT_SNAPSHOT_ENDED,
	/* A tree writer read a ref's root into its own memory under its snapshot. */
	CHUNKS_TRACE_EVENT_CACHED,

	/*
	 * A fold rewrote the stored roots and the versions snapshots keep through
	 * the forwards.
	 */
	CHUNKS_TRACE_EVENT_RELOCATED,
	/* A collection captured its roots, for a full collection or a young one. */
	CHUNKS_TRACE_EVENT_CAPTURED,
	/* Packs were taken out of the table, by number and id. */
	CHUNKS_TRACE_EVENT_RETIRED,
	/* A collection copied a pack's live chunks into another pack. */
	CHUNKS_TRACE_EVENT_COPIED,
	/* Forwards from old locators to their copies were written durably. */
	CHUNKS_TRACE_EVENT_FORWARDED,
	/* A collection ended. */
	CHUNKS_TRACE_EVENT_COLLECTION_ENDED,
	/* Retired packs a verify marking cleared. */
	CHUNKS_TRACE_EVENT_SETTLED,

	/*
	 * A retired pack a verify marking reached was listed again under a fresh
	 * number.
	 */
	CHUNKS_TRACE_EVENT_RESTORED,
	/* A retirement's files were queued for removal, by pack number. */
	CHUNKS_TRACE_EVENT_RETIRED_QUEUED,

	/*
	 * A pack file no list named was listed again at a collection's start,
	 * under a fresh number.
	 */
	CHUNKS_TRACE_EVENT_RELISTED,
	/* A retired pack's file was deleted. */
	CHUNKS_TRACE_EVENT_PACK_DELETED,

	/*
	 * Entries of the forwarding table were dropped durably, by retired pack
	 * and offset.
	 */
	CHUNKS_TRACE_EVENT_FORWARDS_DROPPED,
	/* A base backup began. */
	CHUNKS_TRACE_EVENT_BACKUP_STARTED,
	/* A base backup ended. */
	CHUNKS_TRACE_EVENT_BACKUP_ENDED,
	/* A pack's records in the pack table's and runs files were made durable. */
	CHUNKS_TRACE_EVENT_LIST_RECORDED,
	/* A repository's pack record was made and published. */
	CHUNKS_TRACE_EVENT_RECORD_MADE,

	/*
	 * A repository's pack record that no process held was freed by the pool's
	 * reclaim.
	 */
	CHUNKS_TRACE_EVENT_RECORD_RECLAIMED,
	/* A dropped repository's pack record was freed. */
	CHUNKS_TRACE_EVENT_RECORD_DROPPED,

	/*
	 * The startup process settled a repository's packs, with how much it
	 * changed: the files it removed, the tablespaces whose runs file's end it
	 * recorded, the list files that fell back or were damaged, the runs it
	 * wrote again, and the packs it left out.
	 */
	CHUNKS_TRACE_EVENT_PACKS_SETTLED,

	/*
	 * A dropped repository's directories were removed, as far as they could
	 * be, and its entry freed.
	 */
	CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED,

	/*
	 * A process's open writers' slot was stored: its repository with the
	 * lowest pack number it has open.
	 */
	CHUNKS_TRACE_EVENT_WRITER_STORED,
	/* A process's open writers' slot was cleared. */
	CHUNKS_TRACE_EVENT_WRITER_CLEARED,
	/* A root was registered in one of a process's three slots. */
	CHUNKS_TRACE_EVENT_ROOT_REGISTERED,
	/* A slot's registration was released: its repository's word cleared. */
	CHUNKS_TRACE_EVENT_ROOT_RELEASED,

	/*
	 * A collection's marking of the repository began, covering the packs
	 * numbered up to the newest listed.
	 */
	CHUNKS_TRACE_EVENT_MARKING_BEGAN,
	/* A marking ended with no retirement, its collection having failed. */
	CHUNKS_TRACE_EVENT_MARKING_ENDED,
	/* A marking ended with its retirement, whichever packs it took out. */
	CHUNKS_TRACE_EVENT_MARKING_RETIRED,

	/*
	 * A marking no one ended, its collector gone, was cleared by the next
	 * marking's start.
	 */
	CHUNKS_TRACE_EVENT_MARKING_CLEARED,
	/* A locate recorded a hit in a pack the marking covers. */
	CHUNKS_TRACE_EVENT_HIT_RECORDED,
	/* A generation of a pack's deletion vector was named in the pack table. */
	CHUNKS_TRACE_EVENT_VECTOR_NAMED,

	/*
	 * Entries of the removal queue were reserved, by a process, for no
	 * repository in particular.
	 */
	CHUNKS_TRACE_EVENT_RESERVED,
	/* Entries of the removal queue a reservation didn't fill were given back. */
	CHUNKS_TRACE_EVENT_UNRESERVED,

	/*
	 * A file that left the repository's lists was queued for removal at an
	 * epoch.
	 */
	CHUNKS_TRACE_EVENT_FILE_QUEUED,
	/* A queued run's file or deletion vector's file was removed. */
	CHUNKS_TRACE_EVENT_FILE_REMOVED,
	/* A checkpoint chose its redo point. */
	CHUNKS_TRACE_EVENT_CHECKPOINT_BEGAN,
	/* The pack table's and runs files were written whole. */
	CHUNKS_TRACE_EVENT_LISTS_COMPACTED,
	/* A checkpoint wrote its control file. */
	CHUNKS_TRACE_EVENT_CHECKPOINT_ENDED,
	/* The store was dropped without shutting down. */
	CHUNKS_TRACE_EVENT_CRASHED,
} ChunksTraceEventTag;

/* ChunksTraceEvent is Event: one transition of the storage protocol, recorded where it becomes durable or visible. */
typedef struct ChunksTraceEvent
{
	ChunksTraceEventTag tag;
	union
	{
		struct
		{
			uint64		writer;
			uint64		pack;
		}			writer_opened;
		struct
		{
			uint64		pack;
		}			pack_opened;
		struct
		{
			ChunksAddressAddress address;
			ChunksTrailerLocator locator;
			bool		has_names;
			/* names is a Vec<Locator>. */
			StdVec		names;
		}			chunk_written;
		struct
		{
			uint64		pack;
		}			pack_synced;
		struct
		{
			uint64		pack;
		}			pack_added;
		struct
		{
			uint64		pack;
		}			pack_abandoned;
		struct
		{
			uint64		writer;
			ChunksTrailerLocator root;
		}			registered;
		struct
		{
			uint64		writer;
		}			released;
		struct
		{
			StdString	name;
			ChunksTrailerLocator root;
			ChunksTraceSource from;
		}			ref_stored;
		struct
		{
			StdString	name;
		}			ref_cleared;
		struct
		{
			uint64		csn;
			StdString	name;
		}			snapshot_read;
		struct
		{
			uint64		csn;
		}			snapshot_ended;
		struct
		{
			uint64		writer;
			uint64		csn;
			StdString	name;
		}			cached;
		struct
		{
			bool		full;
		}			captured;
		struct
		{
			/* packs is a Vec<(u64, PackId)>. */
			StdVec		packs;
		}			retired;
		struct
		{
			uint64		from;
			uint64		to;
		}			copied;
		struct
		{
			/* moves is a Vec<(Locator, Locator)>. */
			StdVec		moves;
		}			forwarded;
		struct
		{
			/* packs is a Vec<PackId>. */
			StdVec		packs;
		}			settled;
		struct
		{
			uint64		from;
			uint64		to;
		}			restored;
		struct
		{
			/* packs is a Vec<u64>. */
			StdVec		packs;
		}			retired_queued;
		struct
		{
			uint64		pack;
			ChunksPackPackId id;
		}			relisted;
		struct
		{
			ChunksPackPackId pack;
		}			pack_deleted;
		struct
		{
			/* entries is a Vec<(u64, u64)>. */
			StdVec		entries;
		}			forwards_dropped;
		struct
		{
			uint64		pack;
		}			list_recorded;
		struct
		{
			size_t		removed;
			/* moved is a Vec<u32>. */
			StdVec		moved;
			size_t		mended;
			size_t		rebuilt;
			/* unlisted is a Vec<u64>. */
			StdVec		unlisted;
		}			packs_settled;
		struct
		{
			uint64		lowest;
		}			writer_stored;
		struct
		{
			size_t		slot;
			ChunksTrailerLocator root;
		}			root_registered;
		struct
		{
			size_t		slot;
		}			root_released;
		struct
		{
			uint64		newest;
		}			marking_began;
		struct
		{
			ChunksAddressAddress address;
			ChunksTrailerLocator locator;
		}			hit_recorded;
		struct
		{
			uint64		pack;
			uint64		generation;
		}			vector_named;
		struct
		{
			size_t		entries;
		}			reserved;
		struct
		{
			size_t		entries;
		}			unreserved;
		struct
		{
			ChunksPacksSingleQueued file;
			ChunksPackPackId id;
			uint64		epoch;
		}			file_queued;
		struct
		{
			ChunksPacksSingleQueued file;
			ChunksPackPackId id;
		}			file_removed;
	}			u;
} ChunksTraceEvent;

/* CHUNKS_TRACE_SERVER is the key of the transitions that belong to no repository: a reservation of the removal queue's entries. */
#define CHUNKS_TRACE_SERVER ((uint64) 0)

/* ChunksTraceDroppedTag is Dropped's variants: the transitions a drop makes. */
typedef enum ChunksTraceDroppedTag
{
	/* The event WriterStored, with its lowest number. */
	CHUNKS_TRACE_DROPPED_WRITER_STORED,
	/* The event WriterCleared. */
	CHUNKS_TRACE_DROPPED_WRITER_CLEARED,
	/* The event Unreserved, with its entries. */
	CHUNKS_TRACE_DROPPED_UNRESERVED,
} ChunksTraceDroppedTag;

/*
 * ChunksTraceDropped is Dropped: a transition a drop makes, which takes no
 * lock and allocates nothing, so the drop leaves it for the next emit or
 * take to record.
 */
typedef struct ChunksTraceDropped
{
	ChunksTraceDroppedTag tag;
	union
	{
		uint64		writer_stored;
		uint64		unreserved;
	}			u;
} ChunksTraceDropped;

extern bool chunks_trace_Source_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trace_Source_PartialEq_eq(const ChunksTraceSource *self, const ChunksTraceSource *other);
extern bool chunks_trace_Event_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_trace_Event_PartialEq_eq(const ChunksTraceEvent *self, const ChunksTraceEvent *other);
extern void chunks_trace_emit_in_drop(uint64 repo, ChunksTraceDropped dropped);
extern void chunks_trace_emit(uint64 repo, void (*event) (void *captures, ChunksTraceEvent *result),
							  void *event_captures, void (*event_drop) (void *));
#ifdef DOLTGRES_CHUNKS_TRACE
extern StdVec chunks_trace_take(uint64 repo);
#endif
extern bool chunks_trace_try_emit(uint64 repo,
								  bool (*event) (void *captures, ChunksTraceEvent *result, ChunksErrorError *error),
								  void *event_captures, void (*event_drop) (void *), ChunksErrorError *error);
extern bool chunks_trace_named(const uint8 *stored, size_t stored_len, bool *has_result, StdVec *result,
							   ChunksErrorError *error);
extern void chunks_trace_Event_drop(ChunksTraceEvent *self);

#endif							/* DOLTGRES_CHUNKS_TRACE_H */
