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
 * The C port of chunks' packs/lists.rs: a change to a repository's list
 * files, which under the lists lock appends its records durably, a runs
 * file's before the pack table's, and then installs the snapshot with the
 * tails past them and advances the record's generation.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/path.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "lists.rs"

/* CHUNKS_PACKS_LISTS_COMPACT_SLACK is how many more records than its whole holds a list file takes before a whole write. */
#define CHUNKS_PACKS_LISTS_COMPACT_SLACK 64

/* chunks_packs_lists_fresh returns the generation of a whole write, which no other whole write has: a pack id's first bytes. */
static uint64
chunks_packs_lists_fresh(void)
{
	ChunksPackPackId id = chunks_pack_PackId_new();

	return std_u64_from_le_bytes(id.f0);
}

/*
 * chunks_packs_lists_end returns the tail of a list file that its reader has
 * read to its end, the file's one tail while a change holds the lists lock
 * with its view current to the record, or the startup process settles.
 */
ChunksFileTornTail
chunks_packs_lists_end(const ChunksPacksViewListOps *ops, const ChunksPacksViewRead *read, const void *list,
					   const char *path, size_t path_len)
{
	ChunksFileWhole whole = {read->at, ops->generation(list)};

	return chunks_file_tail(path, path_len, whole);
}

/*
 * chunks_packs_lists_frame_0_t is a list change's state across its pad: its
 * arguments, and the paths, strings, records, and guard it owns until it
 * returns or hands them on.
 */
typedef struct chunks_packs_lists_frame_0_t
{
	ChunksPacksListsChange *self;
	const ChunksPacksViewListOps *ops;
	HostIoIoDoors io;
	const char *path_in;
	size_t		path_in_len;
	void	   *list;
	ChunksPacksViewRead *read;
	bool	   *placing;
	ChunksPacksTableEntry entry;
	uint32		space;
	uint64		number;
	uint64		generation;
	const uint8 *record_in;
	size_t		record_in_len;
	const uint64 *numbers;
	size_t		numbers_len;
	bool		all;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	StdPathBuf	path;
	StdString	name;
	StdString	line;
	StdVec		record;
	StdVec		spaces;
	ChunksPacksViewReadRuns written;
	bool		written_live;
	HostSharedGuard state;
	bool		state_live;
	bool		change_live;
	HostSharedLongGuard long_;
	bool		long_live;
	void	   *taken;
	bool		taken_live;
	ChunksPacksRuns taken_runs;
	ChunksPacksTable taken_table;
} chunks_packs_lists_frame_0_t;

/* chunks_packs_lists_pad_0 is a list change's pad: what its frame owns, the change last. */
static void
chunks_packs_lists_pad_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;

	if (frame->state_live)
	{
		frame->state_live = false;
		host_shared_Guard_drop(&frame->state);
	}
	if (frame->taken_live)
	{
		frame->taken_live = false;
		frame->ops->reset(frame->taken);
	}
	if (frame->written_live)
	{
		frame->written_live = false;
		chunks_packs_Runs_drop(&frame->written.list);
	}
	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&frame->record, 1, 1);
	std_vec_new(&frame->record, 1, 1);
	std_String_drop(&frame->line);
	std_vec_new(&frame->line, 1, 1);
	std_String_drop(&frame->name);
	std_vec_new(&frame->name, 1, 1);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	if (frame->long_live)
	{
		frame->long_live = false;
		host_shared_LongGuard_drop(&frame->long_);
	}
	if (frame->change_live)
	{
		frame->change_live = false;
		chunks_packs_lists_Change_drop(frame->self);
	}
}

/* chunks_packs_lists_run runs a list change's body over its frame, what the frame owns empty to start. */
static bool
chunks_packs_lists_run(chunks_packs_lists_frame_0_t *frame, void (*body) (void *frame))
{
	std_PathBuf_new(&frame->dir);
	std_PathBuf_new(&frame->path);
	std_vec_new(&frame->name, 1, 1);
	std_vec_new(&frame->line, 1, 1);
	std_vec_new(&frame->record, 1, 1);
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_landing(frame, body, chunks_packs_lists_pad_0);
	return frame->ok;
}

/* chunks_packs_lists_record_into makes the frame's record, freeing the one it held. */
static StdVec *
chunks_packs_lists_record_into(chunks_packs_lists_frame_0_t *frame)
{
	std_vec_free_Checked(&frame->record, 1, 1);
	std_vec_new(&frame->record, 1, 1);
	return &frame->record;
}

/* chunks_packs_lists_path_into makes the frame's path a directory's joined with a name, freeing the one it held. */
static void
chunks_packs_lists_path_into(chunks_packs_lists_frame_0_t *frame, const StdPathBuf *dir, const char *name)
{
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &frame->path);
}

/* chunks_packs_lists_space_path_into makes the frame's path a tablespace's runs file. */
static void
chunks_packs_lists_space_path_into(chunks_packs_lists_frame_0_t *frame, uint32 space)
{
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	chunks_packs_view_View_space_dir(frame->self->view, space, &frame->dir);
	chunks_packs_lists_path_into(frame, &frame->dir, CHUNKS_PACKS_RUNS);
}

/* chunks_packs_lists_written_body_0 is written's body: the list renewed and its whole record written. */
static void
chunks_packs_lists_written_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksFileTornTail torn;

	frame->ok = false;
	frame->ops->renew(frame->list, chunks_packs_lists_fresh());
	frame->ops->whole(frame->list, chunks_packs_lists_record_into(frame));
	if (chunks_file_write_appended(frame->io, frame->path_in, frame->path_in_len, frame->record.ptr, frame->record.len,
								   frame->placing, &torn, frame->error))
	{
		frame->read->at = chunks_file_TornTail_end(&torn);
		frame->read->past = 0;
		chunks_file_TornTail_drop(&torn);
		frame->ok = true;
	}
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_written writes a list file whole, with a generation no
 * whole write had, and makes the read the view that wrote it has. It sets
 * placing once the file's renames begin.
 */
bool
chunks_packs_lists_written(const ChunksPacksViewListOps *ops, HostIoIoDoors io, const char *path, size_t path_len,
						   void *list, ChunksPacksViewRead *read, bool *placing, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.ops = ops, .io = io, .path_in = path, .path_in_len = path_len,
	.list = list, .read = read, .placing = placing, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_written_body_0);
}

/* chunks_packs_lists_tail_of returns the snapshot's tail of a list file as a change's view has read and written it. */
ChunksPacksSharedListTail
chunks_packs_lists_tail_of(const ChunksPacksViewListOps *ops, const ChunksPacksViewRead *read, const void *list)
{
	ChunksPacksSharedListTail tail = {{read->at, ops->generation(list)}, read->past};

	return tail;
}

/* chunks_packs_lists_due reports whether a list file a view has read is due a whole write. */
static bool
chunks_packs_lists_due(const ChunksPacksViewRead *read, size_t whole, bool all)
{
	return read->past > (all ? 0 : (uint64) STD_ADD(usize, whole, CHUNKS_PACKS_LISTS_COMPACT_SLACK, 318, 39));
}

/* chunks_packs_lists_past_slack reports whether any list file a view has read is past its slack. */
bool
chunks_packs_lists_past_slack(const ChunksPacksViewView *view)
{
	if (chunks_packs_lists_due(&view->table.read, view->table.list.packs.len, false))
		return true;
	for (size_t k = 0, k_len = view->runs.len; k < k_len; k++)
	{
		const ChunksPacksViewReadRuns *runs = &STD_VEC_AT(&view->runs, chunks_TupleU32ReadRuns_t, k).f1;

		if (chunks_packs_lists_due(&runs->read, runs->list.lines.len, false))
			return true;
	}
	return chunks_packs_lists_due(&view->forwarding.read, view->forwarding.list.lines.len, false);
}

/* chunks_packs_lists_Change_begin_body_0 is begin's body: the view refreshed under the lists lock, then the change. */
static void
chunks_packs_lists_Change_begin_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *result = frame->self;
	HostSharedLongIo io = host_shared_LongGuard_io(&frame->long_);

	frame->ok = false;
	chunks_packs_shared_Snapshot_Default_default(&result->snapshot);
	if (chunks_packs_view_View_refresh_under(result->view, &io, result->record, &result->snapshot, frame->error))
	{
		result->long_ = frame->long_;
		frame->long_live = false;
		result->installed = false;
		result->rotating = CHUNKS_PACKS_LISTS_ROTATING_NO;
#ifdef DOLTGRES_CHUNKS_TRACE
		std_vec_new(&result->noted, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent));
#endif
		frame->ok = true;
	}
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_begin brings the changer's view up to date under
 * the lists lock just taken, which it takes, and makes the change.
 */
bool
chunks_packs_lists_Change_begin(ChunksPacksViewView *view, HostSharedRecord record, HostSharedLongGuard *long_,
								ChunksPacksListsChange *result, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = result, .error = error, .long_ = *long_, .long_live = true};

	result->view = view;
	result->record = record;
	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_begin_body_0);
}

/* chunks_packs_lists_Change_note keeps a transition the change makes for its install to record, built only with tracing. */
void
chunks_packs_lists_Change_note(ChunksPacksListsChange *self, void (*event) (void *captures, ChunksTraceEvent *result),
							   void *captures)
{
#ifdef DOLTGRES_CHUNKS_TRACE
	ChunksTraceEvent made;

	event(captures, &made);
	std_vec_push_Checked(&self->noted, &made, sizeof(made), _Alignof(ChunksTraceEvent));
#else
	(void) self;
	(void) event;
	(void) captures;
#endif
}

/* chunks_packs_lists_pack_added is PackAdded's event, of a pack's number. */
static void
chunks_packs_lists_pack_added(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_PACK_ADDED;
	result->u.pack_added.pack = *(const uint64 *) captures;
}

/* chunks_packs_lists_list_recorded is ListRecorded's event, of a pack's number. */
static void
chunks_packs_lists_list_recorded(void *captures, ChunksTraceEvent *result)
{
	result->tag = CHUNKS_TRACE_EVENT_LIST_RECORDED;
	result->u.list_recorded.pack = *(const uint64 *) captures;
}

/* chunks_packs_lists_vector_named is VectorNamed's event, of a pack's number and its vector's generation. */
static void
chunks_packs_lists_vector_named(void *captures, ChunksTraceEvent *result)
{
	const std_TupleU64U64_t *named = captures;

	result->tag = CHUNKS_TRACE_EVENT_VECTOR_NAMED;
	result->u.vector_named.pack = named->f0;
	result->u.vector_named.generation = named->f1;
}

