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
 * A collection's retirement of packs (chunks' packs/retire.rs, design item
 * 6): under the barrier held exclusively, the collection reads the marking's
 * hits, changes what its marking found dead, and takes the packs out of the
 * pack table under the lists lock, which ends the marking; their files wait
 * unqueued, in the entries reserved before the first step.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/retire.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/marking.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "retire.rs"

/*
 * ChunksPacksRetireMarking is Marking: a collection's marking at its
 * retirement, under the barrier held exclusively, which the retirement's
 * keep runs with.
 */
struct ChunksPacksRetireMarking
{
	ChunksPacksViewView *view;
	HostSharedRecord record;
	HostSharedLongGuard *barrier;
	/* own is the id of the tablespace the repository's own directory is in. */
	uint32		own;

	/*
	 * room is how many entries of the retirement's reservation nothing is to
	 * fill yet.
	 */
	size_t		room;

	/*
	 * replaced is a Vec<File> of the older generations of deletion vectors
	 * the marking replaced, which the retirement queues.
	 */
	StdVec		replaced;
};

/* chunks_packs_retire_corrupt returns a corrupt error of a message with a number in it. */
static ChunksErrorError
chunks_packs_retire_corrupt(const char *before, uint64 number, const char *after)
{
	StdString	message;
	StdStr		head = {before, strlen(before)};
	StdStr		tail = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&head, std_str_Display_fmt}, {&number, std_u64_Display_fmt}, {&tail, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/*
 * chunks_packs_retire_froms_insert inserts a forwarded place into a set of
 * places, a vector in order with each once, as BTreeSet::insert does at a
 * caller's site.
 */
void
chunks_packs_retire_froms_insert(StdVec *froms, const std_TupleU64U64_t *from, const char *file, uint32 line,
								 uint32 col)
{
	size_t		low = 0;
	size_t		high = froms->len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		const std_TupleU64U64_t *at = &STD_VEC_AT(froms, std_TupleU64U64_t, middle);

		if (at->f0 == from->f0 && at->f1 == from->f1)
			return;
		if (at->f0 < from->f0 || (at->f0 == from->f0 && at->f1 < from->f1))
			low = middle + 1;
		else
			high = middle;
	}
	std_vec_insert(froms, low, from, sizeof(*from), _Alignof(std_TupleU64U64_t), file, line, col);
}

/* chunks_packs_retire_Forwarded_of returns the proof that the old places of a repository's packs were forwarded, taking the places. */
void
chunks_packs_retire_Forwarded_of(const ChunksPacksBackendPacks *packs, StdVec *froms,
								 ChunksPacksRetireForwarded *result)
{
	result->packs = chunks_packs_backend_Packs_location(packs);
	result->froms = *froms;
}

/*
 * chunks_packs_retire_Forwarded_join returns the proof of both calls'
 * forwards, as a collection that copies chunks into several packs makes,
 * refusing proofs of two repositories' packs. It takes both proofs.
 */
bool
chunks_packs_retire_Forwarded_join(ChunksPacksRetireForwarded *self, ChunksPacksRetireForwarded *other,
								   ChunksPacksRetireForwarded *result, ChunksErrorError *error)
{
	if (other->packs != self->packs)
	{
		chunks_packs_retire_Forwarded_drop(other);
		chunks_packs_retire_Forwarded_drop(self);
		*error = chunks_error_Error_corrupt_RefStr("forwards of two repositories' packs are joined",
												   strlen("forwards of two repositories' packs are joined"));
		return false;
	}
	for (size_t k = 0, k_len = other->froms.len; k < k_len; k++)
		chunks_packs_retire_froms_insert(&self->froms, &STD_VEC_AT(&other->froms, std_TupleU64U64_t, k), RUST_FILE, 66,
										 20);
	chunks_packs_retire_Forwarded_drop(other);
	*result = *self;
	return true;
}

/* chunks_packs_retire_Forwarded_covers reports whether the chunk at an offset of a pack was forwarded. */
bool
chunks_packs_retire_Forwarded_covers(const ChunksPacksRetireForwarded *self, uint64 pack, uint64 offset)
{
	for (size_t low = 0, high = self->froms.len; low < high;)
	{
		size_t		middle = low + (high - low) / 2;
		const std_TupleU64U64_t *at = &STD_VEC_AT(&self->froms, std_TupleU64U64_t, middle);

		if (at->f0 == pack && at->f1 == offset)
			return true;
		if (at->f0 < pack || (at->f0 == pack && at->f1 < offset))
			low = middle + 1;
		else
			high = middle;
	}
	return false;
}

/* chunks_packs_retire_Place_Debug_fmt is a forwarded place's (u64, u64) Debug. */
static bool
chunks_packs_retire_Place_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *place = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&t, &place->f0, std_u64_Debug_fmt);
	std_DebugTuple_field(&t, &place->f1, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* chunks_packs_retire_Froms_Debug_fmt is the forwarded places' BTreeSet<(u64, u64)> Debug. */
static bool
chunks_packs_retire_Froms_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *froms = value;
	StdDebugList s = std_fmt_debug_set(f);

	for (size_t k = 0, k_len = froms->len; k < k_len; k++)
		std_DebugSet_entry(&s, &STD_VEC_AT(froms, std_TupleU64U64_t, k), chunks_packs_retire_Place_Debug_fmt);
	return std_DebugSet_finish(&s);
}

/* chunks_packs_retire_Borrowed_Debug_fmt is PhantomData<&Packs>'s Debug: PhantomData and its type's name. */
static bool
chunks_packs_retire_Borrowed_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char text[] = "PhantomData<&chunks::packs::backend::Packs>";

	(void) value;
	return std_fmt_write_str(f, text, sizeof(text) - 1);
}

/* chunks_packs_retire_Forwarded_Debug_fmt is Forwarded's derived Debug. */
bool
chunks_packs_retire_Forwarded_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksPacksRetireForwarded *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Forwarded", 9);

	std_DebugStruct_field(&s, "packs", 5, &self->packs, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "froms", 5, &self->froms, chunks_packs_retire_Froms_Debug_fmt);
	std_DebugStruct_field(&s, "borrowed", 8, NULL, chunks_packs_retire_Borrowed_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_packs_retire_Forwarded_drop is Forwarded's drop glue: its places. */
void
chunks_packs_retire_Forwarded_drop(ChunksPacksRetireForwarded *self)
{
	std_vec_free_Checked(&self->froms, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
}

/* chunks_packs_retire_lists takes the lists lock under the barrier's io, as a long guard. */
static bool
chunks_packs_retire_lists(HostSharedLongIo *io, HostSharedLongGuard *result, ChunksErrorError *error)
{
	HostError	host;

	if (host_shared_LongIo_lock_long(io, CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, result, &host))
		return true;
	*error = chunks_error_Error_FromError_from(&host);
	return false;
}

/* chunks_packs_retire_set_dead_frame_0_t is set_dead's state across its pad: the lists' guard, the pack's path, and the change. */
typedef struct chunks_packs_retire_set_dead_frame_0_t
{
	ChunksPacksRetireMarking *self;
	uint64		number;
	uint64		generation;
	const StdBTreeMap *offsets;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedLongGuard lists;
	bool		lists_live;
	ChunksPacksSharedSnapshot snapshot;
	bool		snapshot_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksPacksListsChange change;
	bool		change_live;
} chunks_packs_retire_set_dead_frame_0_t;

/* chunks_packs_retire_set_dead_pad_0 is set_dead's pad: the change, the path, the snapshot, then the lists' guard. */
static void
chunks_packs_retire_set_dead_pad_0(void *frame_)
{
	chunks_packs_retire_set_dead_frame_0_t *frame = frame_;

	if (frame->change_live)
	{
		frame->change_live = false;
		chunks_packs_lists_Change_drop(&frame->change);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->snapshot_live)
	{
		frame->snapshot_live = false;
		chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	}
	if (frame->lists_live)
	{
		frame->lists_live = false;
		host_shared_LongGuard_drop(&frame->lists);
	}
}

/*
 * chunks_packs_retire_set_dead_body_0 is set_dead's body: the view brought
 * up to date under the lists lock, the pack's vector written durably, and
 * its generation named in the pack table, the older one queued.
 */
static void
chunks_packs_retire_set_dead_body_0(void *frame_)
{
	chunks_packs_retire_set_dead_frame_0_t *frame = frame_;
	ChunksPacksRetireMarking *self = frame->self;
	HostSharedLongIo io = host_shared_LongGuard_io(self->barrier);
	HostSharedLongIo lists_io;
	StdArcInner *const *pack;
	const ChunksPackPackReader *reader;
	const ChunksPacksListed *listed;
	ChunksPackPackId id;
	bool		before;
	uint32		space = 0;
	uint64		older = 0;

	frame->ok = false;
	if (!chunks_packs_retire_lists(&io, &frame->lists, frame->error))
		return;
	frame->lists_live = true;
	lists_io = host_shared_LongGuard_io(&frame->lists);
	if (!chunks_packs_view_View_refresh_under(self->view, &lists_io, self->record, &frame->snapshot, frame->error))
		goto done;
	frame->snapshot_live = true;
	chunks_packs_retire_set_dead_pad_0(frame);
	pack = chunks_packs_view_View_pack(self->view, frame->number);
	if (pack == NULL)
	{
		*frame->error = chunks_packs_retire_corrupt("pack number ", frame->number, " is not listed");
		goto done;
	}
	reader = std_arc_Deref_deref(*pack, _Alignof(ChunksPackPackReader));
	{
		size_t		path_len;
		const char *path = chunks_pack_PackReader_path(reader, &path_len);

		std_path_to_path_buf(path, path_len, &frame->path);
		frame->path_live = true;
	}
	id = reader->id;
	listed = chunks_packs_table_packs_get(&chunks_packs_view_View_table(self->view)->packs, frame->number);
	before = listed != NULL && listed->has_dead;
	if (before)
	{
		space = listed->space;
		older = listed->dead;
		if (older == frame->generation)
		{
			StdString	message;
			StdFmtPiece pieces[] = {STD_FMT_LIT("pack number "), STD_FMT_ARG(0), STD_FMT_LIT(" already has the vector "),
			STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&frame->number, std_u64_Display_fmt}, {&frame->generation, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&message, &arguments);
			*frame->error = chunks_error_Error_corrupt(&message);
			goto done;
		}
		if (self->room == 0)
		{
			*frame->result = false;
			frame->ok = true;
			goto done;
		}
	}
	if (!chunks_dead_Dead_write(HOST_SHARED_LONG_IO_IO_DOORS(&io), frame->path.bytes.ptr, frame->path.bytes.len,
								frame->generation, frame->offsets, frame->error))
		goto done;
	if (!chunks_packs_retire_lists(&io, &frame->lists, frame->error))
		goto done;
	if (!chunks_packs_lists_Change_begin(self->view, self->record, &frame->lists, &frame->change, frame->error))
		goto done;
	frame->change_live = true;
	if (!chunks_packs_lists_Change_name_vector(&frame->change, frame->number, frame->generation, frame->error))
		goto done;
	frame->change_live = false;
	if (!chunks_packs_lists_Change_install(&frame->change, frame->error))
		goto done;
	if (before)
	{
		ChunksPacksRemovalFile file = {space == CHUNKS_PACKS_DEFAULT_SPACE ? self->own : space,
		{CHUNKS_PACKS_SINGLE_QUEUED_VECTOR, {older}}, id};

		std_vec_push_Checked(&self->replaced, &file, sizeof(file), _Alignof(ChunksPacksRemovalFile));
		self->room = STD_SUB(usize, self->room, 1, 118, 13);
	}
	*frame->result = true;
	frame->ok = true;
done:
	chunks_packs_retire_set_dead_pad_0(frame);
}

/*
 * chunks_packs_retire_Marking_set_dead makes the chunks a full collection
 * found dead in a listed pack it keeps those a find answers only when no
 * other copy is listed, and reports whether it did: it writes the generation
 * of the pack's deletion vector that the collection gives, durably, and then
 * names that generation in the pack table. A pack's older generation goes
 * through the removal queue, so with no entry of the retirement's
 * reservation left for it the vector isn't replaced.
 */
bool
chunks_packs_retire_Marking_set_dead(ChunksPacksRetireMarking *self, uint64 number, uint64 generation,
									 const StdBTreeMap *offsets, bool *result, ChunksErrorError *error)
{
	chunks_packs_retire_set_dead_frame_0_t frame = {.self = self, .number = number, .generation = generation,
	.offsets = offsets, .result = result, .error = error};

	std_landing(&frame, chunks_packs_retire_set_dead_body_0, chunks_packs_retire_set_dead_pad_0);
	return frame.ok;
}

/* chunks_packs_retire_Packs_retire_frame_0_t is retire's state across its pad: what it takes, its guards, the marking, the change, and what it collects. */
typedef struct chunks_packs_retire_Packs_retire_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	const uint64 *numbers;
	size_t		numbers_len;
	ChunksPacksRetireForwarded *forwarded;
	bool		forwarded_live;
	ChunksPacksRemovalReservation *reservation;
	bool		reservation_live;
	bool		(*keep) (void *captures, ChunksPacksRetireMarking *marking, StdVec *entries, StdVec *result,
						 ChunksErrorError *error);
	void	   *keep_captures;
	void		(*keep_drop) (void *);
	bool		keep_live;
	ChunksPacksRetireRetired *result;
	ChunksErrorError *error;
	bool		ok;
	HostLockLending view;
	bool		view_live;
	HostSharedLongGuard held;
	bool		held_live;
	StdVec		slots;
	bool		slots_live;
	StdVec		recorded;
	bool		recorded_live;

	/*
	 * kept is a BTreeSet<u64> of the packs keep found in use: a Vec in order,
	 * each once.
	 */
	StdVec		kept;
	bool		kept_live;
	ChunksPacksRetireMarking marking;
	bool		marking_live;
	ChunksPacksListsChange change;
	bool		change_live;
	/* retired is a Vec<u64> of the packs it takes out. */
	StdVec		retired;
	bool		retired_live;

	/*
	 * runs is a Vec<(u32, Vec<String>)> of the runs it takes out, by
	 * tablespace.
	 */
	StdVec		runs;
	bool		runs_live;
	/* listed is a Vec<TableEntry> of the packs it takes out. */
	StdVec		listed;
	bool		listed_live;
	StdString	run;
	bool		run_live;
	StdVec		record_bytes;
	bool		record_bytes_live;
} chunks_packs_retire_Packs_retire_frame_0_t;

/* chunks_TupleU32VecString_t is a tablespace and the names of the runs a retirement takes out of it. */
typedef struct chunks_TupleU32VecString_t
{
	uint32		f0;
	StdVec		f1;
} chunks_TupleU32VecString_t;

/* chunks_packs_retire_Packs_retire_pad_0 is retire's pad: what it holds, the latest first. */
static void
chunks_packs_retire_Packs_retire_pad_0(void *frame_)
{
	chunks_packs_retire_Packs_retire_frame_0_t *frame = frame_;

	if (frame->record_bytes_live)
	{
		frame->record_bytes_live = false;
		std_vec_free_Checked(&frame->record_bytes, 1, 1);
	}
	if (frame->run_live)
	{
		frame->run_live = false;
		std_vec_free_Checked(&frame->run, 1, 1);
	}
	if (frame->listed_live)
	{
		frame->listed_live = false;
		std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	}
	if (frame->runs_live)
	{
		frame->runs_live = false;
		for (size_t k = 0, k_len = frame->runs.len; k < k_len; k++)
			std_vec_drop_Checked(&STD_VEC_AT(&frame->runs, chunks_TupleU32VecString_t, k).f1, sizeof(StdString),
								 _Alignof(StdString), std_String_drop);
		std_vec_free_Checked(&frame->runs, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t));
	}
	if (frame->retired_live)
	{
		frame->retired_live = false;
		std_vec_free_Checked(&frame->retired, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->change_live)
	{
		frame->change_live = false;
		chunks_packs_lists_Change_drop(&frame->change);
	}
	if (frame->marking_live)
	{
		frame->marking_live = false;
		std_vec_free_Checked(&frame->marking.replaced, sizeof(ChunksPacksRemovalFile),
							 _Alignof(ChunksPacksRemovalFile));
	}
	if (frame->kept_live)
	{
		frame->kept_live = false;
		std_vec_free_Checked(&frame->kept, sizeof(uint64), _Alignof(uint64));
	}
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
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_LongGuard_drop(&frame->held);
	}
	if (frame->view_live)
	{
		frame->view_live = false;
		host_lock_Lending_drop(&frame->view);
	}
	if (frame->reservation_live)
	{
		frame->reservation_live = false;
		chunks_packs_removal_Reservation_drop(frame->reservation);
	}
	if (frame->forwarded_live)
	{
		frame->forwarded_live = false;
		chunks_packs_retire_Forwarded_drop(frame->forwarded);
	}
	if (frame->keep_live)
	{
		frame->keep_live = false;
		if (frame->keep_drop != NULL)
			frame->keep_drop(frame->keep_captures);
	}
}

