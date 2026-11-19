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
 * A collection's marking of a repository's packs (chunks' packs/marking.rs,
 * design item 6): from its start to its end, a locate that finds a chunk in a
 * pack the marking covers records the hit in a slot of chunks' single
 * record, under the repository's barrier held shared.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/marking.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "marking.rs"

/* chunks_TupleOptionTupleU32U64PathBuf_t is an unfinished pack's file: the process and number its name carries, if it does, and its path. */
typedef struct chunks_TupleOptionTupleU32U64PathBuf_t
{
	bool		has;
	uint32		process;
	uint64		number;
	StdPathBuf	path;
} chunks_TupleOptionTupleU32U64PathBuf_t;

/* chunks_TupleU32PathBufPackId_t is a pack file in a tablespace's directory: the tablespace, the directory, and the pack's id. */
typedef struct chunks_TupleU32PathBufPackId_t
{
	uint32		f0;
	StdPathBuf	f1;
	ChunksPackPackId f2;
} chunks_TupleU32PathBufPackId_t;

/* chunks_packs_marking_host_failed returns a host error as a chunks error. */
static bool
chunks_packs_marking_host_failed(HostError *host, ChunksErrorError *error)
{
	*error = chunks_error_Error_FromError_from(host);
	return false;
}

/* chunks_packs_marking_marking_began is MarkingBegan's event, of the newest pack the marking covers. */
static void
chunks_packs_marking_marking_began(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN;
	result->u.marking_began.newest = *(const uint64 *) captures;
}

/* chunks_packs_marking_marking_cleared is MarkingCleared's event. */
static void
chunks_packs_marking_marking_cleared(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_MARKING_CLEARED;
}

/* chunks_packs_marking_marking_ended is MarkingEnded's event. */
static void
chunks_packs_marking_marking_ended(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_MARKING_ENDED;
}

/* chunks_packs_marking_remove_strays_frame_0_t is remove_strays' state across its pad: the directories, the names, the unfinished files, and the slots. */
typedef struct chunks_packs_marking_remove_strays_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	StdVec		dirs;
	bool		dirs_live;
	StdVec		names;
	bool		names_live;

	/*
	 * unfinished is a Vec<(Option<(u32, u64)>, PathBuf)> of the unfinished
	 * packs' files.
	 */
	StdVec		unfinished;
	bool		unfinished_live;
	StdVec		slots;
	bool		slots_live;
	chunks_TupleOptionTupleU32U64PathBuf_t file;
	bool		file_live;
} chunks_packs_marking_remove_strays_frame_0_t;

/* chunks_packs_marking_remove_strays_pad_0 is remove_strays' pad: what it holds, the latest first. */
static void
chunks_packs_marking_remove_strays_pad_0(void *frame_)
{
	chunks_packs_marking_remove_strays_frame_0_t *frame = frame_;

	if (frame->file_live)
	{
		frame->file_live = false;
		std_vec_free_Checked(&frame->file.path.bytes, 1, 1);
	}
	if (frame->slots_live)
	{
		frame->slots_live = false;
		std_vec_free_Checked(&frame->slots, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->unfinished_live)
	{
		frame->unfinished_live = false;
		for (size_t k = 0, k_len = frame->unfinished.len; k < k_len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->unfinished, chunks_TupleOptionTupleU32U64PathBuf_t, k).path.bytes,
								 1, 1);
		std_vec_free_Checked(&frame->unfinished, sizeof(chunks_TupleOptionTupleU32U64PathBuf_t),
							 _Alignof(chunks_TupleOptionTupleU32U64PathBuf_t));
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (frame->dirs_live)
	{
		frame->dirs_live = false;
		std_vec_drop_Checked(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	}
}

/* chunks_packs_marking_ends_with reports whether a name ends with a text. */
static bool
chunks_packs_marking_ends_with(const StdString *name, const char *text)
{
	size_t		len = strlen(text);

	return name->len >= len && memcmp((const char *) name->ptr + name->len - len, text, len) == 0;
}

/*
 * chunks_packs_marking_remove_strays_body_0 is remove_strays' body: the
 * unfinished packs' files listed first, then each one no process's writers'
 * word covers removed, its failure only logged.
 */
static void
chunks_packs_marking_remove_strays_body_0(void *frame_)
{
	chunks_packs_marking_remove_strays_frame_0_t *frame = frame_;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_open_directories(HOST_IO_IO_READ_DOORS(frame->io), (uint32) frame->self->repository,
									   frame->self->own, &frame->dirs, frame->error))
		return;
	frame->dirs_live = true;
	std_vec_new(&frame->unfinished, sizeof(chunks_TupleOptionTupleU32U64PathBuf_t),
				_Alignof(chunks_TupleOptionTupleU32U64PathBuf_t));
	frame->unfinished_live = true;
	for (size_t d = 0, d_len = frame->dirs.len; d < d_len; d++)
	{
		const StdPathBuf *dir = &STD_VEC_AT(&frame->dirs, StdPathBuf, d);

		if (!chunks_file_file_names(HOST_IO_IO_READ_DOORS(frame->io), dir->bytes.ptr, dir->bytes.len, &frame->names,
									frame->error))
			goto done;
		frame->names_live = true;
		for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
			chunks_TupleOptionTupleU32U64PathBuf_t *file = &frame->file;

			if (!chunks_packs_marking_ends_with(name, CHUNKS_PACK_TEMP))
				continue;
			file->has = chunks_pack_temp_of(name->ptr, name->len, &file->process, &file->number);
			std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, &file->path);
			frame->file_live = true;
			std_vec_push_Checked(&frame->unfinished, file, sizeof(*file),
								 _Alignof(chunks_TupleOptionTupleU32U64PathBuf_t));
			frame->file_live = false;
		}
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-chunks-strays-listed", &host))
	{
		(void) chunks_packs_marking_host_failed(&host, frame->error);
		goto done;
	}
	if (!chunks_packs_writers_slots(&frame->slots, frame->error))
		goto done;
	frame->slots_live = true;
	for (size_t k = 0, k_len = frame->unfinished.len; k < k_len; k++)
	{
		const chunks_TupleOptionTupleU32U64PathBuf_t *file =
			&STD_VEC_AT(&frame->unfinished, chunks_TupleOptionTupleU32U64PathBuf_t, k);
		bool		covered = false;

		if (file->has && !chunks_packs_writers_covers(frame->slots.ptr, frame->slots.len, file->process,
													  frame->self->repository, file->number, &covered, frame->error))
			goto done;
		if (!covered)
			host_remove_leftover(HOST_IO_IO_IO_DOORS(frame->io), file->path.bytes.ptr, file->path.bytes.len);
	}
	frame->ok = true;
done:
	chunks_packs_marking_remove_strays_pad_0(frame);
}

/*
 * chunks_packs_marking_Packs_remove_strays removes each unfinished pack's
 * file in the repository's directories that no process's writers' word
 * covers, and each whose name carries no numbers. It lists the directories
 * first and reads the words afterwards, so a file it lists was made, and its
 * writer's word stored, before the read. A removal that fails is reported in
 * the server's log, and the next collection lists the file again.
 */