/* chunks_packs_lists_lists_compacted is ListsCompacted's event. */
static void
chunks_packs_lists_lists_compacted(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_LISTS_COMPACTED;
}

/* chunks_packs_lists_append_table appends a record to the pack table's file and moves the snapshot's tail past it. */
static bool
chunks_packs_lists_append_table(chunks_packs_lists_frame_0_t *frame, const uint8 *record, size_t record_len)
{
	ChunksPacksListsChange *self = frame->self;
	ChunksPacksViewReadTable *table = &self->view->table;
	HostSharedLongIo io = host_shared_LongGuard_io(&self->long_);
	ChunksFileTornTail end;
	ChunksFileTornTail appended;

	chunks_packs_lists_path_into(frame, &self->view->dir, CHUNKS_PACKS_TABLE);
	end = chunks_packs_lists_end(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &table->read, &table->list, frame->path.bytes.ptr,
								 frame->path.bytes.len);
	if (!chunks_file_append(HOST_SHARED_LONG_IO_IO_DOORS(&io), &end, record, record_len, &appended, frame->error))
		return false;
	table->read.at = chunks_file_TornTail_end(&appended);
	chunks_file_TornTail_drop(&appended);
	if (!chunks_packs_Table_apply(&table->list, record, record_len, frame->error))
		return false;
	table->read.past = STD_ADD(u64, table->read.past, 1, 197, 9);
	self->snapshot.table = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &table->read, &table->list);
	return true;
}

/* chunks_packs_lists_append_made appends the frame's record to the pack table's file. */
static bool
chunks_packs_lists_append_made(chunks_packs_lists_frame_0_t *frame)
{
	return chunks_packs_lists_append_table(frame, frame->record.ptr, frame->record.len);
}

/* chunks_packs_lists_runs_append appends a record to a runs file a view has read and applies it there. */
static bool
chunks_packs_lists_runs_append(HostSharedLongIo *io, ChunksPacksViewReadRuns *runs, const StdPathBuf *path,
							   const uint8 *record, size_t record_len, ChunksErrorError *error)
{
	ChunksFileTornTail end = chunks_packs_lists_end(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &runs->read, &runs->list,
													path->bytes.ptr, path->bytes.len);
	ChunksFileTornTail appended;

	if (!chunks_file_append(HOST_SHARED_LONG_IO_IO_DOORS(io), &end, record, record_len, &appended, error))
		return false;
	runs->read.at = chunks_file_TornTail_end(&appended);
	chunks_file_TornTail_drop(&appended);
	if (!chunks_packs_Runs_apply(&runs->list, record, record_len, error))
		return false;
	runs->read.past = STD_ADD(u64, runs->read.past, 1, 167, 9);
	return true;
}

/*
 * chunks_packs_lists_append_add appends an added pack's records: its run to
 * its tablespace's runs file, written whole first when the tablespace has
 * none, and then the pack to the table's file, with where that runs file
 * then ends.
 */
static bool
chunks_packs_lists_append_add(chunks_packs_lists_frame_0_t *frame, ChunksPacksTableEntry entry)
{
	ChunksPacksListsChange *self = frame->self;
	HostSharedLongIo io = host_shared_LongGuard_io(&self->long_);
	ChunksPacksViewReadRuns *runs;
	ChunksPacksTail tail;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&frame->name, std_String_Display_fmt}, {&entry.f0, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	chunks_packs_lists_space_path_into(frame, entry.f1);
	runs = chunks_packs_view_runs_get(&self->view->runs, entry.f1);
	if (runs == NULL)
	{
		bool		placing = false;

		chunks_packs_Runs_Default_default(&frame->written.list);
		frame->written_live = true;
		if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, HOST_SHARED_LONG_IO_IO_DOORS(&io),
										frame->path.bytes.ptr, frame->path.bytes.len, &frame->written.list,
										&frame->written.read, &placing, frame->error))
			return false;
		runs = chunks_packs_view_runs_entry(&self->view->runs, entry.f1);
		chunks_packs_Runs_drop(&runs->list);
		*runs = frame->written;
		frame->written_live = false;
	}
	std_String_drop(&frame->name);
	frame->name = chunks_pack_PackId_file_name(&entry.f2, "idx", 3);
	std_String_drop(&frame->line);
	std_vec_new(&frame->line, 1, 1);
	std_fmt_format_Checked(&frame->line, &arguments);
	chunks_packs_Runs_added(frame->line.ptr, frame->line.len, chunks_packs_lists_record_into(frame));
	if (!chunks_packs_lists_runs_append(&io, runs, &frame->path, frame->record.ptr, frame->record.len, frame->error))
		return false;
	tail.end = runs->read.at;
	tail.generation = runs->list.generation;
	chunks_packs_Table_added(&entry, tail, chunks_packs_lists_record_into(frame));
	if (!chunks_packs_lists_append_made(frame))
		return false;
	chunks_trace_emit(chunks_packs_shared_load(self->record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX),
					  chunks_packs_lists_list_recorded, &entry.f0, NULL);
	return true;
}

/* chunks_packs_lists_flush appends the records of every deferred add, oldest first. */
static bool
chunks_packs_lists_flush(chunks_packs_lists_frame_0_t *frame)
{
	ChunksPacksListsChange *self = frame->self;

	while (self->snapshot.deferred.len != 0)
	{
		ChunksPacksTableEntry entry = STD_VEC_AT(&self->snapshot.deferred, ChunksPacksTableEntry, 0);
		ChunksPacksTableEntry removed;

		if (!chunks_packs_lists_append_add(frame, entry))
			return false;
		std_vec_remove(&self->snapshot.deferred, 0, &removed, sizeof(removed), _Alignof(ChunksPacksTableEntry),
					   RUST_FILE, 147, 36);
	}
	return true;
}

/* chunks_packs_lists_Change_flush_body_0 is flush's body. */
static void
chunks_packs_lists_Change_flush_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;

	frame->ok = chunks_packs_lists_flush(frame);
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_flush appends the records of every deferred add,
 * oldest first, so the list files hold every pack listed.
 */
bool
chunks_packs_lists_Change_flush(ChunksPacksListsChange *self, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_flush_body_0);
}

/*
 * chunks_packs_lists_Change_add lists a pack at a number no pack has, left
 * out of the list files while deferred and the deferred adds fit, and
 * otherwise appended at once, after every deferred one.
 */
bool
chunks_packs_lists_Change_add(ChunksPacksListsChange *self, const ChunksPacksTableEntry *entry, bool defer,
							  ChunksErrorError *error)
{
	bool		used = chunks_packs_table_packs_get(&self->view->table.list.packs, entry->f0) != NULL;

	for (size_t k = 0, k_len = self->snapshot.deferred.len; k < k_len && !used; k++)
		used = STD_VEC_AT(&self->snapshot.deferred, ChunksPacksTableEntry, k).f0 == entry->f0;
	if (used)
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack number "), STD_FMT_ARG(0), STD_FMT_LIT(" is already in use")};
		StdFmtArg	args[] = {{&entry->f0, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	std_vec_push_Checked(&self->snapshot.deferred, entry, sizeof(*entry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_lists_Change_note(self, chunks_packs_lists_pack_added, (void *) &entry->f0);
	if (defer && self->snapshot.deferred.len <= CHUNKS_PACKS_SHARED_DEFERRED)
		return true;
	return chunks_packs_lists_Change_flush(self, error);
}

/* chunks_packs_lists_Change_change_runs_body_0 is change_runs' body: the record appended, then the table told. */
static void
chunks_packs_lists_Change_change_runs_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	HostSharedLongIo io;
	ChunksPacksViewReadRuns *runs;
	ChunksPacksTail tail;

	frame->ok = false;
	if (!chunks_packs_lists_flush(frame))
		goto done;
	chunks_packs_lists_space_path_into(frame, frame->space);
	runs = chunks_packs_view_runs_get(&self->view->runs, frame->space);
	if (runs == NULL)
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("tablespace "), STD_FMT_ARG(0), STD_FMT_LIT(" has no runs file")};
		StdFmtArg	args[] = {{&frame->space, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		goto done;
	}
	io = host_shared_LongGuard_io(&self->long_);
	if (!chunks_packs_lists_runs_append(&io, runs, &frame->path, frame->record_in, frame->record_in_len, frame->error))
		goto done;
	tail.end = runs->read.at;
	tail.generation = runs->list.generation;
	chunks_packs_Table_moved(frame->space, tail, chunks_packs_lists_record_into(frame));
	frame->ok = chunks_packs_lists_append_made(frame);
done:
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_change_runs appends a record to a tablespace's
 * runs file, after every deferred add's, and then tells the pack table's
 * file where that runs file now ends.
 */
bool
chunks_packs_lists_Change_change_runs(ChunksPacksListsChange *self, uint32 space, const uint8 *record,
									  size_t record_len, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .space = space, .record_in = record,
	.record_in_len = record_len, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_change_runs_body_0);
}

/* chunks_packs_lists_Change_change_forwarding_body_0 is change_forwarding's body. */
static void
chunks_packs_lists_Change_change_forwarding_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	HostSharedLongIo io = host_shared_LongGuard_io(&self->long_);
	ChunksPacksViewReadRuns *forwarding = &self->view->forwarding;

	frame->ok = false;
	chunks_packs_lists_path_into(frame, &self->view->dir, CHUNKS_PACKS_FORWARDING);
	if (chunks_packs_lists_runs_append(&io, forwarding, &frame->path, frame->record_in, frame->record_in_len,
									   frame->error))
	{
		self->snapshot.forwarding = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &forwarding->read,
															   &forwarding->list);
		frame->ok = true;
	}
	chunks_packs_lists_pad_0(frame);
}

/* chunks_packs_lists_Change_change_forwarding appends a record to the forwarding file and moves the snapshot's tail past it. */
bool
chunks_packs_lists_Change_change_forwarding(ChunksPacksListsChange *self, const uint8 *record, size_t record_len,
											ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .record_in = record, .record_in_len = record_len,
	.error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_change_forwarding_body_0);
}

/*
 * chunks_packs_lists_Change_lists_together reports whether a tablespace's
 * runs file lists runs of these names next to each other, in this order.
 */