/* chunks_packs_retire_names reports whether a list of numbers holds one. */
static bool
chunks_packs_retire_names(const uint64 *numbers, size_t numbers_len, uint64 number)
{
	for (size_t k = 0; k < numbers_len; k++)
		if (numbers[k] == number)
			return true;
	return false;
}

/* chunks_packs_retire_hits reads the marking's recorded hits under the barrier, as (Address, Locator) entries. */
static bool
chunks_packs_retire_hits(chunks_packs_retire_Packs_retire_frame_0_t *frame, HostSharedRecord hits)
{
	HostError	host;
	bool		ok = true;

	if (!host_shared_Record_read(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->held, &frame->slots, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	frame->slots_live = true;
	std_vec_new(&frame->recorded, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	frame->recorded_live = true;
	for (size_t k = 0, k_len = frame->slots.len; ok && k < k_len; k++)
	{
		chunks_TupleAddressLocator_t hit;

		ok = chunks_packs_single_hit_of(STD_VEC_AT(&frame->slots, StdVec, k).ptr,
										STD_VEC_AT(&frame->slots, StdVec, k).len, &hit, frame->error);
		if (ok)
			std_vec_push_Checked(&frame->recorded, &hit, sizeof(hit), _Alignof(chunks_TupleAddressLocator_t));
	}
	frame->slots_live = false;
	chunks_pack_VecVecU8_drop(&frame->slots);
	return ok;
}

/* chunks_packs_retire_own_run reports whether a tablespace's runs file, as the view read it, names a run. */
static bool
chunks_packs_retire_own_run(const ChunksPacksViewView *view, uint32 space, const StdString *run)
{
	const ChunksPacksViewReadRuns *runs = chunks_packs_view_runs_get(&view->runs, space);

	if (runs == NULL)
		return false;
	for (size_t k = 0, k_len = runs->list.lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->list.lines, StdString, k);
		StdStr		name = chunks_packs_run_name(line->ptr, line->len);

		if (name.len == run->len && memcmp(name.ptr, run->ptr, name.len) == 0)
			return true;
	}
	return false;
}

/*
 * chunks_packs_retire_runs_push adds a run's name to the names taken out of
 * its tablespace, which it takes from the frame once a vector holds it.
 */
static void
chunks_packs_retire_runs_push(StdVec *runs, uint32 space, StdString *run, bool *run_live)
{
	chunks_TupleU32VecString_t *of = NULL;

	for (size_t k = 0, k_len = runs->len; k < k_len && of == NULL; k++)
		if (STD_VEC_AT(runs, chunks_TupleU32VecString_t, k).f0 == space)
			of = &STD_VEC_AT(runs, chunks_TupleU32VecString_t, k);
	if (of == NULL)
	{
		chunks_TupleU32VecString_t entry = {space};

		std_vec_new(&entry.f1, sizeof(StdString), _Alignof(StdString));
		std_vec_push_Checked(runs, &entry, sizeof(entry), _Alignof(chunks_TupleU32VecString_t));
		of = &STD_VEC_AT(runs, chunks_TupleU32VecString_t, runs->len - 1);
	}
	std_vec_push_Checked(&of->f1, run, sizeof(*run), _Alignof(StdString));
	*run_live = false;
}

/* chunks_packs_retire_files_push adds a file to queue. */
static void
chunks_packs_retire_files_push(StdVec *files, uint32 space, ChunksPacksSingleQueuedTag kind, uint64 vector,
							   ChunksPackPackId id)
{
	ChunksPacksRemovalFile file = {space, {kind}, id};

	if (kind == CHUNKS_PACKS_SINGLE_QUEUED_VECTOR)
		file.f1.u.vector = vector;
	std_vec_push_Checked(files, &file, sizeof(file), _Alignof(ChunksPacksRemovalFile));
}

/* chunks_packs_retire_noted is Retired's event, of the packs taken out by number and id, which the captures give. */
static void
chunks_packs_retire_noted(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_retire_Packs_retire_frame_0_t *frame = captures;
	const StdVec *files = &frame->marking.replaced;
	size_t		at = 0;

	result->tag = CHUNKS_TRACE_EVENT_RETIRED;
	std_vec_new(&result->u.retired.packs, sizeof(chunks_TupleU64PackId_t), _Alignof(chunks_TupleU64PackId_t));
	for (size_t k = 0, k_len = files->len; k < k_len && at < frame->retired.len; k++)
	{
		const ChunksPacksRemovalFile *file = &STD_VEC_AT(files, ChunksPacksRemovalFile, k);
		chunks_TupleU64PackId_t noted;

		if (file->f1.tag != CHUNKS_PACKS_SINGLE_QUEUED_PACK)
			continue;
		noted.f0 = STD_VEC_AT(&frame->retired, uint64, at++);
		noted.f1 = file->f2;
		std_vec_push_Checked(&result->u.retired.packs, &noted, sizeof(noted), _Alignof(chunks_TupleU64PackId_t));
	}
}

/* chunks_packs_retire_marking_retired is MarkingRetired's event. */
static void
chunks_packs_retire_marking_retired(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_MARKING_RETIRED;
}

/* chunks_packs_retire_take takes the packs `keep` didn't keep out of the change, as many as the room holds, in the order given. */
static bool
chunks_packs_retire_take(chunks_packs_retire_Packs_retire_frame_0_t *frame)
{
	const ChunksPacksViewView *view = frame->change.view;
	uint32		own = frame->self->own;
	size_t		room = frame->marking.room;

	for (size_t k = 0; k < frame->numbers_len; k++)
	{
		uint64		number = frame->numbers[k];
		const ChunksPacksListed *found;
		ChunksPacksListed listed;
		ChunksPacksTableEntry entry;
		uint32		space;
		bool		own_run;
		size_t		needed;

		size_t		at;

		if (std_slice_binary_search(frame->kept.ptr, frame->kept.len, sizeof(uint64), &number, std_u64_Ord_cmp, &at))
			continue;
		found = chunks_packs_table_packs_get(&chunks_packs_view_View_table(view)->packs, number);
		if (found == NULL)
		{
			*frame->error = chunks_packs_retire_corrupt("pack number ", number, " is not in the table");
			return false;
		}
		listed = *found;
		space = listed.space == CHUNKS_PACKS_DEFAULT_SPACE ? own : listed.space;
		frame->run = chunks_pack_PackId_file_name(&listed.id, "idx", 3);
		frame->run_live = true;
		own_run = chunks_packs_retire_own_run(view, listed.space, &frame->run);
		needed = STD_ADD(usize, STD_ADD(usize, 1, own_run ? 1 : 0, 177, 30), listed.has_dead ? 1 : 0, 177, 30);
		if (needed > room)
			break;
		room = STD_SUB(usize, room, needed, 181, 17);
		std_vec_push_Checked(&frame->retired, &number, sizeof(number), _Alignof(uint64));
		entry.f0 = number;
		entry.f1 = listed.space;
		entry.f2 = listed.id;
		std_vec_push_Checked(&frame->listed, &entry, sizeof(entry), _Alignof(ChunksPacksTableEntry));
		chunks_packs_retire_files_push(&frame->marking.replaced, space, CHUNKS_PACKS_SINGLE_QUEUED_PACK, 0, listed.id);
		if (listed.has_dead)
			chunks_packs_retire_files_push(&frame->marking.replaced, space, CHUNKS_PACKS_SINGLE_QUEUED_VECTOR,
										   listed.dead, listed.id);
		if (own_run)
		{
			chunks_packs_retire_files_push(&frame->marking.replaced, space, CHUNKS_PACKS_SINGLE_QUEUED_RUN, 0,
										   listed.id);
			chunks_packs_retire_runs_push(&frame->runs, listed.space, &frame->run, &frame->run_live);
		}
		else
		{
			frame->run_live = false;
			std_vec_free_Checked(&frame->run, 1, 1);
		}
	}
	return true;
}

/*
 * chunks_packs_retire_Packs_retire_body_0 is retire's body: the forwards
 * checked, then under the barrier held exclusively the hits told to `keep`,
 * the rest taken out of the pack table in one change with their runs, and
 * the marking ended.
 */
static void
chunks_packs_retire_Packs_retire_body_0(void *frame_)
{
	chunks_packs_retire_Packs_retire_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedRecord record = host_shared_Held_record(&self->held);
	HostSharedRecord hits;
	HostSharedLongIo io;
	HostSharedLongGuard lists;
	HostError	host;
	uint64		marked;

	frame->ok = false;
	if (frame->forwarded->packs != chunks_packs_backend_Packs_location(self))
	{
		*frame->error = chunks_error_Error_corrupt_RefStr("a retirement is given another repository's forwards",
														  strlen("a retirement is given another repository's forwards"));
		goto done;
	}
	for (size_t k = 0, k_len = frame->forwarded->froms.len; k < k_len; k++)
	{
		uint64		pack = STD_VEC_AT(&frame->forwarded->froms, std_TupleU64U64_t, k).f0;

		if (!chunks_packs_retire_names(frame->numbers, frame->numbers_len, pack))
		{
			*frame->error = chunks_packs_retire_corrupt("a forward from pack ", pack,
														" serves no pack its retirement names");
			goto done;
		}
	}
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &hits, &host))
		goto failed;
	frame->view = chunks_packs_backend_Packs_view_lock(self, HOST_IO_IO_TAKES_SHORT(frame->io));
	frame->view_live = true;
	if (!host_shared_Record_lock_long(record, host_lock_Lending_io(&frame->view), CHUNKS_PACKS_SHARED_BARRIER_INDEX,
									  &frame->held, &host))
		goto failed;
	frame->held_live = true;
	marked = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_MARKED_INDEX);
	for (size_t k = 0; k < frame->numbers_len; k++)
		if (frame->numbers[k] >= marked)
		{
			*frame->error = chunks_packs_retire_corrupt("pack number ", frame->numbers[k],
														" is retired by no marking that covers it");
			goto done;
		}
	if (!chunks_packs_retire_hits(frame, hits))
		goto done;
	frame->marking.view = host_lock_Lending_DerefMut_deref_mut(&frame->view);
	frame->marking.record = record;
	frame->marking.barrier = &frame->held;
	frame->marking.own = self->own;
	frame->marking.room = chunks_packs_removal_Reservation_entries(frame->reservation);
	std_vec_new(&frame->marking.replaced, sizeof(ChunksPacksRemovalFile), _Alignof(ChunksPacksRemovalFile));
	frame->marking_live = true;
	frame->recorded_live = false;
	std_vec_new(&frame->kept, sizeof(uint64), _Alignof(uint64));
	frame->kept_live = true;
	frame->keep_live = false;
	if (!frame->keep(frame->keep_captures, &frame->marking, &frame->recorded, &frame->kept, frame->error))
		goto done;

	io = host_shared_LongGuard_io(&frame->held);
	if (!chunks_packs_retire_lists(&io, &lists, frame->error))
		goto done;
	if (!chunks_packs_lists_Change_begin(frame->marking.view, record, &lists, &frame->change, frame->error))
		goto done;
	frame->change_live = true;
	if (!chunks_packs_lists_Change_flush(&frame->change, frame->error))
		goto done;
	std_vec_new(&frame->retired, sizeof(uint64), _Alignof(uint64));
	frame->retired_live = true;
	std_vec_new(&frame->runs, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t));
	frame->runs_live = true;
	std_vec_new(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	frame->listed_live = true;
	if (!chunks_packs_retire_take(frame))
		goto done;
	for (size_t k = 0, k_len = frame->runs.len; k < k_len; k++)
	{
		const chunks_TupleU32VecString_t *of = &STD_VEC_AT(&frame->runs, chunks_TupleU32VecString_t, k);

		chunks_packs_Runs_removed(of->f1.ptr, of->f1.len, &frame->record_bytes);
		frame->record_bytes_live = true;
		if (!chunks_packs_lists_Change_change_runs(&frame->change, of->f0, frame->record_bytes.ptr,
												   frame->record_bytes.len, frame->error))
			goto done;
		frame->record_bytes_live = false;
		std_vec_free_Checked(&frame->record_bytes, 1, 1);
	}
	if (frame->retired.len != 0)
	{
		if (!chunks_packs_lists_Change_retire(&frame->change, frame->retired.ptr, frame->retired.len, frame->error))
			goto done;
		chunks_packs_lists_Change_note(&frame->change, chunks_packs_retire_noted, frame);
	}
	frame->change_live = false;
	if (!chunks_packs_lists_Change_install(&frame->change, frame->error))
		goto done;
	chunks_packs_shared_set(record, CHUNKS_PACKS_SHARED_MARKED_INDEX, 0);
	if (!host_shared_Record_unmark(hits, CHUNKS_PACKS_SINGLE_HITS, &frame->held, &host))
		goto failed;
	chunks_trace_emit(self->repository, chunks_packs_retire_marking_retired, NULL, NULL);
	frame->result->packs = self;
	frame->result->numbers = frame->retired;
	frame->retired_live = false;
	frame->result->listed = frame->listed;
	frame->listed_live = false;
	frame->result->files = frame->marking.replaced;
	frame->marking_live = false;
	frame->result->reservation = *frame->reservation;
	frame->reservation_live = false;
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_retire_Packs_retire_pad_0(frame);
}

