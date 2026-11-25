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
 * The removal queue (chunks' packs/removal.rs, design item 6): the files that
 * left a repository's lists wait in chunks' single record for the readers
 * that may still hold them, reserved before an operation's first step.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "removal.rs"

/* CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS is Reserving's ProcSlots: each process's word for the entries it reserved, which its exit zeroes. */
const HostSharedProcSlotsOps CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS = {
	.id = UINT64CONST(0x6368756e6b730005), .words = 1,
};

/* CHUNKS_PACKS_REMOVAL_RESERVED_INDEX is Reserved's word: how many entries the process has reserved and not yet filled. */
#define CHUNKS_PACKS_REMOVAL_RESERVED_INDEX 0

/* chunks_packs_removal_invariant returns an internal error with a message. */
static ChunksErrorError
chunks_packs_removal_invariant(const char *message)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_INVARIANT};

	std_string_from_Checked(&error.u.invariant, message, strlen(message));
	return error;
}

/* chunks_packs_removal_entry reads the removal queue's entry at an index under the queue's lock. */
static bool
chunks_packs_removal_entry(HostSharedGuard *removing, size_t index, bool *some, ChunksPacksSingleRemoval *result,
						   ChunksErrorError *error)
{
	StdVec		entry;
	HostError	host;
	bool		ok;

	if (!host_shared_Guard_entry(removing, CHUNKS_PACKS_SINGLE_QUEUE, index, &entry, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	ok = chunks_packs_single_Removal_read(entry.ptr, entry.len, some, result, error);
	std_vec_free_Checked(&entry, 1, 1);
	return ok;
}

/* chunks_packs_removal_removing takes the removal queue's lock, Removing, of chunks' single record. */
static bool
chunks_packs_removal_removing(HostIoIo *io, HostSharedGuard *result, ChunksErrorError *error)
{
	HostSharedRecord queue;
	HostError	host;

	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &queue, &host) ||
		!host_shared_Record_lock(queue, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SINGLE_REMOVING_INDEX, result, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/* chunks_packs_removal_Reservation_entries returns how many files the reservation has room for. */
size_t
chunks_packs_removal_Reservation_entries(const ChunksPacksRemovalReservation *self)
{
	return self->entries;
}

/*
 * chunks_packs_removal_Reservation_drop gives back the entries the
 * reservation didn't fill, with a store of the process's own word, which
 * can't fail.
 */
void
chunks_packs_removal_Reservation_drop(ChunksPacksRemovalReservation *self)
{
	host_shared_Mine_store(&self->word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX,
						   STD_SUB(u64, host_shared_Mine_load(&self->word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX),
								   (uint64) self->entries, 68, 37));
	if (self->entries > 0)
	{
		ChunksTraceDropped dropped = {.tag = CHUNKS_TRACE_DROPPED_UNRESERVED, .u.unreserved = self->entries};

		chunks_trace_emit_in_drop(CHUNKS_TRACE_SERVER, dropped);
	}
}

/* chunks_packs_removal_reserved is Reserved's event, of the entries reserved. */
static void
chunks_packs_removal_reserved(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_RESERVED;
	result->u.reserved.entries = *(const size_t *) captures;
}

/* chunks_packs_removal_reserve_frame_0_t is reserve's state across its pad: the queue's guard and the processes, while held. */
typedef struct chunks_packs_removal_reserve_frame_0_t
{
	HostIoIo   *io;
	size_t		wanted;
	ChunksPacksRemovalReservation *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard removing;
	bool		removing_live;
	StdVec		processes;
	bool		processes_live;
} chunks_packs_removal_reserve_frame_0_t;

/* chunks_packs_removal_reserve_pad_0 is reserve's pad: the processes, then the queue's guard. */
static void
chunks_packs_removal_reserve_pad_0(void *frame_)
{
	chunks_packs_removal_reserve_frame_0_t *frame = frame_;

	if (frame->processes_live)
	{
		frame->processes_live = false;
		std_vec_free_Checked(&frame->processes, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->removing_live)
	{
		frame->removing_live = false;
		host_shared_Guard_drop(&frame->removing);
	}
}

/*
 * chunks_packs_removal_reserve_body_0 is reserve's body: what is due
 * removed, then the queue's free entries less those every process has
 * reserved, read under the queue's lock.
 */
static void
chunks_packs_removal_reserve_body_0(void *frame_)
{
	chunks_packs_removal_reserve_frame_0_t *frame = frame_;
	HostSharedMine word;
	HostError	host;
	size_t		room = 0;
	size_t		entries;

	frame->ok = false;
	if (!chunks_packs_removal_remove_due(frame->io, frame->error))
		return;
	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(frame->io), &CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS, &word,
						  &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (!chunks_packs_removal_removing(frame->io, &frame->removing, frame->error))
		return;
	frame->removing_live = true;
	for (size_t index = 0; index < CHUNKS_PACKS_SINGLE_QUEUED; index++)
	{
		ChunksPacksSingleRemoval removal;
		bool		some;

		if (!chunks_packs_removal_entry(&frame->removing, index, &some, &removal, frame->error))
			goto done;
		room = STD_ADD(usize, room, some ? 0 : 1, 85, 9);
	}
	if (!host_shared_processes(&CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS, &frame->processes, &host))
		goto failed;
	frame->processes_live = true;
	for (size_t k = 0, k_len = frame->processes.len; k < k_len; k++)
	{
		uint64		reserved;

		if (!host_shared_load(&CHUNKS_PACKS_REMOVAL_RESERVING_PROC_SLOTS_OPS, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX,
							  STD_VEC_AT(&frame->processes, uint32, k), &reserved, &host))
			goto failed;
		room = std_saturating_sub_usize(room, (size_t) reserved);
	}
	entries = Min(frame->wanted, room);
	host_shared_Mine_store(&word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX,
						   STD_ADD(u64, host_shared_Mine_load(&word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX),
								   (uint64) entries, 91, 28));
	if (entries > 0)
		chunks_trace_emit(CHUNKS_TRACE_SERVER, chunks_packs_removal_reserved, &entries, NULL);
	frame->result->entries = entries;
	frame->result->word = word;
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_removal_reserve_pad_0(frame);
}

/*
 * chunks_packs_removal_reserve reserves room in the removal queue for up to
 * as many files as wanted, after removing what is due, and returns the
 * reservation of as many as there was room for: the queue's free entries
 * less those every process has reserved, read under the queue's lock.
 */
bool
chunks_packs_removal_reserve(HostIoIo *io, size_t wanted, ChunksPacksRemovalReservation *result,
							 ChunksErrorError *error)
{
	chunks_packs_removal_reserve_frame_0_t frame = {.io = io, .wanted = wanted, .result = result, .error = error};

	std_landing(&frame, chunks_packs_removal_reserve_body_0, chunks_packs_removal_reserve_pad_0);
	return frame.ok;
}

/* chunks_packs_removal_queued_t is FileQueued's captures: the file's kind, its id, and the epoch it was queued at. */
typedef struct chunks_packs_removal_queued_t
{
	ChunksPacksSingleQueued file;
	ChunksPackPackId id;
	uint64		epoch;
} chunks_packs_removal_queued_t;

/* chunks_packs_removal_file_queued is FileQueued's event. */
static void
chunks_packs_removal_file_queued(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_removal_queued_t *queued = captures;

	result->tag = CHUNKS_TRACE_EVENT_FILE_QUEUED;
	result->u.file_queued.file = queued->file;
	result->u.file_queued.id = queued->id;
	result->u.file_queued.epoch = queued->epoch;
}

/* chunks_packs_removal_queue_frame_0_t is queue's state across its pad: the reservation it takes and the queue's guard. */
typedef struct chunks_packs_removal_queue_frame_0_t
{
	HostIoIo   *io;
	ChunksPacksRemovalReservation *reservation;
	uint32		repository;
	const ChunksPacksRemovalFile *files;
	size_t		files_len;
	ChunksErrorError *error;
	bool		ok;
	bool		reservation_live;
	HostSharedGuard removing;
	bool		removing_live;
} chunks_packs_removal_queue_frame_0_t;

/* chunks_packs_removal_queue_pad_0 is queue's pad: the queue's guard, then the reservation. */
static void
chunks_packs_removal_queue_pad_0(void *frame_)
{
	chunks_packs_removal_queue_frame_0_t *frame = frame_;

	if (frame->removing_live)
	{
		frame->removing_live = false;
		host_shared_Guard_drop(&frame->removing);
	}
	if (frame->reservation_live)
	{
		frame->reservation_live = false;
		chunks_packs_removal_Reservation_drop(frame->reservation);
	}
}

/*
 * chunks_packs_removal_queue_body_0 is queue's body: the epoch taken and
 * advanced past under the epoch lock, then an entry written for each file
 * under the queue's lock.
 */
static void
chunks_packs_removal_queue_body_0(void *frame_)
{
	chunks_packs_removal_queue_frame_0_t *frame = frame_;
	ChunksPacksRemovalReservation *reservation = frame->reservation;
	HostSharedEpochs epochs;
	HostError	host;
	uint64		epoch;
	bool		spaceless = false;
	size_t		left = 0;

	frame->ok = false;
	for (size_t k = 0; k < frame->files_len; k++)
		spaceless = spaceless || frame->files[k].f0 == CHUNKS_PACKS_DEFAULT_SPACE;
	if (frame->files_len > reservation->entries || spaceless)
	{
		*frame->error = chunks_packs_removal_invariant("files are queued for removal past their reservation or by no tablespace");
		goto done;
	}
	if (!host_shared_epochs(frame->io, &epochs, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	epoch = host_shared_Epochs_advance(&epochs);
	host_shared_Epochs_drop(&epochs);
	if (!chunks_packs_removal_removing(frame->io, &frame->removing, frame->error))
		goto done;
	frame->removing_live = true;
	for (size_t index = 0; index < CHUNKS_PACKS_SINGLE_QUEUED && left < frame->files_len; index++)
	{
		const ChunksPacksRemovalFile *file = &frame->files[left];
		ChunksPacksSingleRemoval removal;
		bool		some;
		uint8		bytes[CHUNKS_PACKS_SINGLE_REMOVAL];
		chunks_packs_removal_queued_t queued;

		if (!chunks_packs_removal_entry(&frame->removing, index, &some, &removal, frame->error))
			goto done;
		if (some)
			continue;
		removal = (ChunksPacksSingleRemoval)
		{
			.repository = frame->repository, .space = file->f0, .kind = file->f1, .id = file->f2, .epoch = epoch,
		};
		chunks_packs_single_Removal_bytes(&removal, bytes);
		if (!host_shared_GuardExclusive_set_entry(&frame->removing, CHUNKS_PACKS_SINGLE_QUEUE, index, bytes,
												  sizeof(bytes), &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		queued = (chunks_packs_removal_queued_t)
		{
			.file = file->f1, .id = file->f2, .epoch = epoch,
		};
		chunks_trace_emit(frame->repository, chunks_packs_removal_file_queued, &queued, NULL);
		left++;
	}
	host_shared_Mine_store(&reservation->word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX,
						   STD_SUB(u64, host_shared_Mine_load(&reservation->word, CHUNKS_PACKS_REMOVAL_RESERVED_INDEX),
								   (uint64) left, 121, 40));
	reservation->entries = STD_SUB(usize, reservation->entries, left, 122, 5);
	if (left == frame->files_len)
		frame->ok = true;
	else
		*frame->error = chunks_packs_removal_invariant("the removal queue has no entry for a reserved file");
done:
	chunks_packs_removal_queue_pad_0(frame);
}

/*
 * chunks_packs_removal_queue queues files that left a repository's lists, in
 * the entries their operation reserved, with no record lock held: it takes
 * the current epoch and advances it past under the epoch lock, and then
 * writes an entry for each file under the queue's lock. It refuses more
 * files than were reserved, and a file whose tablespace is chunks' own
 * number for a repository's directory in place of a tablespace's id, which
 * no remover could resolve. It takes the reservation.
 */
bool
chunks_packs_removal_queue(HostIoIo *io, ChunksPacksRemovalReservation *reservation, uint32 repository,
						   const ChunksPacksRemovalFile *files, size_t files_len, ChunksErrorError *error)
{
	chunks_packs_removal_queue_frame_0_t frame = {.io = io, .reservation = reservation, .repository = repository,
	.files = files, .files_len = files_len, .error = error, .reservation_live = true};

	std_landing(&frame, chunks_packs_removal_queue_body_0, chunks_packs_removal_queue_pad_0);
	return frame.ok;
}

/* chunks_packs_removal_queued_frame_0_t is queued's state across its pad: the queue's guard and the entries. */
typedef struct chunks_packs_removal_queued_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard removing;
	bool		removing_live;
	StdVec		entries;
	bool		entries_live;
} chunks_packs_removal_queued_frame_0_t;

/* chunks_packs_removal_queued_pad_0 is queued's pad: the entries, then the queue's guard. */
static void
chunks_packs_removal_queued_pad_0(void *frame_)
{
	chunks_packs_removal_queued_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(ChunksPacksSingleRemoval), _Alignof(ChunksPacksSingleRemoval));
	}
	if (frame->removing_live)
	{
		frame->removing_live = false;
		host_shared_Guard_drop(&frame->removing);
	}
}

/* chunks_packs_removal_queued_body_0 is queued's body: each of the repository's entries, under the queue's lock. */
static void
chunks_packs_removal_queued_body_0(void *frame_)
{
	chunks_packs_removal_queued_frame_0_t *frame = frame_;

	frame->ok = false;
	if (!chunks_packs_removal_removing(frame->io, &frame->removing, frame->error))
		return;
	frame->removing_live = true;
	std_vec_new(&frame->entries, sizeof(ChunksPacksSingleRemoval), _Alignof(ChunksPacksSingleRemoval));
	frame->entries_live = true;
	for (size_t index = 0; index < CHUNKS_PACKS_SINGLE_QUEUED; index++)
	{
		ChunksPacksSingleRemoval entry;
		bool		some;

		if (!chunks_packs_removal_entry(&frame->removing, index, &some, &entry, frame->error))
		{
			chunks_packs_removal_queued_pad_0(frame);
			return;
		}
		if (some && entry.repository == frame->repository)
			std_vec_push_Checked(&frame->entries, &entry, sizeof(entry), _Alignof(ChunksPacksSingleRemoval));
	}
	*frame->result = frame->entries;
	frame->entries_live = false;
	frame->ok = true;
	chunks_packs_removal_queued_pad_0(frame);
}

/*
 * chunks_packs_removal_queued returns the removal queue's entries for a
 * repository's files, read under the queue's lock, in the queue's order.
 */
bool
chunks_packs_removal_queued(HostIoIo *io, uint32 repository, StdVec *result, ChunksErrorError *error)
{
	chunks_packs_removal_queued_frame_0_t frame = {.io = io, .repository = repository, .result = result,
	.error = error};

	std_landing(&frame, chunks_packs_removal_queued_body_0, chunks_packs_removal_queued_pad_0);
	return frame.ok;
}

/* chunks_packs_removal_insert inserts an id into a set of ids, a vector in the ids' order with each once, as BTreeSet::insert. */
static void
chunks_packs_removal_insert(StdVec *ids, const ChunksPackPackId *id)
{
	size_t		low = 0;
	size_t		high = ids->len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		StdOrdering order = chunks_pack_PackId_Ord_cmp(&STD_VEC_AT(ids, ChunksPackPackId, middle), id);

		if (order == STD_ORDERING_EQUAL)
			return;
		if (order == STD_ORDERING_LESS)
			low = middle + 1;
		else
			high = middle;
	}
	std_vec_insert(ids, low, id, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId), RUST_FILE, 140, 17);
}

/*
 * chunks_packs_removal_queued_packs returns the ids of a repository's pack
 * files the removal queue holds, a BTreeSet<PackId> as a vector in the ids'
 * order, read under the queue's lock, which a collection's re-listing passes
 * over, since each goes once its readers end.
 */
bool
chunks_packs_removal_queued_packs(HostIoIo *io, uint32 repository, StdVec *result, ChunksErrorError *error)
{
	StdVec		entries;

	if (!chunks_packs_removal_queued(io, repository, &entries, error))
		return false;
	std_vec_new(result, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	for (size_t k = 0, k_len = entries.len; k < k_len; k++)
	{
		const ChunksPacksSingleRemoval *entry = &STD_VEC_AT(&entries, ChunksPacksSingleRemoval, k);

		if (entry->kind.tag == CHUNKS_PACKS_SINGLE_QUEUED_PACK)
			chunks_packs_removal_insert(result, &entry->id);
	}
	std_vec_free_Checked(&entries, sizeof(ChunksPacksSingleRemoval), _Alignof(ChunksPacksSingleRemoval));
	return true;
}

/*
 * chunks_packs_removal_file_name returns the name of a queued entry's file,
 * which lies in its repository's directory in the entry's space: a pack, its
 * address-index run, or a generation of its dead vector beside it.
 */
StdString
chunks_packs_removal_file_name(const ChunksPacksSingleRemoval *entry)
{
	StdString	pack;
	StdPathBuf	vector;

	switch (entry->kind.tag)
	{
		case CHUNKS_PACKS_SINGLE_QUEUED_PACK:
			break;
		case CHUNKS_PACKS_SINGLE_QUEUED_RUN:
			return chunks_pack_PackId_file_name(&entry->id, "idx", 3);
		case CHUNKS_PACKS_SINGLE_QUEUED_VECTOR:
			pack = chunks_pack_PackId_file_name(&entry->id, "pack", 4);
			vector = chunks_dead_Dead_path((const char *) pack.ptr, pack.len, entry->kind.u.vector);
			std_vec_free_Checked(&pack, 1, 1);
			return vector.bytes;
	}
	return chunks_pack_PackId_file_name(&entry->id, "pack", 4);
}

/* chunks_packs_removal_Due is a due file's entry: its index in the queue and its removal. */
typedef struct chunks_packs_removal_Due
{
	size_t		index;
	ChunksPacksSingleRemoval entry;
} chunks_packs_removal_Due;

/* chunks_packs_removal_removed is a due file's removal's event: PackDeleted for a pack, and FileRemoved otherwise. */
static void
chunks_packs_removal_removed(void *captures, ChunksTraceEvent *result)
{
	const ChunksPacksSingleRemoval *entry = captures;

	if (entry->kind.tag == CHUNKS_PACKS_SINGLE_QUEUED_PACK)
	{
		result->tag = CHUNKS_TRACE_EVENT_PACK_DELETED;
		result->u.pack_deleted.pack = entry->id;
	}
	else
	{
		result->tag = CHUNKS_TRACE_EVENT_FILE_REMOVED;
		result->u.file_removed.file = entry->kind;
		result->u.file_removed.id = entry->id;
	}
}

/*
 * chunks_packs_removal_remove_due_frame_0_t is remove_due's state across its
 * pad: the due entries, the paths, the queue's guard, and the repository it
 * is at, with its open pack record and its files' flag while held.
 */
typedef struct chunks_packs_removal_remove_due_frame_0_t
{
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	StdVec		due;
	bool		due_live;
	HostSharedGuard removing;
	bool		removing_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	uint32		at;
	bool		at_live;
	bool		skipped;
	HostSharedHeld held;
	bool		held_live;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
} chunks_packs_removal_remove_due_frame_0_t;

/* chunks_packs_removal_remove_due_left releases the repository remove_due is at: its files' flag, then its pack record. */
static void
chunks_packs_removal_remove_due_left(chunks_packs_removal_remove_due_frame_0_t *frame)
{
	frame->at_live = false;
	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
}

/*
 * chunks_packs_removal_remove_due_at moves remove_due to a repository: when a
 * process has its pack record published, it tries the record's files' flag,
 * and skips the repository's files until a later pass when a whole copy of
 * its directory holds the flag (bugs row 451), never waiting on it.
 */
static bool
chunks_packs_removal_remove_due_at(chunks_packs_removal_remove_due_frame_0_t *frame, uint32 repository)
{
	HostError	host;
	uint64		location;
	bool		has;
	bool		taken;

	chunks_packs_removal_remove_due_left(frame);
	frame->at = repository;
	frame->at_live = true;
	frame->skipped = false;
	if (!chunks_packs_open_found(HOST_SHARED_IO_WITHIN(frame->io), repository, &has, &frame->held, &location,
								 frame->error))
		return false;
	if (!has)
		return true;
	frame->held_live = true;
	if (!host_shared_Record_try_flag(host_shared_Held_record(&frame->held), HOST_IO_IO_TAKES_SHORT(frame->io),
									 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &taken, &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	frame->rewriting_live = taken;
	frame->skipped = !taken;
	return true;
}

/* chunks_packs_removal_remove_due_paths frees the paths of the due file remove_due is at. */
static void
chunks_packs_removal_remove_due_paths(chunks_packs_removal_remove_due_frame_0_t *frame)
{
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/* chunks_packs_removal_remove_due_pad_0 is remove_due's pad: the queue's guard, the paths, the repository it is at, then the due entries. */
static void
chunks_packs_removal_remove_due_pad_0(void *frame_)
{
	chunks_packs_removal_remove_due_frame_0_t *frame = frame_;

	if (frame->removing_live)
	{
		frame->removing_live = false;
		host_shared_Guard_drop(&frame->removing);
	}
	chunks_packs_removal_remove_due_paths(frame);
	chunks_packs_removal_remove_due_left(frame);
	if (frame->due_live)
	{
		frame->due_live = false;
		std_vec_free_Checked(&frame->due, sizeof(chunks_packs_removal_Due), _Alignof(chunks_packs_removal_Due));
	}
}

/* chunks_packs_removal_joined joins a file's name to a directory into a path the frame owns from the call. */
static void
chunks_packs_removal_joined(const StdPathBuf *dir, StdString *name, StdPathBuf *result, bool *live)
{
	std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, result);
	*live = true;
	std_vec_free_Checked(name, 1, 1);
}

/*
 * chunks_packs_removal_remove_due_body_0 is remove_due's body: the due
 * entries read under the queue's lock, each due file removed with no lock
 * held, under its repository's files' flag when a process has the
 * repository open, and its entry freed under the lock while it is the one
 * read, then the dropped repositories' pass.
 */
static void
chunks_packs_removal_remove_due_body_0(void *frame_)
{
	chunks_packs_removal_remove_due_frame_0_t *frame = frame_;
	HostSharedEpochs epochs;
	HostError	host;
	uint64		oldest;
	bool		freed;

	frame->ok = false;
	if (host_backups_running())
	{
		frame->ok = true;
		return;
	}
	if (!host_shared_epochs(frame->io, &epochs, &host))
		goto failed;
	oldest = host_shared_Epochs_oldest(&epochs);
	host_shared_Epochs_drop(&epochs);
	std_vec_new(&frame->due, sizeof(chunks_packs_removal_Due), _Alignof(chunks_packs_removal_Due));
	frame->due_live = true;
	if (!chunks_packs_removal_removing(frame->io, &frame->removing, frame->error))
		goto done;
	frame->removing_live = true;
	for (size_t index = 0; index < CHUNKS_PACKS_SINGLE_QUEUED; index++)
	{
		chunks_packs_removal_Due due = {.index = index};
		bool		some;

		if (!chunks_packs_removal_entry(&frame->removing, index, &some, &due.entry, frame->error))
			goto done;
		if (some && due.entry.epoch < oldest)
			std_vec_push_Checked(&frame->due, &due, sizeof(due), _Alignof(chunks_packs_removal_Due));
	}
	frame->removing_live = false;
	host_shared_Guard_drop(&frame->removing);
	for (size_t k = 0, k_len = frame->due.len; k < k_len; k++)
	{
		const chunks_packs_removal_Due *due = &STD_VEC_AT(&frame->due, chunks_packs_removal_Due, k);
		const ChunksPacksSingleRemoval *entry = &due->entry;
		StdString	name;
		StdVec		now;
		uint8		bytes[CHUNKS_PACKS_SINGLE_REMOVAL];
		bool		removed;
		bool		same;
		bool		there;

		if ((!frame->at_live || frame->at != entry->repository) &&
			!chunks_packs_removal_remove_due_at(frame, entry->repository))
			goto done;
		if (frame->skipped)
			continue;
		frame->dir = host_tablespace_dir(entry->repository, entry->space);
		frame->dir_live = true;
		name = chunks_packs_removal_file_name(entry);
		chunks_packs_removal_joined(&frame->dir, &name, &frame->path, &frame->path_live);
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(HOST_IO_IO_IO_DOORS(frame->io)), frame->dir.bytes.ptr,
								   frame->dir.bytes.len, &there, frame->error))
			goto done;
		if (there && !host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-removal-checked", &host))
			goto failed;
		if (there &&
			!chunks_file_remove_if_there(HOST_IO_IO_IO_DOORS(frame->io), frame->path.bytes.ptr, frame->path.bytes.len,
										 &removed, frame->error))
			goto done;
		chunks_trace_emit(entry->repository, chunks_packs_removal_removed, (void *) entry, NULL);
		if (!chunks_packs_removal_removing(frame->io, &frame->removing, frame->error))
			goto done;
		frame->removing_live = true;
		if (!host_shared_Guard_entry(&frame->removing, CHUNKS_PACKS_SINGLE_QUEUE, due->index, &now, &host))
			goto failed;
		chunks_packs_single_Removal_bytes(entry, bytes);
		same = now.len == sizeof(bytes) && memcmp(now.ptr, bytes, sizeof(bytes)) == 0;
		std_vec_free_Checked(&now, 1, 1);
		if (same)
		{
			static const uint8 cleared[CHUNKS_PACKS_SINGLE_REMOVAL] = {0};

			if (!host_shared_GuardExclusive_set_entry(&frame->removing, CHUNKS_PACKS_SINGLE_QUEUE, due->index,
													  cleared, sizeof(cleared), &host))
				goto failed;
		}
		frame->removing_live = false;
		host_shared_Guard_drop(&frame->removing);
		chunks_packs_removal_remove_due_paths(frame);
	}
	chunks_packs_removal_remove_due_left(frame);
	frame->ok = chunks_packs_dropped_remove_dropped(frame->io, &freed, frame->error);
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_removal_remove_due_pad_0(frame);
}

/*
 * chunks_packs_removal_remove_due removes the queued files, of whichever
 * repository, that every live reader started after, as the collector and
 * each checkpoint do: it compares the readers' epochs under the epoch lock,
 * removes each due file with no lock held, finding its directory through the
 * file door's tablespace-directory seam, and frees each file's entry once
 * the file is removed. A file already gone counts as removed, and so does
 * one whose directory is gone, with no directory to sync, as a moved
 * database's old space leaves its entries (bugs row 451), which Doltgrust's
 * delete_waiting passes over as gone the same way. A removal that
 * fails is returned, with its entry and those after it left queued. It then
 * removes the directories of the dropped repositories whose pack records no
 * process holds any more. While a base backup runs it removes nothing, since
 * the backup copies the list files and the packs at different moments, and
 * it leaves queued the files of a repository whose files' flag a whole copy
 * of its directory holds (bugs row 451).
 */
bool
chunks_packs_removal_remove_due(HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_removal_remove_due_frame_0_t frame = {.io = io, .error = error};

	std_landing(&frame, chunks_packs_removal_remove_due_body_0, chunks_packs_removal_remove_due_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_removal_tests_file returns a file to queue: its tablespace's id, its kind, and its id. */
static ChunksPacksRemovalFile
chunks_packs_removal_tests_file(uint32 space, ChunksPacksSingleQueuedTag kind, uint64 vector, ChunksPackPackId id)
{
	ChunksPacksRemovalFile file = {.f0 = space, .f1.tag = kind, .f2 = id};

	if (kind == CHUNKS_PACKS_SINGLE_QUEUED_VECTOR)
		file.f1.u.vector = vector;
	return file;
}

/*
 * chunks_packs_removal_tests_files returns a listed pack's file, its run,
 * and a generation of its deletion vector written beside them, as the files
 * to queue and their paths.
 */
static void
chunks_packs_removal_tests_files(HostIoIo *io, const ChunksPacksBackendPacks *packs, uint64 number,
								 ChunksPacksRemovalFile *queued, StdPathBuf *paths)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 200, 46);
	StdArcInner *arc;
	const ChunksPackPackReader *reader;
	ChunksErrorError error;
	StdBTreeMap none = STD_BTREE_MAP_NEW;
	uint64		generation = 3;
	bool		has;

	if (!chunks_packs_backend_Packs_pack_reader(packs, io, &reading, number, &has, &arc, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 201, 62);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(201, 71);
	reader = std_arc_Deref_deref(arc, _Alignof(ChunksPackPackReader));
	std_slice_to_vec_Checked(&paths[0].bytes, reader->path.bytes.ptr, reader->path.bytes.len, 1, 1);
	if (!chunks_dead_Dead_write(HOST_IO_IO_IO_DOORS(io), paths[0].bytes.ptr, paths[0].bytes.len, generation, &none,
								&error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 203, 62);
	queued[0] = chunks_packs_removal_tests_file(HOST_PG_DEFAULT, CHUNKS_PACKS_SINGLE_QUEUED_PACK, 0, reader->id);
	queued[1] = chunks_packs_removal_tests_file(HOST_PG_DEFAULT, CHUNKS_PACKS_SINGLE_QUEUED_RUN, 0, reader->id);
	queued[2] = chunks_packs_removal_tests_file(HOST_PG_DEFAULT, CHUNKS_PACKS_SINGLE_QUEUED_VECTOR, generation, reader->id);
	std_path_with_extension(paths[0].bytes.ptr, paths[0].bytes.len, "idx", 3, &paths[1]);
	paths[2] = chunks_dead_Dead_path(paths[0].bytes.ptr, paths[0].bytes.len, generation);
	chunks_pack_ArcPackReader_drop(arc);
	host_shared_Reader_drop(&reading);
}

/* chunks_packs_removal_tests_reserved returns a reservation of up to as many entries as wanted, which unwraps at a line and column. */
static ChunksPacksRemovalReservation
chunks_packs_removal_tests_reserved(HostIoIo *io, size_t wanted, uint32 line, uint32 col)
{
	ChunksPacksRemovalReservation reservation;
	ChunksErrorError error;

	if (!chunks_packs_removal_reserve(io, wanted, &reservation, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return reservation;
}

/* chunks_packs_removal_tests_room_eq asserts at a line that the queue's room is as many entries as given, a reservation of all of it unwrapping at column 44. */
static void
chunks_packs_removal_tests_room_eq(HostIoIo *io, size_t want, uint32 line)
{
	ChunksPacksRemovalReservation all = chunks_packs_removal_tests_reserved(io, SIZE_MAX, line, 44);
	size_t		entries = chunks_packs_removal_Reservation_entries(&all);

	if (entries != want)
		std_assert_failed(RUST_FILE, line, 9, "==", &entries, std_usize_Debug_fmt, &want, std_usize_Debug_fmt, NULL);
	chunks_packs_removal_Reservation_drop(&all);
}

/* chunks_packs_removal_tests_removed removes what is due, which unwraps at a line and column 24. */
static void
chunks_packs_removal_tests_removed(HostIoIo *io, uint32 line)
{
	ChunksErrorError error;

	if (!chunks_packs_removal_remove_due(io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, 24);
}

/* chunks_packs_removal_tests_all_there reports whether each of the paths names a file that exists, or each none that does. */
static bool
chunks_packs_removal_tests_all_there(const StdPathBuf *paths, size_t len, bool there)
{
	for (size_t k = 0; k < len; k++)
		if (std_path_Path_exists(paths[k].bytes.ptr, paths[k].bytes.len) != there)
			return false;
	return true;
}

/* chunks_packs_removal_tests_Paths_drop frees the paths. */
static void
chunks_packs_removal_tests_Paths_drop(StdPathBuf *paths, size_t len)
{
	for (size_t k = 0; k < len; k++)
		std_vec_free_Checked(&paths[k].bytes, 1, 1);
}

/* chunks_packs_removal_tests_queued queues files in a reservation, which it takes, and unwraps at a line and column. */
static void
chunks_packs_removal_tests_queued(HostIoIo *io, ChunksPacksRemovalReservation *reservation, uint32 repository,
								  const ChunksPacksRemovalFile *files, size_t files_len, uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_removal_queue(io, reservation, repository, files, files_len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/*
 * test_chunks_packs_removal_tests_a_queued_file_is_removed_once_every_reader_that_may_hold_it_has_ended:
 * a queued file stays while a reader that began before its queueing lives,
 * and is removed, with its entry freed, by whichever process removes what is
 * due once that reader has ended; a file already gone counts as removed.
 */
void
test_chunks_packs_removal_tests_a_queued_file_is_removed_once_every_reader_that_may_hold_it_has_ended(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	ChunksPacksRemovalFile queued[3];
	StdPathBuf	paths[3];
	HostSharedReader reader;
	ChunksPacksRemovalReservation reservation;
	StdIoError	io_error;
	size_t		entries;
	size_t		three = 3;

	chunks_packs_fixtures_created(io, 71, &packs);
	(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_removal_tests_files(io, &packs, 1, queued, paths);
	reader = chunks_packs_tests_reading(RUST_FILE, io, 220, 45);
	reservation = chunks_packs_removal_tests_reserved(io, 3, 221, 42);
	entries = chunks_packs_removal_Reservation_entries(&reservation);
	STD_ASSERT_EQ(222, 9, entries == three, &entries, std_usize_Debug_fmt, &three,
				  std_usize_Debug_fmt);
	chunks_packs_removal_tests_queued(io, &reservation, 71, queued, lengthof(queued), 223, 45);
	chunks_packs_removal_tests_removed(io, 224);
	STD_ASSERT_MSG(225, 9, chunks_packs_removal_tests_all_there(paths, lengthof(paths), true),
				   "a file a reader may hold was removed");
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED - 3, 226);
	if (!std_fs_remove_file(paths[1].bytes.ptr, paths[1].bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(228, 41, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	host_shared_Reader_drop(&reader);
	chunks_packs_removal_tests_removed(io, 230);
	STD_ASSERT(231, 9, chunks_packs_removal_tests_all_there(paths, lengthof(paths), false),
			   "paths.iter().all(|path| !path.exists())");
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED, 232);
	chunks_packs_removal_tests_Paths_drop(paths, lengthof(paths));
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_removal_tests_refused reports whether a queueing, which takes its reservation, is refused as an invariant's error. */
static bool
chunks_packs_removal_tests_refused(HostIoIo *io, ChunksPacksRemovalReservation *reservation,
								   const ChunksPacksRemovalFile *files, size_t files_len)
{
	ChunksErrorError error;
	bool		refused;

	if (chunks_packs_removal_queue(io, reservation, 72, files, files_len, &error))
		return false;
	refused = error.tag == CHUNKS_ERROR_ERROR_INVARIANT;
	chunks_error_Error_drop(&error);
	return refused;
}

/*
 * test_chunks_packs_removal_tests_a_reservation_takes_room_until_it_is_filled_or_dropped:
 * a reservation takes room from every other until it is filled or dropped: a
 * second gets what is left, a queueing fills only as many entries as it has
 * files for and gives the rest back, and files past their reservation, or by
 * chunks' own number for a repository's directory, are refused with nothing
 * queued.
 */
void
test_chunks_packs_removal_tests_a_reservation_takes_room_until_it_is_filled_or_dropped(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reader = chunks_packs_tests_reading(RUST_FILE, io, 242, 46);
	ChunksPacksRemovalReservation first = chunks_packs_removal_tests_reserved(io, CHUNKS_PACKS_SINGLE_QUEUED - 1, 243,
																			  56);
	ChunksPacksRemovalReservation second = chunks_packs_removal_tests_reserved(io, 5, 243, 81);
	ChunksPacksRemovalReservation one;
	ChunksPacksRemovalReservation three;
	ChunksPacksRemovalFile files[2];
	ChunksPacksRemovalFile unnamed;
	bool		refused;

	size_t		first_entries = chunks_packs_removal_Reservation_entries(&first);
	size_t		second_entries = chunks_packs_removal_Reservation_entries(&second);

	if (first_entries != CHUNKS_PACKS_SINGLE_QUEUED - 1 || second_entries != 1)
	{
		size_t		want_first = CHUNKS_PACKS_SINGLE_QUEUED - 1;
		size_t		want_second = 1;
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&first_entries, std_usize_Debug_fmt),
									STD_ITEM(&second_entries, std_usize_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&want_first, std_usize_Debug_fmt),
									 STD_ITEM(&want_second, std_usize_Debug_fmt));

		std_assert_failed(RUST_FILE, 244, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	chunks_packs_removal_Reservation_drop(&first);
	files[0] = chunks_packs_removal_tests_file(HOST_PG_DEFAULT, CHUNKS_PACKS_SINGLE_QUEUED_PACK, 0, chunks_pack_PackId_new());
	files[1] = chunks_packs_removal_tests_file(HOST_PG_DEFAULT, CHUNKS_PACKS_SINGLE_QUEUED_RUN, 0, chunks_pack_PackId_new());
	refused = chunks_packs_removal_tests_refused(io, &second, files, lengthof(files));
	STD_ASSERT(248, 9, refused, "matches!(refused, Err(Error::Invariant(_)))");
	one = chunks_packs_removal_tests_reserved(io, 1, 249, 34);
	unnamed = chunks_packs_removal_tests_file(CHUNKS_PACKS_DEFAULT_SPACE, CHUNKS_PACKS_SINGLE_QUEUED_PACK, 0, chunks_pack_PackId_new());
	refused = chunks_packs_removal_tests_refused(io, &one, &unnamed, 1);
	STD_ASSERT(251, 9, refused, "matches!(unnamed, Err(Error::Invariant(_)))");
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED, 252);
	three = chunks_packs_removal_tests_reserved(io, 3, 254, 36);
	files[0].f2 = chunks_pack_PackId_new();
	chunks_packs_removal_tests_queued(io, &three, 72, files, 1, 255, 53);
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED - 1, 256);
	host_shared_Reader_drop(&reader);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_removal_tests_a_removal_that_fails_is_returned_with_its_entry_left_queued:
 * a removal that fails is returned with its entry left queued, and the next
 * removal takes the file.
 */
void
test_chunks_packs_removal_tests_a_removal_that_fails_is_returned_with_its_entry_left_queued(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	ChunksPacksRemovalFile queued[3];
	StdPathBuf	paths[3];
	ChunksPacksRemovalReservation reservation;
	StdPathBuf	data;
	SimcSim		sim;
	ChunksErrorError error;
	bool		failed;

	chunks_packs_fixtures_created(io, 73, &packs);
	(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_removal_tests_files(io, &packs, 1, queued, paths);
	reservation = chunks_packs_removal_tests_reserved(io, 3, 267, 42);
	chunks_packs_removal_tests_queued(io, &reservation, 73, queued, lengthof(queued), 268, 45);
	data = simc_data_dir();
	sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 17);
	std_vec_free_Checked(&data.bytes, 1, 1);
	simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + 1);
	failed = !chunks_packs_removal_remove_due(io, &error);
	if (failed)
		chunks_error_Error_drop(&error);
	STD_ASSERT(271, 9, failed, "remove_due(io).is_err()");
	simc_Sim_stop(&sim);
	STD_ASSERT(273, 9, chunks_packs_removal_tests_all_there(paths, lengthof(paths), true),
			   "paths.iter().all(|path| path.exists())");
	chunks_packs_removal_tests_removed(io, 274);
	STD_ASSERT(275, 9, chunks_packs_removal_tests_all_there(paths, lengthof(paths), false),
			   "paths.iter().all(|path| !path.exists())");
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED, 276);
	simc_Sim_drop(&sim);
	chunks_packs_removal_tests_Paths_drop(paths, lengthof(paths));
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_removal_tests_holder is the holder actor: a reservation it holds when it is terminated. */
static bool
chunks_packs_removal_tests_holder(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRemovalReservation rest;
	ChunksErrorError failed;

	(void) captures;
	if (!chunks_packs_removal_reserve(io, 10, &reservation, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	if (!chunks_packs_removal_reserve(io, SIZE_MAX, &rest, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		chunks_packs_removal_Reservation_drop(&reservation);
		return false;
	}
	if (chunks_packs_removal_Reservation_entries(&reservation) != 10 || chunks_packs_removal_Reservation_entries(&rest) != CHUNKS_PACKS_SINGLE_QUEUED - 10)
	{
		*error = chunks_packs_tests_failed_text("a reservation took no room");
		chunks_packs_removal_Reservation_drop(&rest);
		chunks_packs_removal_Reservation_drop(&reservation);
		return false;
	}
	chunks_packs_removal_Reservation_drop(&rest);
	for (;;)
		(void) host_unique();
}

/*
 * test_chunks_packs_removal_tests_a_process_terminated_with_a_reservation_gives_its_room_back:
 * a process terminated between its reservation and its last step leaves the
 * queue's room as it was before it reserved, since its exit zeroes its word.
 */
void
test_chunks_packs_removal_tests_a_process_terminated_with_a_reservation_gives_its_room_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	holder = {chunks_packs_removal_tests_holder, NULL};
	SimcRun		run = simc_Run_new(9);
	SimcReport	report;

	simc_Run_actor(&run, "holder", 6, holder);
	simc_Run_terminate_at(&run, "holder", 6, 4000);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 300, 9, &arguments);
	}
	simc_Report_drop(&report);
	chunks_packs_removal_tests_room_eq(io, CHUNKS_PACKS_SINGLE_QUEUED, 301);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