bool
chunks_packs_lists_Change_lists_together(const ChunksPacksListsChange *self, uint32 space, const StdString *names,
										 size_t names_len)
{
	const ChunksPacksViewReadRuns *runs = chunks_packs_view_runs_get(&self->view->runs, space);
	size_t		lines_len = runs == NULL ? 0 : runs->list.lines.len;

	if (names_len == 0)
		std_panic(RUST_FILE, 241, 15, "window size must be non-zero", strlen("window size must be non-zero"));
	for (size_t at = 0; names_len <= lines_len && at <= lines_len - names_len; at++)
	{
		bool		together = true;

		for (size_t k = 0; k < names_len && together; k++)
		{
			const StdString *line = &STD_VEC_AT(&runs->list.lines, StdString, at + k);
			StdStr		run = chunks_packs_run_name(line->ptr, line->len);

			together = run.len == names[k].len && memcmp(run.ptr, names[k].ptr, run.len) == 0;
		}
		if (together)
			return true;
	}
	return false;
}

/* chunks_packs_lists_Change_name_vector_body_0 is name_vector's body: the deferred adds, then the vector's record. */
static void
chunks_packs_lists_Change_name_vector_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	std_TupleU64U64_t named = {frame->number, frame->generation};

	frame->ok = false;
	chunks_packs_lists_Change_note(frame->self, chunks_packs_lists_vector_named, &named);
	if (chunks_packs_lists_flush(frame))
	{
		chunks_packs_Table_vector(frame->number, frame->generation, chunks_packs_lists_record_into(frame));
		frame->ok = chunks_packs_lists_append_made(frame);
	}
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_name_vector names the generation of a listed
 * pack's deletion vector, whose file is durable, in the table, after every
 * deferred add's records.
 */
bool
chunks_packs_lists_Change_name_vector(ChunksPacksListsChange *self, uint64 number, uint64 generation,
									  ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .number = number, .generation = generation, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_name_vector_body_0);
}

/* chunks_packs_lists_Change_reserve_body_0 is reserve's body: the raised bound appended, then given in the record. */
static void
chunks_packs_lists_Change_reserve_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	uint64		numbers = chunks_packs_shared_load(self->record, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	uint64		raised;

	frame->ok = false;
	if (chunks_packs_shared_bound(numbers) != chunks_packs_shared_bound(frame->number))
	{
		frame->ok = true;
		goto done;
	}
	if (chunks_packs_shared_bound(numbers) == CHUNKS_PACKS_SHARED_LAST)
	{
		uint64		database = chunks_packs_shared_load(self->record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX);
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("database with OID "), STD_FMT_ARG(0),
		STD_FMT_LIT(" has used every pack number")};
		StdFmtArg	args[] = {{&database, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_fmt_format_Checked(&message, &arguments);
		frame->error->tag = CHUNKS_ERROR_ERROR_HOST;
		frame->error->u.host = host_Error_fork((const uint8 *) "54000", &message);
		goto done;
	}
	raised = std_min_u64(STD_ADD(u64, chunks_packs_shared_bound(numbers), CHUNKS_PACKS_SHARED_AHEAD, 236, 22),
						 CHUNKS_PACKS_SHARED_LAST);
	chunks_packs_Table_reserved(raised, chunks_packs_lists_record_into(frame));
	if (!chunks_packs_lists_append_made(frame))
		goto done;
	for (;;)
	{
		HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
		uint64		seen;

		exchange.u.exchange.f0 = numbers;
		exchange.u.exchange.f1 = chunks_packs_shared_reserved(raised, chunks_packs_shared_given(numbers));
		seen = host_shared_Record_atomic(self->record, CHUNKS_PACKS_SHARED_RESERVED_INDEX, exchange);
		if (seen == numbers)
			break;
		numbers = seen;
	}
	frame->ok = true;
done:
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_reserve raises the bound of the numbers pack
 * writers may be given by AHEAD when the record still holds the bound its
 * caller read, the table's file holding it durably first; a bound that can't
 * be raised is the repository's last number.
 */
bool
chunks_packs_lists_Change_reserve(ChunksPacksListsChange *self, uint64 read, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .number = read, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_reserve_body_0);
}

/* chunks_packs_lists_Change_unreserve_body_0 is unreserve's body: the record's bound set back, then the file's. */
static void
chunks_packs_lists_Change_unreserve_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	uint64		numbers = chunks_packs_shared_load(self->record, CHUNKS_PACKS_SHARED_RESERVED_INDEX);

	frame->ok = true;
	while (chunks_packs_shared_bound(numbers) != chunks_packs_shared_given(numbers))
	{
		uint64		given = chunks_packs_shared_given(numbers);
		HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
		uint64		seen;

		exchange.u.exchange.f0 = numbers;
		exchange.u.exchange.f1 = chunks_packs_shared_reserved(given, given);
		seen = host_shared_Record_atomic(self->record, CHUNKS_PACKS_SHARED_RESERVED_INDEX, exchange);
		if (seen == numbers)
		{
			chunks_packs_Table_reserved(given, chunks_packs_lists_record_into(frame));
			frame->ok = chunks_packs_lists_append_made(frame);
			break;
		}
		numbers = seen;
	}
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_unreserve sets the bound back to the highest
 * number given, in the record first and then in the table's file, so that a
 * record made again from the file starts right after it.
 */
bool
chunks_packs_lists_Change_unreserve(ChunksPacksListsChange *self, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_unreserve_body_0);
}

/* chunks_packs_lists_Change_retire_body_0 is retire's body: the deferred adds, then the retirement's record. */
static void
chunks_packs_lists_Change_retire_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;

	frame->ok = false;
	if (chunks_packs_lists_flush(frame))
	{
		chunks_packs_Table_retired(frame->numbers, frame->numbers_len, chunks_packs_lists_record_into(frame));
		frame->ok = chunks_packs_lists_append_made(frame);
	}
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_retire takes packs out of the table, after every
 * deferred add's records, so the file lists each pack before its retirement.
 */
bool
chunks_packs_lists_Change_retire(ChunksPacksListsChange *self, const uint64 *numbers, size_t numbers_len,
								 ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .numbers = numbers, .numbers_len = numbers_len,
	.error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_retire_body_0);
}

#ifdef DOLTGRES_CHUNKS_TRACE
/* chunks_packs_lists_given is a noted event's emit: the event the change built, moved out. */
static void
chunks_packs_lists_given(void *captures, ChunksTraceEvent *result)
{
	*result = *(ChunksTraceEvent *) captures;
}
#endif

/*
 * chunks_packs_lists_Change_install_body_0 is install's body: under the state
 * lock the snapshot written and the generation advanced, then what the change
 * added opened in the changer's view.
 */