/*
 * chunks_packs_retire_Packs_retire takes packs out of the pack table, with
 * the address-index runs that were theirs alone, and returns them as
 * Retired, their files unqueued, for a verify marking to list again any it
 * reaches before the rest are queued. Under the barrier held exclusively,
 * `keep` is told the hits the marking recorded and returns the packs it
 * found still in use, which stay; of the rest it retires, in the order given,
 * as many as the reservation has entries for after what `keep` replaced,
 * each pack's file, its own run, and its deletion vector. The marking ends
 * with it. It refuses forwards of another repository's packs, a forward from
 * a pack the retirement doesn't name, and a pack the marking doesn't cover.
 * It takes the forwards and the reservation.
 */
bool
chunks_packs_retire_Packs_retire(const ChunksPacksBackendPacks *self, HostIoIo *io, const uint64 *numbers,
								 size_t numbers_len, ChunksPacksRetireForwarded *forwarded,
								 ChunksPacksRemovalReservation *reservation,
								 bool (*keep) (void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
											   StdVec *result, ChunksErrorError *error),
								 void *keep_captures, void (*keep_drop) (void *), ChunksPacksRetireRetired *result,
								 ChunksErrorError *error)
{
	chunks_packs_retire_Packs_retire_frame_0_t frame = {.self = self, .io = io, .numbers = numbers,
		.numbers_len = numbers_len, .forwarded = forwarded, .forwarded_live = true, .reservation = reservation,
		.reservation_live = true, .keep = keep, .keep_captures = keep_captures, .keep_drop = keep_drop,
	.keep_live = true, .result = result, .error = error};

	std_landing(&frame, chunks_packs_retire_Packs_retire_body_0, chunks_packs_retire_Packs_retire_pad_0);
	return frame.ok;
}

/* chunks_packs_retire_Retired_numbers returns the numbers of the packs the retirement took out, those restored since included. */
const uint64 *
chunks_packs_retire_Retired_numbers(const ChunksPacksRetireRetired *self, size_t *result_len)
{
	*result_len = self->numbers.len;
	return self->numbers.ptr;
}

/* chunks_packs_retire_restore_t is restore's closures' captures: the packs, the reader, and the tablespace. */
typedef struct chunks_packs_retire_restore_t
{
	const ChunksPacksBackendPacks *packs;
	const HostSharedReader *reading;
	uint32		space;
	ChunksPacksTableEntry entry;
	StdPathBuf *dir;
} chunks_packs_retire_restore_t;

/* chunks_packs_retire_Retired_restore_closure_3 is restore's reserved: an op on the record's Reserved. */
static uint64
chunks_packs_retire_Retired_restore_closure_3(void *captures, HostSharedOp op)
{
	const chunks_packs_retire_restore_t *restore = captures;

	return host_shared_Record_atomic(host_shared_Held_record(&restore->packs->held), CHUNKS_PACKS_SHARED_RESERVED_INDEX,
									 op);
}

/* chunks_packs_retire_reserve is restore's raise's change: the bound raised past the one read. */
static bool
chunks_packs_retire_reserve(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	return chunks_packs_lists_Change_reserve(change, *(const uint64 *) captures, error);
}

/* chunks_packs_retire_Retired_restore_closure_2 is restore's raise: a change that raises the bound of the writers' numbers. */
static bool
chunks_packs_retire_Retired_restore_closure_2(void *captures, HostIoIo *io, uint64 numbers, ChunksErrorError *error)
{
	const chunks_packs_retire_restore_t *restore = captures;

	return chunks_packs_backend_Packs_changed(restore->packs, HOST_IO_IO_TAKES_SHORT(io), restore->reading,
											  chunks_packs_retire_reserve, &numbers, error);
}

/* chunks_packs_retire_Retired_restore_closure_4 is restore's read: the tablespace's directory, as the view names it. */
static bool
chunks_packs_retire_Retired_restore_closure_4(void *captures, ChunksPacksViewView *view, HostIoIo *io,
											  ChunksErrorError *error)
{
	const chunks_packs_retire_restore_t *restore = captures;

	(void) io;
	(void) error;
	chunks_packs_view_View_space_dir(view, restore->space, restore->dir);
	return true;
}

/* chunks_packs_retire_Retired_restore_closure_5 is restore's change: the pack added again under its fresh number. */
static bool
chunks_packs_retire_Retired_restore_closure_5(void *captures, ChunksPacksListsChange *change, ChunksErrorError *error)
{
	const chunks_packs_retire_restore_t *restore = captures;

	return chunks_packs_lists_Change_add(change, &restore->entry, false, error);
}

/* chunks_packs_retire_restored_t is Restored's captures: the old number and the fresh one. */
typedef struct chunks_packs_retire_restored_t
{
	uint64		from;
	uint64		to;
} chunks_packs_retire_restored_t;

/* chunks_packs_retire_Retired_restore_closure_7 is Restored's event. */
static void
chunks_packs_retire_Retired_restore_closure_7(void *captures, ChunksTraceEvent *result)
{
	const chunks_packs_retire_restored_t *restored = captures;

	result->tag = CHUNKS_TRACE_EVENT_RESTORED;
	result->u.restored.from = restored->from;
	result->u.restored.to = restored->to;
}

/* chunks_packs_retire_Retired_restore_frame_0_t is restore's state across its pad: the number's hold, the directory, the paths, and the loss. */
typedef struct chunks_packs_retire_Retired_restore_frame_0_t
{
	ChunksPacksRetireRetired *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	uint64		number;
	uint64	   *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksPacksWritersWriting hold;
	bool		hold_live;
	StdPathBuf	dir;
	bool		dir_live;
	StdPathBuf	pack;
	bool		pack_live;
	StdPathBuf	run;
	bool		run_live;
	ChunksPackLost lost;
	bool		lost_live;
	StdString	name;
	bool		name_live;
} chunks_packs_retire_Retired_restore_frame_0_t;

