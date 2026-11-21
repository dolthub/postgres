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
 * The C port of chunks' packs/view.rs: a backend's view of a repository's
 * packs, what the list files hold as the backend last read them, with the
 * packs and runs it opened by them, brought up to date with the pack
 * record's generation before each read.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "view.rs"

#ifdef DOLTGRES_TEST_CHUNKS
/* chunks_packs_view_reread counts the refreshes that read the list files again under the list files' flag. */
static uint64 chunks_packs_view_reread;

/* chunks_packs_view_rereads returns how many refreshes read the list files again under the list files' flag. */
uint64
chunks_packs_view_rereads(void)
{
	return chunks_packs_view_reread;
}

/* chunks_packs_view_count_reread counts one more read of the list files under the list files' flag. */
void
chunks_packs_view_count_reread(void)
{
	chunks_packs_view_reread++;
}
#endif

/* chunks_packs_view_Table_generation is Table's List::generation. */
static uint64
chunks_packs_view_Table_generation(const void *list)
{
	return ((const ChunksPacksTable *) list)->generation;
}

/* chunks_packs_view_Table_apply is Table's List::apply. */
static bool
chunks_packs_view_Table_apply(void *list, const uint8 *record, size_t record_len, ChunksErrorError *error)
{
	return chunks_packs_Table_apply(list, record, record_len, error);
}

/* chunks_packs_view_Table_renew is Table's List::renew. */
static void
chunks_packs_view_Table_renew(void *list, uint64 generation)
{
	((ChunksPacksTable *) list)->generation = generation;
}

/* chunks_packs_view_Table_whole is Table's List::whole. */
static void
chunks_packs_view_Table_whole(const void *list, StdVec *result)
{
	chunks_packs_Table_whole(list, result);
}

/* chunks_packs_view_Table_reset makes a table its Default. */
static void
chunks_packs_view_Table_reset(void *list)
{
	chunks_packs_Table_drop(list);
	chunks_packs_Table_Default_default(list);
}

/* chunks_packs_view_Table_init makes a place a table was moved out of its Default. */
static void
chunks_packs_view_Table_init(void *list)
{
	chunks_packs_Table_Default_default(list);
}

/* CHUNKS_PACKS_VIEW_TABLE_LIST_OPS is Table's List. */
const ChunksPacksViewListOps CHUNKS_PACKS_VIEW_TABLE_LIST_OPS = {
	chunks_packs_view_Table_generation, chunks_packs_view_Table_apply, chunks_packs_view_Table_renew,
	chunks_packs_view_Table_whole, chunks_packs_view_Table_reset, chunks_packs_view_Table_init,
};

/* chunks_packs_view_Runs_generation is Runs' List::generation. */
static uint64
chunks_packs_view_Runs_generation(const void *list)
{
	return ((const ChunksPacksRuns *) list)->generation;
}

/* chunks_packs_view_Runs_apply is Runs' List::apply. */
static bool
chunks_packs_view_Runs_apply(void *list, const uint8 *record, size_t record_len, ChunksErrorError *error)
{
	return chunks_packs_Runs_apply(list, record, record_len, error);
}

/* chunks_packs_view_Runs_renew is Runs' List::renew. */
static void
chunks_packs_view_Runs_renew(void *list, uint64 generation)
{
	((ChunksPacksRuns *) list)->generation = generation;
}

/* chunks_packs_view_Runs_whole is Runs' List::whole. */
static void
chunks_packs_view_Runs_whole(const void *list, StdVec *result)
{
	chunks_packs_Runs_whole(list, result);
}

/* chunks_packs_view_Runs_reset makes runs their Default. */
static void
chunks_packs_view_Runs_reset(void *list)
{
	chunks_packs_Runs_drop(list);
	chunks_packs_Runs_Default_default(list);
}

/* chunks_packs_view_Runs_init makes a place runs were moved out of their Default. */
static void
chunks_packs_view_Runs_init(void *list)
{
	chunks_packs_Runs_Default_default(list);
}

/* CHUNKS_PACKS_VIEW_RUNS_LIST_OPS is Runs' List. */
const ChunksPacksViewListOps CHUNKS_PACKS_VIEW_RUNS_LIST_OPS = {
	chunks_packs_view_Runs_generation, chunks_packs_view_Runs_apply, chunks_packs_view_Runs_renew,
	chunks_packs_view_Runs_whole, chunks_packs_view_Runs_reset, chunks_packs_view_Runs_init,
};