static void
chunks_packs_lists_Change_install_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	HostSharedLongIo io = host_shared_LongGuard_io(&self->long_);
	HostError	host;
	uint64		generation;

	frame->ok = false;
	chunks_packs_shared_Snapshot_bytes(&self->snapshot, chunks_packs_lists_record_into(frame));
	if (!host_shared_LongIo_lock(&io, CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX, &frame->state, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->state_live = true;
	if (!host_shared_GuardExclusive_write_blob(&frame->state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB, frame->record.ptr,
											   frame->record.len, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	generation = STD_ADD(u64, chunks_packs_shared_add(self->record, CHUNKS_PACKS_SHARED_GENERATION_INDEX, 1), 1,
						 285, 13);
	frame->state_live = false;
	host_shared_Guard_drop(&frame->state);
	if (self->rotating != CHUNKS_PACKS_LISTS_ROTATING_NO)
	{
		self->rotating = CHUNKS_PACKS_LISTS_ROTATING_NO;
		chunks_packs_shared_add(self->record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 1);
	}
#ifdef DOLTGRES_CHUNKS_TRACE
	{
		uint64		repository = chunks_packs_shared_load(self->record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX);

		for (size_t k = 0, k_len = self->noted.len; k < k_len; k++)
			chunks_trace_emit(repository, chunks_packs_lists_given, &STD_VEC_AT(&self->noted, ChunksTraceEvent, k),
							  NULL);
		self->noted.len = 0;
	}
#endif
	if (!chunks_packs_view_View_open(self->view, HOST_IO_IO_DOORS_READ_DOORS(HOST_SHARED_LONG_IO_IO_DOORS(&io)),
									 self->snapshot.deferred.ptr, self->snapshot.deferred.len, frame->error))
		goto done;
	std_vec_free_Checked(&self->view->deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	self->view->deferred = self->snapshot.deferred;
	std_vec_new(&self->snapshot.deferred, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	self->view->has_current = true;
	self->view->current = generation;
	self->installed = true;
	frame->ok = true;
done:
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Change_install makes the change visible: under the
 * state lock it writes the snapshot and advances the record's generation,
 * then opens what the change added in the changer's own view. It takes the
 * change.
 */
bool
chunks_packs_lists_Change_install(ChunksPacksListsChange *self, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .error = error, .change_live = true};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Change_install_body_0);
}

/*
 * chunks_packs_lists_Change_drop is Change's drop: a change dropped before it
 * is installed makes its view forget what it read, and one that made the
 * count of rotations odd without renaming makes it even again; then the
 * lists lock goes.
 */
void
chunks_packs_lists_Change_drop(ChunksPacksListsChange *self)
{
	if (!self->installed)
		chunks_packs_view_View_forget(self->view);
	if (self->rotating == CHUNKS_PACKS_LISTS_ROTATING_ODD)
		chunks_packs_shared_add(self->record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 1);
	host_shared_LongGuard_drop(&self->long_);
	chunks_packs_shared_Snapshot_drop(&self->snapshot);
#ifdef DOLTGRES_CHUNKS_TRACE
	std_vec_drop_Checked(&self->noted, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent),
						 (void (*) (void *)) chunks_trace_Event_drop);
#endif
}

/*
 * chunks_packs_lists_Rewrite_begin takes the lists lock under the list
 * files' flag, brings the changer's view up to date, and makes the count of
 * rotations odd.
 */
bool
chunks_packs_lists_Rewrite_begin(ChunksPacksViewView *view, HostSharedRecord record, HostSharedFlagIo *io,
								 ChunksPacksListsChange *result, ChunksErrorError *error)
{
	HostSharedLongGuard long_;
	HostError	host;

	if (!host_shared_FlagIo_lock_long(io, CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, &long_, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	if (!chunks_packs_lists_Change_begin(view, record, &long_, result, error))
		return false;
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX, 1);
	result->rotating = CHUNKS_PACKS_LISTS_ROTATING_ODD;
	return true;
}

/*
 * chunks_packs_lists_Rewrite_whole writes a list file whole from the list
 * moved out of a read, marking the change as one that renamed once the
 * file's renames began. A write that fails leaves the read with an empty
 * list where it was, as the move left it.
 */
static bool
chunks_packs_lists_Rewrite_whole(chunks_packs_lists_frame_0_t *frame, const ChunksPacksViewListOps *ops, void *list,
								 ChunksPacksViewRead *read, void *taken, size_t list_size)
{
	HostSharedLongIo io = host_shared_LongGuard_io(&frame->self->long_);
	bool		placing = false;
	ChunksPacksViewRead written = {0};
	bool		ok;

	memcpy(taken, list, list_size);
	ops->init(list);
	frame->ops = ops;
	frame->taken = taken;
	frame->taken_live = true;
	ok = chunks_packs_lists_written(ops, HOST_SHARED_LONG_IO_IO_DOORS(&io), frame->path.bytes.ptr,
									frame->path.bytes.len, taken, &written, &placing, frame->error);
	if (placing)
		frame->self->rotating = CHUNKS_PACKS_LISTS_ROTATING_RENAMED;
	if (!ok)
		return false;
	frame->taken_live = false;
	ops->reset(list);
	memcpy(list, taken, list_size);
	*read = written;
	return true;
}

/*
 * chunks_packs_lists_Rewrite_compact_body_0 is compact's body: the runs
 * files due written whole first, their tails in the table's whole or its
 * records, then the table, then the forwarding file.
 */
static void
chunks_packs_lists_Rewrite_compact_body_0(void *frame_)
{
	chunks_packs_lists_frame_0_t *frame = frame_;
	ChunksPacksListsChange *self = frame->self;
	ChunksPacksViewView *view = self->view;
	bool		table_due;

	frame->ok = false;
	if (!chunks_packs_lists_flush(frame))
		goto done;
	table_due = chunks_packs_lists_due(&view->table.read, view->table.list.packs.len, frame->all);
	for (size_t k = 0, k_len = view->runs.len; k < k_len; k++)
	{
		const chunks_TupleU32ReadRuns_t *runs = &STD_VEC_AT(&view->runs, chunks_TupleU32ReadRuns_t, k);

		if (chunks_packs_lists_due(&runs->f1.read, runs->f1.list.lines.len, frame->all))
			std_vec_push_Checked(&frame->spaces, &runs->f0, sizeof(uint32), _Alignof(uint32));
	}
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
	{
		uint32		space = STD_VEC_AT(&frame->spaces, uint32, k);
		ChunksPacksViewReadRuns *runs;
		ChunksPacksTail moved;

		chunks_packs_lists_space_path_into(frame, space);
		runs = chunks_packs_view_runs_get(&view->runs, space);
		if (runs == NULL)
			continue;
		if (!chunks_packs_lists_Rewrite_whole(frame, &CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &runs->list, &runs->read,
											  &frame->taken_runs, sizeof(frame->taken_runs)))
			goto done;
		moved.end = runs->read.at;
		moved.generation = runs->list.generation;
		if (table_due)
			chunks_packs_table_runs_insert(&view->table.list.runs, space, moved);
		else
		{
			chunks_packs_Table_moved(space, moved, chunks_packs_lists_record_into(frame));
			if (!chunks_packs_lists_append_made(frame))
				goto done;
		}
	}
	if (table_due)
	{
		chunks_packs_lists_path_into(frame, &view->dir, CHUNKS_PACKS_TABLE);
		if (!chunks_packs_lists_Rewrite_whole(frame, &CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &view->table.list,
											  &view->table.read, &frame->taken_table, sizeof(frame->taken_table)))
			goto done;
		self->snapshot.table = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &view->table.read,
														  &view->table.list);
	}
	if (chunks_packs_lists_due(&view->forwarding.read, view->forwarding.list.lines.len, frame->all))
	{
		chunks_packs_lists_path_into(frame, &view->dir, CHUNKS_PACKS_FORWARDING);
		if (!chunks_packs_lists_Rewrite_whole(frame, &CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &view->forwarding.list,
											  &view->forwarding.read, &frame->taken_runs,
											  sizeof(frame->taken_runs)))
			goto done;
		self->snapshot.forwarding = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS,
															   &view->forwarding.read, &view->forwarding.list);
	}
	if (frame->all)
		chunks_packs_lists_Change_note(self, chunks_packs_lists_lists_compacted, NULL);
	frame->ok = true;
done:
	chunks_packs_lists_pad_0(frame);
}

/*
 * chunks_packs_lists_Rewrite_compact appends every deferred add's records
 * and writes whole each list file past its slack, or any record past its
 * whole when all is set: the runs files first, whose new tails the table's
 * whole record carries, or a record of the table's each when the table isn't
 * written whole.
 */
bool
chunks_packs_lists_Rewrite_compact(ChunksPacksListsChange *self, bool all, ChunksErrorError *error)
{
	chunks_packs_lists_frame_0_t frame = {.self = self, .all = all, .error = error};

	return chunks_packs_lists_run(&frame, chunks_packs_lists_Rewrite_compact_body_0);
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_lists_tests_TupleU64U64_Debug_fmt is a (u64, u64)'s Debug, in a tuple. */
static bool
chunks_packs_lists_tests_TupleU64U64_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU64U64_t *pair = value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "", 0);

	std_DebugTuple_field(&b, &pair->f0, std_u64_Debug_fmt);
	std_DebugTuple_field(&b, &pair->f1, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* chunks_packs_lists_tests_Paths_Debug_fmt is a Vec<&Path>'s Debug, of each path's in order. */
static bool
chunks_packs_lists_tests_Paths_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *paths = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = paths->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(paths, StdStr, k), std_path_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * chunks_packs_lists_tests_repository makes a pack record over list files
 * written whole in a directory, listing no pack.
 */
HostSharedOwned
chunks_packs_lists_tests_repository(HostIoIo *io, const StdPathBuf *dir)
{
	HostSharedOwned owned = chunks_packs_tests_made(RUST_FILE, io, 420, 43);
	HostSharedRecord record = host_shared_Owned_record(&owned);
	ChunksPacksFixturesLists lists;
	StdVec		deferred = chunks_packs_tests_no_deferred();

	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, CHUNKS_PACKS_FIXTURES_REPOSITORY);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_OWN_INDEX, HOST_PG_DEFAULT);
	lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), dir, 7, NULL, 0);
	chunks_packs_tests_installed(RUST_FILE, io, record, &lists, &deferred, 424, 54);
	chunks_packs_fixtures_Lists_drop(&lists);
	return owned;
}