/* chunks_packs_retire_Retired_restore_pad_0 is restore's pad: the loss, the paths, the directory, then the hold. */
static void
chunks_packs_retire_Retired_restore_pad_0(void *frame_)
{
	chunks_packs_retire_Retired_restore_frame_0_t *frame = frame_;

	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
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
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
	if (frame->hold_live)
	{
		frame->hold_live = false;
		chunks_packs_writers_Writing_drop(&frame->hold);
	}
}

/* chunks_packs_retire_Retired_restore_join joins a pack's file name to the directory into a path the frame holds. */
static void
chunks_packs_retire_Retired_restore_join(const StdPathBuf *dir, const ChunksPackPackId *id, const char *extension,
										 StdString *name, bool *name_live, StdPathBuf *result, bool *live)
{
	*name = chunks_pack_PackId_file_name(id, extension, strlen(extension));
	*name_live = true;
	std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, result);
	*live = true;
	*name_live = false;
	std_vec_free_Checked(name, 1, 1);
}

/*
 * chunks_packs_retire_Retired_restore_body_0 is restore's body: a fresh
 * number held in the pack's tablespace, the pack's run written again for it,
 * the pack added under it, and its files kept off the queue.
 */
static void
chunks_packs_retire_Retired_restore_body_0(void *frame_)
{
	chunks_packs_retire_Retired_restore_frame_0_t *frame = frame_;
	ChunksPacksRetireRetired *self = frame->self;
	const ChunksPacksBackendPacks *packs = self->packs;
	chunks_packs_retire_restore_t restore = {.packs = packs, .reading = frame->reading, .dir = &frame->dir};
	chunks_packs_retire_restored_t restored = {frame->number};
	ChunksPacksTableEntry taken;
	size_t		at = self->listed.len;
	size_t		kept = 0;

	frame->ok = false;
	for (size_t k = 0, k_len = self->listed.len; k < k_len && at == k_len; k++)
		if (STD_VEC_AT(&self->listed, ChunksPacksTableEntry, k).f0 == frame->number)
			at = k;
	if (at == self->listed.len)
	{
		*frame->error = chunks_packs_retire_corrupt("pack number ", frame->number, " isn't one the retirement took");
		return;
	}
	std_vec_remove(&self->listed, at, &taken, sizeof(taken), _Alignof(ChunksPacksTableEntry), RUST_FILE, 239, 42);
	restore.space = taken.f1;
	if (!chunks_packs_writers_Writing_open(frame->io, packs->repository, taken.f1,
										   chunks_packs_retire_Retired_restore_closure_3, &restore, NULL,
										   chunks_packs_retire_Retired_restore_closure_2, &restore, NULL, &frame->hold,
										   frame->error))
		return;
	frame->hold_live = true;
	restored.to = chunks_packs_writers_Writing_number(&frame->hold);
	if (!chunks_packs_backend_Packs_viewed(packs, frame->io, chunks_packs_retire_Retired_restore_closure_4, &restore,
										   frame->error))
		goto done;
	frame->dir_live = true;
	chunks_packs_retire_Retired_restore_join(&frame->dir, &taken.f2, "pack", &frame->name, &frame->name_live,
											 &frame->pack, &frame->pack_live);
	chunks_packs_retire_Retired_restore_join(&frame->dir, &taken.f2, "idx", &frame->name, &frame->name_live,
											 &frame->run, &frame->run_live);
	if (!chunks_packs_settle_rebuilt(frame->io, frame->pack.bytes.ptr, frame->pack.bytes.len, restored.to,
									 frame->run.bytes.ptr, frame->run.bytes.len, &frame->lost, frame->error))
		goto done;
	frame->lost_live = true;
	restore.entry.f0 = restored.to;
	restore.entry.f1 = taken.f1;
	restore.entry.f2 = taken.f2;
	if (!chunks_packs_backend_Packs_changed(packs, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading,
											chunks_packs_retire_Retired_restore_closure_5, &restore, frame->error))
		goto done;
	frame->hold_live = false;
	chunks_packs_writers_Writing_drop(&frame->hold);
	for (size_t k = 0, k_len = self->files.len; k < k_len; k++)
	{
		const ChunksPacksRemovalFile *file = &STD_VEC_AT(&self->files, ChunksPacksRemovalFile, k);

		if (!chunks_pack_PackId_PartialEq_eq(&file->f2, &taken.f2) || file->f1.tag == CHUNKS_PACKS_SINGLE_QUEUED_VECTOR)
			STD_VEC_AT(&self->files, ChunksPacksRemovalFile, kept++) = *file;
	}
	self->files.len = kept;
	chunks_trace_emit(packs->repository, chunks_packs_retire_Retired_restore_closure_7, &restored, NULL);
	*frame->result = restored.to;
	frame->ok = true;
done:
	chunks_packs_retire_Retired_restore_pad_0(frame);
}

/*
 * chunks_packs_retire_Retired_restore lists a retired pack's file again
 * under a fresh number in its tablespace, its address-index run written
 * again from the pack for that number, and takes its files out of those to
 * queue; it returns the fresh number. The forwards out of its old number
 * stay as they are, which is harmless.
 */
bool
chunks_packs_retire_Retired_restore(ChunksPacksRetireRetired *self, HostIoIo *io, const HostSharedReader *reading,
									uint64 number, uint64 *result, ChunksErrorError *error)
{
	chunks_packs_retire_Retired_restore_frame_0_t frame = {.self = self, .io = io, .reading = reading,
	.number = number, .result = result, .error = error};

	std_landing(&frame, chunks_packs_retire_Retired_restore_body_0, chunks_packs_retire_Retired_restore_pad_0);
	return frame.ok;
}