/* chunks_packs_view_path_error returns a corrupt error of a path's display followed by a text. */
static ChunksErrorError
chunks_packs_view_path_error(const char *path, size_t path_len, const char *suffix)
{
	StdString	message;
	StdStr		shown = {path, path_len};
	StdStr		after = {suffix, strlen(suffix)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&after, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/* chunks_packs_view_Read_bring_reset makes a read one that has read nothing: a Read's Default. */
void
chunks_packs_view_Read_bring_reset(const ChunksPacksViewListOps *ops, ChunksPacksViewRead *read, void *list)
{
	read->at = 0;
	read->past = 0;
	ops->reset(list);
}

/*
 * chunks_packs_view_Read_read_to_frame_0_t is read_to's state across its
 * pad: the records it read.
 */
typedef struct chunks_packs_view_Read_read_to_frame_0_t
{
	const ChunksPacksViewListOps *ops;
	ChunksPacksViewRead *read;
	void	   *list;
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	ChunksPacksTail tail;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		records;
} chunks_packs_view_Read_read_to_frame_0_t;

/* chunks_packs_view_Read_read_to_pad_0 is read_to's pad: the records read. */
static void
chunks_packs_view_Read_read_to_pad_0(void *frame_)
{
	chunks_packs_view_Read_read_to_frame_0_t *frame = frame_;

	chunks_pack_VecVecU8_drop(&frame->records);
	std_vec_new(&frame->records, sizeof(StdVec), _Alignof(StdVec));
}

/* chunks_packs_view_Read_read_to_body_0 is read_to's body: the records to the tail read and applied. */
static void
chunks_packs_view_Read_read_to_body_0(void *frame_)
{
	chunks_packs_view_Read_read_to_frame_0_t *frame = frame_;
	ChunksPacksViewRead *read = frame->read;

	frame->ok = false;
	if (read->at == 0 || frame->ops->generation(frame->list) != frame->tail.generation)
	{
		read->at = sizeof(CHUNKS_FILE_APPENDED);
		read->past = 0;
	}
	if (read->at != frame->tail.end)
	{
		bool		whole = read->at == sizeof(CHUNKS_FILE_APPENDED);
		bool		has_records;

		chunks_pack_VecVecU8_drop(&frame->records);
		if (!chunks_file_appended_between(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->path, frame->path_len,
										  read->at, frame->tail.end, &has_records, &frame->records, frame->error))
		{
			std_vec_new(&frame->records, sizeof(StdVec), _Alignof(StdVec));
			goto done;
		}
		if (!has_records)
		{
			std_vec_new(&frame->records, sizeof(StdVec), _Alignof(StdVec));
			*frame->result = false;
			frame->ok = true;
			goto done;
		}
		for (size_t k = 0, k_len = frame->records.len; k < k_len; k++)
		{
			const StdVec *record = &STD_VEC_AT(&frame->records, StdVec, k);

			if (!frame->ops->apply(frame->list, record->ptr, record->len, frame->error))
				goto done;
		}
		read->past = STD_ADD(u64, read->past,
							 STD_SUB(u64, (uint64) frame->records.len,
									 (uint64) (whole && frame->records.len != 0), 115, 26),
							 115, 13);
	}
	if (frame->ops->generation(frame->list) != frame->tail.generation)
	{
		*frame->error = chunks_packs_view_path_error(frame->path, frame->path_len,
													 " is not of the whole write its tail names");
		goto done;
	}
	read->at = frame->tail.end;
	*frame->result = true;
	frame->ok = true;
done:
	chunks_packs_view_Read_read_to_pad_0(frame);
}

/* chunks_packs_view_Read_read_to is bring without the forgetting. */
static bool
chunks_packs_view_Read_read_to(const ChunksPacksViewListOps *ops, ChunksPacksViewRead *read, void *list,
							   HostIoIoDoors io, const char *path, size_t path_len, ChunksPacksTail tail,
							   bool *result, ChunksErrorError *error)
{
	chunks_packs_view_Read_read_to_frame_0_t frame = {.ops = ops, .read = read, .list = list, .io = io,
	.path = path, .path_len = path_len, .tail = tail, .result = result, .error = error};

	std_vec_new(&frame.records, sizeof(StdVec), _Alignof(StdVec));
	std_landing(&frame, chunks_packs_view_Read_read_to_body_0, chunks_packs_view_Read_read_to_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_view_Read_bring reads a list file's records from where the
 * view last read it to a tail and applies them, reading it from its start
 * when the tail is of another whole write. A read that fails, or finds no
 * file, which it answers as false, forgets what it read.
 */
static bool
chunks_packs_view_Read_bring(const ChunksPacksViewListOps *ops, ChunksPacksViewRead *read, void *list,
							 HostIoIoDoors io, const char *path, size_t path_len, ChunksPacksTail tail, bool *result,
							 ChunksErrorError *error)
{
	bool		ok = chunks_packs_view_Read_read_to(ops, read, list, io, path, path_len, tail, result, error);

	if (!ok || !*result)
		chunks_packs_view_Read_bring_reset(ops, read, list);
	return ok;
}

/* chunks_packs_view_absent_frame_0_t is absent's state across its pad: the action's description. */
typedef struct chunks_packs_view_absent_frame_0_t
{
	const char *path;
	size_t		path_len;
	ChunksErrorError error;
	StdString	described;
} chunks_packs_view_absent_frame_0_t;

/* chunks_packs_view_absent_pad_0 is absent's pad: the description. */
static void
chunks_packs_view_absent_pad_0(void *frame_)
{
	chunks_packs_view_absent_frame_0_t *frame = frame_;

	std_String_drop(&frame->described);
	std_vec_new(&frame->described, 1, 1);
}

/* chunks_packs_view_absent_body_0 is absent's body: the open described, then the error of it. */
static void
chunks_packs_view_absent_body_0(void *frame_)
{
	chunks_packs_view_absent_frame_0_t *frame = frame_;
	ChunksErrorAction open = {.tag = CHUNKS_ERROR_ACTION_OPEN};
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("could not "), STD_FMT_ARG(0), STD_FMT_LIT(": No such file or directory")};
	StdFmtArg	args[] = {{&frame->described, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	std_String_drop(&frame->described);
	chunks_error_Action_describe(&open, frame->path, frame->path_len, &frame->described);
	std_fmt_format_Checked(&message, &arguments);
	frame->error.tag = CHUNKS_ERROR_ERROR_HOST;
	frame->error.u.host = host_Error_fork((const uint8 *) "58P01", &message);
	chunks_packs_view_absent_pad_0(frame);
}

/* chunks_packs_view_absent returns the error of a file that isn't there, as Postgres fails to open a relation's file. */
static ChunksErrorError
chunks_packs_view_absent(const char *path, size_t path_len)
{
	chunks_packs_view_absent_frame_0_t frame = {.path = path, .path_len = path_len};

	std_vec_new(&frame.described, 1, 1);
	std_landing(&frame, chunks_packs_view_absent_body_0, chunks_packs_view_absent_pad_0);
	return frame.error;
}

/* chunks_packs_view_runs_entry_drop is a (u32, Read<Runs>)'s drop glue. */
static void
chunks_packs_view_runs_entry_drop(void *value)
{
	chunks_packs_Runs_drop(&((chunks_TupleU32ReadRuns_t *) value)->f1.list);
}

/* chunks_packs_view_reader_drop is Arc<PackReader>'s drop glue, of a (u64, Arc<PackReader>). */
static void
chunks_packs_view_reader_entry_drop(void *value)
{
	chunks_pack_ArcPackReader_drop(((chunks_TupleU64ArcPackReader_t *) value)->f1);
}

/* chunks_packs_view_index_entry_drop is a (u32, AddressIndex)'s drop glue. */
static void
chunks_packs_view_index_entry_drop(void *value)
{
	chunks_index_AddressIndexLocator_drop(&((chunks_TupleU32AddressIndex_t *) value)->f1);
}

/* chunks_packs_view_dead_entry_drop is a (u64, (NonZeroU64, Dead))'s drop glue. */
static void
chunks_packs_view_dead_entry_drop(void *value)
{
	chunks_dead_Dead_drop(&((chunks_TupleU64Dead_t *) value)->dead);
}

/* chunks_packs_view_View_Default_default is View's derived Default: no directory, no repository, and nothing read. */
void
chunks_packs_view_View_Default_default(ChunksPacksViewView *result)
{
	memset(result, 0, sizeof(*result));
	std_PathBuf_new(&result->dir);
	chunks_packs_Table_Default_default(&result->table.list);
	std_vec_new(&result->runs, sizeof(chunks_TupleU32ReadRuns_t), _Alignof(chunks_TupleU32ReadRuns_t));
	chunks_packs_Runs_Default_default(&result->forwarding.list);
	std_vec_new(&result->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&result->readers, sizeof(chunks_TupleU64ArcPackReader_t), _Alignof(chunks_TupleU64ArcPackReader_t));
	std_vec_new(&result->indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t));
	result->forwards = chunks_index_AddressIndexLocator_Default_default();
	std_vec_new(&result->dead, sizeof(chunks_TupleU64Dead_t), _Alignof(chunks_TupleU64Dead_t));
	std_vec_new(&result->missing, sizeof(uint32), _Alignof(uint32));
}

/* chunks_packs_view_View_new returns a view that has read nothing, of a repository's packs. */
void
chunks_packs_view_View_new(uint32 repository, uint32 own, ChunksPacksViewView *result)
{
	chunks_packs_view_View_Default_default(result);
	std_vec_free_Checked(&result->dir.bytes, 1, 1);
	result->dir = host_tablespace_dir(repository, own);
	result->repository = repository;
}

/* chunks_packs_view_View_drop is View's drop glue: what it read and what it holds open. */
void
chunks_packs_view_View_drop(ChunksPacksViewView *self)
{
	std_vec_free_Checked(&self->dir.bytes, 1, 1);
	chunks_packs_Table_drop(&self->table.list);
	std_vec_drop_Checked(&self->runs, sizeof(chunks_TupleU32ReadRuns_t), _Alignof(chunks_TupleU32ReadRuns_t),
						 chunks_packs_view_runs_entry_drop);
	chunks_packs_Runs_drop(&self->forwarding.list);
	std_vec_free_Checked(&self->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_drop_Checked(&self->readers, sizeof(chunks_TupleU64ArcPackReader_t),
						 _Alignof(chunks_TupleU64ArcPackReader_t), chunks_packs_view_reader_entry_drop);
	std_vec_drop_Checked(&self->indexes, sizeof(chunks_TupleU32AddressIndex_t),
						 _Alignof(chunks_TupleU32AddressIndex_t), chunks_packs_view_index_entry_drop);
	chunks_index_AddressIndexLocator_drop(&self->forwards);
	std_vec_drop_Checked(&self->dead, sizeof(chunks_TupleU64Dead_t), _Alignof(chunks_TupleU64Dead_t),
						 chunks_packs_view_dead_entry_drop);
	std_vec_free_Checked(&self->missing, sizeof(uint32), _Alignof(uint32));
}

/*
 * chunks_packs_view_View_space_dir returns the directory of the repository's
 * packs in a tablespace: its own for the tablespace numbered 0, and the one
 * the file door names for the tablespace's id otherwise.
 */
void
chunks_packs_view_View_space_dir(const ChunksPacksViewView *self, uint32 space, StdPathBuf *result)
{
	if (space == CHUNKS_PACKS_DEFAULT_SPACE)
		std_PathBuf_Clone_clone(&self->dir, result);
	else
		*result = host_tablespace_dir(self->repository, space);
}

/* chunks_packs_view_runs_find reports whether a map of runs files holds a tablespace, and where it goes. */
static bool
chunks_packs_view_runs_find(const StdVec *runs, uint32 space, size_t *at)
{
	size_t		low = 0;
	size_t		high = runs->len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		uint32		found = STD_VEC_AT(runs, chunks_TupleU32ReadRuns_t, middle).f0;

		if (found == space)
		{
			*at = middle;
			return true;
		}
		if (found < space)
			low = middle + 1;
		else
			high = middle;
	}
	*at = low;
	return false;
}

/* chunks_packs_view_runs_get returns a tablespace's runs file as the view read it, or NULL. */
ChunksPacksViewReadRuns *
chunks_packs_view_runs_get(const StdVec *runs, uint32 space)
{
	size_t		at;

	if (!chunks_packs_view_runs_find(runs, space, &at))
		return NULL;
	return &STD_VEC_AT(runs, chunks_TupleU32ReadRuns_t, at).f1;
}

/* chunks_packs_view_runs_entry returns a tablespace's runs file as the view read it, a new one read to nothing if none. */
ChunksPacksViewReadRuns *
chunks_packs_view_runs_entry(StdVec *runs, uint32 space)
{
	size_t		at;
	chunks_TupleU32ReadRuns_t entry = {.f0 = space};

	if (!chunks_packs_view_runs_find(runs, space, &at))
	{
		chunks_packs_Runs_Default_default(&entry.f1.list);
		std_vec_insert(runs, at, &entry, sizeof(entry), _Alignof(chunks_TupleU32ReadRuns_t), RUST_FILE, 0, 0);
	}
	return &STD_VEC_AT(runs, chunks_TupleU32ReadRuns_t, at).f1;
}

/* chunks_packs_view_runs_remove drops a tablespace's runs file as the view read it, if it has one. */
void
chunks_packs_view_runs_remove(StdVec *runs, uint32 space)
{
	size_t		at;
	chunks_TupleU32ReadRuns_t removed;

	if (!chunks_packs_view_runs_find(runs, space, &at))
		return;
	std_vec_remove(runs, at, &removed, sizeof(removed), _Alignof(chunks_TupleU32ReadRuns_t), RUST_FILE, 0, 0);
	chunks_packs_Runs_drop(&removed.f1.list);
}

/* chunks_packs_view_missing_contains reports whether the view found a tablespace's directory missing. */
bool
chunks_packs_view_missing_contains(const StdVec *missing, uint32 space)
{
	for (size_t k = 0, k_len = missing->len; k < k_len; k++)
		if (STD_VEC_AT(missing, uint32, k) == space)
			return true;
	return false;
}

/* chunks_packs_view_missing_insert adds a tablespace to the view's missing ones, kept in order. */
static void
chunks_packs_view_missing_insert(StdVec *missing, uint32 space)
{
	size_t		at = 0;

	while (at < missing->len && STD_VEC_AT(missing, uint32, at) < space)
		at++;
	if (at < missing->len && STD_VEC_AT(missing, uint32, at) == space)
		return;
	std_vec_insert(missing, at, &space, sizeof(space), _Alignof(uint32), RUST_FILE, 0, 0);
}

/* chunks_packs_view_View_copied returns the record's generation and its snapshot, read together under the state lock. */
static bool
chunks_packs_view_View_copied(HostSharedRecord record, HostSharedGuard *state, uint64 *generation,
							  ChunksPacksSharedSnapshot *snapshot, ChunksErrorError *error)
{
	StdVec		bytes;
	bool		ok;

	*generation = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_GENERATION_INDEX);
	bytes = host_shared_Guard_read_blob(state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB);
	ok = chunks_packs_shared_Snapshot_read(bytes.ptr, bytes.len, snapshot, error);
	std_vec_free_Checked(&bytes, 1, 1);
	return ok;
}

/* chunks_packs_view_View_forget makes the view one that has read nothing, keeping the packs and runs it has open. */
void
chunks_packs_view_View_forget(ChunksPacksViewView *self)
{
	self->has_current = false;
	chunks_packs_view_Read_bring_reset(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &self->table.read, &self->table.list);
	chunks_packs_view_Read_bring_reset(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &self->forwarding.read,
									   &self->forwarding.list);
	std_vec_clear(&self->runs, sizeof(chunks_TupleU32ReadRuns_t), chunks_packs_view_runs_entry_drop);
}

/* chunks_packs_view_paths_frame_0_t is a view function's state across its pad: the paths and name it makes. */
typedef struct chunks_packs_view_paths_frame_0_t
{
	ChunksPacksViewView *self;
	HostIoIoDoors io;
	uint32		space;
	const ChunksPackPackId *id;
	const ChunksPacksSharedSnapshot *snapshot;
	bool	   *has_absent;
	StdPathBuf *absent;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdString	name;
	StdVec		tails;
	ChunksErrorError made;
} chunks_packs_view_paths_frame_0_t;

/* chunks_packs_view_paths_pad_0 is a view function's pad: the paths, name, and tails it made. */
static void
chunks_packs_view_paths_pad_0(void *frame_)
{
	chunks_packs_view_paths_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->tails, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	std_vec_new(&frame->tails, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	std_String_drop(&frame->name);
	std_vec_new(&frame->name, 1, 1);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_view_paths_run runs a view function's body over its frame, its paths, name, and tails empty to start. */
static void
chunks_packs_view_paths_run(chunks_packs_view_paths_frame_0_t *frame, void (*body) (void *frame))
{
	std_PathBuf_new(&frame->dir);
	std_PathBuf_new(&frame->path);
	std_vec_new(&frame->name, 1, 1);
	std_vec_new(&frame->tails, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	std_landing(frame, body, chunks_packs_view_paths_pad_0);
}

/* chunks_packs_view_space_dir_into makes the frame's directory a tablespace's, freeing the one it held. */
static void
chunks_packs_view_space_dir_into(chunks_packs_view_paths_frame_0_t *frame, uint32 space)
{
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	chunks_packs_view_View_space_dir(frame->self, space, &frame->dir);
}

/* chunks_packs_view_path_into makes the frame's path a directory's joined with a name, freeing the one it held. */
static void
chunks_packs_view_path_into(chunks_packs_view_paths_frame_0_t *frame, const StdPathBuf *dir, const char *name,
							size_t name_len)
{
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_path_join(dir->bytes.ptr, dir->bytes.len, name, name_len, &frame->path);
}

/* chunks_packs_view_View_look_again_body_0 is look_again's body: each missing tablespace's directory looked for. */
static void
chunks_packs_view_View_look_again_body_0(void *frame_)
{
	chunks_packs_view_paths_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;

	frame->ok = false;
	for (size_t k = 0, k_len = self->missing.len; k < k_len; k++)
	{
		bool		there;

		chunks_packs_view_space_dir_into(frame, STD_VEC_AT(&self->missing, uint32, k));
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len,
								   &there, frame->error))
			goto done;
		if (there)
		{
			self->has_current = false;
			break;
		}
	}
	frame->ok = true;
done:
	chunks_packs_view_paths_pad_0(frame);
}

/*
 * chunks_packs_view_View_look_again makes the view read the lists again when
 * the directory of a tablespace it found missing is back.
 */
static bool
chunks_packs_view_View_look_again(ChunksPacksViewView *self, HostIoIoDoors io, ChunksErrorError *error)
{
	chunks_packs_view_paths_frame_0_t frame = {.self = self, .io = io, .error = error};

	chunks_packs_view_paths_run(&frame, chunks_packs_view_View_look_again_body_0);
	return frame.ok;
}

/* chunks_packs_view_View_unread_body_0 is unread's body: the pack's path in its tablespace, then its error. */
static void
chunks_packs_view_View_unread_body_0(void *frame_)
{
	chunks_packs_view_paths_frame_0_t *frame = frame_;

	chunks_packs_view_space_dir_into(frame, frame->space);
	std_String_drop(&frame->name);
	frame->name = chunks_pack_PackId_file_name(frame->id, "pack", 4);
	chunks_packs_view_path_into(frame, &frame->dir, frame->name.ptr, frame->name.len);
	frame->made = chunks_packs_view_absent(frame->path.bytes.ptr, frame->path.bytes.len);
	chunks_packs_view_paths_pad_0(frame);
}

/* chunks_packs_view_View_unread returns the error of a read into a listed pack whose tablespace's directory is missing. */
ChunksErrorError
chunks_packs_view_View_unread(const ChunksPacksViewView *self, uint32 space, const ChunksPackPackId *id)
{
	chunks_packs_view_paths_frame_0_t frame = {.self = (ChunksPacksViewView *) self, .space = space, .id = id};

	chunks_packs_view_paths_run(&frame, chunks_packs_view_View_unread_body_0);
	return frame.made;
}

/*
 * chunks_packs_view_absent_path answers a read of the lists with the list
 * file that isn't there, the frame's path, which it moves out.
 */
static void
chunks_packs_view_absent_path(chunks_packs_view_paths_frame_0_t *frame)
{
	*frame->has_absent = true;
	*frame->absent = frame->path;
	std_PathBuf_new(&frame->path);
}

/*
 * chunks_packs_view_View_read_lists_body_0 is read_lists' body: the pack
 * table's file, each tablespace's runs file, and the forwarding file read to
 * their tails, a tablespace whose directory is missing kept unread.
 */
static void
chunks_packs_view_View_read_lists_body_0(void *frame_)
{
	chunks_packs_view_paths_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;
	bool		brought;
	size_t		kept = 0;

	frame->ok = false;
	std_vec_clear(&self->missing, sizeof(uint32), NULL);
	*frame->has_absent = false;
	chunks_packs_view_path_into(frame, &self->dir, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE));
	if (!chunks_packs_view_Read_bring(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &self->table.read, &self->table.list,
									  frame->io, frame->path.bytes.ptr, frame->path.bytes.len,
									  frame->snapshot->table.tail, &brought, frame->error))
		goto done;
	if (!brought)
	{
		chunks_packs_view_absent_path(frame);
		frame->ok = true;
		goto done;
	}
	for (size_t k = 0, k_len = self->runs.len; k < k_len; k++)
	{
		chunks_TupleU32ReadRuns_t *entry = &STD_VEC_AT(&self->runs, chunks_TupleU32ReadRuns_t, k);

		if (chunks_packs_table_runs_get(&self->table.list.runs, entry->f0) != NULL)
			STD_VEC_AT(&self->runs, chunks_TupleU32ReadRuns_t, kept++) = *entry;
		else
			chunks_packs_Runs_drop(&entry->f1.list);
	}
	self->runs.len = kept;
	std_vec_clone(&frame->tails, &self->table.list.runs, sizeof(chunks_TupleU32Tail_t), _Alignof(chunks_TupleU32Tail_t));
	for (size_t k = 0, k_len = frame->tails.len; k < k_len; k++)
	{
		chunks_TupleU32Tail_t tail = STD_VEC_AT(&frame->tails, chunks_TupleU32Tail_t, k);
		ChunksPacksViewReadRuns *runs;
		bool		there;

		chunks_packs_view_space_dir_into(frame, tail.f0);
		chunks_packs_view_path_into(frame, &frame->dir, CHUNKS_PACKS_RUNS, strlen(CHUNKS_PACKS_RUNS));
		runs = chunks_packs_view_runs_entry(&self->runs, tail.f0);
		if (!chunks_packs_view_Read_bring(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &runs->read, &runs->list, frame->io,
										  frame->path.bytes.ptr, frame->path.bytes.len, tail.f1, &brought,
										  frame->error))
			goto done;
		if (brought)
			continue;
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len,
								   &there, frame->error))
			goto done;
		if (there)
		{
			chunks_packs_view_absent_path(frame);
			frame->ok = true;
			goto done;
		}
		chunks_packs_view_runs_remove(&self->runs, tail.f0);
		chunks_packs_view_missing_insert(&self->missing, tail.f0);
	}
	chunks_packs_view_path_into(frame, &self->dir, CHUNKS_PACKS_FORWARDING, strlen(CHUNKS_PACKS_FORWARDING));
	if (!chunks_packs_view_Read_bring(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &self->forwarding.read, &self->forwarding.list,
									  frame->io, frame->path.bytes.ptr, frame->path.bytes.len,
									  frame->snapshot->forwarding.tail, &brought, frame->error))
		goto done;
	if (!brought)
		chunks_packs_view_absent_path(frame);
	frame->ok = true;
done:
	chunks_packs_view_paths_pad_0(frame);
}

/*
 * chunks_packs_view_View_read_lists reads the pack table's file, each
 * tablespace's runs file, and the forwarding file to their tails, and
 * answers the list file that isn't there, if one isn't, keeping a
 * tablespace whose directory is missing unread.
 */
static bool
chunks_packs_view_View_read_lists(ChunksPacksViewView *self, HostIoIoDoors io, const ChunksPacksSharedSnapshot *snapshot,
								  bool *has_absent, StdPathBuf *absent, ChunksErrorError *error)
{
	chunks_packs_view_paths_frame_0_t frame = {.self = self, .io = io, .snapshot = snapshot,
	.has_absent = has_absent, .absent = absent, .error = error};

	chunks_packs_view_paths_run(&frame, chunks_packs_view_View_read_lists_body_0);
	return frame.ok;
}

/*
 * chunks_packs_view_View_refresh_frame_0_t is refresh's state across its
 * pad: the state lock's guard, the snapshot, and the list files' flag.
 */
typedef struct chunks_packs_view_View_refresh_frame_0_t
{
	ChunksPacksViewView *self;
	HostIoIo   *io;
	HostSharedRecord record;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard state;
	bool		state_live;
	ChunksPacksSharedSnapshot snapshot;
	bool		snapshot_live;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
} chunks_packs_view_View_refresh_frame_0_t;

/* chunks_packs_view_View_refresh_pad_0 is refresh's pad: the guards, then the snapshot. */
static void
chunks_packs_view_View_refresh_pad_0(void *frame_)
{
	chunks_packs_view_View_refresh_frame_0_t *frame = frame_;

	if (frame->state_live)
	{
		frame->state_live = false;
		host_shared_Guard_drop(&frame->state);
	}
	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	if (frame->snapshot_live)
	{
		frame->snapshot_live = false;
		chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	}
}

/* chunks_packs_view_View_refresh_copied copies the generation and snapshot under the state lock just taken. */
static bool
chunks_packs_view_View_refresh_copied(chunks_packs_view_View_refresh_frame_0_t *frame, uint64 *generation)
{
	bool		ok;

	frame->state_live = true;
	if (frame->snapshot_live)
	{
		frame->snapshot_live = false;
		chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	}
	ok = chunks_packs_view_View_copied(frame->record, &frame->state, generation, &frame->snapshot, frame->error);
	frame->state_live = false;
	host_shared_Guard_drop(&frame->state);
	frame->snapshot_live = ok;
	return ok;
}

/*
 * chunks_packs_view_View_refresh_body_0 is refresh's body: the lists read
 * under no lock, and again under the list files' flag when a whole write may
 * have come between the reads.
 */
static void
chunks_packs_view_View_refresh_body_0(void *frame_)
{
	chunks_packs_view_View_refresh_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(frame->io);
	uint64		rotations;
	uint64		generation;
	bool		read;
	bool		has_absent;
	StdPathBuf	absent;
	HostError	host;

	frame->ok = false;
	if (!chunks_packs_view_View_look_again(self, io, frame->error))
		return;
	if (self->has_current &&
		self->current == chunks_packs_shared_load(frame->record, CHUNKS_PACKS_SHARED_GENERATION_INDEX))
	{
		frame->ok = true;
		return;
	}
	self->has_current = false;
	rotations = chunks_packs_shared_load(frame->record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX);
	if (!host_shared_Record_lock_shared(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io),
										CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX, &frame->state, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (!chunks_packs_view_View_refresh_copied(frame, &generation))
		goto done;
	read = STD_REM_FOLDED(u64, rotations, 2) == 0;
	if (read)
	{
		ChunksErrorError failed;

		if (chunks_packs_view_View_read_lists(self, io, &frame->snapshot, &has_absent, &absent, &failed))
		{
			read = !has_absent;
			if (has_absent)
				std_vec_free_Checked(&absent.bytes, 1, 1);
		}
		else
		{
			ChunksErrorOwn own;

			if (!chunks_error_Error_own(&failed, &own, frame->error))
				goto done;
			chunks_error_Own_drop(&own);
			read = false;
		}
	}
	if (!read || chunks_packs_shared_load(frame->record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX) != rotations)
	{
		HostSharedFlagIo flag_io;

#ifdef DOLTGRES_TEST_CHUNKS
		chunks_packs_view_count_reread();
#endif
		if (!host_shared_Record_flag(frame->record, HOST_IO_IO_TAKES_SHORT(frame->io),
									 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &frame->rewriting, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		frame->rewriting_live = true;
		flag_io = host_shared_FlagGuard_io(&frame->rewriting);
		if (!host_shared_FlagIo_lock_shared(&flag_io, CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX, &frame->state, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		if (!chunks_packs_view_View_refresh_copied(frame, &generation))
			goto done;
		chunks_packs_view_View_forget(self);
		if (!chunks_packs_view_View_read_lists(self, HOST_SHARED_FLAG_IO_IO_DOORS(&flag_io), &frame->snapshot,
											   &has_absent, &absent, frame->error))
			goto done;
		if (has_absent)
		{
			*frame->error = chunks_packs_view_absent(absent.bytes.ptr, absent.bytes.len);
			std_vec_free_Checked(&absent.bytes, 1, 1);
			goto done;
		}
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
	if (!chunks_packs_view_View_open(self, HOST_IO_IO_READ_DOORS(frame->io), frame->snapshot.deferred.ptr,
									 frame->snapshot.deferred.len, frame->error))
		goto done;
	std_vec_free_Checked(&self->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	self->deferred = frame->snapshot.deferred;
	std_vec_new(&frame->snapshot.deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	self->has_current = true;
	self->current = generation;
	frame->ok = true;
done:
	chunks_packs_view_View_refresh_pad_0(frame);
}

/*
 * chunks_packs_view_View_refresh brings the view up to date with the pack
 * record, reading the list files under no lock, and again from their start
 * under the list files' flag when the count of rotations was odd or moved, or
 * a read found a list file missing or not as its tail says.
 */
bool
chunks_packs_view_View_refresh(ChunksPacksViewView *self, HostIoIo *io, HostSharedRecord record,
							   ChunksErrorError *error)
{
	chunks_packs_view_View_refresh_frame_0_t frame = {.self = self, .io = io, .record = record, .error = error};

	std_landing(&frame, chunks_packs_view_View_refresh_body_0, chunks_packs_view_View_refresh_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_view_View_refresh_under_frame_0_t is refresh_under's state
 * across its pad: the state lock's guard, the snapshot until its caller has
 * it, and the path of a list file that isn't there.
 */
typedef struct chunks_packs_view_View_refresh_under_frame_0_t
{
	ChunksPacksViewView *self;
	HostSharedLongIo *io;
	HostSharedRecord record;
	ChunksPacksSharedSnapshot *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard state;
	bool		state_live;
	ChunksPacksSharedSnapshot snapshot;
	StdPathBuf	absent;
} chunks_packs_view_View_refresh_under_frame_0_t;

/* chunks_packs_view_View_refresh_under_pad_0 is refresh_under's pad: the guard, the path, then the snapshot. */
static void
chunks_packs_view_View_refresh_under_pad_0(void *frame_)
{
	chunks_packs_view_View_refresh_under_frame_0_t *frame = frame_;

	if (frame->state_live)
	{
		frame->state_live = false;
		host_shared_Guard_drop(&frame->state);
	}
	std_vec_free_Checked(&frame->absent.bytes, 1, 1);
	std_PathBuf_new(&frame->absent);
	chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	chunks_packs_shared_Snapshot_Default_default(&frame->snapshot);
}

/* chunks_packs_view_View_refresh_under_body_0 is refresh_under's body: the snapshot copied, then the lists read. */
static void
chunks_packs_view_View_refresh_under_body_0(void *frame_)
{
	chunks_packs_view_View_refresh_under_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;
	HostIoIoDoors doors = HOST_SHARED_LONG_IO_IO_DOORS(frame->io);
	HostError	host;
	uint64		generation;
	bool		has_absent;
	bool		copied;

	frame->ok = false;
	if (!chunks_packs_view_View_look_again(self, doors, frame->error))
		goto done;
	if (!host_shared_LongIo_lock_shared(frame->io, CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX, &frame->state, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->state_live = true;
	chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	copied = chunks_packs_view_View_copied(frame->record, &frame->state, &generation, &frame->snapshot, frame->error);
	if (!copied)
		chunks_packs_shared_Snapshot_Default_default(&frame->snapshot);
	frame->state_live = false;
	host_shared_Guard_drop(&frame->state);
	if (!copied)
		goto done;
	if (!self->has_current || self->current != generation)
	{
		self->has_current = false;
		if (!chunks_packs_view_View_read_lists(self, doors, &frame->snapshot, &has_absent, &frame->absent,
											   frame->error))
			goto done;
		if (has_absent)
		{
			*frame->error = chunks_packs_view_absent(frame->absent.bytes.ptr, frame->absent.bytes.len);
			goto done;
		}
		if (!chunks_packs_view_View_open(self, HOST_IO_IO_DOORS_READ_DOORS(doors), frame->snapshot.deferred.ptr,
										 frame->snapshot.deferred.len, frame->error))
			goto done;
		std_vec_free_Checked(&self->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
		std_vec_new(&self->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
		std_vec_clone(&self->deferred, &frame->snapshot.deferred, sizeof(ChunksPacksTableEntry),
					  _Alignof(ChunksPacksTableEntry));
		self->has_current = true;
		self->current = generation;
	}
	*frame->result = frame->snapshot;
	chunks_packs_shared_Snapshot_Default_default(&frame->snapshot);
	frame->ok = true;
done:
	chunks_packs_view_View_refresh_under_pad_0(frame);
}

/*
 * chunks_packs_view_View_refresh_under brings the view up to date under the
 * lists lock, which keeps every append and whole write out, and returns the
 * snapshot it is then current to, keeping its own copy of the deferred adds.
 */
bool
chunks_packs_view_View_refresh_under(ChunksPacksViewView *self, HostSharedLongIo *io, HostSharedRecord record,
									 ChunksPacksSharedSnapshot *result, ChunksErrorError *error)
{
	chunks_packs_view_View_refresh_under_frame_0_t frame = {.self = self, .io = io, .record = record,
	.result = result, .error = error};

	chunks_packs_shared_Snapshot_Default_default(&frame.snapshot);
	std_PathBuf_new(&frame.absent);
	std_landing(&frame, chunks_packs_view_View_refresh_under_body_0, chunks_packs_view_View_refresh_under_pad_0);
	return frame.ok;
}

/* chunks_packs_view_readers_find reports whether the view's open packs hold a number, and where it goes. */
static bool
chunks_packs_view_readers_find(const StdVec *readers, uint64 number, size_t *at)
{
	size_t		low = 0;
	size_t		high = readers->len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		uint64		found = STD_VEC_AT(readers, chunks_TupleU64ArcPackReader_t, middle).f0;

		if (found == number)
		{
			*at = middle;
			return true;
		}
		if (found < number)
			low = middle + 1;
		else
			high = middle;
	}
	*at = low;
	return false;
}

/* chunks_packs_view_View_pack returns the open pack of a number, if the view lists one, or NULL. */
StdArcInner *const *
chunks_packs_view_View_pack(const ChunksPacksViewView *self, uint64 number)
{
	size_t		at;

	if (!chunks_packs_view_readers_find(&self->readers, number, &at))
		return NULL;
	return &STD_VEC_AT(&self->readers, chunks_TupleU64ArcPackReader_t, at).f1;
}

/* chunks_packs_view_listed_insert puts a pack's entry in a listing kept in number order, replacing one of its number. */
static void
chunks_packs_view_listed_insert(StdVec *listed, const ChunksPacksTableEntry *entry)
{
	size_t		at = 0;

	while (at < listed->len && STD_VEC_AT(listed, ChunksPacksTableEntry, at).f0 < entry->f0)
		at++;
	if (at < listed->len && STD_VEC_AT(listed, ChunksPacksTableEntry, at).f0 == entry->f0)
		STD_VEC_AT(listed, ChunksPacksTableEntry, at) = *entry;
	else
		std_vec_insert(listed, at, entry, sizeof(*entry), _Alignof(ChunksPacksTableEntry), RUST_FILE, 0, 0);
}

/*
 * chunks_packs_view_View_listed returns the tablespace and id of each pack the
 * view lists, by number: the table's and the deferred ones, a
 * BTreeMap<u64, (u32, PackId)> as TableEntries in number order.
 */
void
chunks_packs_view_View_listed(const ChunksPacksViewView *self, StdVec *result)
{
	std_vec_new(result, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	for (size_t k = 0, k_len = self->table.list.packs.len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *pack = &STD_VEC_AT(&self->table.list.packs, chunks_TupleU64Listed_t, k);
		ChunksPacksTableEntry entry = {pack->f0, pack->f1.space, pack->f1.id};

		std_vec_push_Checked(result, &entry, sizeof(entry), _Alignof(ChunksPacksTableEntry));
	}
	for (size_t k = 0, k_len = self->deferred.len; k < k_len; k++)
		chunks_packs_view_listed_insert(result, &STD_VEC_AT(&self->deferred, ChunksPacksTableEntry, k));
}

/* chunks_packs_view_View_index returns a tablespace's address index, if the view has runs there, or NULL. */
const ChunksIndexAddressIndexLocator *
chunks_packs_view_View_index(const ChunksPacksViewView *self, uint32 space)
{
	for (size_t k = 0, k_len = self->indexes.len; k < k_len; k++)
		if (STD_VEC_AT(&self->indexes, chunks_TupleU32AddressIndex_t, k).f0 == space)
			return &STD_VEC_AT(&self->indexes, chunks_TupleU32AddressIndex_t, k).f1;
	return NULL;
}

/* chunks_packs_view_View_forwards returns the repository's forwards. */
const ChunksIndexAddressIndexLocator *
chunks_packs_view_View_forwards(const ChunksPacksViewView *self)
{
	return &self->forwards;
}

/* chunks_packs_view_View_table returns the pack table as its file's records give it, without the deferred adds. */
const ChunksPacksTable *
chunks_packs_view_View_table(const ChunksPacksViewView *self)
{
	return &self->table.list;
}

/*
 * chunks_packs_view_reopened_frame_0_t is reopened's state across its pad:
 * the runs it gathers, the path of one, and a run it opened, until the runs
 * hold it.
 */
typedef struct chunks_packs_view_reopened_frame_0_t
{
	HostIoReadDoors io;
	const StdPathBuf *dir;
	const StdVec *names;
	const ChunksIndexAddressIndexLocator *before;
	ChunksIndexAddressIndexLocator *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		runs;
	StdPathBuf	path;
} chunks_packs_view_reopened_frame_0_t;

/* chunks_packs_view_reopened_pad_0 is reopened's pad: the path, then the runs gathered. */
static void
chunks_packs_view_reopened_pad_0(void *frame_)
{
	chunks_packs_view_reopened_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_drop_Checked(&frame->runs, sizeof(StdArcInner *), _Alignof(StdArcInner *), chunks_index_ArcRunLocator_drop);
	std_vec_new(&frame->runs, sizeof(StdArcInner *), _Alignof(StdArcInner *));
}

/* chunks_packs_view_reopened_body_0 is reopened's body: each run shared from the index before, or opened. */
static void
chunks_packs_view_reopened_body_0(void *frame_)
{
	chunks_packs_view_reopened_frame_0_t *frame = frame_;
	size_t		open_len;
	StdArcInner *const *open = chunks_index_AddressIndexLocator_runs(frame->before, &open_len);

	frame->ok = false;
	std_vec_reserve_Checked(&frame->runs, frame->names->len, sizeof(StdArcInner *), _Alignof(StdArcInner *));
	for (size_t k = 0, k_len = frame->names->len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(frame->names, StdString, k);
		StdArcInner *run = NULL;

		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		std_PathBuf_new(&frame->path);
		std_path_join(frame->dir->bytes.ptr, frame->dir->bytes.len, name->ptr, name->len, &frame->path);
		for (size_t n = 0; n < open_len && run == NULL; n++)
		{
			size_t		run_path_len;
			const char *run_path = chunks_index_RunLocator_path(std_arc_Deref_deref(open[n],
																					_Alignof(ChunksIndexRunLocator)),
																&run_path_len);

			if (std_path_PartialEq_eq(run_path, run_path_len, frame->path.bytes.ptr, frame->path.bytes.len))
				run = std_arc_clone(open[n]);
		}
		if (run == NULL)
		{
			ChunksIndexRunLocator opened;

			if (!chunks_index_RunLocator_open(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &opened,
											  frame->error))
				goto done;
			chunks_index_ArcRunLocator_new(&opened, &run);
		}
		std_vec_push_Checked(&frame->runs, &run, sizeof(run), _Alignof(StdArcInner *));
	}
	*frame->result = chunks_index_AddressIndexLocator_shared(&frame->runs);
	std_vec_new(&frame->runs, sizeof(StdArcInner *), _Alignof(StdArcInner *));
	frame->ok = true;
done:
	chunks_packs_view_reopened_pad_0(frame);
}

/*
 * chunks_packs_view_reopened returns the index of the runs named in a
 * directory, oldest first, sharing each run the view's index before held
 * open and opening the others.
 */
static bool
chunks_packs_view_reopened(HostIoReadDoors io, const StdPathBuf *dir, const StdVec *names,
						   const ChunksIndexAddressIndexLocator *before, ChunksIndexAddressIndexLocator *result,
						   ChunksErrorError *error)
{
	chunks_packs_view_reopened_frame_0_t frame = {.io = io, .dir = dir, .names = names, .before = before,
	.result = result, .error = error};

	std_vec_new(&frame.runs, sizeof(StdArcInner *), _Alignof(StdArcInner *));
	std_PathBuf_new(&frame.path);
	std_landing(&frame, chunks_packs_view_reopened_body_0, chunks_packs_view_reopened_pad_0);
	return frame.ok;
}

/* chunks_packs_view_names_push adds a name to a tablespace's names unless they hold it. */
static void
chunks_packs_view_names_push(StdVec *named, const char *name, size_t name_len)
{
	StdString	owned;

	for (size_t k = 0, k_len = named->len; k < k_len; k++)
	{
		const StdString *have = &STD_VEC_AT(named, StdString, k);

		if (have->len == name_len && memcmp(have->ptr, name, name_len) == 0)
			return;
	}
	std_string_from_Checked(&owned, name, name_len);
	std_vec_push_Checked(named, &owned, sizeof(owned), _Alignof(StdString));
}

/* chunks_TupleU32VecString_t is a tablespace's names of runs. */
typedef struct chunks_TupleU32VecString_t
{
	uint32		f0;
	StdVec		f1;
} chunks_TupleU32VecString_t;

/* chunks_packs_view_names_entry returns a tablespace's names of runs, a new empty list if none, kept in order. */
static StdVec *
chunks_packs_view_names_entry(StdVec *names, uint32 space)
{
	size_t		at = 0;
	chunks_TupleU32VecString_t entry = {.f0 = space};

	while (at < names->len && STD_VEC_AT(names, chunks_TupleU32VecString_t, at).f0 < space)
		at++;
	if (at == names->len || STD_VEC_AT(names, chunks_TupleU32VecString_t, at).f0 != space)
	{
		std_vec_new(&entry.f1, sizeof(StdString), _Alignof(StdString));
		std_vec_insert(names, at, &entry, sizeof(entry), _Alignof(chunks_TupleU32VecString_t), RUST_FILE, 0, 0);
	}
	return &STD_VEC_AT(names, chunks_TupleU32VecString_t, at).f1;
}

/* chunks_packs_view_names_entry_drop is a (u32, Vec<String>)'s drop glue. */
static void
chunks_packs_view_names_entry_drop(void *value)
{
	std_vec_drop_Checked(&((chunks_TupleU32VecString_t *) value)->f1, sizeof(StdString), _Alignof(StdString),
						 std_String_drop);
}

/* chunks_packs_view_run_names returns the file names of the runs a runs file's lines name. */
static void
chunks_packs_view_run_names(const ChunksPacksRuns *runs, StdVec *result)
{
	std_vec_with_capacity_Checked(result, runs->lines.len, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = runs->lines.len; k < k_len; k++)
	{
		const StdString *line = &STD_VEC_AT(&runs->lines, StdString, k);
		StdStr		name = chunks_packs_run_name(line->ptr, line->len);
		StdString	owned;

		std_string_from_Checked(&owned, name.ptr, name.len);
		std_vec_push_Checked(result, &owned, sizeof(owned), _Alignof(StdString));
	}
}

/*
 * chunks_packs_view_open_frame_0_t is open's state across its pad: the packs
 * listed, each tablespace's names of runs, the indexes made, the forwards'
 * names, a pack just opened until the view holds it, and the paths and name
 * it makes.
 */
typedef struct chunks_packs_view_open_frame_0_t
{
	ChunksPacksViewView *self;
	HostIoReadDoors io;
	const ChunksPacksTableEntry *deferred;
	size_t		deferred_len;
	ChunksErrorError *error;
	bool		ok;
	StdVec		listed;
	StdVec		names;
	StdVec		indexes;
	StdVec		forwards;
	ChunksIndexAddressIndexLocator before;
	StdArcInner *opened;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdString	name;
} chunks_packs_view_open_frame_0_t;

/* chunks_packs_view_open_pad_0 is open's pad: what it holds. */
static void
chunks_packs_view_open_pad_0(void *frame_)
{
	chunks_packs_view_open_frame_0_t *frame = frame_;

	if (frame->opened != NULL)
	{
		chunks_pack_ArcPackReader_drop(frame->opened);
		frame->opened = NULL;
	}
	std_String_drop(&frame->name);
	std_vec_new(&frame->name, 1, 1);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	chunks_index_AddressIndexLocator_drop(&frame->before);
	frame->before = chunks_index_AddressIndexLocator_Default_default();
	std_vec_drop_Checked(&frame->forwards, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->forwards, sizeof(StdString), _Alignof(StdString));
	std_vec_drop_Checked(&frame->indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t),
						 chunks_packs_view_index_entry_drop);
	std_vec_new(&frame->indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t));
}

/*
 * chunks_packs_view_View_open_packs_body_0 is open_packs' body: the packs
 * no longer named let go, then those the listing names that the view hasn't
 * open opened, a tablespace whose directory is missing kept unread.
 */
static void
chunks_packs_view_View_open_packs_body_0(void *frame_)
{
	chunks_packs_view_open_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;
	size_t		kept = 0;

	frame->ok = false;
	for (size_t k = 0, k_len = self->readers.len; k < k_len; k++)
	{
		chunks_TupleU64ArcPackReader_t *reader = &STD_VEC_AT(&self->readers, chunks_TupleU64ArcPackReader_t, k);
		const ChunksPackPackReader *open = std_arc_Deref_deref(reader->f1, _Alignof(ChunksPackPackReader));
		bool		named = false;

		for (size_t n = 0, n_len = frame->listed.len; n < n_len && !named; n++)
		{
			const ChunksPacksTableEntry *entry = &STD_VEC_AT(&frame->listed, ChunksPacksTableEntry, n);

			named = entry->f0 == reader->f0 && chunks_pack_PackId_PartialEq_eq(&entry->f2, &open->id);
		}
		if (named)
			STD_VEC_AT(&self->readers, chunks_TupleU64ArcPackReader_t, kept++) = *reader;
		else
			chunks_pack_ArcPackReader_drop(reader->f1);
	}
	self->readers.len = kept;
	kept = 0;
	for (size_t k = 0, k_len = self->dead.len; k < k_len; k++)
	{
		chunks_TupleU64Dead_t *dead = &STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, k);
		size_t		at;

		if (chunks_packs_view_readers_find(&self->readers, dead->f0, &at))
			STD_VEC_AT(&self->dead, chunks_TupleU64Dead_t, kept++) = *dead;
		else
			chunks_dead_Dead_drop(&dead->dead);
	}
	self->dead.len = kept;
	for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
	{
		const ChunksPacksTableEntry *entry = &STD_VEC_AT(&frame->listed, ChunksPacksTableEntry, k);
		size_t		at;
		bool		exists;
		bool		there = true;
		ChunksPackPackReader reader;
		chunks_TupleU64ArcPackReader_t opened;

		if (chunks_packs_view_readers_find(&self->readers, entry->f0, &at) ||
			chunks_packs_view_missing_contains(&self->missing, entry->f1))
			continue;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
		std_PathBuf_new(&frame->dir);
		chunks_packs_view_View_space_dir(self, entry->f1, &frame->dir);
		std_String_drop(&frame->name);
		frame->name = chunks_pack_PackId_file_name(&entry->f2, "pack", 4);
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		std_PathBuf_new(&frame->path);
		std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, frame->name.ptr, frame->name.len, &frame->path);
		if (!chunks_file_exists(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &exists, frame->error) ||
			(!exists && !chunks_file_dir_there(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, &there,
											   frame->error)))
			goto done;
		if (!exists && !there)
		{
			chunks_packs_view_missing_insert(&self->missing, entry->f1);
			continue;
		}
		if (!chunks_pack_PackReader_open(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &reader,
										 frame->error))
			goto done;
		opened.f0 = entry->f0;
		opened.f1 = chunks_pack_ArcPackReader_new(&reader);
		frame->opened = opened.f1;
		std_vec_insert(&self->readers, at, &opened, sizeof(opened), _Alignof(chunks_TupleU64ArcPackReader_t),
					   RUST_FILE, 361, 30);
		frame->opened = NULL;
	}
	frame->ok = true;
done:
	chunks_packs_view_open_pad_0(frame);
}

/* chunks_packs_view_View_open_packs opens the packs the lists and the deferred adds name that the view hasn't open. */
static bool
chunks_packs_view_View_open_packs(chunks_packs_view_open_frame_0_t *frame)
{
	std_landing(frame, chunks_packs_view_View_open_packs_body_0, chunks_packs_view_open_pad_0);
	return frame->ok;
}

/*
 * chunks_packs_view_View_open_body_0 is open's body: the packs opened, then
 * each tablespace's index of its runs and the deferred packs' runs, then the
 * forwards'.
 */
static void
chunks_packs_view_View_open_body_0(void *frame_)
{
	chunks_packs_view_open_frame_0_t *frame = frame_;
	ChunksPacksViewView *self = frame->self;
	ChunksIndexAddressIndexLocator reopened;

	frame->ok = false;
	for (size_t k = 0, k_len = self->table.list.packs.len; k < k_len; k++)
	{
		const chunks_TupleU64Listed_t *pack = &STD_VEC_AT(&self->table.list.packs, chunks_TupleU64Listed_t, k);
		ChunksPacksTableEntry entry = {pack->f0, pack->f1.space, pack->f1.id};

		std_vec_push_Checked(&frame->listed, &entry, sizeof(entry), _Alignof(ChunksPacksTableEntry));
	}
	std_vec_extend_from_slice_Checked(&frame->listed, frame->deferred, frame->deferred_len,
									  sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	if (!chunks_packs_view_View_open_packs(frame))
		goto done;
	frame->ok = false;
	for (size_t k = 0, k_len = self->runs.len; k < k_len; k++)
	{
		const chunks_TupleU32ReadRuns_t *runs = &STD_VEC_AT(&self->runs, chunks_TupleU32ReadRuns_t, k);
		StdVec	   *named = chunks_packs_view_names_entry(&frame->names, runs->f0);

		std_vec_drop_Checked(named, sizeof(StdString), _Alignof(StdString), std_String_drop);
		std_vec_new(named, sizeof(StdString), _Alignof(StdString));
		chunks_packs_view_run_names(&runs->f1.list, named);
	}
	for (size_t k = 0; k < frame->deferred_len; k++)
	{
		StdVec	   *named = chunks_packs_view_names_entry(&frame->names, frame->deferred[k].f1);

		std_String_drop(&frame->name);
		frame->name = chunks_pack_PackId_file_name(&frame->deferred[k].f2, "idx", 3);
		chunks_packs_view_names_push(named, frame->name.ptr, frame->name.len);
	}
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const chunks_TupleU32VecString_t *named = &STD_VEC_AT(&frame->names, chunks_TupleU32VecString_t, k);
		bool		there = true;
		chunks_TupleU32AddressIndex_t index = {.f0 = named->f0};

		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
		std_PathBuf_new(&frame->dir);
		chunks_packs_view_View_space_dir(self, named->f0, &frame->dir);
		chunks_index_AddressIndexLocator_drop(&frame->before);
		frame->before = chunks_index_AddressIndexLocator_Default_default();
		for (size_t n = 0, n_len = self->indexes.len; n < n_len; n++)
			if (STD_VEC_AT(&self->indexes, chunks_TupleU32AddressIndex_t, n).f0 == named->f0)
			{
				chunks_TupleU32AddressIndex_t removed;

				std_vec_remove(&self->indexes, n, &removed, sizeof(removed), _Alignof(chunks_TupleU32AddressIndex_t),
							   RUST_FILE, 375, 69);
				chunks_index_AddressIndexLocator_drop(&frame->before);
				frame->before = removed.f1;
				break;
			}
		if (named->f1.len != 0)
		{
			const StdString *first = &STD_VEC_AT(&named->f1, StdString, 0);
			bool		exists;

			std_vec_free_Checked(&frame->path.bytes, 1, 1);
			std_PathBuf_new(&frame->path);
			std_path_join(frame->dir.bytes.ptr, frame->dir.bytes.len, first->ptr, first->len, &frame->path);
			if (!chunks_file_exists(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &exists, frame->error) ||
				(!exists && !chunks_file_dir_there(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, &there,
												   frame->error)))
				goto done;
		}
		if (chunks_packs_view_missing_contains(&self->missing, named->f0) || !there)
		{
			chunks_packs_view_missing_insert(&self->missing, named->f0);
			continue;
		}
		if (!chunks_packs_view_reopened(frame->io, &frame->dir, &named->f1, &frame->before, &index.f1, frame->error))
			goto done;
		std_vec_push_Checked(&frame->indexes, &index, sizeof(index), _Alignof(chunks_TupleU32AddressIndex_t));
	}
	std_vec_drop_Checked(&self->indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t),
						 chunks_packs_view_index_entry_drop);
	self->indexes = frame->indexes;
	std_vec_new(&frame->indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t));
	chunks_packs_view_run_names(&self->forwarding.list, &frame->forwards);
	if (!chunks_packs_view_reopened(frame->io, &self->dir, &frame->forwards, &self->forwards, &reopened, frame->error))
		goto done;
	chunks_index_AddressIndexLocator_drop(&self->forwards);
	self->forwards = reopened;
	frame->ok = true;
done:
	std_vec_drop_Checked(&frame->names, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t),
						 chunks_packs_view_names_entry_drop);
	std_vec_new(&frame->names, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t));
	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_view_open_pad_0(frame);
}

/* chunks_packs_view_open_all_pad_0 is open's outer pad: the listing and the names, then the rest. */
static void
chunks_packs_view_open_all_pad_0(void *frame_)
{
	chunks_packs_view_open_frame_0_t *frame = frame_;

	std_vec_drop_Checked(&frame->names, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t),
						 chunks_packs_view_names_entry_drop);
	std_vec_new(&frame->names, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t));
	std_vec_free_Checked(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame->listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_view_open_pad_0(frame);
}

/*
 * chunks_packs_view_View_open opens the packs and runs the lists and the
 * deferred adds name that the view hasn't open, and lets go of those they no
 * longer name, keeping a tablespace whose directory is missing unread.
 */
bool
chunks_packs_view_View_open(ChunksPacksViewView *self, HostIoReadDoors io, const ChunksPacksTableEntry *deferred,
							size_t deferred_len, ChunksErrorError *error)
{
	chunks_packs_view_open_frame_0_t frame = {.self = self, .io = io, .deferred = deferred,
	.deferred_len = deferred_len, .error = error};

	std_vec_new(&frame.listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	std_vec_new(&frame.names, sizeof(chunks_TupleU32VecString_t), _Alignof(chunks_TupleU32VecString_t));
	std_vec_new(&frame.indexes, sizeof(chunks_TupleU32AddressIndex_t), _Alignof(chunks_TupleU32AddressIndex_t));
	std_vec_new(&frame.forwards, sizeof(StdString), _Alignof(StdString));
	frame.before = chunks_index_AddressIndexLocator_Default_default();
	std_PathBuf_new(&frame.dir);
	std_PathBuf_new(&frame.path);
	std_vec_new(&frame.name, 1, 1);
	std_landing(&frame, chunks_packs_view_View_open_body_0, chunks_packs_view_open_all_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_PACKS_VIEW_TESTS_GOT is the columns of view.rs' tests' gets: the assertion's, then its two unwraps'. */
static const uint32 CHUNKS_PACKS_VIEW_TESTS_GOT[] = {9, 34, 57};

/* chunks_packs_view_tests_added_run appends a pack's run to the runs file and its add to the table, both of generation 7. */
static void
chunks_packs_view_tests_added_run(HostIoIo *io, ChunksPacksFixturesLists *lists, const ChunksPacksTableEntry *entry,
								  uint32 line)
{
	StdString	run = chunks_packs_fixtures_line(entry);
	StdVec		record;
	ChunksPacksTail runs = {.generation = 7};

	chunks_packs_Runs_added(run.ptr, run.len, &record);
	std_vec_free_Checked(&run, 1, 1);
	chunks_packs_tests_appended(RUST_FILE, io, &lists->runs, &record, line, 73);
	runs.end = chunks_file_TornTail_end(&lists->runs);
	chunks_packs_Table_added(entry, runs, &record);
	chunks_packs_tests_appended(RUST_FILE, io, &lists->table, &record, line + 2, 68);
}

/*
 * test_chunks_packs_view_tests_a_view_reads_the_lists_to_their_tails_and_then_what_was_appended_since:
 * a view lists what the list files hold up to the tails its snapshot names
 * and no further, then what was appended since once the snapshot names it,
 * keeping open the packs and runs it had, and lets go of a pack and a run
 * the records retire.
 */
void
test_chunks_packs_view_tests_a_view_reads_the_lists_to_their_tails_and_then_what_was_appended_since(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksFixturesLists lists;
	ChunksPacksViewView view;
	StdVec		deferred;
	StdArcInner *const *pack;
	StdArcInner *reader;
	StdArcInner *run;
	StdVec		record_bytes;
	StdString	removed;
	uint64		retired = 1;
	ChunksPacksTail moved = {.generation = 7};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_tests_made(RUST_FILE, io, 423, 43);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), &dir, 7, &first.entry, 1);
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 427, 46);
	chunks_packs_fixtures_view(&view);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 429, 34);
	chunks_packs_tests_listed(RUST_FILE, &view, &first.entry, 1, 430, 9);
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &first.address, &first.at, 431, CHUNKS_PACKS_VIEW_TESTS_GOT);
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &second.address, NULL, 432, CHUNKS_PACKS_VIEW_TESTS_GOT);
	pack = chunks_packs_view_View_pack(&view, 1);
	if (pack == NULL)
		STD_OPTION_UNWRAP_FAILED(433, 54);
	reader = std_arc_clone(*pack);
	run = std_arc_clone(chunks_packs_tests_run(RUST_FILE, &view, 0, 433, 91, 77));

	chunks_packs_view_tests_added_run(io, &lists, &second.entry, 435);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 438, 34);
	STD_ASSERT_MSG(439, 9, chunks_packs_view_View_pack(&view, 2) == NULL,
				   "a view read past the tail its snapshot named");
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 440, 46);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 441, 34);
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};

		chunks_packs_tests_listed(RUST_FILE, &view, both, lengthof(both), 442, 9);
	}
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &second.address, &second.at, 443, CHUNKS_PACKS_VIEW_TESTS_GOT);
	pack = chunks_packs_view_View_pack(&view, 1);
	if (pack == NULL)
		STD_OPTION_UNWRAP_FAILED(444, 42);
	STD_ASSERT_MSG(444, 9, *pack == reader, "a refresh opened again a pack the view held open");
	STD_ASSERT_MSG(445, 9, chunks_packs_tests_run(RUST_FILE, &view, 0, 445, 44, 30) == run,
				   "a refresh opened again a run the view held");

	removed = chunks_pack_PackId_file_name(&first.entry.f2, "idx", 3);
	chunks_packs_Runs_removed(&removed, 1, &record_bytes);
	std_vec_free_Checked(&removed, 1, 1);
	chunks_packs_tests_appended(RUST_FILE, io, &lists.runs, &record_bytes, 447, 88);
	chunks_packs_Table_retired(&retired, 1, &record_bytes);
	chunks_packs_tests_appended(RUST_FILE, io, &lists.table, &record_bytes, 448, 70);
	moved.end = chunks_file_TornTail_end(&lists.runs);
	chunks_packs_Table_moved(0, moved, &record_bytes);
	chunks_packs_tests_appended(RUST_FILE, io, &lists.table, &record_bytes, 450, 64);
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 451, 46);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 452, 34);
	chunks_packs_tests_listed(RUST_FILE, &view, &second.entry, 1, 453, 9);
	STD_ASSERT(454, 9, chunks_packs_view_View_pack(&view, 1) == NULL, "view.pack(1).is_none()");
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &first.address, NULL, 455, CHUNKS_PACKS_VIEW_TESTS_GOT);
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &second.address, &second.at, 456, CHUNKS_PACKS_VIEW_TESTS_GOT);

	chunks_index_ArcRunLocator_drop(&run);
	chunks_pack_ArcPackReader_drop(reader);
	chunks_packs_view_View_drop(&view);
	chunks_packs_fixtures_Lists_drop(&lists);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_view_tests_a_view_reads_a_file_written_whole_since_from_its_start:
 * a view that read list files of one whole write reads them from their start
 * once they were written whole again, and lists what the new files hold.
 */