static bool
chunks_packs_marking_Packs_remove_strays(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_marking_remove_strays_frame_0_t frame = {.self = self, .io = io, .error = error};

	std_landing(&frame, chunks_packs_marking_remove_strays_body_0, chunks_packs_marking_remove_strays_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_strays_t is relist_strays' changes' captures: the packs, the files found, the queue's ids, and the strays. */
typedef struct chunks_packs_marking_strays_t
{
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
	/* found is a Vec<(u32, PathBuf, PackId)> of the pack files found. */
	StdVec	   *found;
	/* queued is the BTreeSet<PackId> of the queue's ids, a vector in order. */
	const StdVec *queued;
	/* strays is the Vec<(u32, PathBuf, PackId)> of the files no list names. */
	StdVec	   *strays;
	uint32		space;
	ChunksPackPackId id;
	uint64		fresh;
} chunks_packs_marking_strays_t;

/* chunks_packs_marking_lists_id reports whether a view's listed packs hold one of an id. */
static bool
chunks_packs_marking_lists_id(const ChunksPacksViewView *view, const ChunksPackPackId *id)
{
	StdVec		listed;
	bool		found = false;

	chunks_packs_view_View_listed(view, &listed);
	for (size_t k = 0, k_len = listed.len; k < k_len && !found; k++)
		found = chunks_pack_PackId_PartialEq_eq(&STD_VEC_AT(&listed, ChunksPacksTableEntry, k).f2, id);
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	return found;
}

/* chunks_packs_marking_queued_holds reports whether the queue's ids, a vector in order, hold one. */
static bool
chunks_packs_marking_queued_holds(const StdVec *queued, const ChunksPackPackId *id)
{
	for (size_t low = 0, high = queued->len; low < high;)
	{
		size_t		middle = low + (high - low) / 2;
		StdOrdering order = chunks_pack_PackId_Ord_cmp(&STD_VEC_AT(queued, ChunksPackPackId, middle), id);

		if (order == STD_ORDERING_EQUAL)
			return true;
		if (order == STD_ORDERING_LESS)
			low = middle + 1;
		else
			high = middle;
	}
	return false;
}

/* chunks_packs_marking_strays is relist_strays' first change: the files found that no list names and the queue doesn't hold, while no writer holds a number of the repository. */
static bool
chunks_packs_marking_strays(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	chunks_packs_marking_strays_t *strays = captures;
	bool		writing;

	if (!chunks_packs_writers_writing_in(strays->packs->repository, &writing, error))
		return false;
	if (writing)
		return true;
	for (size_t k = 0, k_len = strays->found->len; k < k_len; k++)
	{
		chunks_TupleU32PathBufPackId_t *found = &STD_VEC_AT(strays->found, chunks_TupleU32PathBufPackId_t, k);

		if (chunks_packs_marking_lists_id(change->view, &found->f2) ||
			chunks_packs_marking_queued_holds(strays->queued, &found->f2))
			continue;
		std_vec_push_Checked(strays->strays, found, sizeof(*found), _Alignof(chunks_TupleU32PathBufPackId_t));
		std_PathBuf_new(&found->f1);
	}
	return true;
}

/* chunks_packs_marking_reserved is relist_strays' reserved: an op on the record's Reserved. */
static uint64
chunks_packs_marking_reserved(void *captures, HostSharedOp op)
{
	const chunks_packs_marking_strays_t *strays = captures;

	return host_shared_Record_atomic(host_shared_Held_record(&strays->packs->held), CHUNKS_PACKS_SHARED_RESERVED_INDEX,
									 op);
}

/* chunks_packs_marking_reserve is relist_strays' raise's change: the bound raised past the one read. */
static bool
chunks_packs_marking_reserve(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	return chunks_packs_lists_Change_reserve(change, *(const uint64 *) captures, error);
}

/* chunks_packs_marking_raise is relist_strays' raise: a change that raises the bound of the writers' numbers. */
static bool
chunks_packs_marking_raise(void *captures, HostIoIo *io, uint64 numbers, ChunksErrorError *error)
{
	const chunks_packs_marking_strays_t *strays = captures;

	return chunks_packs_backend_Packs_changed(strays->packs, HOST_IO_IO_TAKES_SHORT(io), strays->reading,
											  chunks_packs_marking_reserve, &numbers, error);
}

/* chunks_packs_marking_relisted is relist_strays' second change: the stray added under its fresh number while no listed pack has its id. */
static bool
chunks_packs_marking_relisted(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	const chunks_packs_marking_strays_t *strays = captures;
	ChunksPacksTableEntry entry = {strays->fresh, strays->space, strays->id};

	if (chunks_packs_marking_lists_id(change->view, &strays->id))
		return true;
	return chunks_packs_lists_Change_add(change, &entry, false, error);
}

/* chunks_packs_marking_relisted_event is Relisted's event, of the stray's fresh number and its id. */
static void
chunks_packs_marking_relisted_event(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_marking_strays_t *strays = captures;

	result->tag = CHUNKS_TRACE_EVENT_RELISTED;
	result->u.relisted.pack = strays->fresh;
	result->u.relisted.id = strays->id;
}

/* chunks_packs_marking_relist_strays_frame_0_t is relist_strays' state across its pad: the directories, the names, the files found, the queue's ids, the strays, the hold, and the paths. */
typedef struct chunks_packs_marking_relist_strays_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	ChunksErrorError *error;
	bool		ok;

	/*
	 * spaced is a Vec<(u32, PathBuf)> of the repository's directories by
	 * tablespace.
	 */
	StdVec		spaced;
	bool		spaced_live;
	StdVec		names;
	bool		names_live;
	StdVec		found;
	bool		found_live;
	StdVec		queued;
	bool		queued_live;
	StdVec		strays;
	bool		strays_live;
	ChunksPacksWritersWriting hold;
	bool		hold_live;
	StdPathBuf	pack;
	bool		pack_live;
	StdPathBuf	run;
	bool		run_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdString	name;
	bool		name_live;
	chunks_TupleU32PathBufPackId_t file;
	bool		file_live;
} chunks_packs_marking_relist_strays_frame_0_t;

/* chunks_packs_marking_Found_drop frees a Vec<(u32, PathBuf, PackId)>. */
static void
chunks_packs_marking_Found_drop(StdVec *found)
{
	for (size_t k = 0, k_len = found->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(found, chunks_TupleU32PathBufPackId_t, k).f1.bytes, 1, 1);
	std_vec_free_Checked(found, sizeof(chunks_TupleU32PathBufPackId_t), _Alignof(chunks_TupleU32PathBufPackId_t));
}

/* chunks_packs_marking_relist_strays_pad_0 is relist_strays' pad: what it holds, the latest first. */
static void
chunks_packs_marking_relist_strays_pad_0(void *frame_)
{
	chunks_packs_marking_relist_strays_frame_0_t *frame = frame_;

	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
	if (frame->file_live)
	{
		frame->file_live = false;
		std_vec_free_Checked(&frame->file.f1.bytes, 1, 1);
	}
	if (frame->lost_live)
	{
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
	}
	if (frame->run_live)
	{
		frame->run_live = false;
		std_vec_free_Checked(&frame->run.bytes, 1, 1);
	}
	if (frame->pack_live)
	{
		frame->pack_live = false;
		std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(&frame->hold);
	}
	if (frame->strays_live)
	{
		frame->strays_live = false;
		chunks_packs_marking_Found_drop(&frame->strays);
	}
	if (frame->queued_live)
	{
		frame->queued_live = false;
		std_vec_free_Checked(&frame->queued, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		chunks_packs_marking_Found_drop(&frame->found);
	}
	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (frame->spaced_live)
	{
		frame->spaced_live = false;
		for (size_t k = 0, k_len = frame->spaced.len; k < k_len; k++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->spaced, chunks_TupleU32PathBuf_t, k).f1.bytes, 1, 1);
		std_vec_free_Checked(&frame->spaced, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	}
}

/* chunks_packs_marking_join joins a pack's file name to a directory into a path the frame holds. */
static void
chunks_packs_marking_join(const StdPathBuf *dir, const ChunksPackPackId *id, const char *extension, StdString *name,
						  bool *name_live, StdPathBuf *result, bool *live)
{
	*name = chunks_pack_PackId_file_name(id, extension, strlen(extension));
	*name_live = true;
	std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, result);
	*live = true;
	*name_live = false;
	std_vec_free_Checked(name, 1, 1);
}

/*
 * chunks_packs_marking_relist_strays_body_0 is relist_strays' body: the pack
 * files found in the repository's directories, those no list names and the
 * queue doesn't hold taken under the lists lock, and each listed again under
 * a fresh number with its run written again.
 */
static void
chunks_packs_marking_relist_strays_body_0(void *frame_)
{
	chunks_packs_marking_relist_strays_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	chunks_packs_marking_strays_t strays = {.packs = self, .reading = frame->reading, .found = &frame->found,
	.queued = &frame->queued, .strays = &frame->strays};

	frame->ok = false;
	if (!chunks_packs_open_spaced_directories(HOST_IO_IO_READ_DOORS(frame->io), (uint32) self->repository, self->own,
											  &frame->spaced, frame->error))
		return;
	frame->spaced_live = true;
	std_vec_new(&frame->found, sizeof(chunks_TupleU32PathBufPackId_t), _Alignof(chunks_TupleU32PathBufPackId_t));
	frame->found_live = true;
	for (size_t d = 0, d_len = frame->spaced.len; d < d_len; d++)
	{
		const chunks_TupleU32PathBuf_t *spaced = &STD_VEC_AT(&frame->spaced, chunks_TupleU32PathBuf_t, d);

		if (!chunks_file_file_names(HOST_IO_IO_READ_DOORS(frame->io), spaced->f1.bytes.ptr, spaced->f1.bytes.len,
									&frame->names, frame->error))
			goto done;
		frame->names_live = true;
		for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

			frame->file.f0 = spaced->f0;
			if (!chunks_packs_marking_ends_with(name, ".pack") ||
				!chunks_pack_PackId_of_name(name->ptr, name->len - strlen(".pack"), &frame->file.f2))
				continue;
			std_PathBuf_Clone_clone(&spaced->f1, &frame->file.f1);
			frame->file_live = true;
			std_vec_push_Checked(&frame->found, &frame->file, sizeof(frame->file),
								 _Alignof(chunks_TupleU32PathBufPackId_t));
			frame->file_live = false;
		}
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (frame->found.len == 0)
	{
		frame->ok = true;
		goto done;
	}
	if (!chunks_packs_removal_queued_packs(frame->io, (uint32) self->repository, &frame->queued, frame->error))
		goto done;
	frame->queued_live = true;
	std_vec_new(&frame->strays, sizeof(chunks_TupleU32PathBufPackId_t), _Alignof(chunks_TupleU32PathBufPackId_t));
	frame->strays_live = true;
	if (!chunks_packs_backend_Packs_changed(self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading,
											chunks_packs_marking_strays, &strays, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->strays.len; k < k_len; k++)
	{
		const chunks_TupleU32PathBufPackId_t *stray = &STD_VEC_AT(&frame->strays, chunks_TupleU32PathBufPackId_t, k);

		strays.space = stray->f0;
		strays.id = stray->f2;
		if (!chunks_packs_writers_Writing_open(frame->io, self->repository, stray->f0, chunks_packs_marking_reserved,
											   &strays, NULL, chunks_packs_marking_raise, &strays, NULL, &frame->hold,
											   frame->error))
			goto done;
		frame->hold_live = true;
		strays.fresh = chunks_packs_writers_Writing_number(&frame->hold);
		chunks_packs_marking_join(&stray->f1, &stray->f2, "pack", &frame->name, &frame->name_live, &frame->pack,
								  &frame->pack_live);
		chunks_packs_marking_join(&stray->f1, &stray->f2, "idx", &frame->name, &frame->name_live, &frame->run,
								  &frame->run_live);
		if (!chunks_packs_settle_rebuilt(frame->io, frame->pack.bytes.ptr, frame->pack.bytes.len, strays.fresh,
										 frame->run.bytes.ptr, frame->run.bytes.len, &frame->lost, frame->error))
			goto done;
		frame->lost_live = true;
		if (!chunks_packs_backend_Packs_changed(self, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading,
												chunks_packs_marking_relisted, &strays, frame->error))
			goto done;
		frame->lost_live = false;
		chunks_pack_Lost_drop(&frame->lost);
		frame->run_live = false;
		std_vec_free_Checked(&frame->run.bytes, 1, 1);
		frame->pack_live = false;
		std_vec_free_Checked(&frame->pack.bytes, 1, 1);
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(&frame->hold);
		chunks_trace_emit(self->repository, chunks_packs_marking_relisted_event, &strays, NULL);
	}
	frame->ok = true;
done:
	chunks_packs_marking_relist_strays_pad_0(frame);
}

/*
 * chunks_packs_marking_Packs_relist_strays lists again under a fresh number
 * each pack file in the repository's directories that the pack table
 * doesn't list and the removal queue doesn't hold, as a crash between a
 * retirement and its queueing leaves them, so a collection takes each as an
 * ordinary pack. While any process's writers hold a number of the repository
 * it lists none. Each stray's run is written again from the pack for its
 * fresh number, and it is added only while the table still lists no pack of
 * its id.
 */
static bool
chunks_packs_marking_Packs_relist_strays(const ChunksPacksBackendPacks *self, HostIoIo *io,
										 const HostSharedReader *reading, ChunksErrorError *error)
{
	chunks_packs_marking_relist_strays_frame_0_t frame = {.self = self, .io = io, .reading = reading, .error = error};

	std_landing(&frame, chunks_packs_marking_relist_strays_body_0, chunks_packs_marking_relist_strays_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_begin_frame_0_t is begin_marking's state across its pad: the numbers listed, the barriers, and the other record. */
typedef struct chunks_packs_marking_begin_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	uint64	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedLongGuard barrier;
	bool		barrier_live;
	HostSharedHeld other;
	bool		other_live;
} chunks_packs_marking_begin_frame_0_t;

/* chunks_packs_marking_begin_pad_0 is begin_marking's pad: the barrier, then the other record. */
static void
chunks_packs_marking_begin_pad_0(void *frame_)
{
	chunks_packs_marking_begin_frame_0_t *frame = frame_;

	if (frame->barrier_live)
	{
		frame->barrier_live = false;
		host_shared_LongGuard_drop(&frame->barrier);
	}
	if (frame->other_live)
	{
		frame->other_live = false;
		host_shared_Held_drop(&frame->other);
	}
}

/*
 * chunks_packs_marking_begin_body_0 is begin_marking's body: the strays
 * removed and listed again, then the hits named for the repository's record
 * under its barrier, a name another collector left cleared first under its
 * repository's barrier.
 */
static void
chunks_packs_marking_begin_body_0(void *frame_)
{
	chunks_packs_marking_begin_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedRecord record = host_shared_Held_record(&self->held);
	HostSharedRecord hits;
	HostError	host;
	StdVec		numbers;
	uint64		newest;

	frame->ok = false;
	if (!chunks_packs_marking_Packs_remove_strays(self, frame->io, frame->error) ||
		!chunks_packs_marking_Packs_relist_strays(self, frame->io, frame->reading, frame->error))
		return;
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &hits, &host))
	{
		(void) chunks_packs_marking_host_failed(&host, frame->error);
		return;
	}
	if (!chunks_packs_backend_Packs_numbers(self, frame->io, frame->reading, &numbers, frame->error))
		return;
	newest = numbers.len == 0 ? 0 : STD_VEC_AT(&numbers, uint64, numbers.len - 1);
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	for (;;)
	{
		bool		marked;
		bool		has;
		uint64		left;
		HostSharedRecord other;

		if (!host_shared_Record_lock_long(record, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SHARED_BARRIER_INDEX,
										  &frame->barrier, &host))
			goto failed;
		frame->barrier_live = true;
		if (!host_shared_Record_mark(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->barrier, &marked, &host))
			goto failed;
		if (marked)
		{
			chunks_packs_shared_set(record, CHUNKS_PACKS_SHARED_MARKED_INDEX, STD_ADD(u64, newest, 1, 53, 39));
			chunks_trace_emit(self->repository, chunks_packs_marking_marking_began, &newest, NULL);
			*frame->result = newest;
			frame->ok = true;
			goto done;
		}
		frame->barrier_live = false;
		host_shared_LongGuard_drop(&frame->barrier);
		if (!host_shared_Record_named(hits, CHUNKS_PACKS_SINGLE_HITS, &left))
			continue;
		if (!host_shared_open(HOST_SHARED_IO_WITHIN(frame->io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, left, &has,
							  &frame->other, &host))
			goto failed;
		if (!has)
			continue;
		frame->other_live = true;
		other = host_shared_Held_record(&frame->other);
		if (!host_shared_Record_lock_long(other, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SHARED_BARRIER_INDEX,
										  &frame->barrier, &host))
			goto failed;
		frame->barrier_live = true;
		chunks_packs_shared_set(other, CHUNKS_PACKS_SHARED_MARKED_INDEX, 0);
		if (!host_shared_Record_unmark(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->barrier, &host))
			goto failed;
		chunks_trace_emit(chunks_packs_shared_load(other, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX),
						  chunks_packs_marking_marking_cleared, NULL, NULL);
		chunks_packs_marking_begin_pad_0(frame);
	}

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_marking_begin_pad_0(frame);
}

/*
 * chunks_packs_marking_Packs_begin_marking starts a collection's marking of
 * the repository's packs and returns the newest pack number it covers, the
 * newest listed: it names the repository's record as the one whose barrier
 * guards the hits, which empties them, and from then a locate records each
 * chunk it finds in a pack numbered at or below that. A name left by a
 * collector that was terminated in its marking is cleared first under that
 * repository's barrier. Before any of it, with nothing held and nothing
 * marked, it removes the unfinished packs no writer has open.
 */
bool
chunks_packs_marking_Packs_begin_marking(const ChunksPacksBackendPacks *self, HostIoIo *io,
										 const HostSharedReader *reading, uint64 *result, ChunksErrorError *error)
{
	chunks_packs_marking_begin_frame_0_t frame = {.self = self, .io = io, .reading = reading, .result = result,
	.error = error};

	std_landing(&frame, chunks_packs_marking_begin_body_0, chunks_packs_marking_begin_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_barrier_frame_0_t is end_marking's and hits' state across their pads: the barrier, and hits' slots and hits. */
typedef struct chunks_packs_marking_barrier_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedLongGuard barrier;
	bool		barrier_live;
	StdVec		slots;
	bool		slots_live;
	StdVec		recorded;
	bool		recorded_live;
} chunks_packs_marking_barrier_frame_0_t;

/* chunks_packs_marking_barrier_pad_0 is end_marking's and hits' pad: the hits, the slots, then the barrier. */
static void
chunks_packs_marking_barrier_pad_0(void *frame_)
{
	chunks_packs_marking_barrier_frame_0_t *frame = frame_;

	if (frame->recorded_live)
	{
		frame->recorded_live = false;
		std_vec_free_Checked(&frame->recorded, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->slots_live)
	{
		frame->slots_live = false;
		chunks_pack_VecVecU8_drop(&frame->slots);
	}
	if (frame->barrier_live)
	{
		frame->barrier_live = false;
		host_shared_LongGuard_drop(&frame->barrier);
	}
}

/* chunks_packs_marking_barrier takes the repository's barrier exclusively, and chunks' single record. */
static bool
chunks_packs_marking_barrier(chunks_packs_marking_barrier_frame_0_t *frame, HostSharedRecord *hits, HostError *host)
{
	if (!host_shared_Record_lock_long(host_shared_Held_record(&frame->self->held), HOST_IO_IO_TAKES_SHORT(frame->io),
									  CHUNKS_PACKS_SHARED_BARRIER_INDEX, &frame->barrier, host))
		return false;
	frame->barrier_live = true;
	return host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, hits, host);
}

/* chunks_packs_marking_end_body_0 is end_marking's body: Marked cleared and the hits' name taken away under the barrier. */
static void
chunks_packs_marking_end_body_0(void *frame_)
{
	chunks_packs_marking_barrier_frame_0_t *frame = frame_;
	HostSharedRecord hits;
	HostError	host;

	frame->ok = false;
	if (!host_shared_Record_lock_long(host_shared_Held_record(&frame->self->held), HOST_IO_IO_TAKES_SHORT(frame->io),
									  CHUNKS_PACKS_SHARED_BARRIER_INDEX, &frame->barrier, &host))
		goto failed;
	frame->barrier_live = true;
	chunks_packs_shared_set(host_shared_Held_record(&frame->self->held), CHUNKS_PACKS_SHARED_MARKED_INDEX, 0);
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &hits, &host) ||
		!host_shared_Record_unmark(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->barrier, &host))
		goto failed;
	chunks_trace_emit(frame->self->repository, chunks_packs_marking_marking_ended, NULL, NULL);
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_marking_barrier_pad_0(frame);
}

/* chunks_packs_marking_Packs_end_marking ends the repository's marking, for a collection that ends without retiring: a locate records nothing from then. */
bool
chunks_packs_marking_Packs_end_marking(const ChunksPacksBackendPacks *self, HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_marking_barrier_frame_0_t frame = {.self = self, .io = io, .error = error};

	std_landing(&frame, chunks_packs_marking_end_body_0, chunks_packs_marking_barrier_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_hits_body_0 is hits' body: the slots read under the barrier, each a hit. */
static void
chunks_packs_marking_hits_body_0(void *frame_)
{
	chunks_packs_marking_barrier_frame_0_t *frame = frame_;
	HostSharedRecord hits;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_marking_barrier(frame, &hits, &host) ||
		!host_shared_Record_read(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->barrier, &frame->slots, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->slots_live = true;
	std_vec_new(&frame->recorded, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->recorded_live = true;
	for (size_t k = 0, k_len = frame->slots.len; k < k_len; k++)
	{
		chunks_TupleAddressLocator_t hit;

		if (!chunks_packs_single_hit_of(STD_VEC_AT(&frame->slots, StdVec, k).ptr,
										STD_VEC_AT(&frame->slots, StdVec, k).len, &hit, frame->error))
			goto done;
		std_vec_push_Checked(&frame->recorded, &hit, sizeof(hit), _Alignof(chunks_TupleAddressLocator_t));
	}
	*frame->result = frame->recorded;
	frame->recorded_live = false;
	frame->ok = true;
done:
	chunks_packs_marking_barrier_pad_0(frame);
}

/* chunks_packs_marking_Packs_hits returns the hits locate recorded since the repository's marking began, in the order they were recorded. */
bool
chunks_packs_marking_Packs_hits(const ChunksPacksBackendPacks *self, HostIoIo *io, StdVec *result,
								ChunksErrorError *error)
{
	chunks_packs_marking_barrier_frame_0_t frame = {.self = self, .io = io, .result = result, .error = error};

	std_landing(&frame, chunks_packs_marking_hits_body_0, chunks_packs_marking_barrier_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_hit_recorded_t is HitRecorded's captures: the chunk's address and where it was found. */
typedef struct chunks_packs_marking_hit_recorded_t
{
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
} chunks_packs_marking_hit_recorded_t;

/* chunks_packs_marking_hit_recorded is HitRecorded's event. */
static void
chunks_packs_marking_hit_recorded(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_marking_hit_recorded_t *hit = captures;

	result->tag = CHUNKS_TRACE_EVENT_HIT_RECORDED;
	result->u.hit_recorded.address = *hit->address;
	result->u.hit_recorded.locator = hit->locator;
}

/* chunks_packs_marking_noted_frame_0_t is noted's state across its pad: the view's guard and the barrier. */
typedef struct chunks_packs_marking_noted_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
	bool	   *has_result;
	ChunksTrailerLocator *result;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
	HostSharedLongGuard barrier;
	bool		barrier_live;
} chunks_packs_marking_noted_frame_0_t;

/* chunks_packs_marking_noted_pad_0 is noted's pad: the barrier, then the view's guard. */
static void
chunks_packs_marking_noted_pad_0(void *frame_)
{
	chunks_packs_marking_noted_frame_0_t *frame = frame_;

	if (frame->barrier_live)
	{
		frame->barrier_live = false;
		host_shared_LongGuard_drop(&frame->barrier);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
}

/*
 * chunks_packs_marking_noted_body_0 is noted's body: under the barrier held
 * shared with the view current, the hit recorded when a marking covers the
 * place's pack, and no answer for a chunk found dead, for a pack retired
 * since, and for a hit past the slots a marking has.
 */
static void
chunks_packs_marking_noted_body_0(void *frame_)
{
	chunks_packs_marking_noted_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedRecord record = host_shared_Held_record(&self->held);
	ChunksPacksViewView *view;
	HostError	host;

	frame->ok = false;
	*frame->has_result = false;
	frame->view = chunks_packs_backend_Packs_view_lock(self, HOST_IO_IO_TAKES_SHORT(frame->io));
	frame->view_live = true;
	view = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	for (;;)
	{
		HostSharedLongIo barrier_io;
		HostSharedRecord hits;
		uint8		hit[CHUNKS_PACKS_SINGLE_HIT];
		bool		dead;
		bool		recorded;

		if (!chunks_packs_view_View_refresh(view, frame->io, record, frame->error))
			goto done;
		if (!host_shared_Record_lock_long_shared(record, frame->io, CHUNKS_PACKS_SHARED_BARRIER_INDEX, &frame->barrier,
												 &host))
			goto failed;
		frame->barrier_live = true;
		if (!view->has_current || view->current != chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_GENERATION_INDEX))
		{
			frame->barrier_live = false;
			host_shared_LongGuard_drop(&frame->barrier);
			continue;
		}
		if (chunks_packs_view_View_pack(view, frame->locator.pack) == NULL)
			break;
		barrier_io = host_shared_LongGuard_io(&frame->barrier);
		if (!chunks_packs_reads_View_is_dead(view, HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_LONG_IO_IO_DOORS(&barrier_io)),
											 frame->locator, &dead, frame->error))
			goto done;
		if (dead)
			break;
		if (frame->locator.pack >= chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_MARKED_INDEX))
		{
			*frame->has_result = true;
			*frame->result = frame->locator;
			break;
		}
		if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &hits, &host))
			goto failed;
		chunks_packs_single_hit(frame->address, frame->locator, hit);
		if (!host_shared_Record_claim(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->barrier, hit, sizeof(hit), &recorded,
									  &host))
			goto failed;
		if (recorded)
		{
			chunks_packs_marking_hit_recorded_t event = {frame->address, frame->locator};

			chunks_trace_emit(self->repository, chunks_packs_marking_hit_recorded, &event, NULL);
			*frame->has_result = true;
			*frame->result = frame->locator;
		}
		break;
	}
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_marking_noted_pad_0(frame);
}

/*
 * chunks_packs_marking_Packs_noted answers a locate with the place it found,
 * under the barrier held shared with the view current, so that no
 * retirement comes between its answer and its hit: it records the hit when
 * a marking covers the place's pack, and answers nothing, so that the writer
 * that asked writes a copy of its own, for a chunk a full collection found
 * dead, for a pack retired since the chunk was found, and for a hit past the
 * slots a marking has.
 */
static bool
chunks_packs_marking_Packs_noted(const ChunksPacksBackendPacks *self, HostIoIo *io, const ChunksAddressAddress *address,
								 bool found, ChunksTrailerLocator locator, bool *has_result,
								 ChunksTrailerLocator *result, ChunksErrorError *error)
{
	chunks_packs_marking_noted_frame_0_t frame = {.self = self, .io = io, .address = address, .locator = locator,
	.has_result = has_result, .result = result, .error = error};

	if (!found)
	{
		*has_result = false;
		return true;
	}
	std_landing(&frame, chunks_packs_marking_noted_body_0, chunks_packs_marking_noted_pad_0);
	return frame.ok;
}

/* chunks_packs_marking_Packs_locate returns where the chunk with the address is, as find does, for a writer that keeps the chunk rather than writing it again. */
bool
chunks_packs_marking_Packs_locate(const ChunksPacksBackendPacks *self, HostIoIo *io, const HostSharedReader *reading,
								  const ChunksAddressAddress *address, bool *has_result, ChunksTrailerLocator *result,
								  ChunksErrorError *error)
{
	ChunksTrailerLocator locator;
	bool		found;

	return chunks_packs_backend_Packs_find(self, io, reading, address, &found, &locator, error) &&
		chunks_packs_marking_Packs_noted(self, io, address, found, locator, has_result, result, error);
}

/*
 * chunks_packs_marking_Packs_locate_in returns where a tablespace's packs
 * hold the chunk with the address, as find_in does, for a writer into the
 * tablespace that keeps the chunk rather than writing it again.
 */
bool
chunks_packs_marking_Packs_locate_in(const ChunksPacksBackendPacks *self, HostIoIo *io,
									 const HostSharedReader *reading, const ChunksAddressAddress *address,
									 uint32 space, bool *has_result, ChunksTrailerLocator *result,
									 ChunksErrorError *error)
{
	ChunksTrailerLocator locator;
	bool		found;

	return chunks_packs_backend_Packs_find_in(self, io, reading, address, space, &found, &locator, error) &&
		chunks_packs_marking_Packs_noted(self, io, address, found, locator, has_result, result, error);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_marking_tests_Hit_Debug_fmt is an (Address, Locator)'s Debug. */
static bool
chunks_packs_marking_tests_Hit_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressLocator_t *hit = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&hit->f0, chunks_address_Address_Debug_fmt),
								  STD_ITEM(&hit->f1, chunks_trailer_Locator_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_packs_marking_tests_Hits_Debug_fmt is a Vec<(Address, Locator)>'s Debug. */
static bool
chunks_packs_marking_tests_Hits_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *hits = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = hits->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(hits, chunks_TupleAddressLocator_t, k),
							chunks_packs_marking_tests_Hit_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_marking_tests_answer_eq asserts at a line and column that an answer is the place given, or none when it is NULL. */
static void
chunks_packs_marking_tests_answer_eq(bool has, const ChunksTrailerLocator *found, const ChunksTrailerLocator *want,
									 uint32 line, uint32 col)
{
	if (has != (want != NULL) || (has && !chunks_trailer_Locator_PartialEq_eq(found, want)))
	{
		StdOptionRef got = {has, STD_ITEM(found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef wanted = {want != NULL, STD_ITEM(want, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, col, "==", &got, std_option_Debug_fmt, &wanted, std_option_Debug_fmt,
						  NULL);
	}
}

/* chunks_packs_marking_tests_located_eq asserts at a line that a locate, which unwraps at a column, answers the place given, or none when it is NULL. */
static void
chunks_packs_marking_tests_located_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									  const HostSharedReader *reading, const ChunksAddressAddress *address,
									  const ChunksTrailerLocator *want, uint32 line, uint32 assert_col,
									  uint32 unwrap_col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_marking_Packs_locate(packs, io, reading, address, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, unwrap_col);
	chunks_packs_marking_tests_answer_eq(has, &found, want, line, assert_col);
}

/* chunks_packs_marking_tests_hits returns the marking's hits, which unwraps at a line and column. */
static StdVec
chunks_packs_marking_tests_hits(const ChunksPacksBackendPacks *packs, HostIoIo *io, uint32 line, uint32 col)
{
	StdVec		hits;
	ChunksErrorError error;

	if (!chunks_packs_marking_Packs_hits(packs, io, &hits, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return hits;
}

/* chunks_packs_marking_tests_hits_eq asserts at a line that the marking's hits, which unwrap at a column, are those given. */
static void
chunks_packs_marking_tests_hits_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
								   const chunks_TupleAddressLocator_t *want, size_t want_len, uint32 line, uint32 col)
{
	StdVec		hits = chunks_packs_marking_tests_hits(packs, io, line, col);
	bool		same = hits.len == want_len;

	for (size_t k = 0; same && k < want_len; k++)
	{
		const chunks_TupleAddressLocator_t *hit = &STD_VEC_AT(&hits, chunks_TupleAddressLocator_t, k);

		same = chunks_address_Address_PartialEq_eq(&hit->f0, &want[k].f0) &&
			chunks_trailer_Locator_PartialEq_eq(&hit->f1, &want[k].f1);
	}
	if (!same)
	{
		StdVec		wanted = {unconstify(chunks_TupleAddressLocator_t *, want), want_len, want_len};

		std_assert_failed(RUST_FILE, line, 9, "==", &hits, chunks_packs_marking_tests_Hits_Debug_fmt, &wanted,
						  chunks_packs_marking_tests_Hits_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&hits, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
}

/* chunks_packs_marking_tests_hits_refused reports whether the marking's hits are refused with a host error. */
static bool
chunks_packs_marking_tests_hits_refused(const ChunksPacksBackendPacks *packs, HostIoIo *io)
{
	StdVec		hits;
	ChunksErrorError error;
	bool		refused = !chunks_packs_marking_Packs_hits(packs, io, &hits, &error);

	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_HOST;
		chunks_error_Error_drop(&error);
	}
	else
		std_vec_free_Checked(&hits, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return refused;
}

/* chunks_packs_marking_tests_marked begins a marking of the repository's packs, which unwraps at a line and column, and returns its newest pack. */
static uint64
chunks_packs_marking_tests_marked(const ChunksPacksBackendPacks *packs, HostIoIo *io,
								  const HostSharedReader *reading, uint32 line, uint32 col)
{
	uint64		newest;
	ChunksErrorError error;

	if (!chunks_packs_marking_Packs_begin_marking(packs, io, reading, &newest, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return newest;
}

/* chunks_packs_marking_tests_ended ends the repository's marking, which unwraps at a line and column. */
static void
chunks_packs_marking_tests_ended(const ChunksPacksBackendPacks *packs, HostIoIo *io, uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_marking_Packs_end_marking(packs, io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/*
 * test_chunks_packs_marking_tests_a_locate_records_its_hit_in_a_pack_the_marking_covers:
 * a locate records its hit while a marking covers the pack it found the
 * chunk in, once for each time it finds it: none before the marking, none in
 * a pack added after the marking began, none for an address it doesn't find,
 * and none after the marking ended; the next marking begins with no hit.
 */
void
test_chunks_packs_marking_tests_a_locate_records_its_hit_in_a_pack_the_marking_covers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 232, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	chunks_TupleAddressLocator_t both[2];
	ChunksAddressAddress none = chunks_address_Address_of((const uint8 *) "none", 4);
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	chunks_packs_fixtures_created(io, 51, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, &one.f1, 235, 9, 53);
	STD_ASSERT_MSG(236, 9, chunks_packs_marking_tests_hits_refused(&packs, io), "hits were read outside a marking");

	STD_ASSERT_EQ_U64(238, 9, chunks_packs_marking_tests_marked(&packs, io, &reading, 238, 54), 1);
	two = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, &one.f1, 240, 9, 53);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &two.f0, &two.f1, 241, 9, 53);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &none, NULL, 242, 9, 70);
	if (!chunks_packs_marking_Packs_locate_in(&packs, io, &reading, &one.f0, 0, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 243, 59);
	chunks_packs_marking_tests_answer_eq(has, &found, &one.f1, 243, 9);
	both[0] = one;
	both[1] = one;
	chunks_packs_marking_tests_hits_eq(&packs, io, both, lengthof(both), 244, 35);
	chunks_packs_marking_tests_ended(&packs, io, 245, 31);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, &one.f1, 246, 9, 53);

	STD_ASSERT_EQ_U64(248, 9, chunks_packs_marking_tests_marked(&packs, io, &reading, 248, 54), 2);
	chunks_packs_marking_tests_hits_eq(&packs, io, NULL, 0, 249, 35);
	chunks_packs_marking_tests_ended(&packs, io, 250, 31);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_marking_tests_a_locate_answers_no_dead_chunk_and_no_place_in_an_unlisted_pack:
 * a locate answers nothing for a chunk whose only copy a full collection
 * found dead, and for a place in a pack that is no longer listed, so the
 * writer that asked writes a copy of its own.
 */
void
test_chunks_packs_marking_tests_a_locate_answers_no_dead_chunk_and_no_place_in_an_unlisted_pack(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 259, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t one;
	ChunksTrailerLocator found;
	ChunksErrorError error;
	uint64		offset;
	bool		has;
	bool		set;

	chunks_packs_fixtures_created(io, 52, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	offset = chunks_trailer_Locator_offset(&one.f1);
	if (!chunks_packs_marking_Packs_noted(&packs, io, &one.f0, true,
										  chunks_trailer_Locator_at(9, offset, chunks_trailer_Locator_length(&one.f1)),
										  &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 262, 98);
	chunks_packs_marking_tests_answer_eq(has, &found, NULL, 262, 9);
	if (!chunks_packs_fixtures_dead(io, &packs, 1, 1, &offset, 1, &set, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 263, 60);
	STD_ASSERT(263, 9, set, "dead(io, &packs, 1, 1, &[at_one.offset()]).unwrap()");
	chunks_packs_tests_found(RUST_FILE, &packs, io, &reading, &one.f0, &one.f1, 264, 9, 51);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, NULL, 265, 9, 53);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_marking_tests_over returns a process's own handle of the packs whose record is at a location. */
static bool
chunks_packs_marking_tests_over(HostIoIo *io, uint64 location, ChunksPacksBackendPacks *result, HostError *error)
{
	HostSharedHeld held;
	ChunksErrorError failed;
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &held,
						  error))
		return false;
	if (!has)
	{
		*error = chunks_packs_tests_failed_text("no record");
		return false;
	}
	if (!chunks_packs_backend_Packs_over(io, &held, location, result, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	return true;
}

/* chunks_packs_marking_tests_marking is a_marking_clears_the_name_a_terminated_collector_left's collector: it begins a marking and ends without ending it. */
static bool
chunks_packs_marking_tests_marking(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint64		newest;
	bool		ok;

	if (!host_shared_read(io, &reading, error))
		return false;
	ok = chunks_packs_marking_tests_over(io, *location, &packs, error);
	if (ok)
	{
		ok = chunks_packs_marking_Packs_begin_marking(&packs, io, &reading, &newest, &failed);
		if (!ok)
			*error = chunks_packs_tests_raised(&failed);
		chunks_packs_backend_Packs_drop(&packs);
	}
	host_shared_Reader_drop(&reading);
	return ok;
}

/*
 * test_chunks_packs_marking_tests_a_marking_clears_the_name_a_terminated_collector_left:
 * a locate in another repository during a marking records nothing and is
 * answered. A marking that finds the name a collector terminated in its
 * marking left clears it under that repository's barrier, so a locate there
 * records nothing from then and is answered.
 */
void
test_chunks_packs_marking_tests_a_marking_clears_the_name_a_terminated_collector_left(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 275, 46);
	ChunksPacksBackendPacks first;
	ChunksPacksBackendPacks second;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	uint64		location;
	SimcActor	collector = {chunks_packs_marking_tests_marking, &location};
	SimcRun		run = simc_Run_new(7);
	SimcReport	report;

	chunks_packs_fixtures_created(io, 53, &first);
	chunks_packs_fixtures_created(io, 54, &second);
	one = chunks_packs_fixtures_added(io, &first, (const uint8 *) "one", 3);
	two = chunks_packs_fixtures_added(io, &second, (const uint8 *) "two", 3);
	location = chunks_packs_backend_Packs_location(&first);
	simc_Run_actor(&run, "collector", 9, collector);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 290, 9, &arguments);
	}

	chunks_packs_marking_tests_located_eq(&second, io, &reading, &two.f0, &two.f1, 292, 9, 54);
	chunks_packs_marking_tests_located_eq(&first, io, &reading, &one.f0, &one.f1, 293, 9, 53);
	chunks_packs_marking_tests_hits_eq(&first, io, &one, 1, 294, 35);
	STD_ASSERT_EQ_U64(295, 9, chunks_packs_marking_tests_marked(&second, io, &reading, 295, 55), 1);
	chunks_packs_marking_tests_located_eq(&first, io, &reading, &one.f0, &one.f1, 296, 9, 53);
	STD_ASSERT(297, 9, chunks_packs_marking_tests_hits_refused(&first, io), "matches!(first.hits(io), Err(Error::Host(_)))");
	chunks_packs_marking_tests_hits_eq(&second, io, NULL, 0, 298, 36);
	chunks_packs_marking_tests_ended(&second, io, 299, 32);
	simc_Report_drop(&report);
	chunks_packs_backend_Packs_drop(&second);
	chunks_packs_backend_Packs_drop(&first);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_marking_tests_named keeps every pack a hit names. */
static bool
chunks_packs_marking_tests_named(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries, StdVec *result,
								 ChunksErrorError *error)
{
	(void) captures;
	(void) marking;
	(void) error;
	for (size_t k = 0, k_len = entries->len; k < k_len; k++)
	{
		uint64		pack = STD_VEC_AT(entries, chunks_TupleAddressLocator_t, k).f1.pack;
		size_t		at;

		if (!std_slice_binary_search(result->ptr, result->len, sizeof(uint64), &pack, std_u64_Ord_cmp, &at))
			std_vec_insert(result, at, &pack, sizeof(pack), _Alignof(uint64), RUST_FILE, 321, 50);
	}
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/*
 * test_chunks_packs_marking_tests_a_locate_past_a_markings_slots_answers_nothing:
 * a marking records as many hits as it has slots: a locate past them answers
 * nothing, so no writer is answered from a pack the collector doesn't keep.
 * Its retirement keeps every pack a recorded hit names and takes out only a
 * pack no writer was answered from.
 */
void
test_chunks_packs_marking_tests_a_locate_past_a_markings_slots_answers_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 309, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	StdVec		hits;
	StdVec		numbers;
	size_t		len;
	size_t		slots = CHUNKS_PACKS_SINGLE_HIT_SLOTS;
	bool		ok;
	static const uint64 retiring[] = {1, 2};
	static const uint64 queued[] = {2};
	static const uint64 kept[] = {1};

	chunks_packs_fixtures_created(io, 55, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	two = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "two", 3);
	(void) chunks_packs_marking_tests_marked(&packs, io, &reading, 312, 43);
	for (size_t k = 0; k < CHUNKS_PACKS_SINGLE_HIT_SLOTS; k++)
		chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, &one.f1, 314, 13, 57);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, NULL, 316, 9, 53);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &two.f0, NULL, 317, 9, 53);
	hits = chunks_packs_marking_tests_hits(&packs, io, 318, 35);
	len = hits.len;
	std_vec_free_Checked(&hits, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	STD_ASSERT_EQ(318, 9, len == slots, &len, std_usize_Debug_fmt, &slots, std_usize_Debug_fmt);
	if (!chunks_packs_removal_reserve(io, 2, &reservation, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 319, 63);
	if (!chunks_packs_backend_Packs_forward(&packs, io, &reading, NULL, 0, &forwarded, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 319, 106);
	ok = chunks_packs_retire_Packs_retire(&packs, io, retiring, lengthof(retiring), &forwarded, &reservation,
										  chunks_packs_marking_tests_named, NULL, NULL, &retired, &error);
	if (!chunks_packs_fixtures_queued(io, &reading, ok, &retired, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 324, 50);
	if (numbers.len != lengthof(queued) || memcmp(numbers.ptr, queued, sizeof(queued)) != 0)
	{
		StdVec		wanted;

		std_slice_to_vec_Checked(&wanted, queued, lengthof(queued), sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 324, 9, "==", &numbers, chunks_packs_tests_Numbers_Debug_fmt, &wanted,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, kept, lengthof(kept), NULL, 325, 9, 48);
	chunks_packs_marking_tests_located_eq(&packs, io, &reading, &one.f0, &one.f1, 326, 9, 53);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_marking_tests_String_PartialOrd_lt_FnMut_call_mut is the fn item String's PartialOrd::lt as the stable sort's is_less. */
static bool
chunks_packs_marking_tests_String_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_String_PartialOrd_lt(a, b);
}

/* The stable sort's shared parts at String, which its instance below calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS String
#define STD_SORT_T StdString
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* sort at String, which unfinished and the tests' kept names sort with. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS String
#define STD_STABLE_SORT_T StdString
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT chunks_packs_marking_tests_String_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/* chunks_packs_marking_tests_Names_Debug_fmt is a Vec<String>'s Debug. */
static bool
chunks_packs_marking_tests_Names_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *names = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = names->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(names, StdString, k), std_String_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_marking_tests_Names_drop drops a Vec<String>. */
static void
chunks_packs_marking_tests_Names_drop(StdVec *names)
{
	std_vec_drop_Checked(names, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* chunks_packs_marking_tests_unfinished returns the names of the unfinished packs' files in a directory, in order. */
static StdVec
chunks_packs_marking_tests_unfinished(const StdPathBuf *dir)
{
	StdVec		names;
	StdVec		result;
	StdIoError	io_error;
	bool		entry;
	size_t		temp = strlen(CHUNKS_PACK_TEMP);

	if (!std_fs_read_dir_names(dir->bytes.ptr, dir->bytes.len, &names, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(331, 71, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(331, 44, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	std_vec_new(&result, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&names, StdString, k);

		if (name->len >= temp && memcmp((const char *) name->ptr + name->len - temp, CHUNKS_PACK_TEMP, temp) == 0)
			std_vec_push_Checked(&result, name, sizeof(StdString), _Alignof(StdString));
		else
			std_String_drop(name);
	}
	std_vec_free_Checked(&names, sizeof(StdString), _Alignof(StdString));
	std_slice_sort_String(result.ptr, result.len);
	return result;
}

/* chunks_packs_marking_tests_names_eq asserts at a line that two lists of names, which it drops, are the same, with a message, or none when it is NULL. */
static void
chunks_packs_marking_tests_names_eq(StdVec *got, StdVec *want, const char *message, uint32 line)
{
	bool		same = got->len == want->len;

	for (size_t k = 0, k_len = got->len; same && k < k_len; k++)
	{
		const StdString *a = &STD_VEC_AT(got, StdString, k);
		const StdString *b = &STD_VEC_AT(want, StdString, k);

		same = a->len == b->len && memcmp(a->ptr, b->ptr, a->len) == 0;
	}
	if (!same)
	{
		StdFmtPiece pieces[] = {{message, message == NULL ? 0 : strlen(message), 0, {0}}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

		std_assert_failed(RUST_FILE, line, 9, "==", got, chunks_packs_marking_tests_Names_Debug_fmt, want,
						  chunks_packs_marking_tests_Names_Debug_fmt, message == NULL ? NULL : &arguments);
	}
	chunks_packs_marking_tests_Names_drop(got);
	chunks_packs_marking_tests_Names_drop(want);
}

/* chunks_packs_marking_tests_none_left asserts at a line that a directory holds no unfinished pack. */
static void
chunks_packs_marking_tests_none_left(const StdPathBuf *dir, uint32 line)
{
	StdVec		got = chunks_packs_marking_tests_unfinished(dir);
	StdVec		want;

	std_vec_new(&want, sizeof(StdString), _Alignof(StdString));
	chunks_packs_marking_tests_names_eq(&got, &want, NULL, line);
}

/* chunks_packs_marking_tests_stray writes an unfinished pack's file as a writer of a process with a pack's number would have left it, and returns its name. */
static StdString
chunks_packs_marking_tests_stray(const StdPathBuf *dir, uint32 process, uint32 number)
{
	char		formatted[128];
	StdString	name;
	StdPathBuf	path;
	StdIoError	io_error;

	snprintf(formatted, sizeof(formatted), "0123456789abcdef0123456789abcdef.%u.%u%s", process, number, CHUNKS_PACK_TEMP);
	std_string_from_Checked(&name, formatted, strlen(formatted));
	std_path_join(dir->bytes.ptr, dir->bytes.len, name.ptr, name.len, &path);
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, (const uint8 *) "unfinished", 10, &io_error))
		STD_RESULT_EXPECT_FAILED(341, 56, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return name;
}

/* chunks_packs_marking_tests_strayed writes an unfinished pack's file as stray does, and drops its name. */
static void
chunks_packs_marking_tests_strayed(const StdPathBuf *dir, uint32 process, uint32 number)
{
	StdString	name = chunks_packs_marking_tests_stray(dir, process, number);

	std_String_drop(&name);
}

/* chunks_packs_marking_tests_collect begins and ends a marking, as a collection that retires nothing does. */
static void
chunks_packs_marking_tests_collect(HostIoIo *io, const ChunksPacksBackendPacks *packs)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 347, 46);

	(void) chunks_packs_marking_tests_marked(packs, io, &reading, 348, 43);
	chunks_packs_marking_tests_ended(packs, io, 349, 31);
	host_shared_Reader_drop(&reading);
}

/* chunks_packs_marking_tests_with appends a name's copy to a list of names. */
static void
chunks_packs_marking_tests_with(StdVec *names, const StdString *name)
{
	StdString	copy;

	std_string_from_Checked(&copy, name->ptr, name->len);
	std_vec_push_Checked(names, &copy, sizeof(StdString), _Alignof(StdString));
}

/*
 * test_chunks_packs_marking_tests_a_collection_removes_the_unfinished_packs_no_writers_word_covers:
 * a collection removes the unfinished packs that no process's writers' word
 * covers and keeps the others: with no writer open, every one, a name with
 * no numbers included; with writers open, those of another process and those
 * numbered below the lowest number the process has open, while a file under
 * a number given again stays for as long as the process holds a number at or
 * below it; a dropped writer's file goes once the process holds no number at
 * or below it, and stays while it holds a lower one.
 */
void
test_chunks_packs_marking_tests_a_collection_removes_the_unfinished_packs_no_writers_word_covers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 368, 46);
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(56, HOST_PG_DEFAULT);
	uint32		me = host_shared_proc_number();
	StdPathBuf	path;
	StdIoError	io_error;
	ChunksPackPackWriterInDir first;
	ChunksPackPackWriterInDir second;
	ChunksPackPackWriterInDir lower;
	ChunksPackPackWriterInDir higher;
	StdVec		open;
	StdString	again;
	StdString	later;
	StdVec		kept;
	StdVec		got;
	StdVec		seconds;
	ChunksAddressAddress address;
	StdVec		bytes;
	ChunksTrailerLocator at;
	ChunksPackPack pack;
	ChunksErrorError error;
	StdVec		numbers;
	ChunksTrailerLocator found;
	bool		has;
	uint64		number_pair[2];
	static const uint64 want_pair[] = {1, 2};
	const char *nameless = "fedcba9876543210fedcba9876543210" CHUNKS_PACK_TEMP;

	chunks_packs_fixtures_created(io, 56, &packs);
	std_path_join(dir.bytes.ptr, dir.bytes.len, nameless, strlen(nameless), &path);
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, (const uint8 *) "unfinished", 10, &io_error))
		STD_RESULT_EXPECT_FAILED(373, 100, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&path.bytes, 1, 1);
	chunks_packs_marking_tests_strayed(&dir, me, 1);
	chunks_packs_marking_tests_strayed(&dir, me + 1, 1);
	chunks_packs_marking_tests_collect(io, &packs);
	chunks_packs_marking_tests_none_left(&dir, 377);

	first = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 379, 62);
	second = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 379, 102);
	number_pair[0] = chunks_pack_PackWriterInDir_number(&first);
	number_pair[1] = chunks_pack_PackWriterInDir_number(&second);
	if (number_pair[0] != want_pair[0] || number_pair[1] != want_pair[1])
	{
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&number_pair[0], std_u64_Debug_fmt),
									 STD_ITEM(&number_pair[1], std_u64_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&want_pair[0], std_u64_Debug_fmt),
									  STD_ITEM(&want_pair[1], std_u64_Debug_fmt));

		std_assert_failed(RUST_FILE, 380, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	open = chunks_packs_marking_tests_unfinished(&dir);
	again = chunks_packs_marking_tests_stray(&dir, me, 1);
	later = chunks_packs_marking_tests_stray(&dir, me, 2);
	chunks_packs_marking_tests_strayed(&dir, me + 1, 1);
	chunks_packs_marking_tests_strayed(&dir, me, 0);
	chunks_packs_marking_tests_collect(io, &packs);
	std_vec_new(&kept, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = open.len; k < k_len; k++)
		chunks_packs_marking_tests_with(&kept, &STD_VEC_AT(&open, StdString, k));
	std_vec_push_Checked(&kept, &again, sizeof(StdString), _Alignof(StdString));
	chunks_packs_marking_tests_with(&kept, &later);
	std_slice_sort_String(kept.ptr, kept.len);
	got = chunks_packs_marking_tests_unfinished(&dir);
	chunks_packs_marking_tests_names_eq(&got, &kept,
										"an unfinished pack a writer's word covers was removed, or another kept", 388);

	chunks_pack_PackWriterInDir_drop(&first);
	chunks_packs_marking_tests_collect(io, &packs);
	std_vec_new(&seconds, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = open.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&open, StdString, k);
		uint32		process;
		uint64		number;

		if (chunks_pack_temp_of(name->ptr, name->len, &process, &number) && process == me && number == 2)
			std_vec_push_Checked(&seconds, name, sizeof(StdString), _Alignof(StdString));
		else
			std_String_drop(name);
	}
	std_vec_free_Checked(&open, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&kept, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = seconds.len; k < k_len; k++)
		chunks_packs_marking_tests_with(&kept, &STD_VEC_AT(&seconds, StdString, k));
	std_vec_push_Checked(&kept, &later, sizeof(StdString), _Alignof(StdString));
	std_slice_sort_String(kept.ptr, kept.len);
	got = chunks_packs_marking_tests_unfinished(&dir);
	chunks_packs_marking_tests_names_eq(&got, &kept, "a dropped writer's pack stayed, or the open writer's went", 395);

	chunks_packs_fixtures_stored((const uint8 *) "one", 3, &address, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&second, io, address, bytes.ptr, bytes.len, false, &at, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 399, 59);
	if (!chunks_pack_PackWriterInDir_finish(&second, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 400, 38);
	if (!chunks_packs_backend_Packs_add(&packs, io, &reading, &pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 401, 39);
	chunks_packs_marking_tests_collect(io, &packs);
	chunks_packs_marking_tests_none_left(&dir, 403);
	if (!chunks_packs_backend_Packs_numbers(&packs, io, &reading, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 405, 42);
	if (!chunks_packs_backend_Packs_find(&packs, io, &reading, &address, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 405, 87);
	if (numbers.len != 1 || STD_VEC_AT(&numbers, uint64, 0) != 2 || !has ||
		!chunks_trailer_Locator_PartialEq_eq(&found, &at))
	{
		uint64		two = 2;
		StdVec		want_numbers = {&two, 1, 1};
		StdOptionRef got_found = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want_found = {true, STD_ITEM(&at, chunks_trailer_Locator_Debug_fmt)};
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&numbers, chunks_packs_tests_Numbers_Debug_fmt),
									 STD_ITEM(&got_found, std_option_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&want_numbers, chunks_packs_tests_Numbers_Debug_fmt),
									  STD_ITEM(&want_found, std_option_Debug_fmt));

		std_assert_failed(RUST_FILE, 404, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));

	lower = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 409, 62);
	higher = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 409, 102);
	chunks_pack_PackWriterInDir_drop(&higher);
	chunks_packs_marking_tests_collect(io, &packs);
	got = chunks_packs_marking_tests_unfinished(&dir);
	if (got.len != 2)
	{
		size_t		two = 2;
		StdFmtPiece pieces[] = {STD_FMT_LIT("a dropped writer's pack went under a lower number its process holds")};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

		std_assert_failed(RUST_FILE, 412, 9, "==", &got.len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt,
						  &arguments);
	}
	chunks_packs_marking_tests_Names_drop(&got);
	if (!chunks_pack_PackWriterInDir_abandon(&lower, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 413, 27);
	chunks_packs_marking_tests_collect(io, &packs);
	chunks_packs_marking_tests_none_left(&dir, 415);
	chunks_packs_marking_tests_Names_drop(&seconds);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_marking_tests_a_collection_removes_an_unfinished_pack_alone_in_its_tablespace:
 * a collection removes an unfinished pack that is the only file of its
 * tablespace, which no list names.
 */
void
test_chunks_packs_marking_tests_a_collection_removes_an_unfinished_pack_alone_in_its_tablespace(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(57, 16400);
	StdIoError	io_error;

	chunks_packs_fixtures_created(io, 57, &packs);
	if (!std_fs_create_dir_all(dir.bytes.ptr, dir.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(425, 39, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	chunks_packs_marking_tests_strayed(&dir, host_shared_proc_number() + 1, 1);
	chunks_packs_marking_tests_collect(io, &packs);
	chunks_packs_marking_tests_none_left(&dir, 428);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_marking_tests_a_collection_goes_on_past_an_unfinished_pack_it_cant_remove:
 * a collection whose removal of an unfinished pack fails ends with no error
 * and leaves the file, which the next collection removes.
 */
void
test_chunks_packs_marking_tests_a_collection_goes_on_past_an_unfinished_pack_it_cant_remove(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir = host_tablespace_dir(58, HOST_PG_DEFAULT);
	StdString	left;
	StdPathBuf	data;
	SimcSim		sim;
	StdVec		got;
	StdVec		want;

	chunks_packs_fixtures_created(io, 58, &packs);
	left = chunks_packs_marking_tests_stray(&dir, host_shared_proc_number() + 1, 1);
	data = simc_data_dir();
	sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 29);
	std_vec_free_Checked(&data.bytes, 1, 1);
	simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + 1);
	chunks_packs_marking_tests_collect(io, &packs);
	simc_Sim_stop(&sim);
	got = chunks_packs_marking_tests_unfinished(&dir);
	std_vec_new(&want, sizeof(StdString), _Alignof(StdString));
	std_vec_push_Checked(&want, &left, sizeof(StdString), _Alignof(StdString));
	chunks_packs_marking_tests_names_eq(&got, &want, "the removal wasn't made to fail", 444);
	chunks_packs_marking_tests_collect(io, &packs);
	chunks_packs_marking_tests_none_left(&dir, 446);
	simc_Sim_drop(&sim);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_marking_tests_left_t is a_collection_removes_a_terminated_backends_unfinished_pack's actors' captures: the record's location, the repository's directory, and the number of the one unfinished pack the collector saw left. */
typedef struct chunks_packs_marking_tests_left_t
{
	uint64		location;
	StdPathBuf	dir;
	volatile uint64 left;
} chunks_packs_marking_tests_left_t;

/* chunks_packs_marking_tests_keeper is the keeper: it holds a writer open until it is let go, then abandons it. */
static bool
chunks_packs_marking_tests_keeper(void *captures, HostIoIo *io, HostError *error)
{
	chunks_packs_marking_tests_left_t *left = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;
	bool		ok = false;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!chunks_packs_marking_tests_over(io, left->location, &packs, error))
		goto reading;
	if (!chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto packs;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-kept", error))
	{
		chunks_pack_PackWriterInDir_drop(&writer);
		goto packs;
	}
	ok = chunks_pack_PackWriterInDir_abandon(&writer, HOST_IO_IO_TAKES_SHORT(io), &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
packs:
	chunks_packs_backend_Packs_drop(&packs);
reading:
	host_shared_Reader_drop(&reading);
	return ok;
}

/* chunks_packs_marking_tests_doomed is the doomed backend: it holds a writer open until it is terminated. */
static bool
chunks_packs_marking_tests_doomed(void *captures, HostIoIo *io, HostError *error)
{
	chunks_packs_marking_tests_left_t *left = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!chunks_packs_marking_tests_over(io, left->location, &packs, error))
		goto reading;
	if (!chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto packs;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-doomed", error))
	{
		chunks_pack_PackWriterInDir_drop(&writer);
		goto packs;
	}
	for (;;)
		(void) host_unique();
packs:
	chunks_packs_backend_Packs_drop(&packs);
reading:
	host_shared_Reader_drop(&reading);
	return false;
}

/* chunks_packs_marking_tests_collector is the collector: it collects until one unfinished pack is left, and stores its number. */
static bool
chunks_packs_marking_tests_collector(void *captures, HostIoIo *io, HostError *error)
{
	chunks_packs_marking_tests_left_t *left = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint64		newest;
	bool		ok = false;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!chunks_packs_marking_tests_over(io, left->location, &packs, error))
		goto reading;
	for (int k = 0; k < 10000; k++)
	{
		StdVec		names;
		size_t		count = 0;
		uint64		seen = 0;

		if (!chunks_packs_marking_Packs_begin_marking(&packs, io, &reading, &newest, &failed) ||
			!chunks_packs_marking_Packs_end_marking(&packs, io, &failed))
		{
			*error = chunks_packs_tests_raised(&failed);
			goto packs;
		}
		names = chunks_packs_marking_tests_unfinished(&left->dir);
		for (size_t n = 0, n_len = names.len; n < n_len; n++)
		{
			const StdString *name = &STD_VEC_AT(&names, StdString, n);
			uint32		process;
			uint64		number;

			if (chunks_pack_temp_of(name->ptr, name->len, &process, &number))
			{
				seen = number;
				count++;
			}
		}
		chunks_packs_marking_tests_Names_drop(&names);
		if (count == 1)
		{
			left->left = seen;
			ok = true;
			goto packs;
		}
	}
	*error = chunks_packs_tests_failed_text("the terminated backend's unfinished pack stayed");
packs:
	chunks_packs_backend_Packs_drop(&packs);
reading:
	host_shared_Reader_drop(&reading);
	return ok;
}

/* chunks_packs_marking_tests_ending_eq asserts at a line that an actor's ending is the one given, with the endings' Debug as the message. */
static void
chunks_packs_marking_tests_ending_eq(const StdVec *endings, size_t index, const SimcEnding *want, uint32 line)
{
	const SimcEnding *ending;

	std_index_check(index, endings->len, RUST_FILE, line, 20);
	ending = &STD_VEC_AT(endings, simc_TupleStringEnding_t, index).f1;
	if (!simc_Ending_PartialEq_eq(ending, want))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{endings, chunks_packs_tests_Endings_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, line, 9, "==", ending, simc_Ending_Debug_fmt, want, simc_Ending_Debug_fmt,
						  &arguments);
	}
}

/*
 * test_chunks_packs_marking_tests_a_collection_removes_a_terminated_backends_unfinished_pack:
 * a backend terminated with a writer open leaves its unfinished pack, which
 * the next collection removes while another backend holds a lower number
 * open in the same repository, whose own unfinished pack stays.
 */
void
test_chunks_packs_marking_tests_a_collection_removes_a_terminated_backends_unfinished_pack(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;
	chunks_packs_marking_tests_left_t left = {.dir = host_tablespace_dir(59, HOST_PG_DEFAULT), .left = PG_UINT64_MAX};
	SimcActor	keeper = {chunks_packs_marking_tests_keeper, &left};
	SimcActor	doomed = {chunks_packs_marking_tests_doomed, &left};
	SimcActor	collector = {chunks_packs_marking_tests_collector, &left};
	SimcStep	steps[] = {{"doomed", 6, PG_UINT64_MAX}, {"collector", 9, PG_UINT64_MAX}, {"keeper", 6, PG_UINT64_MAX}};
	SimcRun		run = simc_Run_new(31);
	SimcReport	report;
	SimcEnding	returned = {SIMC_ENDING_RETURNED};
	SimcEnding	terminated = {SIMC_ENDING_TERMINATED};
	uint64		seen;
	uint64		one = 1;

	chunks_packs_fixtures_created(io, 59, &packs);
	left.location = chunks_packs_backend_Packs_location(&packs);
	simc_Run_actor(&run, "keeper", 6, keeper);
	simc_Run_actor(&run, "doomed", 6, doomed);
	simc_Run_actor(&run, "collector", 9, collector);
	simc_Run_until(&run, "keeper", 6, "doltgres-chunks-case-kept");
	simc_Run_until(&run, "doomed", 6, "doltgres-chunks-case-doomed");
	simc_Run_terminate_at(&run, "doomed", 6, 5000);
	simc_Run_until(&run, "collector", 9, "doltgres-chunks-strays-listed");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 500, 67, &arguments);
	}
	chunks_packs_marking_tests_ending_eq(&report.outcome.u.ended, 0, &returned, 501);
	chunks_packs_marking_tests_ending_eq(&report.outcome.u.ended, 1, &terminated, 502);
	chunks_packs_marking_tests_ending_eq(&report.outcome.u.ended, 2, &returned, 503);
	seen = left.left;
	STD_ASSERT_EQ_MSG(504, 9, seen == one, &seen, std_u64_Debug_fmt, &one, std_u64_Debug_fmt,
					  "the keeper's pack, numbered 1, isn't the one unfinished pack left");
	simc_Report_drop(&report);
	std_vec_free_Checked(&left.dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_marking_tests_collecting is a_collection_never_removes_a_live_writers_unfinished_pack's collector: one whole collection. */
static bool
chunks_packs_marking_tests_collecting(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	uint64		newest;
	bool		ok = false;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (chunks_packs_marking_tests_over(io, *location, &packs, error))
	{
		ok = chunks_packs_marking_Packs_begin_marking(&packs, io, &reading, &newest, &failed) &&
			chunks_packs_marking_Packs_end_marking(&packs, io, &failed);
		if (!ok)
			*error = chunks_packs_tests_raised(&failed);
		chunks_packs_backend_Packs_drop(&packs);
	}
	host_shared_Reader_drop(&reading);
	return ok;
}

/* chunks_packs_marking_tests_writing is a_collection_never_removes_a_live_writers_unfinished_pack's writer: it starts a pack, waits, then writes a chunk, finishes the pack, and adds it. */
static bool
chunks_packs_marking_tests_writing(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	HostSharedReader reading;
	ChunksPacksBackendPacks packs;
	ChunksPackPackWriterInDir writer;
	ChunksAddressAddress address;
	StdVec		bytes;
	ChunksTrailerLocator at;
	ChunksPackPack pack;
	ChunksErrorError failed;
	bool		ok = false;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!chunks_packs_marking_tests_over(io, *location, &packs, error))
		goto reading;
	if (!chunks_packs_backend_Packs_writer(&packs, io, &reading, 0, &writer, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto packs;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-writing", error))
	{
		chunks_pack_PackWriterInDir_drop(&writer);
		goto packs;
	}
	chunks_packs_fixtures_stored((const uint8 *) "one", 3, &address, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, address, bytes.ptr, bytes.len, false, &at, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		chunks_pack_PackWriterInDir_drop(&writer);
		goto bytes;
	}
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto bytes;
	}
	ok = chunks_packs_backend_Packs_add(&packs, io, &reading, &pack, &failed);
	if (!ok)
		*error = chunks_packs_tests_raised(&failed);
bytes:
	std_vec_free_Checked(&bytes, 1, 1);
packs:
	chunks_packs_backend_Packs_drop(&packs);
reading:
	host_shared_Reader_drop(&reading);
	return ok;
}

/*
 * test_chunks_packs_marking_tests_a_collection_never_removes_a_live_writers_unfinished_pack:
 * a writer that stores its slot, reserves its number, and makes its file
 * after a collection listed the directories and before it read the writers'
 * words keeps its file, and finishes its pack; and so does one whose slot was
 * stored, and nothing more, when a whole collection ran.
 */
void
test_chunks_packs_marking_tests_a_collection_never_removes_a_live_writers_unfinished_pack(void)
{
	static const char *const helds[] = {"doltgres-chunks-case-writing", "doltgres-chunks-writer-stored"};

	for (size_t k = 0; k < lengthof(helds); k++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 519, 50);
		ChunksPacksBackendPacks packs;
		uint64		location;
		SimcActor	collector = {chunks_packs_marking_tests_collecting, &location};
		SimcActor	writer = {chunks_packs_marking_tests_writing, &location};
		SimcStep	steps[] = {{"collector", 9, PG_UINT64_MAX}, {"writer", 6, PG_UINT64_MAX}};
		SimcRun		run = simc_Run_new(37);
		SimcReport	report;
		StdStr		held = {helds[k], strlen(helds[k])};
		static const uint64 one[] = {1};
		StdFmtPiece held_pieces[] = {STD_FMT_LIT("held at "), STD_FMT_ARG(0)};
		StdFmtArg	held_args[] = {{&held, std_str_Debug_fmt}};
		StdFmtArguments held_at = {held_pieces, lengthof(held_pieces), held_args};

		chunks_packs_fixtures_created(io, 60, &packs);
		location = chunks_packs_backend_Packs_location(&packs);
		simc_Run_actor(&run, "collector", 9, collector);
		simc_Run_actor(&run, "writer", 6, writer);
		if (k == 0)
			simc_Run_until(&run, "collector", 9, "doltgres-chunks-strays-listed");
		simc_Run_until(&run, "writer", 6, helds[k]);
		simc_Run_script(&run, steps, lengthof(steps));
		report = simc_Run_finish(run);
		if (!chunks_packs_tests_returned(&report.outcome))
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("held at "), STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&held, std_str_Debug_fmt}, {&report.outcome, simc_Outcome_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 550, 13, &arguments);
		}
		chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, one, lengthof(one), &held_at, 556, 13, 52);
		chunks_packs_backend_Packs_drop(&packs);
		chunks_packs_tests_freed(RUST_FILE, io, location, 558, 79, 88);
		simc_Report_drop(&report);
		host_shared_Reader_drop(&reading);
		simc_TestIo_drop(&test_io);
	}
}

#endif							/* DOLTGRES_TEST_CHUNKS */