/* chunks_packs_retire_queued is RetiredQueued's event, of the numbers the captures' entries give. */
static void
chunks_packs_retire_queued(void *captures, ChunksTraceEvent *result)
{
	const StdVec *listed = captures;

	result->tag = CHUNKS_TRACE_EVENT_RETIRED_QUEUED;
	std_vec_new(&result->u.retired_queued.packs, sizeof(uint64), _Alignof(uint64));
	for (size_t k = 0, k_len = listed->len; k < k_len; k++)
		std_vec_push_Checked(&result->u.retired_queued.packs, &STD_VEC_AT(listed, ChunksPacksTableEntry, k).f0,
							 sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_retire_Retired_queue_frame_0_t is queue's state across its pad: the retirement it takes, but for its reservation once the queue takes it. */
typedef struct chunks_packs_retire_Retired_queue_frame_0_t
{
	ChunksPacksRetireRetired *self;
	HostIoIo   *io;
	const HostSharedReader *reading;
	ChunksErrorError *error;
	bool		ok;
	bool		reservation_live;
	bool		rest_live;
} chunks_packs_retire_Retired_queue_frame_0_t;

/* chunks_packs_retire_Retired_queue_pad_0 is queue's pad: the retirement's numbers, entries, and files, and its reservation while it holds it. */
static void
chunks_packs_retire_Retired_queue_pad_0(void *frame_)
{
	chunks_packs_retire_Retired_queue_frame_0_t *frame = frame_;

	if (frame->rest_live)
	{
		frame->rest_live = false;
		std_vec_free_Checked(&frame->self->numbers, sizeof(uint64), _Alignof(uint64));
		std_vec_free_Checked(&frame->self->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
		std_vec_free_Checked(&frame->self->files, sizeof(ChunksPacksRemovalFile), _Alignof(ChunksPacksRemovalFile));
	}
	if (frame->reservation_live)
	{
		frame->reservation_live = false;
		chunks_packs_removal_Reservation_drop(&frame->self->reservation);
	}
}

/* chunks_packs_retire_Retired_queue_body_0 is queue's body: the files queued in the reservation, then the list files compacted. */
static void
chunks_packs_retire_Retired_queue_body_0(void *frame_)
{
	chunks_packs_retire_Retired_queue_frame_0_t *frame = frame_;
	ChunksPacksRetireRetired *self = frame->self;
	const ChunksPacksBackendPacks *packs = self->packs;

	frame->ok = false;
	frame->reservation_live = false;
	if (!chunks_packs_removal_queue(frame->io, &self->reservation, (uint32) packs->repository, self->files.ptr,
									self->files.len, frame->error))
		goto done;
	chunks_trace_emit(packs->repository, chunks_packs_retire_queued, &self->listed, NULL);
	frame->ok = chunks_packs_backend_Packs_compact(packs, HOST_IO_IO_TAKES_SHORT(frame->io), frame->reading, false,
												   frame->error);
done:
	chunks_packs_retire_Retired_queue_pad_0(frame);
}

/*
 * chunks_packs_retire_Retired_queue queues the files of the packs the
 * retirement took out and didn't restore, in its reservation, and writes
 * whole any list file the change left past its slack. It takes the
 * retirement.
 */
bool
chunks_packs_retire_Retired_queue(ChunksPacksRetireRetired *self, HostIoIo *io, const HostSharedReader *reading,
								  ChunksErrorError *error)
{
	chunks_packs_retire_Retired_queue_frame_0_t frame = {.self = self, .io = io, .reading = reading, .error = error,
	.reservation_live = true, .rest_live = true};

	std_landing(&frame, chunks_packs_retire_Retired_queue_body_0, chunks_packs_retire_Retired_queue_pad_0);
	return frame.ok;
}

/* chunks_packs_retire_Retired_drop is Retired's drop glue: its numbers, entries, and files, and its reservation's. */
void
chunks_packs_retire_Retired_drop(ChunksPacksRetireRetired *self)
{
	std_vec_free_Checked(&self->numbers, sizeof(uint64), _Alignof(uint64));
	std_vec_free_Checked(&self->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_free_Checked(&self->files, sizeof(ChunksPacksRemovalFile), _Alignof(ChunksPacksRemovalFile));
	chunks_packs_removal_Reservation_drop(&self->reservation);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_retire_tests_names returns how many files of a repository's own directory end with some text. */
static size_t
chunks_packs_retire_tests_names(uint32 id, const char *ending)
{
	StdPathBuf	dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
	StdVec		files;
	StdIoError	io_error;
	bool		entry;
	size_t		count = 0;

	if (!std_fs_read_dir_names(dir.bytes.ptr, dir.bytes.len, &files, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(275, 43, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(274, 83, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	for (size_t k = 0, k_len = files.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&files, StdString, k);

		if (name->len >= strlen(ending) &&
			memcmp((const char *) name->ptr + name->len - strlen(ending), ending, strlen(ending)) == 0)
			count++;
	}
	std_vec_drop_Checked(&files, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return count;
}

/* chunks_packs_retire_tests_none keeps no pack, whatever the marking's hits. */
static bool
chunks_packs_retire_tests_none(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries, StdVec *result,
							   ChunksErrorError *error)
{
	(void) captures;
	(void) marking;
	(void) result;
	(void) error;
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/* chunks_packs_retire_tests_reserved returns a reservation of up to as many entries as wanted, which unwraps at a line and column. */
static ChunksPacksRemovalReservation
chunks_packs_retire_tests_reserved(HostIoIo *io, size_t wanted, uint32 line, uint32 col)
{
	ChunksPacksRemovalReservation reservation;
	ChunksErrorError error;

	if (!chunks_packs_removal_reserve(io, wanted, &reservation, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return reservation;
}

/* chunks_packs_retire_tests_forwarded returns the proof of a forward of moves, which unwraps at a line and column. */
static ChunksPacksRetireForwarded
chunks_packs_retire_tests_forwarded(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									const HostSharedReader *reading,
									const chunks_packs_backend_Packs_forward_moves_t *moves, size_t moves_len,
									uint32 line, uint32 col)
{
	ChunksPacksRetireForwarded forwarded;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_forward(packs, io, reading, moves, moves_len, &forwarded, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return forwarded;
}

/* chunks_packs_retire_tests_marked begins a marking of the repository's packs, which unwraps at a line and column, and returns its newest pack. */
static uint64
chunks_packs_retire_tests_marked(const ChunksPacksBackendPacks *packs, HostIoIo *io, const HostSharedReader *reading,
								 uint32 line, uint32 col)
{
	uint64		newest;
	ChunksErrorError error;

	if (!chunks_packs_marking_Packs_begin_marking(packs, io, reading, &newest, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return newest;
}

/* chunks_packs_retire_tests_eq asserts at a line and column that a number is the one given. */
static void
chunks_packs_retire_tests_eq(uint64 got, uint64 want, uint32 line, uint32 col)
{
	if (got != want)
		std_assert_failed(RUST_FILE, line, col, "==", &got, std_u64_Debug_fmt, &want, std_u64_Debug_fmt, NULL);
}

/* chunks_packs_retire_tests_queued_eq asserts at a line that a retirement's queueing, which unwraps at a column, took out the numbers given. */
static void
chunks_packs_retire_tests_queued_eq(HostIoIo *io, const HostSharedReader *reading, bool retired_ok,
									ChunksPacksRetireRetired *retired, ChunksErrorError *error, const uint64 *want,
									size_t want_len, uint32 line, uint32 col)
{
	StdVec		numbers;

	if (!chunks_packs_fixtures_queued(io, reading, retired_ok, retired, &numbers, error))
		chunks_packs_tests_unwrapped(RUST_FILE, error, line, col);
	if (numbers.len != want_len || (want_len != 0 && memcmp(numbers.ptr, want, want_len * sizeof(uint64)) != 0))
	{
		StdVec		wanted;

		std_slice_to_vec_Checked(&wanted, want, want_len, sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, line, 9, "==", &numbers, chunks_packs_tests_Numbers_Debug_fmt, &wanted,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
}

/* chunks_packs_retire_tests_forwarded_eq asserts at a line that a place's forward leads to the copy given, which unwraps at a column. */
static void
chunks_packs_retire_tests_forwarded_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
									   const HostSharedReader *reading, ChunksTrailerLocator from,
									   const ChunksTrailerLocator *to, uint32 line, uint32 col)
{
	ChunksTrailerLocator found;
	ChunksErrorError error;
	bool		has;

	if (!chunks_packs_backend_Packs_forwarded(packs, io, reading, from, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&found, to))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(to, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
}

/* chunks_packs_retire_tests_read reads a chunk through a repository's packs, which unwraps at a line and column. */
static ChunksChunkChunk
chunks_packs_retire_tests_read(const ChunksPacksBackendPacks *packs, HostIoIo *io, const HostSharedReader *reading,
							   const ChunksAddressAddress *address, ChunksTrailerLocator at, uint32 line, uint32 col)
{
	ChunksChunkChunk chunk;
	ChunksErrorError error;

	if (!chunks_packs_backend_Packs_read(packs, io, reading, address, at, &chunk, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return chunk;
}

/* chunks_packs_retire_tests_hits_refused asserts at a line that the repository's marking is over: its hits are refused with a host error. */
static void
chunks_packs_retire_tests_hits_refused(const ChunksPacksBackendPacks *packs, HostIoIo *io, const char *message,
									   uint32 line)
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
	if (!refused)
		std_panic(RUST_FILE, line, 9, message, strlen(message));
}

/* chunks_packs_retire_tests_removed removes what is due, which unwraps at a line and column 24. */
static void
chunks_packs_retire_tests_removed(HostIoIo *io, uint32 line)
{
	ChunksErrorError error;

	if (!chunks_packs_removal_remove_due(io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, 24);
}

/* chunks_packs_retire_tests_names_eq asserts at a line that a repository's directory holds as many files with two endings as given. */
static void
chunks_packs_retire_tests_names_eq(uint32 id, const char *first, const char *second, size_t want_first,
								   size_t want_second, uint32 line)
{
	size_t		got[] = {chunks_packs_retire_tests_names(id, first), chunks_packs_retire_tests_names(id, second)};
	size_t		want[] = {want_first, want_second};

	if (got[0] != want[0] || got[1] != want[1])
	{
		StdTupleRef got_pair = STD_TUPLE(2, STD_ITEM(&got[0], std_usize_Debug_fmt), STD_ITEM(&got[1], std_usize_Debug_fmt));
		StdTupleRef want_pair = STD_TUPLE(2, STD_ITEM(&want[0], std_usize_Debug_fmt),
										  STD_ITEM(&want[1], std_usize_Debug_fmt));

		std_assert_failed(RUST_FILE, line, 9, "==", &got_pair, std_tuple_Debug_fmt, &want_pair, std_tuple_Debug_fmt,
						  NULL);
	}
}

/*
 * test_chunks_packs_retire_tests_a_retirement_takes_its_packs_out_and_queues_their_files:
 * a retirement takes its packs out for every reader, after which a place in
 * one leads to its copy through the forwards, ends the marking, and queues
 * each pack's file and its run, which go once no reader that began before
 * the retirement lives.
 */
void
test_chunks_packs_retire_tests_a_retirement_takes_its_packs_out_and_queues_their_files(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 290, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks again;
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t copy;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	ChunksChunkChunk chunk;
	StdVec		copied;
	StdVec		read;
	uint64		location;
	bool		ok;
	static const uint64 one[] = {1};
	static const uint64 two[] = {2};

	chunks_packs_fixtures_created(io, 81, &packs);
	first = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	copy = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_retire_tests_eq(chunks_packs_retire_tests_marked(&packs, io, &reading, 293, 54), 2, 293, 9);
	reservation = chunks_packs_retire_tests_reserved(io, 2, 294, 42);
	moves[0].f0 = first.f1;
	moves[0].f1 = copy.f1;
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, moves, lengthof(moves), 295, 77);
	STD_ASSERT(296, 9, chunks_packs_retire_Forwarded_covers(&forwarded, 1, chunks_trailer_Locator_offset(&first.f1)) &&
			   !chunks_packs_retire_Forwarded_covers(&forwarded, 2, chunks_trailer_Locator_offset(&copy.f1)),
			   "forwarded.covers(1, at_first.offset()) && !forwarded.covers(2, at_copy.offset())");
	ok = chunks_packs_retire_Packs_retire(&packs, io, one, lengthof(one), &forwarded, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, one, lengthof(one), 298, 50);
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, two, lengthof(two), NULL, 299, 9, 48);
	chunks_packs_tests_found(RUST_FILE, &packs, io, &reading, &first.f0, &copy.f1, 300, 9, 51);
	chunks_packs_retire_tests_forwarded_eq(&packs, io, &reading, first.f1, &copy.f1, 301, 60);
	chunk = chunks_packs_retire_tests_read(&packs, io, &reading, &first.f0, copy.f1, 302, 60);
	chunks_chunk_Chunk_into_bytes(&chunk, &copied);
	chunk = chunks_packs_retire_tests_read(&packs, io, &reading, &first.f0, first.f1, 303, 61);
	chunks_chunk_Chunk_into_bytes(&chunk, &read);
	if (read.len != copied.len || memcmp(read.ptr, copied.ptr, read.len) != 0)
	{
		StdBytes	got = {read.ptr, read.len};
		StdBytes	want = {copied.ptr, copied.len};

		std_assert_failed(RUST_FILE, 303, 9, "==", &got, std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&read, 1, 1);
	std_vec_free_Checked(&copied, 1, 1);
	chunks_packs_retire_tests_hits_refused(&packs, io, "the marking went on past its retirement", 304);
	chunks_packs_retire_tests_removed(io, 305);
	chunks_packs_retire_tests_names_eq(81, ".pack", ".idx", 2, 2, 306);
	host_shared_Reader_drop(&reading);
	chunks_packs_retire_tests_removed(io, 308);
	chunks_packs_retire_tests_names_eq(81, ".pack", ".idx", 1, 1, 309);
	reading = chunks_packs_tests_reading(RUST_FILE, io, 310, 46);
	location = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 313, 75, 84);
	chunks_packs_tests_opened(RUST_FILE, io, 81, 314, 67, 95, &again);
	chunks_packs_tests_found(RUST_FILE, &again, io, &reading, &first.f0, &copy.f1, 315, 9, 51);
	chunks_packs_retire_tests_forwarded_eq(&again, io, &reading, first.f1, &copy.f1, 316, 60);
	chunks_packs_backend_Packs_drop(&again);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_retire_tests_Hit_Debug_fmt is an (Address, Locator)'s Debug. */
static bool
chunks_packs_retire_tests_Hit_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressLocator_t *hit = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&hit->f0, chunks_address_Address_Debug_fmt),
								  STD_ITEM(&hit->f1, chunks_trailer_Locator_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/* chunks_packs_retire_tests_Hits_Debug_fmt is a Vec<(Address, Locator)>'s Debug. */
static bool
chunks_packs_retire_tests_Hits_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *hits = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = hits->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(hits, chunks_TupleAddressLocator_t, k), chunks_packs_retire_tests_Hit_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_retire_tests_keep_t is a_retirement_keeps_the_packs_its_marking_found_in_use's keep's captures: the hit it expects. */
typedef struct chunks_packs_retire_tests_keep_t
{
	chunks_TupleAddressLocator_t one;
} chunks_packs_retire_tests_keep_t;

/* chunks_packs_retire_tests_keep_hits is a keep that checks the hits are the one expected and keeps their packs. */
static bool
chunks_packs_retire_tests_keep_hits(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
									StdVec *result, ChunksErrorError *error)
{
	const chunks_packs_retire_tests_keep_t *keep = captures;

	(void) marking;
	(void) error;
	if (entries->len != 1 ||
		!chunks_address_Address_PartialEq_eq(&STD_VEC_AT(entries, chunks_TupleAddressLocator_t, 0).f0, &keep->one.f0) ||
		!chunks_trailer_Locator_PartialEq_eq(&STD_VEC_AT(entries, chunks_TupleAddressLocator_t, 0).f1, &keep->one.f1))
	{
		StdVec		want;

		std_slice_to_vec_Checked(&want, &keep->one, 1, sizeof(keep->one), _Alignof(chunks_TupleAddressLocator_t));
		std_assert_failed(RUST_FILE, 332, 13, "==", entries, chunks_packs_retire_tests_Hits_Debug_fmt, &want,
						  chunks_packs_retire_tests_Hits_Debug_fmt, NULL);
	}
	for (size_t k = 0, k_len = entries->len; k < k_len; k++)
	{
		uint64		pack = STD_VEC_AT(entries, chunks_TupleAddressLocator_t, k).f1.pack;
		size_t		at;

		if (!std_slice_binary_search(result->ptr, result->len, sizeof(uint64), &pack, std_u64_Ord_cmp, &at))
			std_vec_insert(result, at, &pack, sizeof(pack), _Alignof(uint64), RUST_FILE, 333, 51);
	}
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	return true;
}

/*
 * test_chunks_packs_retire_tests_a_retirement_keeps_the_packs_its_marking_found_in_use:
 * a retirement tells keep the hits its marking recorded, and a pack keep
 * answers stays listed, with the entries reserved for it given back.
 */
void
test_chunks_packs_retire_tests_a_retirement_keeps_the_packs_its_marking_found_in_use(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 325, 46);
	ChunksPacksBackendPacks packs;
	chunks_packs_retire_tests_keep_t keep;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRemovalReservation all;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	ChunksTrailerLocator located;
	bool		has;
	bool		ok;
	size_t		entries;
	size_t		queued = CHUNKS_PACKS_SINGLE_QUEUED;
	static const uint64 one[] = {1};

	chunks_packs_fixtures_created(io, 82, &packs);
	keep.one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 328, 43);
	if (!chunks_packs_marking_Packs_locate(&packs, io, &reading, &keep.one.f0, &has, &located, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 329, 53);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&located, &keep.one.f1))
	{
		StdOptionRef got = {has, STD_ITEM(&located, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(&keep.one.f1, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, 329, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
	reservation = chunks_packs_retire_tests_reserved(io, 2, 330, 56);
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, NULL, 0, 330, 99);
	ok = chunks_packs_retire_Packs_retire(&packs, io, one, lengthof(one), &forwarded, &reservation,
										  chunks_packs_retire_tests_keep_hits, &keep, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, NULL, 0, 335, 50);
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, one, lengthof(one), NULL, 336, 9, 48);
	all = chunks_packs_retire_tests_reserved(io, SIZE_MAX, 337, 44);
	entries = chunks_packs_removal_Reservation_entries(&all);
	if (entries != queued)
		std_assert_failed(RUST_FILE, 337, 9, "==", &entries, std_usize_Debug_fmt, &queued, std_usize_Debug_fmt, NULL);
	chunks_packs_removal_Reservation_drop(&all);
	chunks_packs_backend_Packs_drop(&packs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_retire_tests_refused reports whether a retirement of numbers with a proof, which it takes, is refused as corrupt. */
static bool
chunks_packs_retire_tests_refused(const ChunksPacksBackendPacks *first, HostIoIo *io, const uint64 *numbers,
								  size_t numbers_len, ChunksPacksRetireForwarded *forwarded)
{
	ChunksPacksRemovalReservation reservation = chunks_packs_retire_tests_reserved(io, 4, 351, 46);
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	bool		refused;

	if (chunks_packs_retire_Packs_retire(first, io, numbers, numbers_len, forwarded, &reservation,
										 chunks_packs_retire_tests_none, NULL, NULL, &retired, &error))
	{
		chunks_packs_retire_Retired_drop(&retired);
		return false;
	}
	refused = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	chunks_error_Error_drop(&error);
	return refused;
}

/*
 * test_chunks_packs_retire_tests_a_retirement_is_refused_without_its_marking_or_its_own_forwards:
 * a retirement is refused with no marking, for a pack added after its
 * marking began, with another repository's forwards, with a forward from a
 * pack it doesn't name, and for a number the table doesn't list; each
 * refusal takes nothing out.
 */
void
test_chunks_packs_retire_tests_a_retirement_is_refused_without_its_marking_or_its_own_forwards(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 347, 46);
	ChunksPacksBackendPacks first;
	ChunksPacksBackendPacks second;
	chunks_TupleAddressLocator_t at_one;
	chunks_TupleAddressLocator_t at_two;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRetireForwarded nothing;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	bool		ok;
	static const uint64 one[] = {1};
	static const uint64 two[] = {2};
	static const uint64 both[] = {1, 2};

	chunks_packs_fixtures_created(io, 83, &first);
	chunks_packs_fixtures_created(io, 84, &second);
	at_one = chunks_packs_fixtures_added(io, &first, (const uint8 *) "one", 3);
	nothing = chunks_packs_retire_tests_forwarded(&first, io, &reading, NULL, 0, 354, 56);
	STD_ASSERT_MSG(355, 9, chunks_packs_retire_tests_refused(&first, io, one, 1, &nothing),
				   "a retirement with no marking");
	(void) chunks_packs_retire_tests_marked(&first, io, &reading, 356, 43);
	at_two = chunks_packs_fixtures_added(io, &first, (const uint8 *) "two", 3);
	nothing = chunks_packs_retire_tests_forwarded(&first, io, &reading, NULL, 0, 358, 56);
	STD_ASSERT_MSG(359, 9, chunks_packs_retire_tests_refused(&first, io, two, 1, &nothing),
				   "a pack added after the marking began");
	nothing = chunks_packs_retire_tests_forwarded(&second, io, &reading, NULL, 0, 360, 56);
	STD_ASSERT_MSG(361, 9, chunks_packs_retire_tests_refused(&first, io, one, 1, &nothing),
				   "another repository's forwards");
	moves[0].f0 = at_two.f1;
	moves[0].f1 = at_one.f1;
	nothing = chunks_packs_retire_tests_forwarded(&first, io, &reading, moves, lengthof(moves), 362, 72);
	STD_ASSERT_MSG(363, 9, chunks_packs_retire_tests_refused(&first, io, one, 1, &nothing),
				   "a forward from a pack the retirement doesn't name");
	chunks_packs_tests_numbers_eq(RUST_FILE, &first, io, &reading, both, lengthof(both), NULL, 364, 9, 48);

	reservation = chunks_packs_retire_tests_reserved(io, 4, 366, 54);
	nothing = chunks_packs_retire_tests_forwarded(&first, io, &reading, NULL, 0, 366, 97);
	ok = chunks_packs_retire_Packs_retire(&first, io, one, lengthof(one), &nothing, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, one, lengthof(one), 368, 50);
	(void) chunks_packs_retire_tests_marked(&first, io, &reading, 369, 43);
	nothing = chunks_packs_retire_tests_forwarded(&first, io, &reading, NULL, 0, 370, 56);
	STD_ASSERT_MSG(371, 9, chunks_packs_retire_tests_refused(&first, io, one, 1, &nothing),
				   "a number the table doesn't list");
	chunks_packs_tests_numbers_eq(RUST_FILE, &first, io, &reading, two, lengthof(two), NULL, 372, 9, 48);
	chunks_packs_backend_Packs_drop(&second);
	chunks_packs_backend_Packs_drop(&first);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_retire_tests_a_retirement_takes_out_only_the_packs_it_reserved_entries_for:
 * a collection that would retire more than the removal queue has room for,
 * with a reader holding the epoch, retires the packs it reserved entries for
 * and returns with no wait; after the reader ends, the next collection
 * retires the rest.
 */
void
test_chunks_packs_retire_tests_a_retirement_takes_out_only_the_packs_it_reserved_entries_for(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 382, 46);
	HostSharedReader reader;
	ChunksPacksBackendPacks packs;
	ChunksPacksRemovalReservation taken;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	bool		ok;
	static const char *const bodies[] = {"one", "two", "six"};
	static const uint64 all[] = {1, 2, 3};
	static const uint64 one[] = {1};
	static const uint64 rest[] = {2, 3};

	chunks_packs_fixtures_created(io, 85, &packs);
	for (size_t k = 0; k < lengthof(bodies); k++)
		(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) bodies[k], 3);
	reader = chunks_packs_tests_reading(RUST_FILE, io, 387, 45);
	taken = chunks_packs_retire_tests_reserved(io, CHUNKS_PACKS_SINGLE_QUEUED - 3, 388, 45);
	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 389, 43);
	reservation = chunks_packs_retire_tests_reserved(io, 6, 390, 56);
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, NULL, 0, 390, 99);
	chunks_packs_retire_tests_eq(chunks_packs_removal_Reservation_entries(&reservation), 3, 391, 9);
	ok = chunks_packs_retire_Packs_retire(&packs, io, all, lengthof(all), &forwarded, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, one, lengthof(one), 393, 50);
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, rest, lengthof(rest), NULL, 394, 9, 48);

	host_shared_Reader_drop(&reader);
	chunks_packs_removal_Reservation_drop(&taken);
	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 397, 43);
	reservation = chunks_packs_retire_tests_reserved(io, 6, 398, 56);
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, NULL, 0, 398, 99);
	ok = chunks_packs_retire_Packs_retire(&packs, io, rest, lengthof(rest), &forwarded, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, rest, lengthof(rest), 400, 50);
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, NULL, 0, NULL, 401, 9, 48);
	host_shared_Reader_drop(&reading);
	chunks_packs_retire_tests_removed(io, 403);
	chunks_packs_retire_tests_names_eq(85, ".pack", ".idx", 0, 0, 404);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_retire_tests_a_retired_pack_is_restored_under_a_fresh_number_and_reads_both_ways:
 * a retired pack a verify reached is listed again under a fresh number, its
 * chunk read there and, from its old place, through the forward to its
 * copy; the files of the packs it didn't restore are queued, and the
 * restored pack's file stays.
 */
void
test_chunks_packs_retire_tests_a_retired_pack_is_restored_under_a_fresh_number_and_reads_both_ways(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 414, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t copy;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	ChunksTrailerLocator relisted;
	ChunksChunkChunk chunk;
	ChunksAddressAddress address;
	const uint64 *numbers;
	size_t		numbers_len;
	uint64		fresh;
	uint64		again;
	bool		twice;
	uint64		listed[2];
	static const uint64 one[] = {1};

	chunks_packs_fixtures_created(io, 90, &packs);
	first = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	copy = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_retire_tests_eq(chunks_packs_retire_tests_marked(&packs, io, &reading, 417, 54), 2, 417, 9);
	moves[0].f0 = first.f1;
	moves[0].f1 = copy.f1;
	reservation = chunks_packs_retire_tests_reserved(io, 2, 419, 29);
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, moves, lengthof(moves), 419, 91);
	if (!chunks_packs_retire_Packs_retire(&packs, io, one, lengthof(one), &forwarded, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 420, 80);
	numbers = chunks_packs_retire_Retired_numbers(&retired, &numbers_len);
	if (numbers_len != 1 || numbers[0] != 1)
	{
		StdVec		got;
		StdVec		want;

		std_slice_to_vec_Checked(&got, numbers, numbers_len, sizeof(uint64), _Alignof(uint64));
		std_slice_to_vec_Checked(&want, one, lengthof(one), sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 421, 9, "==", &got, chunks_packs_tests_Numbers_Debug_fmt, &want,
						  chunks_packs_tests_Numbers_Debug_fmt, NULL);
	}
	if (!chunks_packs_retire_Retired_restore(&retired, io, &reading, 1, &fresh, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 422, 54);
	STD_ASSERT(423, 9, fresh > 2, "fresh > 2");
	twice = !chunks_packs_retire_Retired_restore(&retired, io, &reading, 1, &again, &error);
	if (twice)
		chunks_error_Error_drop(&error);
	STD_ASSERT_MSG(424, 9, twice, "a pack was restored twice");
	if (!chunks_packs_retire_Retired_queue(&retired, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 425, 37);
	listed[0] = 2;
	listed[1] = fresh;
	chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, listed, lengthof(listed), NULL, 426, 9, 48);
	relisted = chunks_trailer_Locator_at(fresh, chunks_trailer_Locator_offset(&first.f1),
										 chunks_trailer_Locator_length(&first.f1));
	chunk = chunks_packs_retire_tests_read(&packs, io, &reading, &first.f0, relisted, 428, 61);
	address = chunks_chunk_Chunk_address(&chunk);
	chunks_chunk_Chunk_drop(&chunk);
	if (!chunks_address_Address_PartialEq_eq(&address, &first.f0))
		std_assert_failed(RUST_FILE, 428, 9, "==", &address, chunks_address_Address_Debug_fmt, &first.f0,
						  chunks_address_Address_Debug_fmt, NULL);
	chunks_packs_retire_tests_forwarded_eq(&packs, io, &reading, first.f1, &copy.f1, 429, 60);
	host_shared_Reader_drop(&reading);
	chunks_packs_retire_tests_removed(io, 431);
	chunks_packs_retire_tests_eq(chunks_packs_retire_tests_names(90, ".pack"), 2, 432, 9);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_retire_tests_set_t is a_markings_newer_vector_replaces_the_older_through_the_queue's keep's captures: whether the vector was set. */
typedef struct chunks_packs_retire_tests_set_t
{
	bool		set;
} chunks_packs_retire_tests_set_t;

/* chunks_packs_retire_tests_set_second is a keep that sets the first pack's vector's second generation, of no offsets. */
static bool
chunks_packs_retire_tests_set_second(void *captures, ChunksPacksRetireMarking *marking, StdVec *entries,
									 StdVec *result, ChunksErrorError *error)
{
	chunks_packs_retire_tests_set_t *set = captures;
	StdBTreeMap none = std_btree_BTreeSetU64_new();
	bool		ok;

	(void) result;
	std_vec_free_Checked(entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	ok = chunks_packs_retire_Marking_set_dead(marking, 1, 2, &none, &set->set, error);
	std_btree_BTreeMapU64Set_drop(&none);
	return ok;
}

/* chunks_packs_retire_tests_dead marks the chunks at offsets of the first pack dead, which unwraps at a line and column, and reports whether it did. */
static bool
chunks_packs_retire_tests_dead(HostIoIo *io, const ChunksPacksBackendPacks *packs, uint64 generation,
							   const uint64 *offsets, size_t offsets_len, uint32 line, uint32 col)
{
	ChunksErrorError error;
	bool		set;

	if (!chunks_packs_fixtures_dead(io, packs, 1, generation, offsets, offsets_len, &set, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return set;
}

/* chunks_packs_retire_tests_located_names_eq asserts at a line that a locate answers nothing for a chunk, which unwraps at column 54, and the directory holds as many files with an ending as given. */
static void
chunks_packs_retire_tests_located_names_eq(const ChunksPacksBackendPacks *packs, HostIoIo *io,
										   const HostSharedReader *reading, const ChunksAddressAddress *address,
										   const char *ending, size_t want, uint32 line)
{
	ChunksTrailerLocator located;
	ChunksErrorError error;
	bool		has;
	size_t		names;

	if (!chunks_packs_marking_Packs_locate(packs, io, reading, address, &has, &located, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, 54);
	names = chunks_packs_retire_tests_names(86, ending);
	if (has || names != want)
	{
		StdOptionRef got_located = {has, STD_ITEM(&located, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef none = {false, STD_ITEM(&located, chunks_trailer_Locator_Debug_fmt)};
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&got_located, std_option_Debug_fmt), STD_ITEM(&names, std_usize_Debug_fmt));
		StdTupleRef wanted = STD_TUPLE(2, STD_ITEM(&none, std_option_Debug_fmt), STD_ITEM(&want, std_usize_Debug_fmt));

		std_assert_failed(RUST_FILE, line, 9, "==", &got, std_tuple_Debug_fmt, &wanted, std_tuple_Debug_fmt, NULL);
	}
}

/*
 * test_chunks_packs_retire_tests_a_markings_newer_vector_replaces_the_older_through_the_queue:
 * a marking's newer generation of a pack's deletion vector replaces the
 * older one, whose file goes through the removal queue; with no entry
 * reserved for it the vector isn't replaced, and a generation the pack
 * already has is refused.
 */
void
test_chunks_packs_retire_tests_a_markings_newer_vector_replaces_the_older_through_the_queue(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 442, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t one;
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	chunks_packs_retire_tests_set_t set = {true};
	ChunksErrorError error;
	ChunksTrailerLocator located;
	StdVec		numbers;
	uint64		offset;
	bool		has;
	bool		ok;
	bool		refused;

	chunks_packs_fixtures_created(io, 86, &packs);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	offset = chunks_trailer_Locator_offset(&one.f1);
	STD_ASSERT(445, 9, chunks_packs_retire_tests_dead(io, &packs, 1, &offset, 1, 445, 60),
			   "dead(io, &packs, 1, 1, &[at_one.offset()]).unwrap()");
	chunks_packs_retire_tests_located_names_eq(&packs, io, &reading, &one.f0, ".1.dead", 1, 446);

	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 448, 43);
	reservation = chunks_packs_retire_tests_reserved(io, 0, 449, 56);
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, NULL, 0, 449, 99);
	ok = chunks_packs_retire_Packs_retire(&packs, io, NULL, 0, &forwarded, &reservation,
										  chunks_packs_retire_tests_set_second, &set, NULL, &retired, &error);
	if (!chunks_packs_fixtures_queued(io, &reading, ok, &retired, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 455, 54);
	if (numbers.len != 0 || set.set)
	{
		StdVec		none;
		bool		unset = false;
		StdTupleRef got = STD_TUPLE(2, STD_ITEM(&numbers, chunks_packs_tests_Numbers_Debug_fmt),
									STD_ITEM(&set.set, std_bool_Debug_fmt));
		StdTupleRef want = STD_TUPLE(2, STD_ITEM(&none, chunks_packs_tests_Numbers_Debug_fmt),
									 STD_ITEM(&unset, std_bool_Debug_fmt));

		std_vec_new(&none, sizeof(uint64), _Alignof(uint64));
		std_assert_failed(RUST_FILE, 455, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	chunks_packs_retire_tests_hits_refused(&packs, io, "a retirement of no pack left its marking on", 456);
	chunks_packs_retire_tests_located_names_eq(&packs, io, &reading, &one.f0, ".2.dead", 0, 457);

	STD_ASSERT(459, 9, chunks_packs_retire_tests_dead(io, &packs, 2, NULL, 0, 459, 45),
			   "dead(io, &packs, 1, 2, &[]).unwrap()");
	if (!chunks_packs_marking_Packs_locate(&packs, io, &reading, &one.f0, &has, &located, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 460, 53);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&located, &one.f1))
	{
		StdOptionRef got = {has, STD_ITEM(&located, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(&one.f1, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, 460, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
	chunks_packs_retire_tests_names_eq(86, ".1.dead", ".2.dead", 1, 1, 461);
	host_shared_Reader_drop(&reading);
	chunks_packs_retire_tests_removed(io, 463);
	chunks_packs_retire_tests_names_eq(86, ".1.dead", ".2.dead", 0, 1, 464);
	refused = !chunks_packs_fixtures_dead(io, &packs, 1, 2, NULL, 0, &set.set, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
		chunks_error_Error_drop(&error);
	}
	STD_ASSERT(465, 9, refused, "matches!(dead(io, &packs, 1, 2, &[]), Err(Error::Corrupt(_)))");
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

#ifdef DOLTGRES_CHUNKS_TRACE

/* chunks_packs_retire_tests_Events_Debug_fmt is a Vec<Event>'s Debug. */
static bool
chunks_packs_retire_tests_Events_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *events = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = events->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(events, ChunksTraceEvent, k), chunks_trace_Event_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_packs_retire_tests_events_eq asserts at a line that events are those given. */
static void
chunks_packs_retire_tests_events_eq(const ChunksTraceEvent *got, size_t got_len, const ChunksTraceEvent *want,
									size_t want_len, uint32 line)
{
	bool		eq = got_len == want_len;

	for (size_t k = 0; eq && k < want_len; k++)
		eq = chunks_trace_Event_PartialEq_eq(&got[k], &want[k]);
	if (!eq)
	{
		StdVec		left = {unconstify(ChunksTraceEvent *, got), got_len, got_len};
		StdVec		right = {unconstify(ChunksTraceEvent *, want), want_len, want_len};

		std_assert_failed(RUST_FILE, line, 9, "==", &left, chunks_packs_retire_tests_Events_Debug_fmt, &right,
						  chunks_packs_retire_tests_Events_Debug_fmt, NULL);
	}
}

/* chunks_packs_retire_tests_events_drop drops a Vec<Event>. */
static void
chunks_packs_retire_tests_events_drop(StdVec *events)
{
	std_vec_drop_Checked(events, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent),
						 (void (*) (void *)) chunks_trace_Event_drop);
}

/* chunks_packs_retire_tests_taken_eq asserts at a line that the events taken of a repository's trace are those given. */
static void
chunks_packs_retire_tests_taken_eq(uint64 repo, const ChunksTraceEvent *want, size_t want_len, uint32 line)
{
	StdVec		events = chunks_trace_take(repo);

	chunks_packs_retire_tests_events_eq(events.ptr, events.len, want, want_len, line);
	chunks_packs_retire_tests_events_drop(&events);
}

/* chunks_packs_retire_tests_cleared drops the events of a repository's trace. */
static void
chunks_packs_retire_tests_cleared(uint64 repo)
{
	StdVec		events = chunks_trace_take(repo);

	chunks_packs_retire_tests_events_drop(&events);
}

/*
 * test_chunks_packs_retire_tests_a_packs_writing_adding_forwarding_and_retirement_are_traced_in_order:
 * a pack's writing, its adding, the forwards, and the retirement are each
 * traced in order with the writers' slot's stores around each pack and the
 * reservation in the server's own trace; a refused add is traced as no add,
 * an abandoned pack as abandoned, and a whole write of every list file as
 * one.
 */
void
test_chunks_packs_retire_tests_a_packs_writing_adding_forwarding_and_retirement_are_traced_in_order(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 480, 46);
	ChunksPacksBackendPacks packs;
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t second;
	ChunksAddressAddress address;
	ChunksTrailerLocator old;
	ChunksTrailerLocator copy;
	ChunksPackPackId id;
	StdArcInner *arc;
	chunks_packs_backend_Packs_forward_moves_t moves[1];
	ChunksPacksRemovalReservation reservation;
	ChunksPacksRetireForwarded forwarded;
	ChunksPacksRetireRetired retired;
	ChunksErrorError error;
	StdVec		traced;
	StdVec		queued;
	const ChunksTraceEvent *events;
	ChunksPackPackWriterInDir writer;
	ChunksAddressAddress refused;
	StdVec		bytes;
	ChunksTrailerLocator at;
	ChunksPackPack pack;
	uint64		number;
	size_t		from;
	bool		has;
	bool		ok;
	bool		added = false;

	chunks_packs_fixtures_created(io, 88, &packs);
	first = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "traced", 6);
	second = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "traced", 6);
	address = first.f0;
	old = first.f1;
	copy = second.f1;
	if (!chunks_packs_backend_Packs_pack_reader(&packs, io, &reading, old.pack, &has, &arc, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 483, 60);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(483, 69);
	id = ((const ChunksPackPackReader *) std_arc_Deref_deref(arc, _Alignof(ChunksPackPackReader)))->id;
	chunks_pack_ArcPackReader_drop(arc);
	chunks_packs_retire_tests_cleared(CHUNKS_TRACE_SERVER);
	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 485, 43);
	reservation = chunks_packs_retire_tests_reserved(io, 2, 486, 56);
	moves[0].f0 = old;
	moves[0].f1 = copy;
	forwarded = chunks_packs_retire_tests_forwarded(&packs, io, &reading, moves, lengthof(moves), 486, 110);
	ok = chunks_packs_retire_Packs_retire(&packs, io, &old.pack, 1, &forwarded, &reservation,
										  chunks_packs_retire_tests_none, NULL, NULL, &retired, &error);
	chunks_packs_retire_tests_queued_eq(io, &reading, ok, &retired, &error, &old.pack, 1, 488, 50);
	{
		chunks_TupleLocatorLocator_t forwards[] = {{old, copy}};
		chunks_TupleU64PackId_t took[] = {{old.pack, id}};
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_RECORD_MADE},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_STORED, .u.writer_stored.lowest = old.pack},
			{.tag = CHUNKS_TRACE_EVENT_PACK_OPENED, .u.pack_opened.pack = old.pack},
			{.tag = CHUNKS_TRACE_EVENT_CHUNK_WRITTEN, .u.chunk_written = {address, old, true, STD_VEC_NEW}},
			{.tag = CHUNKS_TRACE_EVENT_PACK_SYNCED, .u.pack_synced.pack = old.pack},
			{.tag = CHUNKS_TRACE_EVENT_LIST_RECORDED, .u.list_recorded.pack = old.pack},
			{.tag = CHUNKS_TRACE_EVENT_PACK_ADDED, .u.pack_added.pack = old.pack},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_CLEARED},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_STORED, .u.writer_stored.lowest = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_PACK_OPENED, .u.pack_opened.pack = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_CHUNK_WRITTEN, .u.chunk_written = {address, copy, true, STD_VEC_NEW}},
			{.tag = CHUNKS_TRACE_EVENT_PACK_SYNCED, .u.pack_synced.pack = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_LIST_RECORDED, .u.list_recorded.pack = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_PACK_ADDED, .u.pack_added.pack = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_CLEARED},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN, .u.marking_began.newest = copy.pack},
			{.tag = CHUNKS_TRACE_EVENT_FORWARDED, .u.forwarded.moves = {forwards, 1, 1}},
			{.tag = CHUNKS_TRACE_EVENT_RETIRED, .u.retired.packs = {took, 1, 1}},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_RETIRED},
		};

		traced = chunks_trace_take(88);
		std_vec_split_off(&traced, lengthof(expected), sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent),
						  &queued, RUST_FILE, 507, 29);
		chunks_packs_retire_tests_events_eq(traced.ptr, traced.len, expected, lengthof(expected), 508);
	}
	events = queued.ptr;
	if (queued.len != 3 || events[0].tag != CHUNKS_TRACE_EVENT_FILE_QUEUED ||
		events[0].u.file_queued.file.tag != CHUNKS_PACKS_SINGLE_QUEUED_PACK ||
		events[1].tag != CHUNKS_TRACE_EVENT_FILE_QUEUED ||
		events[1].u.file_queued.file.tag != CHUNKS_PACKS_SINGLE_QUEUED_RUN ||
		events[2].tag != CHUNKS_TRACE_EVENT_RETIRED_QUEUED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&queued, chunks_packs_retire_tests_Events_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 515, 13, &arguments);
	}
	STD_ASSERT(517, 9, chunks_pack_PackId_PartialEq_eq(&events[0].u.file_queued.id, &id) &&
			   chunks_pack_PackId_PartialEq_eq(&events[1].u.file_queued.id, &id) &&
			   events[0].u.file_queued.epoch == events[1].u.file_queued.epoch &&
			   events[2].u.retired_queued.packs.len == 1 &&
			   STD_VEC_AT(&events[2].u.retired_queued.packs, uint64, 0) == old.pack,
			   "*pack == id && *run == id && epoch == same && *numbers == [old.pack]");
	chunks_packs_retire_tests_events_drop(&queued);
	chunks_packs_retire_tests_events_drop(&traced);
	{
		ChunksTraceEvent expected[] = {{.tag = CHUNKS_TRACE_EVENT_RESERVED, .u.reserved.entries = 2}};

		chunks_packs_retire_tests_taken_eq(CHUNKS_TRACE_SERVER, expected, lengthof(expected), 518);
	}
	host_shared_Reader_drop(&reading);
	chunks_packs_retire_tests_removed(io, 520);
	reading = chunks_packs_tests_reading(RUST_FILE, io, 521, 46);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_PACK_DELETED, .u.pack_deleted.pack = id},
			{.tag = CHUNKS_TRACE_EVENT_FILE_REMOVED, .u.file_removed = {{.tag = CHUNKS_PACKS_SINGLE_QUEUED_RUN}, id}},
		};

		chunks_packs_retire_tests_taken_eq(88, expected, lengthof(expected), 522);
	}

	writer = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 524, 77);
	chunks_packs_fixtures_stored((const uint8 *) "refused", 7, &refused, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, refused, bytes.ptr, bytes.len, false, &at, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 525, 59);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 526, 42);
	if (pack.has_owner)
	{
		pack.has_owner = false;
		chunks_packs_writers_Writing_drop(&pack.owner);
	}
	ok = chunks_packs_backend_Packs_add(&packs, io, &reading, &pack, &error);
	if (!ok)
		chunks_error_Error_drop(&error);
	STD_ASSERT(528, 9, !ok, "packs.add(io, &reading, pack).is_err()");
	traced = chunks_trace_take(88);
	events = traced.ptr;
	for (size_t k = 0; k < traced.len; k++)
		added = added || events[k].tag == CHUNKS_TRACE_EVENT_PACK_ADDED;
	if (added)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&traced, chunks_packs_retire_tests_Events_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 530, 9, &arguments);
	}
	from = STD_SUB(usize, traced.len, 2, 531, 27);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_PACK_SYNCED, .u.pack_synced.pack = at.pack},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_CLEARED},
		};

		chunks_packs_retire_tests_events_eq(&events[from], traced.len - from, expected, lengthof(expected), 531);
	}
	chunks_packs_retire_tests_events_drop(&traced);

	writer = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 533, 52);
	number = chunks_pack_PackWriterInDir_number(&writer);
	if (!chunks_pack_PackWriterInDir_abandon(&writer, HOST_IO_IO_TAKES_SHORT(io), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 535, 28);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_WRITER_STORED, .u.writer_stored.lowest = number},
			{.tag = CHUNKS_TRACE_EVENT_PACK_OPENED, .u.pack_opened.pack = number},
			{.tag = CHUNKS_TRACE_EVENT_PACK_ABANDONED, .u.pack_abandoned.pack = number},
			{.tag = CHUNKS_TRACE_EVENT_WRITER_CLEARED},
		};

		chunks_packs_retire_tests_taken_eq(88, expected, lengthof(expected), 542);
	}
	if (!chunks_packs_backend_Packs_compact_lists(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 543, 43);
	{
		ChunksTraceEvent expected[] = {{.tag = CHUNKS_TRACE_EVENT_LISTS_COMPACTED}};

		chunks_packs_retire_tests_taken_eq(88, expected, lengthof(expected), 544);
	}
	std_vec_free_Checked(&bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_retire_tests_pinned_t is the_markings_the_roots_and_the_records_transitions_are_traced's registration's captures: both repositories' packs and the root. */
typedef struct chunks_packs_retire_tests_pinned_t
{
	const ChunksPacksBackendPacks *packs;
	const ChunksPacksBackendPacks *other;
	chunks_TupleAddressLocator_t root;
} chunks_packs_retire_tests_pinned_t;

/* chunks_packs_retire_tests_pinned_both pins the root in one slot through each repository's packs in turn, dropping each pin. */
static bool
chunks_packs_retire_tests_pinned_both(void *captures, HostIoRegistering *registering,
									  ChunksPacksRootsRootSlots *slots, void *result, ChunksErrorError *error)
{
	const chunks_packs_retire_tests_pinned_t *pinned = captures;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, registering};
	ChunksPacksRootsRootSlot *slot;
	ChunksPacksRootsPin pin;

	(void) result;
	chunks_packs_roots_RootSlots_one(slots, &slot);
	if (!chunks_packs_roots_Packs_pin(pinned->packs, io, slot, pinned->root.f0, pinned->root.f1, &pin, error))
		return false;
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	chunks_packs_roots_RootSlots_one(slots, &slot);
	if (!chunks_packs_roots_Packs_pin(pinned->other, io, slot, pinned->root.f0, pinned->root.f1, &pin, error))
		return false;
	chunks_packs_roots_Pin_drop(&pin);
	chunks_packs_roots_RootSlot_unused(pin.slot);
	return true;
}

/*
 * test_chunks_packs_retire_tests_the_markings_the_roots_and_the_records_transitions_are_traced:
 * a marking's start, a hit, and each of its two ends, a vector's name, a
 * reservation taken and given back, a root's registration and release, a
 * record's drop, and a settle are each traced, the marking a collector left
 * on in the trace of its own repository.
 */
void
test_chunks_packs_retire_tests_the_markings_the_roots_and_the_records_transitions_are_traced(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 557, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks other;
	chunks_TupleAddressLocator_t one;
	chunks_packs_retire_tests_pinned_t pinned;
	ChunksTrailerLocator found;
	ChunksPacksSettleSettled settled;
	ChunksPacksSettleSettled unchanged;
	ChunksErrorError error;
	StdVec		numbers;
	uint64		offset;
	uint64		location;
	uint8		unit;
	bool		has;

	chunks_packs_fixtures_created(io, 89, &packs);
	chunks_packs_fixtures_created(io, 90, &other);
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	chunks_packs_retire_tests_cleared(89);
	chunks_packs_retire_tests_cleared(90);
	chunks_packs_retire_tests_cleared(CHUNKS_TRACE_SERVER);

	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 562, 43);
	if (!chunks_packs_marking_Packs_locate(&packs, io, &reading, &one.f0, &has, &found, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 563, 53);
	if (!has || !chunks_trailer_Locator_PartialEq_eq(&found, &one.f1))
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(&one.f1, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, 563, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
	if (!chunks_packs_marking_Packs_end_marking(&packs, io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 564, 31);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN, .u.marking_began.newest = 1},
			{.tag = CHUNKS_TRACE_EVENT_HIT_RECORDED, .u.hit_recorded = {one.f0, one.f1}},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_ENDED},
		};

		chunks_packs_retire_tests_taken_eq(89, expected, lengthof(expected), 566);
	}

	offset = chunks_trailer_Locator_offset(&one.f1);
	STD_ASSERT(568, 9, chunks_packs_retire_tests_dead(io, &packs, 4, &offset, 1, 568, 60),
			   "dead(io, &packs, 1, 4, &[at_one.offset()]).unwrap()");
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN, .u.marking_began.newest = 1},
			{.tag = CHUNKS_TRACE_EVENT_VECTOR_NAMED, .u.vector_named = {1, 4}},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_RETIRED},
			{.tag = CHUNKS_TRACE_EVENT_RETIRED_QUEUED, .u.retired_queued.packs = STD_VEC_NEW},
		};
		ChunksTraceEvent server[] = {
			{.tag = CHUNKS_TRACE_EVENT_RESERVED, .u.reserved.entries = 1},
			{.tag = CHUNKS_TRACE_EVENT_UNRESERVED, .u.unreserved.entries = 1},
		};

		chunks_packs_retire_tests_taken_eq(89, expected, lengthof(expected), 571);
		chunks_packs_retire_tests_taken_eq(CHUNKS_TRACE_SERVER, server, lengthof(server), 572);
	}

	(void) chunks_packs_retire_tests_marked(&packs, io, &reading, 574, 43);
	(void) chunks_packs_retire_tests_marked(&other, io, &reading, 575, 43);
	if (!chunks_packs_marking_Packs_end_marking(&other, io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 576, 31);
	{
		ChunksTraceEvent cleared[] = {
			{.tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN, .u.marking_began.newest = 1},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_CLEARED},
		};
		ChunksTraceEvent ended[] = {
			{.tag = CHUNKS_TRACE_EVENT_MARKING_BEGAN, .u.marking_began.newest = 0},
			{.tag = CHUNKS_TRACE_EVENT_MARKING_ENDED},
		};

		chunks_packs_retire_tests_taken_eq(89, cleared, lengthof(cleared), 577);
		chunks_packs_retire_tests_taken_eq(90, ended, lengthof(ended), 578);
	}

	pinned.packs = &packs;
	pinned.other = &other;
	pinned.root = one;
	if (!chunks_packs_roots_RootSlots_register(io, chunks_packs_retire_tests_pinned_both, &pinned, NULL, &unit,
											   &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 584, 10);
	if (!chunks_packs_backend_Packs_numbers(&other, io, &reading, &numbers, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 585, 37);
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_ROOT_REGISTERED, .u.root_registered = {0, one.f1}},
			{.tag = CHUNKS_TRACE_EVENT_ROOT_RELEASED, .u.root_released.slot = 0},
		};

		chunks_packs_retire_tests_taken_eq(89, expected, lengthof(expected), 586);
		chunks_packs_retire_tests_taken_eq(90, expected, lengthof(expected), 587);
	}

	chunks_packs_backend_Packs_drop(&other);
	if (!chunks_packs_open_Packs_dropped(io, 90, HOST_PG_DEFAULT, &has, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 590, 86);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_RECORD_DROPPED},
			{.tag = CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED},
		};

		chunks_packs_retire_tests_taken_eq(90, expected, lengthof(expected), 591);
	}
	location = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 594, 75, 84);
	chunks_packs_retire_tests_cleared(89);
	if (!chunks_packs_settle_Packs_settle(io, 89, HOST_PG_DEFAULT, &settled, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 596, 99);
	chunks_packs_settle_Settled_Default_default(&unchanged);
	STD_ASSERT_EQ(597, 9, chunks_packs_settle_Settled_PartialEq_eq(&settled, &unchanged), &settled,
				  chunks_packs_settle_Settled_Debug_fmt, &unchanged, chunks_packs_settle_Settled_Debug_fmt);
	chunks_packs_settle_Settled_drop(&unchanged);
	chunks_packs_settle_Settled_drop(&settled);
	{
		ChunksTraceEvent expected[] = {
			{.tag = CHUNKS_TRACE_EVENT_PACKS_SETTLED, .u.packs_settled = {0, STD_VEC_NEW, 0, 0, STD_VEC_NEW}},
		};

		chunks_packs_retire_tests_taken_eq(89, expected, lengthof(expected), 600);
	}
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_CHUNKS_TRACE */

#endif							/* DOLTGRES_TEST_CHUNKS */