void
test_chunks_packs_view_tests_a_view_reads_a_file_written_whole_since_from_its_start(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksFixturesLists lists;
	ChunksPacksViewView view;
	StdVec		deferred;
	StdVec		listed;
	size_t		listed_len;
	size_t		two = 2;
	uint64		generation;
	uint64		eight = 8;
	size_t		runs_len;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_tests_made(RUST_FILE, io, 466, 43);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};

		lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), &dir, 7, both, lengthof(both));
	}
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 470, 46);
	chunks_packs_fixtures_view(&view);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 472, 34);
	chunks_packs_view_View_listed(&view, &listed);
	listed_len = listed.len;
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	STD_ASSERT_EQ(473, 9, listed_len == two, &listed_len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);

	chunks_packs_fixtures_Lists_drop(&lists);
	lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), &dir, 8, &second.entry, 1);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 2);
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 477, 46);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 478, 34);
	generation = chunks_packs_view_View_table(&view)->generation;
	STD_ASSERT_EQ(479, 9, generation == eight, &generation, std_u64_Debug_fmt, &eight, std_u64_Debug_fmt);
	chunks_packs_tests_listed(RUST_FILE, &view, &second.entry, 1, 480, 9);
	(void) chunks_index_AddressIndexLocator_runs(chunks_packs_tests_index(RUST_FILE, &view, 0, 481, 57), &runs_len);
	STD_ASSERT(481, 9, chunks_packs_view_View_pack(&view, 1) == NULL && runs_len == 1,
			   "view.pack(1).is_none() && view.index(0).unwrap().runs().len() == 1");
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &second.address, &second.at, 482, CHUNKS_PACKS_VIEW_TESTS_GOT);

	chunks_packs_view_View_drop(&view);
	chunks_packs_fixtures_Lists_drop(&lists);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_view_tests_a_view_lists_the_packs_its_snapshot_defers: a
 * view lists the packs its snapshot defers, which the list files don't hold
 * yet, and reads them through their runs; once their records are appended
 * and nothing is deferred it lists the same packs and holds the same runs
 * open.
 */