/* chunks_packs_lists_tests_begun begins a change of a view under the record's lists lock, which unwraps at a line and columns. */
static void
chunks_packs_lists_tests_begun(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
							   ChunksPacksListsChange *result, uint32 line, uint32 lock_col, uint32 begin_col)
{
	HostSharedLongGuard long_;
	HostError	host;
	ChunksErrorError error;

	if (!host_shared_Record_lock_long(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, &long_,
									  &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, line, lock_col);
	if (!chunks_packs_lists_Change_begin(view, record, &long_, result, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, begin_col);
}

/* chunks_packs_lists_tests_changed runs a change of a view under the record's lists lock and installs it. */
void
chunks_packs_lists_tests_changed(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
								 void (*body) (ChunksPacksListsChange *change, void *captures), void *captures)
{
	ChunksPacksListsChange change;
	ChunksErrorError error;

	chunks_packs_lists_tests_begun(io, record, view, &change, 435, 88, 98);
	body(&change, captures);
	if (!chunks_packs_lists_Change_install(&change, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 437, 26);
}

/*
 * chunks_packs_lists_tests_compacted writes whole the list files of a view's
 * repository that are due, or all of them, under the list files' flag.
 */
static void
chunks_packs_lists_tests_compacted(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view, bool all)
{
	HostSharedFlagGuard rewriting;
	HostSharedFlagIo flag_io;
	ChunksPacksListsChange rewrite;
	HostError	host;
	ChunksErrorError error;

	if (!host_shared_Record_flag(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_REWRITING_INDEX, &rewriting,
								 &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 443, 58);
	flag_io = host_shared_FlagGuard_io(&rewriting);
	if (!chunks_packs_lists_Rewrite_begin(view, record, &flag_io, &rewrite, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 445, 65);
	if (!chunks_packs_lists_Rewrite_compact(&rewrite, all, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 446, 30);
	if (!chunks_packs_lists_Change_install(&rewrite, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 447, 27);
	host_shared_FlagGuard_drop(&rewriting);
}

/* chunks_packs_lists_tests_size returns a file's length, in a directory by its name. */
static uint64
chunks_packs_lists_tests_size(const StdPathBuf *dir, const char *name)
{
	StdPathBuf	path;
	StdFsMetadata metadata;
	StdIoError	error;
	uint64		len;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	if (!std_fs_metadata(path.bytes.ptr, path.bytes.len, &metadata, &error))
		STD_RESULT_EXPECT_FAILED(452, 33, CHUNKS_PACKS_FIXTURES_UNWRAP, &error, std_io_Error_Debug_fmt);
	len = std_fs_Metadata_len(&metadata);
	std_fs_Metadata_drop(&metadata);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return len;
}

/*
 * ChunksPacksListsTestsAdds is a change's adds, each with whether it is
 * deferred, and the line and column each unwraps at.
 */
typedef struct ChunksPacksListsTestsAdds
{
	const ChunksPacksTableEntry *entries;
	const bool *defers;
	size_t		len;
	const uint32 *lines;
	const uint32 *cols;
} ChunksPacksListsTestsAdds;

/* chunks_packs_lists_tests_added is a change's body that adds its packs, each unwrapping where it says. */
static void
chunks_packs_lists_tests_added(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksListsTestsAdds *adds = captures;
	ChunksErrorError error;

	for (size_t k = 0; k < adds->len; k++)
		if (!chunks_packs_lists_Change_add(change, &adds->entries[k], adds->defers[k], &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, adds->lines[k], adds->cols[k]);
}

/* chunks_packs_lists_tests_one changes a view with one add of a pack, which unwraps at a line and column. */
static void
chunks_packs_lists_tests_one(HostIoIo *io, HostSharedRecord record, ChunksPacksViewView *view,
							 const ChunksPacksTableEntry *entry, bool defer, uint32 line, uint32 col)
{
	ChunksPacksListsTestsAdds adds = {entry, &defer, 1, &line, &col};

	chunks_packs_lists_tests_changed(io, record, view, chunks_packs_lists_tests_added, &adds);
}

/* chunks_packs_lists_tests_entries returns the table's entries of packs of one chunk numbered 1 up to `last`, written in a directory. */
static StdVec
chunks_packs_lists_tests_entries(HostIoIo *io, const StdPathBuf *dir, uint64 last)
{
	StdVec		packs;

	std_vec_new(&packs, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	for (uint64 number = 1; number <= last; number++)
	{
		ChunksPacksFixturesPacked packed = chunks_packs_fixtures_pack(io, dir, number);

		std_vec_push_Checked(&packs, &packed.entry, sizeof(packed.entry), _Alignof(ChunksPacksTableEntry));
	}
	return packs;
}

/* chunks_packs_lists_tests_entry returns a pack's entry by its place in a list of entries, checked at a line and column. */
static const ChunksPacksTableEntry *
chunks_packs_lists_tests_entry(const StdVec *packs, size_t place, uint32 line, uint32 col)
{
	std_index_check(place, packs->len, RUST_FILE, line, col);
	return &STD_VEC_AT(packs, ChunksPacksTableEntry, place);
}

/* chunks_packs_lists_tests_forwards writes a run of forwards for moves in a directory by its name, which unwraps at a line and column. */
static void
chunks_packs_lists_tests_forwards(HostIoIo *io, const StdPathBuf *dir, const char *name,
								  const chunks_TupleLocatorLocator_t *moves, size_t moves_len, uint32 line, uint32 col)
{
	StdPathBuf	path;
	StdVec		entries;
	ChunksIndexRunLocator run;
	ChunksErrorError error;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	chunks_packs_forward_entries(moves, moves_len, &entries);
	if (!chunks_index_RunLocator_write(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, entries.ptr,
									   entries.len, &run, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	chunks_index_RunLocator_drop(&run);
	std_vec_free_Checked(&entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	std_vec_free_Checked(&path.bytes, 1, 1);
}

/* chunks_packs_lists_tests_forwarding changes a change's forwarding file with a record that adds a run by its line, which unwraps at a line and column. */
static void
chunks_packs_lists_tests_forwarding(ChunksPacksListsChange *change, const char *line_text, uint32 line, uint32 col)
{
	StdVec		record;
	ChunksErrorError error;

	chunks_packs_Runs_added(line_text, strlen(line_text), &record);
	if (!chunks_packs_lists_Change_change_forwarding(change, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	std_vec_free_Checked(&record, 1, 1);
}

/* chunks_packs_lists_tests_retired retires packs in a change, which unwraps at a line and column. */
static void
chunks_packs_lists_tests_retired(ChunksPacksListsChange *change, const uint64 *numbers, size_t numbers_len,
								 uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_lists_Change_retire(change, numbers, numbers_len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_lists_tests_runs_removed changes pg_default's runs file with a record that removes runs by their names, which unwraps at a line and column. */
static void
chunks_packs_lists_tests_runs_removed(ChunksPacksListsChange *change, const StdString *names, size_t names_len,
									  uint32 line, uint32 col)
{
	StdVec		record;
	ChunksErrorError error;

	chunks_packs_Runs_removed(names, names_len, &record);
	if (!chunks_packs_lists_Change_change_runs(change, 0, record.ptr, record.len, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	std_vec_free_Checked(&record, 1, 1);
}

/* chunks_packs_lists_tests_eq_u64 asserts at a line and column that two u64s are equal, with a literal message or none. */
static void
chunks_packs_lists_tests_eq_u64(uint64 left, uint64 right, const char *message, uint32 line, uint32 col)
{
	if (left != right)
	{
		StdFmtPiece pieces[] = {{message, message == NULL ? 0 : strlen(message)}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), NULL};

		std_assert_failed(RUST_FILE, line, col, "==", &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt,
						  message == NULL ? NULL : &arguments);
	}
}

/* chunks_packs_lists_tests_ne_u64 asserts at a line and column that two u64s differ, with a message or none. */
static void
chunks_packs_lists_tests_ne_u64(uint64 left, uint64 right, const StdFmtArguments *message, uint32 line, uint32 col)
{
	if (left == right)
		std_assert_failed(RUST_FILE, line, col, "!=", &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt, message);
}

/* chunks_packs_lists_tests_runs_len returns how many runs a view's index of a tablespace holds, which unwraps at a line and column. */
static uint64
chunks_packs_lists_tests_runs_len(const ChunksPacksViewView *view, uint32 space, uint32 line, uint32 col)
{
	size_t		runs_len;

	(void) chunks_packs_tests_runs(RUST_FILE, view, space, line, col, &runs_len);
	return runs_len;
}

/* chunks_packs_lists_tests_listed_len returns how many packs a view lists. */
static uint64
chunks_packs_lists_tests_listed_len(const ChunksPacksViewView *view)
{
	StdVec		listed;
	uint64		len;

	chunks_packs_view_View_listed(view, &listed);
	len = listed.len;
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	return len;
}

/* chunks_packs_lists_tests_followed follows a place through a view's forwards, which unwraps at a line and column. */
static ChunksTrailerLocator
chunks_packs_lists_tests_followed(HostIoIo *io, const ChunksPacksViewView *view, ChunksTrailerLocator copy,
								  uint32 line, uint32 col)
{
	ChunksTrailerLocator followed;
	ChunksErrorError error;

	if (!chunks_packs_followed(HOST_IO_IO_READ_DOORS(io), chunks_packs_view_View_forwards(view),
							   chunks_packs_view_View_table(view), copy, &followed, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return followed;
}

/* CHUNKS_PACKS_LISTS_TESTS_GOT is the columns of a get in a loop over views: the assertion's, then its two unwraps'. */
static const uint32 CHUNKS_PACKS_LISTS_TESTS_GOT[] = {13, 38, 61};

/*
 * test_chunks_packs_lists_tests_a_pack_added_is_listed_by_every_view: a pack
 * added at once is in the list files when its change ends: the changer's view
 * lists it and reads its chunk, and so does another backend's view at its
 * next refresh, which also finds a number already in use refused.
 */
void
test_chunks_packs_lists_tests_a_pack_added_is_listed_by_every_view(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView *views[2] = {&mine, &other};
	ChunksPacksListsChange change;
	ChunksErrorError error;
	bool		corrupt;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	chunks_packs_tests_refreshed(RUST_FILE, &other, io, record, 467, 35);
	chunks_packs_lists_tests_one(io, record, &mine, &first.entry, false, 468, 74);
	chunks_packs_lists_tests_one(io, record, &mine, &second.entry, false, 469, 75);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};

		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 471, 38);
		chunks_packs_tests_listed(RUST_FILE, views[k], both, lengthof(both), 472, 13);
		chunks_packs_lists_tests_eq_u64(chunks_packs_view_View_table(views[k])->added, 2, NULL, 473, 13);
		chunks_packs_tests_got(RUST_FILE, views[k], io, 0, &first.address, &first.at, 474,
							   CHUNKS_PACKS_LISTS_TESTS_GOT);
		chunks_packs_tests_got(RUST_FILE, views[k], io, 0, &second.address, &second.at, 475,
							   CHUNKS_PACKS_LISTS_TESTS_GOT);
	}
	chunks_packs_lists_tests_begun(io, record, &other, &change, 477, 94, 104);
	corrupt = !chunks_packs_lists_Change_add(&change, &first.entry, false, &error) &&
		error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
	if (!corrupt)
		STD_PANIC(478, 9, "assertion failed: matches!(change.add(first, false), Err(Error::Corrupt(_)))");
	chunks_error_Error_drop(&error);
	chunks_packs_lists_Change_drop(&change);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_lists_tests_deferred_adds_are_appended_by_the_add_that_passes_their_bound:
 * deferred adds stay out of the list files, in the snapshot, where every
 * view lists them, until the add that passes DEFERRED appends them all; the
 * files then hold each, in the order they were added.
 */
void
test_chunks_packs_lists_tests_deferred_adds_are_appended_by_the_add_that_passes_their_bound(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	StdVec		packs;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView fresh;
	ChunksPacksViewView *views[3] = {&mine, &other, &fresh};
	std_TupleU64U64_t sizes;
	std_TupleU64U64_t now;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	packs = chunks_packs_lists_tests_entries(io, &dir, CHUNKS_PACKS_SHARED_DEFERRED + 1);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	sizes.f0 = chunks_packs_lists_tests_size(&dir, CHUNKS_PACKS_TABLE);
	sizes.f1 = chunks_packs_lists_tests_size(&dir, CHUNKS_PACKS_RUNS);
	std_range_to_check(CHUNKS_PACKS_SHARED_DEFERRED, packs.len, RUST_FILE, 493, 24);
	for (size_t k = 0; k < CHUNKS_PACKS_SHARED_DEFERRED; k++)
		chunks_packs_lists_tests_one(io, record, &mine, &STD_VEC_AT(&packs, ChunksPacksTableEntry, k), true, 494, 77);
	now.f0 = chunks_packs_lists_tests_size(&dir, CHUNKS_PACKS_TABLE);
	now.f1 = chunks_packs_lists_tests_size(&dir, CHUNKS_PACKS_RUNS);
	STD_ASSERT_EQ_MSG(496, 9, now.f0 == sizes.f0 && now.f1 == sizes.f1, &now, chunks_packs_lists_tests_TupleU64U64_Debug_fmt, &sizes,
					  chunks_packs_lists_tests_TupleU64U64_Debug_fmt, "a deferred add was appended");
	chunks_packs_tests_refreshed(RUST_FILE, &other, io, record, 497, 35);
	{
		size_t		listed = chunks_packs_lists_tests_listed_len(&other);
		size_t		deferred = CHUNKS_PACKS_SHARED_DEFERRED;

		STD_ASSERT_EQ(498, 9, listed == deferred, &listed, std_usize_Debug_fmt, &deferred, std_usize_Debug_fmt);
	}
	STD_ASSERT(499, 9, chunks_packs_view_View_table(&other)->packs.len == 0 &&
			   chunks_packs_view_View_pack(&other, CHUNKS_PACKS_SHARED_DEFERRED) != NULL,
			   "other.table().packs.is_empty() && other.pack(DEFERRED as u64).is_some()");

	chunks_packs_lists_tests_one(io, record, &mine,
								 chunks_packs_lists_tests_entry(&packs, CHUNKS_PACKS_SHARED_DEFERRED, 501, 60), true,
								 501, 83);
	chunks_packs_fixtures_view(&fresh);
	for (size_t v = 0; v < lengthof(views); v++)
	{
		size_t		runs_len;
		StdArcInner *const *runs;
		size_t		held;
		size_t		all = CHUNKS_PACKS_SHARED_DEFERRED + 1;
		StdVec		names;
		StdVec		written;
		StdVec		paths;
		bool		same;

		chunks_packs_tests_refreshed(RUST_FILE, views[v], io, record, 504, 38);
		chunks_packs_tests_listed(RUST_FILE, views[v], packs.ptr, packs.len, 507, 13);
		held = chunks_packs_view_View_table(views[v])->packs.len;
		STD_ASSERT_EQ_MSG(508, 13, held == all, &held, std_usize_Debug_fmt, &all, std_usize_Debug_fmt,
						  "the files don't hold every pack");
		runs = chunks_packs_tests_runs(RUST_FILE, views[v], 0, 509, 51, &runs_len);
		std_vec_new(&names, sizeof(StdStr), _Alignof(StdStr));
		for (size_t k = 0; k < runs_len; k++)
		{
			const ChunksIndexRunLocator *run = STD_ARC_VALUE(runs[k], _Alignof(ChunksIndexRunLocator));
			StdStr		name;

			name.ptr = chunks_index_RunLocator_path(run, &name.len);
			std_vec_push_Checked(&names, &name, sizeof(name), _Alignof(StdStr));
		}
		std_vec_new(&paths, sizeof(StdPathBuf), _Alignof(StdPathBuf));
		std_vec_new(&written, sizeof(StdStr), _Alignof(StdStr));
		for (size_t k = 0; k < packs.len; k++)
		{
			StdString	file = chunks_pack_PackId_file_name(&STD_VEC_AT(&packs, ChunksPacksTableEntry, k).f2, "idx",
															3);
			StdPathBuf	path;

			std_path_join(dir.bytes.ptr, dir.bytes.len, file.ptr, file.len, &path);
			std_vec_free_Checked(&file, 1, 1);
			std_vec_push_Checked(&paths, &path, sizeof(path), _Alignof(StdPathBuf));
		}
		for (size_t k = 0; k < paths.len; k++)
		{
			StdStr		path = {STD_VEC_AT(&paths, StdPathBuf, k).bytes.ptr,
			STD_VEC_AT(&paths, StdPathBuf, k).bytes.len};

			std_vec_push_Checked(&written, &path, sizeof(path), _Alignof(StdStr));
		}
		same = names.len == written.len;
		for (size_t k = 0; same && k < names.len; k++)
			same = std_path_PartialEq_eq(STD_VEC_AT(&names, StdStr, k).ptr, STD_VEC_AT(&names, StdStr, k).len,
										 STD_VEC_AT(&written, StdStr, k).ptr, STD_VEC_AT(&written, StdStr, k).len);
		STD_ASSERT_EQ(511, 13, same, &names, chunks_packs_lists_tests_Paths_Debug_fmt, &written,
					  chunks_packs_lists_tests_Paths_Debug_fmt);
		std_vec_free_Checked(&written, sizeof(StdStr), _Alignof(StdStr));
		std_vec_drop_Checked(&paths, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
		std_vec_free_Checked(&names, sizeof(StdStr), _Alignof(StdStr));
	}
	chunks_packs_view_View_drop(&fresh);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	std_vec_free_Checked(&packs, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_lists_tests_the_first_pack_of_a_tablespace_writes_its_runs_file:
 * the first pack added in a tablespace writes that tablespace's runs file
 * whole before its record, and a view reads the file through the tail the
 * table's record of the pack carries.
 */
void
test_chunks_packs_lists_tests_the_first_pack_of_a_tablespace_writes_its_runs_file(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	space;
	StdPathBuf	runs;
	StdIoError	io_error;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked packed;
	ChunksPacksTableEntry spaced;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView *views[2] = {&mine, &other};
	static const uint32 got_cols[] = {13, 38, 65};

	chunks_packs_fixtures_home(&dir);
	space = host_tablespace_dir(CHUNKS_PACKS_FIXTURES_REPOSITORY, 5);
	if (!std_fs_create_dir_all(space.bytes.ptr, space.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(522, 40, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	packed = chunks_packs_fixtures_pack(io, &space, 1);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	std_path_join(space.bytes.ptr, space.bytes.len, CHUNKS_PACKS_RUNS, strlen(CHUNKS_PACKS_RUNS), &runs);
	STD_ASSERT(527, 9, !std_path_Path_exists(runs.bytes.ptr, runs.bytes.len), "!space.join(RUNS).exists()");
	std_vec_free_Checked(&runs.bytes, 1, 1);
	spaced = packed.entry;
	spaced.f1 = 5;
	chunks_packs_lists_tests_one(io, record, &mine, &spaced, false, 528, 84);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 530, 38);
		chunks_packs_tests_listed(RUST_FILE, views[k], &spaced, 1, 531, 13);
		chunks_packs_tests_got(RUST_FILE, views[k], io, 5, &packed.address, &packed.at, 532, got_cols);
		STD_ASSERT(533, 13, chunks_packs_table_runs_get(&chunks_packs_view_View_table(views[k])->runs, 5) != NULL,
				   "view.table().runs.contains_key(&5)");
	}
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&space.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_lists_tests_a_change_dropped_before_its_install_changes_nothing_a_view_lists:
 * a change dropped before it is installed leaves the record as it was and
 * its view with nothing read, so the view's next refresh reads the lists
 * again and lists what the record does.
 */
void
test_chunks_packs_lists_tests_a_change_dropped_before_its_install_changes_nothing_a_view_lists(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView *views[2] = {&mine, &other};
	ChunksPacksListsChange change;
	ChunksErrorError error;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	chunks_packs_lists_tests_begun(io, record, &mine, &change, 548, 93, 103);
	if (!chunks_packs_lists_Change_add(&change, &first.entry, false, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 549, 34);
	chunks_packs_lists_Change_drop(&change);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 552, 38);
		STD_ASSERT(553, 13, chunks_packs_lists_tests_listed_len(views[k]) == 0 &&
				   chunks_packs_view_View_table(views[k])->packs.len == 0,
				   "view.listed().is_empty() && view.table().packs.is_empty()");
	}
	chunks_packs_lists_tests_one(io, record, &other, &first.entry, false, 555, 75);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 556, 34);
	chunks_packs_tests_listed(RUST_FILE, &mine, &first.entry, 1, 557, 9);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_lists_tests_written_whole_body is list_files_written_whole_list_what_they_listed's change: three adds and a forward's run. */
static void
chunks_packs_lists_tests_written_whole_body(ChunksPacksListsChange *change, void *captures)
{
	const StdVec *packs = captures;
	static const uint32 lines[] = {574, 575, 576};
	static const uint32 cols[] = {41, 41, 40};
	static const bool defers[] = {false, false, true};
	ChunksErrorError error;

	for (size_t k = 0; k < 3; k++)
		if (!chunks_packs_lists_Change_add(change, chunks_packs_lists_tests_entry(packs, k, lines[k], 24), defers[k],
										   &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, lines[k], cols[k]);
	chunks_packs_lists_tests_forwarding(change, "a.fwd:1", 577, 63);
}

/* chunks_packs_lists_tests_retire_two is list_files_written_whole_list_what_they_listed's second change: pack 2 retired. */
static void
chunks_packs_lists_tests_retire_two(ChunksPacksListsChange *change, void *captures)
{
	static const uint64 numbers[] = {2};

	(void) captures;
	chunks_packs_lists_tests_retired(change, numbers, lengthof(numbers), 594, 70);
}

/*
 * test_chunks_packs_lists_tests_list_files_written_whole_list_what_they_listed:
 * a whole write of every list file appends the deferred adds first and gives
 * each file a new generation, with its previous one kept beside it; views
 * that read the old files, and one that read none, then list the same packs
 * and forwards, and the record's count of rotations is even again.
 */
void
test_chunks_packs_lists_tests_list_files_written_whole_list_what_they_listed(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	table;
	StdPathBuf	previous;
	HostSharedOwned owned;
	HostSharedRecord record;
	StdVec		packs;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView fresh;
	ChunksPacksViewView *views[3] = {&mine, &other, &fresh};
	uint64		before;
	static const uint64 all[] = {1, 2, 3};
	static const uint64 kept[] = {1, 3};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	packs = chunks_packs_lists_tests_entries(io, &dir, 3);
	chunks_packs_lists_tests_forwards(io, &dir, "a.fwd", NULL, 0, 571, 72);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_written_whole_body, &packs);
	chunks_packs_tests_refreshed(RUST_FILE, &other, io, record, 579, 35);
	before = chunks_packs_view_View_table(&other)->generation;

	chunks_packs_lists_tests_compacted(io, record, &mine, true);
	chunks_packs_lists_tests_eq_u64(chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX), 2, NULL,
									583, 9);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	STD_ASSERT(584, 9, std_path_Path_exists(previous.bytes.ptr, previous.bytes.len),
			   "crate::file::previous(&dir.join(TABLE)).exists()");
	std_vec_free_Checked(&previous.bytes, 1, 1);
	std_vec_free_Checked(&table.bytes, 1, 1);
	chunks_packs_fixtures_view(&fresh);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		size_t		held;
		size_t		three = 3;
		size_t		forwards_len;

		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 587, 38);
		chunks_packs_lists_tests_ne_u64(chunks_packs_view_View_table(views[k])->generation, before, NULL, 588, 13);
		held = chunks_packs_view_View_table(views[k])->packs.len;
		STD_ASSERT_EQ_MSG(589, 13, held == three, &held, std_usize_Debug_fmt, &three, std_usize_Debug_fmt,
						  "the table's file leaves out a pack that was deferred");
		chunks_packs_tests_keys(RUST_FILE, views[k], all, lengthof(all), NULL, 590, 13);
		chunks_packs_lists_tests_eq_u64(chunks_packs_lists_tests_runs_len(views[k], 0, 591, 38), 3, NULL, 591, 13);
		(void) chunks_index_AddressIndexLocator_runs(chunks_packs_view_View_forwards(views[k]), &forwards_len);
		chunks_packs_lists_tests_eq_u64(forwards_len, 1, NULL, 592, 13);
	}
	chunks_packs_lists_tests_changed(io, record, &other, chunks_packs_lists_tests_retire_two, NULL);
	chunks_packs_tests_refreshed(RUST_FILE, &mine, io, record, 595, 34);
	chunks_packs_tests_keys(RUST_FILE, &mine, kept, lengthof(kept), NULL, 596, 9);
	chunks_packs_view_View_drop(&fresh);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	std_vec_free_Checked(&packs, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_lists_tests_add_all is only_a_list_file_past_its_slack_is_written_whole's change: every pack added at once. */
static void
chunks_packs_lists_tests_add_all(ChunksPacksListsChange *change, void *captures)
{
	const StdVec *packs = captures;
	ChunksErrorError error;

	for (size_t k = 0; k < packs->len; k++)
		if (!chunks_packs_lists_Change_add(change, &STD_VEC_AT(packs, ChunksPacksTableEntry, k), false, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 611, 105);
}

/* chunks_packs_lists_tests_remove_runs is only_a_list_file_past_its_slack_is_written_whole's second change: runs taken out by name. */
static void
chunks_packs_lists_tests_remove_runs(ChunksPacksListsChange *change, void *captures)
{
	const StdVec *names = captures;

	chunks_packs_lists_tests_runs_removed(change, names->ptr, names->len, 618, 95);
}

/*
 * test_chunks_packs_lists_tests_only_a_list_file_past_its_slack_is_written_whole:
 * a list file is written whole only once it holds more than its slack of
 * records past what its whole would hold: a runs file that passed it is, and
 * the pack table's file, which didn't, gets a record of where that runs file
 * now ends.
 */
void
test_chunks_packs_lists_tests_only_a_list_file_past_its_slack_is_written_whole(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	StdVec		packs;
	StdVec		names;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView *views[2] = {&mine, &other};
	std_TupleU64U64_t generations;
	std_TupleU64U64_t sevens = {7, 7};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	packs = chunks_packs_lists_tests_entries(io, &dir, CHUNKS_PACKS_LISTS_COMPACT_SLACK + 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_add_all, &packs);
	chunks_packs_lists_tests_compacted(io, record, &mine, false);
	chunks_packs_tests_refreshed(RUST_FILE, &other, io, record, 613, 35);
	generations.f0 = chunks_packs_view_View_table(&other)->generation;
	generations.f1 = chunks_packs_tests_runs_at(chunks_packs_view_View_table(&other), 0)->generation;
	STD_ASSERT_EQ_MSG(615, 9, generations.f0 == sevens.f0 && generations.f1 == sevens.f1, &generations,
					  chunks_packs_lists_tests_TupleU64U64_Debug_fmt, &sevens, chunks_packs_lists_tests_TupleU64U64_Debug_fmt,
					  "a file within its slack was written whole");

	std_range_from_check(1, packs.len, RUST_FILE, 617, 34);
	std_vec_new(&names, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 1; k < packs.len; k++)
	{
		StdString	name = chunks_pack_PackId_file_name(&STD_VEC_AT(&packs, ChunksPacksTableEntry, k).f2, "idx", 3);

		std_vec_push_Checked(&names, &name, sizeof(name), _Alignof(StdString));
	}
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_remove_runs, &names);
	chunks_packs_lists_tests_compacted(io, record, &mine, false);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		const ChunksPacksTable *table;

		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 621, 38);
		table = chunks_packs_view_View_table(views[k]);
		chunks_packs_lists_tests_eq_u64(table->generation, 7, NULL, 622, 13);
		chunks_packs_lists_tests_ne_u64(chunks_packs_tests_runs_at(table, 0)->generation, 7, NULL, 623, 13);
		chunks_packs_lists_tests_eq_u64(chunks_packs_lists_tests_runs_len(views[k], 0, 624, 38), 1, NULL, 624, 13);
		chunks_packs_lists_tests_eq_u64(chunks_packs_lists_tests_listed_len(views[k]), packs.len, NULL, 625, 13);
	}
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	std_vec_free_Checked(&packs, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksListsTestsRetirement is a retirement's change's captures: the pack it retires. */
typedef struct ChunksPacksListsTestsRetirement
{
	const ChunksPacksTableEntry *first;
} ChunksPacksListsTestsRetirement;

/* chunks_packs_lists_tests_retirement is a_retirements_records_lead_every_view_to_the_copies' second change: the forwards' run, the retirement, and the run's removal. */
static void
chunks_packs_lists_tests_retirement(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksListsTestsRetirement *retirement = captures;
	static const uint64 numbers[] = {1};
	StdString	name = chunks_pack_PackId_file_name(&retirement->first->f2, "idx", 3);

	chunks_packs_lists_tests_forwarding(change, "a.fwd:1", 649, 63);
	chunks_packs_lists_tests_retired(change, numbers, lengthof(numbers), 650, 33);
	chunks_packs_lists_tests_runs_removed(change, &name, 1, 651, 80);
	std_vec_free_Checked(&name, 1, 1);
}

/*
 * test_chunks_packs_lists_tests_a_retirements_records_lead_every_view_to_the_copies:
 * a retirement's records, the forwards' run added to the forwarding file,
 * the pack taken out of the table, and its run out of its runs file, leave
 * every view following a place in the retired pack to its copy.
 */
void
test_chunks_packs_lists_tests_a_retirements_records_lead_every_view_to_the_copies(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned owned;
	HostSharedRecord record;
	ChunksPacksFixturesPacked first;
	ChunksPacksFixturesPacked second;
	ChunksPacksViewView mine;
	ChunksPacksViewView other;
	ChunksPacksViewView *views[2] = {&mine, &other};
	ChunksPacksListsTestsRetirement retirement;
	ChunksTrailerLocator followed;
	chunks_TupleLocatorLocator_t moved;

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	first = chunks_packs_fixtures_pack(io, &dir, 1);
	second = chunks_packs_fixtures_pack(io, &dir, 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_fixtures_view(&other);
	{
		ChunksPacksTableEntry both[] = {first.entry, second.entry};
		static const bool defers[] = {false, true};
		static const uint32 lines[] = {641, 642};
		static const uint32 cols[] = {38, 38};
		ChunksPacksListsTestsAdds adds = {both, defers, 2, lines, cols};

		chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_added, &adds);
	}
	chunks_packs_tests_refreshed(RUST_FILE, &other, io, record, 644, 35);
	followed = chunks_packs_lists_tests_followed(io, &other, first.at, 645, 72);
	STD_ASSERT_EQ(645, 9, chunks_trailer_Locator_PartialEq_eq(&followed, &first.at), &followed,
				  chunks_trailer_Locator_Debug_fmt, &first.at, chunks_trailer_Locator_Debug_fmt);

	moved.f0 = first.at;
	moved.f1 = second.at;
	chunks_packs_lists_tests_forwards(io, &dir, "a.fwd", &moved, 1, 647, 82);
	retirement.first = &first.entry;
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_retirement, &retirement);
	for (size_t k = 0; k < lengthof(views); k++)
	{
		ChunksTrailerLocator unled = chunks_trailer_Locator_at(first.at.pack,
															   chunks_trailer_Locator_offset(&first.at) + 1,
															   chunks_trailer_Locator_length(&first.at));

		chunks_packs_tests_refreshed(RUST_FILE, views[k], io, record, 654, 38);
		chunks_packs_tests_listed(RUST_FILE, views[k], &second.entry, 1, 655, 13);
		chunks_packs_lists_tests_eq_u64(chunks_packs_lists_tests_runs_len(views[k], 0, 656, 38), 1, NULL, 656, 13);
		followed = chunks_packs_lists_tests_followed(io, views[k], first.at, 657, 74);
		STD_ASSERT_EQ(657, 13, chunks_trailer_Locator_PartialEq_eq(&followed, &second.at), &followed,
					  chunks_trailer_Locator_Debug_fmt, &second.at, chunks_trailer_Locator_Debug_fmt);
		followed = chunks_packs_lists_tests_followed(io, views[k], unled, 660, 22);
		chunks_packs_lists_tests_eq_u64(followed.pack, 1, NULL, 658, 13);
	}
	chunks_packs_view_View_drop(&other);
	chunks_packs_view_View_drop(&mine);
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* ChunksPacksListsTestsActor is an actor's captures in a_refresh_across_a_whole_write_lists_the_packs_before_it_or_after_it. */
typedef struct ChunksPacksListsTestsActor
{
	uint64		location;
	ChunksPacksTableEntry second;
	uint64	   *count;
} ChunksPacksListsTestsActor;

/* chunks_packs_lists_tests_opened opens the record at the actor's location, or fails as its actor does. */
static bool
chunks_packs_lists_tests_opened(HostIoIo *io, uint64 location, HostSharedHeld *result, HostError *error)
{
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, result,
						  error))
		return false;
	if (!has)
	{
		*error = chunks_packs_tests_failed_text("the location names no record");
		return false;
	}
	return true;
}

/* chunks_packs_lists_tests_reader is the reader actor: four refreshes, each listing the packs before the add or after it. */
static bool
chunks_packs_lists_tests_reader(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksListsTestsActor *actor = captures;
	HostSharedHeld held;
	ChunksPacksViewView view;
	ChunksErrorError failed;
	bool		ok = true;

	if (!chunks_packs_lists_tests_opened(io, actor->location, &held, error))
		return false;
	chunks_packs_fixtures_view(&view);
	for (int k = 0; k < 4 && ok; k++)
	{
		StdVec		listed;
		uint64		generation;
		bool		one;
		bool		two;
		bool		has_two;
		bool		whole;

		if (!chunks_packs_view_View_refresh(&view, io, host_shared_Held_record(&held), &failed))
		{
			*error = chunks_packs_tests_raised(&failed);
			ok = false;
			break;
		}
		chunks_packs_view_View_listed(&view, &listed);
		one = listed.len == 1 && STD_VEC_AT(&listed, ChunksPacksTableEntry, 0).f0 == 1;
		two = listed.len == 2 && STD_VEC_AT(&listed, ChunksPacksTableEntry, 0).f0 == 1 &&
			STD_VEC_AT(&listed, ChunksPacksTableEntry, 1).f0 == 2;
		has_two = chunks_packs_view_View_pack(&view, 2) != NULL;
		generation = chunks_packs_view_View_table(&view)->generation;
		whole = generation == 7 ? (one && !has_two) || (two && has_two) : two && has_two;
		if (!whole)
		{
			StdVec		numbers;
			StdString	message;
			StdFmtPiece pieces[] = {STD_FMT_LIT("a view of generation "), STD_FMT_ARG(0), STD_FMT_LIT(" lists "),
			STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&generation, std_u64_Display_fmt}, {&numbers, chunks_packs_tests_Numbers_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_vec_new(&numbers, sizeof(uint64), _Alignof(uint64));
			for (size_t n = 0; n < listed.len; n++)
				std_vec_push_Checked(&numbers, &STD_VEC_AT(&listed, ChunksPacksTableEntry, n).f0, sizeof(uint64),
									 _Alignof(uint64));
			std_fmt_format_Checked(&message, &arguments);
			std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
			*error = chunks_packs_tests_failed(&message);
			ok = false;
		}
		std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	}
	if (ok)
		*actor->count += chunks_packs_view_rereads();
	chunks_packs_view_View_drop(&view);
	host_shared_Held_drop(&held);
	return ok;
}

/* chunks_packs_lists_tests_writer is the writer actor: an add of the second pack, then a whole write of every list file. */
static bool
chunks_packs_lists_tests_writer(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksListsTestsActor *actor = captures;
	HostSharedHeld held;
	HostSharedRecord record;
	ChunksPacksViewView view;
	HostSharedLongGuard long_;
	HostSharedFlagGuard rewriting;
	HostSharedFlagIo flag_io;
	ChunksPacksListsChange change;
	ChunksErrorError failed;
	bool		ok = false;

	if (!chunks_packs_lists_tests_opened(io, actor->location, &held, error))
		return false;
	record = host_shared_Held_record(&held);
	chunks_packs_fixtures_view(&view);
	if (!host_shared_Record_lock_long(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, &long_,
									  error))
		goto view;
	if (!chunks_packs_lists_Change_begin(&view, record, &long_, &change, &failed))
		goto raised;
	if (!chunks_packs_lists_Change_add(&change, &actor->second, false, &failed))
	{
		chunks_packs_lists_Change_drop(&change);
		goto raised;
	}
	if (!chunks_packs_lists_Change_install(&change, &failed))
		goto raised;
	if (!host_shared_Record_flag(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_REWRITING_INDEX, &rewriting,
								 error))
		goto view;
	flag_io = host_shared_FlagGuard_io(&rewriting);
	if (!chunks_packs_lists_Rewrite_begin(&view, record, &flag_io, &change, &failed))
		goto flag_raised;
	if (!chunks_packs_lists_Rewrite_compact(&change, true, &failed))
	{
		chunks_packs_lists_Change_drop(&change);
		goto flag_raised;
	}
	ok = chunks_packs_lists_Change_install(&change, &failed);
	if (!ok)
		goto flag_raised;
	host_shared_FlagGuard_drop(&rewriting);
	goto view;
flag_raised:
	host_shared_FlagGuard_drop(&rewriting);
raised:
	*error = chunks_packs_tests_raised(&failed);
view:
	chunks_packs_view_View_drop(&view);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * test_chunks_packs_lists_tests_a_refresh_across_a_whole_write_lists_the_packs_before_it_or_after_it:
 * a view brought up to date while another backend adds a pack and then
 * writes every list file whole lists, at every interleaving, the packs
 * before the add or after it, never a mix and never an error; some
 * interleavings put the whole write between the refresh's reads, which then
 * reads again under the list files' flag.
 */
void
test_chunks_packs_lists_tests_a_refresh_across_a_whole_write_lists_the_packs_before_it_or_after_it(void)
{
	uint64		reread = 0;

	for (uint64 seed = 1; seed <= 32; seed++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		StdPathBuf	path;
		HostSharedOwned owned;
		ChunksPacksFixturesPacked first;
		ChunksPacksFixturesPacked second;
		ChunksPacksFixturesLists lists;
		StdVec		deferred = chunks_packs_tests_no_deferred();
		uint64		location;
		SimcRun		run;
		SimcReport	report;
		ChunksPacksListsTestsActor actor;
		SimcActor	reader = {chunks_packs_lists_tests_reader, &actor};
		SimcActor	writer = {chunks_packs_lists_tests_writer, &actor};
		HostSharedHeld held;
		HostError	host;
		bool		has;
		ChunksPacksViewView after;
		static const uint64 both[] = {1, 2};
		StdFmtPiece pieces[] = {STD_FMT_LIT("seed "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&seed, std_u64_Display_fmt}};
		StdFmtArguments seeded = {pieces, lengthof(pieces), args};

		chunks_packs_fixtures_home(&path);
		owned = chunks_packs_tests_made(RUST_FILE, io, 678, 47);
		first = chunks_packs_fixtures_pack(io, &path, 1);
		second = chunks_packs_fixtures_pack(io, &path, 2);
		lists = chunks_packs_fixtures_written(HOST_IO_IO_IO_DOORS(io), &path, 7, &first.entry, 1);
		chunks_packs_tests_installed(RUST_FILE, io, host_shared_Owned_record(&owned), &lists, &deferred, 681, 58);
		location = host_shared_Owned_publish(&owned);

		actor.location = location;
		actor.second = second.entry;
		actor.count = &reread;
		run = simc_Run_new(seed);
		simc_Run_actor(&run, "reader", 6, reader);
		simc_Run_actor(&run, "writer", 6, writer);
		report = simc_Run_finish(run);
		if (!chunks_packs_tests_returned(&report.outcome))
		{
			StdFmtPiece failed_pieces[] = {STD_FMT_LIT("seed "), STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
			StdFmtArg	failed_args[] = {{&seed, std_u64_Display_fmt}, {&report.outcome, simc_Outcome_Debug_fmt}};
			StdFmtArguments arguments = {failed_pieces, lengthof(failed_pieces), failed_args};

			std_panic_fmt(RUST_FILE, 722, 13, &arguments);
		}
		simc_Report_drop(&report);
		if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &held,
							  &host))
			chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 728, 56);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(728, 65);
		chunks_packs_fixtures_view(&after);
		chunks_packs_tests_refreshed(RUST_FILE, &after, io, host_shared_Held_record(&held), 730, 46);
		chunks_packs_tests_keys(RUST_FILE, &after, both, lengthof(both), &seeded, 731, 13);
		chunks_packs_lists_tests_ne_u64(chunks_packs_view_View_table(&after)->generation, 7, &seeded, 732, 13);
		chunks_packs_view_View_drop(&after);
		host_shared_Held_drop(&held);
		chunks_packs_fixtures_Lists_drop(&lists);
		std_vec_free_Checked(&path.bytes, 1, 1);
		simc_TestIo_drop(&test_io);
	}
	STD_ASSERT_MSG(734, 9, reread > 0, "no interleaving put the whole write between a refresh's reads");
}

/* chunks_packs_lists_tests_two_adds is a_whole_write_that_fails_before_its_renames_leaves_the_count_even's change: two adds. */
static void
chunks_packs_lists_tests_two_adds(ChunksPacksListsChange *change, void *captures)
{
	const StdVec *packs = captures;
	static const uint32 lines[] = {749, 750};
	ChunksErrorError error;

	for (size_t k = 0; k < 2; k++)
		if (!chunks_packs_lists_Change_add(change, chunks_packs_lists_tests_entry(packs, k, lines[k], 24), false,
										   &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, lines[k], 41);
}

/*
 * test_chunks_packs_lists_tests_a_whole_write_that_fails_before_its_renames_leaves_the_count_even:
 * a whole write of the list files that fails before it renames anything
 * leaves the count of rotations even, so the next refresh reads under no lock
 * what the files held.
 */
void
test_chunks_packs_lists_tests_a_whole_write_that_fails_before_its_renames_leaves_the_count_even(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	data;
	HostSharedOwned owned;
	HostSharedRecord record;
	StdVec		packs;
	ChunksPacksViewView mine;
	ChunksPacksViewView fresh;
	SimcSim		sim;
	bool		failed = false;
	ChunksErrorError error;
	static const uint64 both[] = {1, 2};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	record = host_shared_Owned_record(&owned);
	packs = chunks_packs_lists_tests_entries(io, &dir, 2);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_lists_tests_changed(io, record, &mine, chunks_packs_lists_tests_two_adds, &packs);
	data = simc_data_dir();
	sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 61);
	std_vec_free_Checked(&data.bytes, 1, 1);
	for (uint64 fault = 1; fault < 16; fault++)
	{
		HostSharedFlagGuard rewriting;
		HostSharedFlagIo flag_io;
		ChunksPacksListsChange rewrite;
		HostError	host;

		simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + fault);
		if (!host_shared_Record_flag(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_REWRITING_INDEX,
									 &rewriting, &host))
			chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 756, 62);
		flag_io = host_shared_FlagGuard_io(&rewriting);
		if (!chunks_packs_lists_Rewrite_begin(&mine, record, &flag_io, &rewrite, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 758, 74);
		if (!chunks_packs_lists_Rewrite_compact(&rewrite, true, &error))
		{
			chunks_packs_lists_Change_drop(&rewrite);
			failed = true;
		}
		else
			failed = !chunks_packs_lists_Change_install(&rewrite, &error);
		host_shared_FlagGuard_drop(&rewriting);
		if (failed)
			break;
	}
	simc_Sim_stop(&sim);
	STD_ASSERT_MSG(765, 9, failed, "no whole write was made to fail");
	chunks_error_Error_drop(&error);
	chunks_packs_lists_tests_eq_u64(chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_ROTATIONS_INDEX) % 2, 0,
									"a whole write that renamed nothing left the count odd", 766, 9);
	chunks_packs_fixtures_view(&fresh);
	chunks_packs_tests_refreshed(RUST_FILE, &fresh, io, record, 772, 35);
	chunks_packs_tests_keys(RUST_FILE, &fresh, both, lengthof(both), NULL, 773, 9);
	chunks_packs_view_View_drop(&fresh);
	simc_Sim_drop(&sim);
	chunks_packs_view_View_drop(&mine);
	std_vec_free_Checked(&packs, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	host_shared_Owned_drop(&owned);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