void
test_chunks_packs_view_tests_a_view_lists_the_packs_its_snapshot_defers(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksFixturesLists lists;
	ChunksPacksViewView view;
	StdVec		deferred;
	StdArcInner *run;
	size_t		runs_len;
	size_t		two = 2;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_tests_made(RUST_FILE, io, 493, 43);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), &dir, 7, &first.entry, 1);
	std_slice_to_vec_Checked(&deferred, &second.entry, 1, sizeof(ChunksPacksTableEntry),
							 _Alignof(ChunksPacksTableEntry));
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 497, 46);
	chunks_packs_fixtures_view(&view);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 499, 34);
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};

		chunks_packs_tests_listed(RUST_FILE, &view, both, lengthof(both), 500, 9);
	}
	STD_ASSERT(501, 9, chunks_packs_view_View_pack(&view, 2) != NULL &&
			   chunks_packs_table_packs_get(&chunks_packs_view_View_table(&view)->packs, 2) == NULL,
			   "view.pack(2).is_some() && !view.table().packs.contains_key(&2)");
	chunks_packs_tests_got(RUST_FILE, &view, io, 0, &second.address, &second.at, 502, CHUNKS_PACKS_VIEW_TESTS_GOT);
	run = std_arc_clone(chunks_packs_tests_run(RUST_FILE, &view, 1, 503, 45, 31));

	chunks_packs_view_tests_added_run(io, &lists, &second.entry, 505);
	deferred = chunks_packs_tests_no_deferred();
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 508, 46);
	chunks_packs_tests_refreshed(RUST_FILE, &view, io, record, 509, 34);
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};

		chunks_packs_tests_listed(RUST_FILE, &view, both, lengthof(both), 510, 9);
	}
	STD_ASSERT(511, 9, chunks_packs_table_packs_get(&chunks_packs_view_View_table(&view)->packs, 2) != NULL,
			   "view.table().packs.contains_key(&2)");
	(void) chunks_index_AddressIndexLocator_runs(chunks_packs_tests_index(RUST_FILE, &view, 0, 512, 34), &runs_len);
	STD_ASSERT_EQ(512, 9, runs_len == two, &runs_len, std_usize_Debug_fmt, &two, std_usize_Debug_fmt);
	STD_ASSERT_MSG(513, 9, chunks_packs_tests_run(RUST_FILE, &view, 1, 513, 44, 30) == run,
				   "a deferred pack's run was opened again");

	chunks_index_ArcRunLocator_drop(&run);
	chunks_packs_view_View_drop(&view);
	chunks_packs_fixtures_Lists_drop(&lists);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
