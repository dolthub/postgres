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
 * The C port of chunks' packs/open.rs: opening a repository's packs, by
 * finding its pack record by its id or making it from the repository's list
 * files under the lock every maker holds, and the directories' makes, moves,
 * and removals around a repository's life.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/lists.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/packs/view.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "open.rs"

/* chunks_packs_open_path_error returns a corrupt error of a text with a path's display between two parts. */
static ChunksErrorError
chunks_packs_open_path_error(const char *before, const char *path, size_t path_len, const char *after)
{
	StdString	message;
	StdStr		shown = {path, path_len};
	StdStr		first = {before, strlen(before)};
	StdStr		last = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&first, std_str_Display_fmt}, {&shown, std_path_Display_fmt}, {&last, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/* chunks_packs_open_gone returns the refusal of a repository whose list file is gone with its previous generation. */
ChunksErrorError
chunks_packs_open_gone(const char *path, size_t path_len)
{
	return chunks_packs_open_path_error("required file \"", path, path_len, "\" does not exist");
}

/*
 * chunks_packs_open_read_whole_frame_0_t is read_whole's state across its
 * pad: its arguments, its result, and the file's read it holds.
 */
typedef struct chunks_packs_open_read_whole_frame_0_t
{
	const ChunksPacksViewListOps *ops;
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	void	   *list;
	ChunksPacksViewRead *read;
	ChunksErrorError *error;
	bool		ok;
	ChunksFileAppended appended;
	bool		appended_live;
} chunks_packs_open_read_whole_frame_0_t;

/* chunks_packs_open_read_whole_pad_0 is read_whole's pad: the file's read. */
static void
chunks_packs_open_read_whole_pad_0(void *frame_)
{
	chunks_packs_open_read_whole_frame_0_t *frame = frame_;

	if (frame->appended_live)
	{
		frame->appended_live = false;
		chunks_file_Appended_drop(&frame->appended);
	}
}

/* chunks_packs_open_read_whole_body_0 is read_whole's body. */
static void
chunks_packs_open_read_whole_body_0(void *frame_)
{
	chunks_packs_open_read_whole_frame_0_t *frame = frame_;
	bool		has_appended;

	if (!chunks_file_read_appended(frame->io, frame->path, frame->path_len, &has_appended, &frame->appended,
								   frame->error))
		return;
	if (!has_appended)
	{
		*frame->error = chunks_packs_open_gone(frame->path, frame->path_len);
		return;
	}
	frame->appended_live = true;
	if (!frame->appended.has_end || frame->appended.has_fallback || frame->appended.kept ||
		frame->appended.has_damage || frame->appended.records.len == 0)
	{
		*frame->error = chunks_packs_open_path_error("", frame->path, frame->path_len,
													 " is not whole, which the startup process settles");
		goto done;
	}
	frame->ops->init(frame->list);
	for (size_t k = 0, k_len = frame->appended.records.len; k < k_len; k++)
	{
		const StdVec *record = &STD_VEC_AT(&frame->appended.records, StdVec, k);

		if (!frame->ops->apply(frame->list, record->ptr, record->len, frame->error))
		{
			frame->ops->reset(frame->list);
			goto done;
		}
	}
	frame->read->at = frame->appended.end.end;
	frame->read->past = (uint64) frame->appended.records.len - 1;
	frame->ok = true;
done:
	chunks_packs_open_read_whole_pad_0(frame);
}

/*
 * chunks_packs_open_read_whole reads a list file from its start to where its
 * whole records end, for a process that settles nothing, refusing a file
 * gone with its previous generation, damaged, or readable only as its
 * previous generation.
 */
bool
chunks_packs_open_read_whole(const ChunksPacksViewListOps *ops, HostIoReadDoors io, const char *path, size_t path_len,
							 void *list, ChunksPacksViewRead *read, ChunksErrorError *error)
{
	chunks_packs_open_read_whole_frame_0_t frame = {.ops = ops, .io = io, .path = path, .path_len = path_len,
	.list = list, .read = read, .error = error};

	std_landing(&frame, chunks_packs_open_read_whole_body_0, chunks_packs_open_read_whole_pad_0);
	return frame.ok;
}

/* chunks_packs_open_joined returns a directory's path joined with a name. */
static void
chunks_packs_open_joined(const StdPathBuf *dir, const char *name, StdPathBuf *result)
{
	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), result);
}

/* chunks_packs_open_record_made is RecordMade's event. */
static void
chunks_packs_open_record_made(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_RECORD_MADE;
}

/*
 * chunks_packs_open_made_frame_0_t is made's state across its pad: its
 * arguments, its result, and the directory, path, lists, record, snapshot,
 * bytes, and guard it holds.
 */
typedef struct chunks_packs_open_made_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	HostSharedOwned *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	StdPathBuf	path;
	ChunksPacksViewReadTable table;
	ChunksPacksViewReadRuns forwarding;
	HostSharedOwned owned;
	bool		owned_live;
	ChunksPacksSharedSnapshot snapshot;
	bool		snapshot_live;
	StdVec		bytes;
	HostSharedGuard state;
	bool		state_live;
} chunks_packs_open_made_frame_0_t;

/* chunks_packs_open_made_pad_0 is made's pad: the guard, the bytes, the snapshot, the record, then the lists and paths. */
static void
chunks_packs_open_made_pad_0(void *frame_)
{
	chunks_packs_open_made_frame_0_t *frame = frame_;

	if (frame->state_live)
	{
		frame->state_live = false;
		host_shared_Guard_drop(&frame->state);
	}
	std_vec_free_Checked(&frame->bytes, 1, 1);
	std_vec_new(&frame->bytes, 1, 1);
	if (frame->snapshot_live)
	{
		frame->snapshot_live = false;
		chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	}
	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
	chunks_packs_Runs_drop(&frame->forwarding.list);
	chunks_packs_Runs_Default_default(&frame->forwarding.list);
	chunks_packs_Table_drop(&frame->table.list);
	chunks_packs_Table_Default_default(&frame->table.list);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_open_made_body_0 is made's body. */
static void
chunks_packs_open_made_body_0(void *frame_)
{
	chunks_packs_open_made_frame_0_t *frame = frame_;
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(frame->io);
	HostSharedRecord record;
	HostError	host;
	bool		marked;
	bool		listed;
	uint64		start;

	frame->dir = host_tablespace_dir(frame->repository, frame->own);
	if (!chunks_packs_dropped_marked(read, frame->dir.bytes.ptr, frame->dir.bytes.len, &marked, frame->error))
		goto done;
	if (marked)
	{
		*frame->error = chunks_packs_dropped_refused(frame->repository);
		goto done;
	}
	chunks_packs_open_joined(&frame->dir, CHUNKS_PACKS_TABLE, &frame->path);
	listed = chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, read, frame->path.bytes.ptr,
										  frame->path.bytes.len, &frame->table.list, &frame->table.read,
										  frame->error);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	if (!listed)
		goto done;
	chunks_packs_open_joined(&frame->dir, CHUNKS_PACKS_FORWARDING, &frame->path);
	listed = chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, read, frame->path.bytes.ptr,
										  frame->path.bytes.len, &frame->forwarding.list, &frame->forwarding.read,
										  frame->error);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	if (!listed)
		goto done;
	if (!host_shared_make(&CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, frame->io, &frame->owned, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->owned_live = true;
	record = host_shared_Owned_record(&frame->owned);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, frame->repository);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_OWN_INDEX, frame->own);
	start = std_max_u64(frame->table.list.reserved, frame->table.list.added);
	chunks_packs_shared_add(record, CHUNKS_PACKS_SHARED_RESERVED_INDEX, chunks_packs_shared_reserved(start, start));
	chunks_packs_shared_Snapshot_Default_default(&frame->snapshot);
	frame->snapshot_live = true;
	frame->snapshot.table = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, &frame->table.read,
													   &frame->table.list);
	frame->snapshot.forwarding = chunks_packs_lists_tail_of(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, &frame->forwarding.read,
															&frame->forwarding.list);
	chunks_packs_shared_Snapshot_bytes(&frame->snapshot, &frame->bytes);
	frame->snapshot_live = false;
	chunks_packs_shared_Snapshot_drop(&frame->snapshot);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX,
								 &frame->state, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->state_live = true;
	if (!host_shared_GuardExclusive_write_blob(&frame->state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB, frame->bytes.ptr,
											   frame->bytes.len, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->state_live = false;
	host_shared_Guard_drop(&frame->state);
	frame->owned_live = false;
	*frame->result = frame->owned;
	frame->ok = true;
done:
	chunks_packs_open_made_pad_0(frame);
}

/*
 * chunks_packs_open_made makes the pack record of a repository from the list
 * files in its directory, owned by its maker, which publishes it; its
 * numbers begin past the highest a pack was added at.
 */
static bool
chunks_packs_open_made(HostIoIo *io, uint32 repository, uint32 own, HostSharedOwned *result, ChunksErrorError *error)
{
	chunks_packs_open_made_frame_0_t frame = {.io = io, .repository = repository, .own = own, .result = result,
	.error = error};

	std_PathBuf_new(&frame.dir);
	std_PathBuf_new(&frame.path);
	chunks_packs_Table_Default_default(&frame.table.list);
	chunks_packs_Runs_Default_default(&frame.forwarding.list);
	std_vec_new(&frame.bytes, 1, 1);
	std_landing(&frame, chunks_packs_open_made_body_0, chunks_packs_open_made_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_spaces_frame_0_t is spaces' state across its pad: its
 * arguments, its result, and the lists it holds.
 */
typedef struct chunks_packs_open_spaces_frame_0_t
{
	HostIoReadDoors io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		others;
	StdVec		spaces;
} chunks_packs_open_spaces_frame_0_t;

/* chunks_packs_open_spaces_pad_0 is spaces' pad: its lists. */
static void
chunks_packs_open_spaces_pad_0(void *frame_)
{
	chunks_packs_open_spaces_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&frame->others, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->others, sizeof(uint32), _Alignof(uint32));
}

/* chunks_packs_open_spaces_body_0 is spaces' body. */
static void
chunks_packs_open_spaces_body_0(void *frame_)
{
	chunks_packs_open_spaces_frame_0_t *frame = frame_;
	HostError	host;
	uint32		first = HOST_PG_DEFAULT;

	if (!host_tablespaces(frame->io, &frame->others, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	std_vec_with_capacity_Checked(&frame->spaces, frame->others.len + 1, sizeof(uint32), _Alignof(uint32));
	std_vec_push_Checked(&frame->spaces, &first, sizeof(first), _Alignof(uint32));
	std_vec_extend_from_slice_Checked(&frame->spaces, frame->others.ptr, frame->others.len, sizeof(uint32),
									  _Alignof(uint32));
	*frame->result = frame->spaces;
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	frame->ok = true;
	chunks_packs_open_spaces_pad_0(frame);
}

/* chunks_packs_open_spaces returns the server's tablespaces: the data directory's first, then the others. */
static bool
chunks_packs_open_spaces(HostIoReadDoors io, StdVec *result, ChunksErrorError *error)
{
	chunks_packs_open_spaces_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_vec_new(&frame.others, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame.spaces, sizeof(uint32), _Alignof(uint32));
	std_landing(&frame, chunks_packs_open_spaces_body_0, chunks_packs_open_spaces_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_exists_frame_0_t is exists' state across its pad: its
 * arguments, its result, and the path it holds.
 */
typedef struct chunks_packs_open_exists_frame_0_t
{
	HostIoReadDoors io;
	const StdPathBuf *dir;
	const char *name;
	bool		previous;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	path;
} chunks_packs_open_exists_frame_0_t;

/* chunks_packs_open_exists_pad_0 is exists' pad: the path. */
static void
chunks_packs_open_exists_pad_0(void *frame_)
{
	chunks_packs_open_exists_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
}

/* chunks_packs_open_exists_body_0 is exists' body. */
static void
chunks_packs_open_exists_body_0(void *frame_)
{
	chunks_packs_open_exists_frame_0_t *frame = frame_;

	chunks_packs_open_joined(frame->dir, frame->name, &frame->path);
	if (frame->previous)
	{
		StdPathBuf	before = chunks_file_previous(frame->path.bytes.ptr, frame->path.bytes.len);

		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		frame->path = before;
	}
	frame->ok = chunks_file_exists(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, frame->result,
								   frame->error);
	chunks_packs_open_exists_pad_0(frame);
}

/* chunks_packs_open_exists reports whether a file is at a directory's path joined with a name, or its previous generation. */
static bool
chunks_packs_open_exists(HostIoReadDoors io, const StdPathBuf *dir, const char *name, bool previous, bool *result,
						 ChunksErrorError *error)
{
	chunks_packs_open_exists_frame_0_t frame = {.io = io, .dir = dir, .name = name, .previous = previous,
	.result = result, .error = error};

	std_PathBuf_new(&frame.path);
	std_landing(&frame, chunks_packs_open_exists_body_0, chunks_packs_open_exists_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_repositories_frame_0_t is repositories' state across its
 * pad: its arguments, its result, and the lists and paths it holds.
 */
typedef struct chunks_packs_open_repositories_frame_0_t
{
	HostIoReadDoors io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		spaces;
	StdVec		found;
	StdPathBuf	versioned;
	StdVec		listed;
	StdPathBuf	dir;
} chunks_packs_open_repositories_frame_0_t;

/* chunks_packs_open_repositories_pad_0 is repositories' pad: the directory, the names, the path, then the lists. */
static void
chunks_packs_open_repositories_pad_0(void *frame_)
{
	chunks_packs_open_repositories_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	std_vec_drop_Checked(&frame->listed, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->listed, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&frame->versioned.bytes, 1, 1);
	std_PathBuf_new(&frame->versioned);
	std_vec_free_Checked(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_vec_new(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
}

/* chunks_packs_open_repositories_body_0 is repositories' body. */
static void
chunks_packs_open_repositories_body_0(void *frame_)
{
	static const char *const names[] = {CHUNKS_PACKS_TABLE, CHUNKS_PACKS_FORWARDING, CHUNKS_PACKS_MANIFEST};
	chunks_packs_open_repositories_frame_0_t *frame = frame_;
	HostError	host;

	if (!chunks_packs_open_spaces(frame->io, &frame->spaces, frame->error))
		goto done;
	for (size_t s = 0, s_len = frame->spaces.len; s < s_len; s++)
	{
		uint32		space = STD_VEC_AT(&frame->spaces, uint32, s);
		bool		has_listed;

		frame->versioned = host_versioned_dir(space);
		if (!host_list_dir_if_there(frame->io, frame->versioned.bytes.ptr, frame->versioned.bytes.len, &has_listed,
									&frame->listed, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		std_vec_free_Checked(&frame->versioned.bytes, 1, 1);
		std_PathBuf_new(&frame->versioned);
		if (!has_listed)
			continue;
		for (size_t k = 0, k_len = frame->listed.len; k < k_len; k++)
		{
			const StdString *name = &STD_VEC_AT(&frame->listed, StdString, k);
			uint32		repository;
			StdParseIntError parsed;
			bool		moved;
			bool		own;
			bool		looked = true;

			if (!std_str_parse_u32(name->ptr, name->len, &repository, &parsed) || repository == 0)
				continue;
			frame->dir = host_tablespace_dir(repository, space);
			if (!chunks_packs_open_exists(frame->io, &frame->dir, CHUNKS_PACKS_OPEN_MOVED_FROM, false, &moved,
										  frame->error))
				looked = false;
			else if (!moved)
			{
				looked = chunks_packs_dropped_marked(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, &own,
													 frame->error);
				for (size_t n = 0; n < lengthof(names) && looked && !own; n++)
					looked = chunks_packs_open_exists(frame->io, &frame->dir, names[n], false, &own, frame->error) &&
						(own || chunks_packs_open_exists(frame->io, &frame->dir, names[n], true, &own, frame->error));
				if (looked && own)
				{
					std_TupleU32U32_t found = {repository, space};

					std_vec_push_Checked(&frame->found, &found, sizeof(found), _Alignof(std_TupleU32U32_t));
				}
			}
			std_vec_free_Checked(&frame->dir.bytes, 1, 1);
			std_PathBuf_new(&frame->dir);
			if (!looked)
				goto done;
		}
		std_vec_drop_Checked(&frame->listed, sizeof(StdString), _Alignof(StdString), std_String_drop);
		std_vec_new(&frame->listed, sizeof(StdString), _Alignof(StdString));
	}
	for (size_t k = 1; k < frame->found.len; k++)
		for (size_t j = k; j > 0; j--)
		{
			std_TupleU32U32_t *a = &STD_VEC_AT(&frame->found, std_TupleU32U32_t, j - 1);
			std_TupleU32U32_t *b = &STD_VEC_AT(&frame->found, std_TupleU32U32_t, j);
			std_TupleU32U32_t swapped;

			if (a->f0 < b->f0 || (a->f0 == b->f0 && a->f1 <= b->f1))
				break;
			swapped = *a;
			*a = *b;
			*b = swapped;
		}
	*frame->result = frame->found;
	std_vec_new(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	frame->ok = true;
done:
	chunks_packs_open_repositories_pad_0(frame);
}

/*
 * chunks_packs_open_repositories returns every repository the server's
 * tablespaces hold a directory of, in order of id, each with the tablespace
 * of its own directory: the one that holds the pack table's file, the
 * forwarding file, or the manifest, in either generation, or the dropped
 * mark, and none a move's finish marked.
 */
bool
chunks_packs_open_repositories(HostIoReadDoors io, StdVec *result, ChunksErrorError *error)
{
	chunks_packs_open_repositories_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_vec_new(&frame.spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame.found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_PathBuf_new(&frame.versioned);
	std_vec_new(&frame.listed, sizeof(StdString), _Alignof(StdString));
	std_PathBuf_new(&frame.dir);
	std_landing(&frame, chunks_packs_open_repositories_body_0, chunks_packs_open_repositories_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_spaced_directories_frame_0_t is spaced_directories_into's
 * state across its pad: its arguments, its result, and the directories,
 * tablespaces, and entry it holds.
 */
typedef struct chunks_packs_open_spaced_directories_frame_0_t
{
	HostIoReadDoors io;
	uint32		repository;
	uint32		own;
	bool		spaced;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	size_t		size;
	size_t		align;
	StdVec		dirs;
	StdVec		spaces;
	chunks_TupleU32PathBuf_t entry;
} chunks_packs_open_spaced_directories_frame_0_t;

/* chunks_packs_open_spaced_directories_pad_0 is spaced_directories_into's pad: the entry, the tablespaces, then the directories. */
static void
chunks_packs_open_spaced_directories_pad_0(void *frame_)
{
	chunks_packs_open_spaced_directories_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->entry.f1.bytes, 1, 1);
	std_PathBuf_new(&frame->entry.f1);
	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
		std_vec_free_Checked(frame->spaced ? &STD_VEC_AT(&frame->dirs, chunks_TupleU32PathBuf_t, k).f1.bytes :
							 &STD_VEC_AT(&frame->dirs, StdPathBuf, k).bytes, 1, 1);
	std_vec_free_Checked(&frame->dirs, frame->size, frame->align);
	std_vec_new(&frame->dirs, frame->size, frame->align);
}

/* chunks_packs_open_spaced_directories_kept pushes the entry's directory, which the list then holds. */
static void
chunks_packs_open_spaced_directories_kept(chunks_packs_open_spaced_directories_frame_0_t *frame)
{
	std_vec_push_Checked(&frame->dirs, frame->spaced ? (void *) &frame->entry : (void *) &frame->entry.f1,
						 frame->size, frame->align);
	std_PathBuf_new(&frame->entry.f1);
}

/* chunks_packs_open_spaced_directories_body_0 is spaced_directories_into's body. */
static void
chunks_packs_open_spaced_directories_body_0(void *frame_)
{
	chunks_packs_open_spaced_directories_frame_0_t *frame = frame_;

	frame->entry.f0 = CHUNKS_PACKS_DEFAULT_SPACE;
	frame->entry.f1 = host_tablespace_dir(frame->repository, frame->own);
	chunks_packs_open_spaced_directories_kept(frame);
	if (!chunks_packs_open_spaces(frame->io, &frame->spaces, frame->error))
		goto done;
	for (size_t s = 0, s_len = frame->spaces.len; s < s_len; s++)
	{
		uint32		space = STD_VEC_AT(&frame->spaces, uint32, s);
		bool		there;

		if (space == frame->own)
			continue;
		frame->entry.f0 = space;
		frame->entry.f1 = host_tablespace_dir(frame->repository, space);
		if (!chunks_file_dir_there(frame->io, frame->entry.f1.bytes.ptr, frame->entry.f1.bytes.len, &there,
								   frame->error))
			goto done;
		if (there)
			chunks_packs_open_spaced_directories_kept(frame);
		else
		{
			std_vec_free_Checked(&frame->entry.f1.bytes, 1, 1);
			std_PathBuf_new(&frame->entry.f1);
		}
	}
	*frame->result = frame->dirs;
	std_vec_new(&frame->dirs, frame->size, frame->align);
	frame->ok = true;
done:
	chunks_packs_open_spaced_directories_pad_0(frame);
}

/*
 * chunks_packs_open_spaced_directories returns the directories that hold
 * files of a repository, its own first with DEFAULT_SPACE, and then its
 * directory in each other tablespace that has one, with that tablespace.
 */
static bool
chunks_packs_open_spaced_directories_into(HostIoReadDoors io, uint32 repository, uint32 own, bool spaced,
										  StdVec *result, ChunksErrorError *error)
{
	chunks_packs_open_spaced_directories_frame_0_t frame = {.io = io, .repository = repository, .own = own,
		.spaced = spaced, .result = result, .error = error,
		.size = spaced ? sizeof(chunks_TupleU32PathBuf_t) : sizeof(StdPathBuf),
	.align = spaced ? _Alignof(chunks_TupleU32PathBuf_t) : _Alignof(StdPathBuf)};

	std_vec_new(&frame.dirs, frame.size, frame.align);
	std_vec_new(&frame.spaces, sizeof(uint32), _Alignof(uint32));
	std_PathBuf_new(&frame.entry.f1);
	std_landing(&frame, chunks_packs_open_spaced_directories_body_0, chunks_packs_open_spaced_directories_pad_0);
	return frame.ok;
}

/* chunks_packs_open_directories returns the directories that hold files of a repository, its own first. */
bool
chunks_packs_open_directories(HostIoReadDoors io, uint32 repository, uint32 own, StdVec *result,
							  ChunksErrorError *error)
{
	return chunks_packs_open_spaced_directories_into(io, repository, own, false, result, error);
}

/* chunks_packs_open_spaced_directories returns the repository's directories, each with the tablespace its packs are listed under. */
bool
chunks_packs_open_spaced_directories(HostIoReadDoors io, uint32 repository, uint32 own, StdVec *result,
									 ChunksErrorError *error)
{
	return chunks_packs_open_spaced_directories_into(io, repository, own, true, result, error);
}

/*
 * chunks_packs_open_found_frame_0_t is found's state across its pad: its
 * arguments, its results, and the sweep's list it holds.
 */
typedef struct chunks_packs_open_found_frame_0_t
{
	HostSharedWithin within;
	uint32		repository;
	bool	   *has_result;
	HostSharedHeld *held;
	uint64	   *location;
	ChunksErrorError *error;
	bool		ok;
	StdVec		swept;
} chunks_packs_open_found_frame_0_t;

/* chunks_packs_open_found_pad_0 is found's pad: the sweep's list. */
static void
chunks_packs_open_found_pad_0(void *frame_)
{
	chunks_packs_open_found_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
}

/* chunks_packs_open_found_body_0 is found's body. */
static void
chunks_packs_open_found_body_0(void *frame_)
{
	chunks_packs_open_found_frame_0_t *frame = frame_;
	HostError	host;

	if (!host_shared_sweep(frame->within, &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &frame->swept, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	*frame->has_result = false;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		std_TupleU64U64_t pair = STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		opened;

		if (pair.f1 != frame->repository)
			continue;
		if (!host_shared_open(frame->within, &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, pair.f0, &opened, frame->held,
							  &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		if (!opened)
			continue;
		if (chunks_packs_shared_load(host_shared_Held_record(frame->held), CHUNKS_PACKS_SHARED_REPOSITORY_INDEX) ==
			frame->repository)
		{
			*frame->has_result = true;
			*frame->location = pair.f0;
			break;
		}
		host_shared_Held_drop(frame->held);
	}
	frame->ok = true;
done:
	chunks_packs_open_found_pad_0(frame);
}

/*
 * chunks_packs_open_found returns the repository's published pack record,
 * opened, with its location: the first a sweep lists with the repository's
 * id that still carries the id once it is open.
 */
bool
chunks_packs_open_found(HostSharedWithin within, uint32 repository, bool *has_result, HostSharedHeld *held,
						uint64 *location, ChunksErrorError *error)
{
	chunks_packs_open_found_frame_0_t frame = {.within = within, .repository = repository, .has_result = has_result,
	.held = held, .location = location, .error = error};

	std_vec_new(&frame.swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_landing(&frame, chunks_packs_open_found_body_0, chunks_packs_open_found_pad_0);
	return frame.ok;
}

/* chunks_packs_open_spaces_lock takes chunks' single record's Spaces lock, long, which a directory's make or removal holds. */
static bool
chunks_packs_open_spaces_lock(HostIoIo *io, HostSharedLongGuard *result, ChunksErrorError *error)
{
	HostSharedRecord state;
	HostError	host;

	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &state, &host) ||
		!host_shared_Record_lock_long(state, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SINGLE_SPACES_INDEX, result,
									  &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/*
 * chunks_packs_open_spaced_frame_0_t is the state across its pad of a step
 * under the Spaces lock: its arguments, its result, and the lock's guard and
 * the guard's Io, which the step's doors borrow, the mark's path and file,
 * and the names and path it holds.
 */
typedef struct chunks_packs_open_spaced_frame_0_t
{
	HostIoIo   *io;
	const char *dir;
	size_t		dir_len;
	const char *copy;
	size_t		copy_len;
	ChunksErrorError *error;
	bool		ok;
	HostSharedLongGuard spaces;
	bool		spaces_live;
	HostSharedLongIo held;
	StdPathBuf	mark;
	HostFile	file;
	bool		file_live;
	StdVec		names;
	StdPathBuf	path;
} chunks_packs_open_spaced_frame_0_t;

/* chunks_packs_open_spaced_pad_0 is a Spaces step's pad: the path, the names, the file, the mark's path, then the guard. */
static void
chunks_packs_open_spaced_pad_0(void *frame_)
{
	chunks_packs_open_spaced_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	std_vec_free_Checked(&frame->mark.bytes, 1, 1);
	std_PathBuf_new(&frame->mark);
	if (frame->spaces_live)
	{
		frame->spaces_live = false;
		host_shared_LongGuard_drop(&frame->spaces);
	}
}

/* chunks_packs_open_spaced runs a step under the Spaces lock in a frame of its arguments. */
static bool
chunks_packs_open_spaced(HostIoIo *io, const char *copy, size_t copy_len, const char *dir, size_t dir_len,
						 void (*body) (void *frame_), ChunksErrorError *error)
{
	chunks_packs_open_spaced_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len, .copy = copy,
	.copy_len = copy_len, .error = error};

	std_PathBuf_new(&frame.mark);
	std_vec_new(&frame.names, sizeof(StdString), _Alignof(StdString));
	std_PathBuf_new(&frame.path);
	std_landing(&frame, body, chunks_packs_open_spaced_pad_0);
	return frame.ok;
}

/* chunks_packs_open_spaced_lock takes the Spaces lock for a step, returning its doors. */
static bool
chunks_packs_open_spaced_lock(chunks_packs_open_spaced_frame_0_t *frame, HostIoIoDoors *result)
{
	if (!chunks_packs_open_spaces_lock(frame->io, &frame->spaces, frame->error))
		return false;
	frame->spaces_live = true;
	frame->held = host_shared_LongGuard_io(&frame->spaces);
	*result = HOST_SHARED_LONG_IO_IO_DOORS(&frame->held);
	return true;
}

/* chunks_packs_open_made_dir_body_0 is made_dir's body. */
static void
chunks_packs_open_made_dir_body_0(void *frame_)
{
	chunks_packs_open_spaced_frame_0_t *frame = frame_;
	HostIoIoDoors doors;
	StdStr		parent;

	if (!chunks_packs_open_spaced_lock(frame, &doors))
		return;
	parent = host_directory_of(frame->dir, frame->dir_len);
	frame->ok = chunks_file_create_dir(doors, parent.ptr, parent.len, frame->error) &&
		chunks_file_create_dir(doors, frame->dir, frame->dir_len, frame->error);
	chunks_packs_open_spaced_pad_0(frame);
}

/*
 * chunks_packs_open_made_dir makes a repository's directory in a tablespace,
 * and first the tablespace's directory of repositories it goes in, under the
 * lock the removal of an empty one takes.
 */
bool
chunks_packs_open_made_dir(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	return chunks_packs_open_spaced(io, NULL, 0, dir, dir_len, chunks_packs_open_made_dir_body_0, error);
}

/*
 * chunks_packs_open_emptied_frame_0_t is emptied's state across its pad: its
 * arguments, its result, and the tablespaces, guard, directory, and names it
 * holds.
 */
typedef struct chunks_packs_open_emptied_frame_0_t
{
	HostIoIo   *io;
	ChunksErrorError *error;
	bool		ok;
	StdVec		spaces;
	HostSharedLongGuard held;
	bool		held_live;
	StdPathBuf	dir;
	StdVec		names;
} chunks_packs_open_emptied_frame_0_t;

/* chunks_packs_open_emptied_pad_0 is emptied's pad: the names, the directory, the guard, then the tablespaces. */
static void
chunks_packs_open_emptied_pad_0(void *frame_)
{
	chunks_packs_open_emptied_frame_0_t *frame = frame_;

	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_LongGuard_drop(&frame->held);
	}
	std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
}

/* chunks_packs_open_emptied_body_0 is emptied's body. */
static void
chunks_packs_open_emptied_body_0(void *frame_)
{
	chunks_packs_open_emptied_frame_0_t *frame = frame_;
	HostSharedLongIo held_io;
	HostIoIoDoors doors;
	HostError	host;

	if (!host_tablespaces(HOST_IO_IO_READ_DOORS(frame->io), &frame->spaces, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (!chunks_packs_open_spaces_lock(frame->io, &frame->held, frame->error))
		goto done;
	frame->held_live = true;
	held_io = host_shared_LongGuard_io(&frame->held);
	doors = HOST_SHARED_LONG_IO_IO_DOORS(&held_io);
	frame->ok = true;
	for (size_t k = 0, k_len = frame->spaces.len; k < k_len && frame->ok; k++)
	{
		bool		has_names;
		bool		removed;

		frame->dir = host_versioned_dir(STD_VEC_AT(&frame->spaces, uint32, k));
		frame->ok = host_list_dir_if_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->dir.bytes.ptr,
										   frame->dir.bytes.len, &has_names, &frame->names, &host);
		if (frame->ok && has_names)
		{
			if (frame->names.len == 0)
				frame->ok = host_remove_tree(doors, frame->dir.bytes.ptr, frame->dir.bytes.len, &removed, &host);
			std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
			std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
		}
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
		std_PathBuf_new(&frame->dir);
		if (!frame->ok)
			*frame->error = chunks_error_Error_FromError_from(&host);
	}
done:
	chunks_packs_open_emptied_pad_0(frame);
}

/*
 * chunks_packs_open_emptied removes each tablespace's directory of
 * repositories that holds none, the data directory's aside, under the lock
 * every make in one takes.
 */
bool
chunks_packs_open_emptied(HostIoIo *io, ChunksErrorError *error)
{
	chunks_packs_open_emptied_frame_0_t frame = {.io = io, .error = error};

	std_vec_new(&frame.spaces, sizeof(uint32), _Alignof(uint32));
	std_PathBuf_new(&frame.dir);
	std_vec_new(&frame.names, sizeof(StdString), _Alignof(StdString));
	std_landing(&frame, chunks_packs_open_emptied_body_0, chunks_packs_open_emptied_pad_0);
	return frame.ok;
}

/* chunks_packs_open_making takes chunks' single record's makers' lock, Making, with the record. */
static bool
chunks_packs_open_making(HostIoIo *io, HostSharedRecord *makers, HostSharedGuard *result, ChunksErrorError *error)
{
	HostError	host;

	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, makers, &host) ||
		!host_shared_Record_lock(*makers, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SINGLE_MAKING_INDEX, result, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/*
 * chunks_packs_open_forgot_frame_0_t is forgot's state across its pad: its
 * arguments, its result, and the guard, sweep's list, and record it holds.
 */
typedef struct chunks_packs_open_forgot_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		from;
	ChunksErrorError *error;
	bool		ok;
	HostSharedGuard making;
	bool		making_live;
	StdVec		swept;
	HostSharedOwned owned;
	bool		owned_live;
} chunks_packs_open_forgot_frame_0_t;

/* chunks_packs_open_forgot_pad_0 is forgot's pad: the record, the sweep's list, then the guard. */
static void
chunks_packs_open_forgot_pad_0(void *frame_)
{
	chunks_packs_open_forgot_frame_0_t *frame = frame_;

	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
}

/* chunks_packs_open_forgot_body_0 is forgot's body. */
static void
chunks_packs_open_forgot_body_0(void *frame_)
{
	chunks_packs_open_forgot_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	HostError	host;

	if (!chunks_packs_open_making(frame->io, &makers, &frame->making, frame->error))
		return;
	frame->making_live = true;
	chunks_packs_shared_add(makers, CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX, 1);
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &frame->swept, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		std_TupleU64U64_t pair = STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		claimed;
		HostSharedRecord record;

		if (pair.f1 != frame->repository)
			continue;
		if (!host_shared_claim(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
							   pair.f0, &claimed, &frame->owned, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		if (!claimed)
			continue;
		frame->owned_live = true;
		record = host_shared_Owned_record(&frame->owned);
		frame->owned_live = false;
		if (chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX) == frame->repository &&
			chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_OWN_INDEX) == frame->from)
			host_shared_free(&frame->owned);
		else
			host_shared_Owned_drop(&frame->owned);
	}
	frame->ok = true;
done:
	chunks_packs_open_forgot_pad_0(frame);
}

/*
 * chunks_packs_open_forgot frees the pack record a repository has whose own
 * directory is in the tablespace it moved from, whatever processes hold it,
 * giving back one whose own directory is in another tablespace.
 */
bool
chunks_packs_open_forgot(HostIoIo *io, uint32 repository, uint32 from, ChunksErrorError *error)
{
	chunks_packs_open_forgot_frame_0_t frame = {.io = io, .repository = repository, .from = from, .error = error};

	std_vec_new(&frame.swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_landing(&frame, chunks_packs_open_forgot_body_0, chunks_packs_open_forgot_pad_0);
	return frame.ok;
}

/* chunks_packs_open_moved_in_body_0 is moved_in's body. */
static void
chunks_packs_open_moved_in_body_0(void *frame_)
{
	chunks_packs_open_spaced_frame_0_t *frame = frame_;
	HostIoIoDoors doors;
	bool		copy_there;
	bool		dir_there;
	HostError	host;

	if (!chunks_packs_open_spaced_lock(frame, &doors))
		return;
	if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->copy, frame->copy_len, &copy_there,
							   frame->error))
		goto done;
	if (copy_there)
	{
		StdStr		parent = host_directory_of(frame->dir, frame->dir_len);

		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->dir, frame->dir_len, &dir_there,
								   frame->error))
			goto done;
		if (!dir_there &&
			(!host_rename(doors, frame->copy, frame->copy_len, frame->dir, frame->dir_len, &host) ||
			 !host_sync_path(doors, frame->dir, frame->dir_len, true, &host) ||
			 !host_sync_path(doors, parent.ptr, parent.len, true, &host)))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
	}
	frame->ok = true;
done:
	chunks_packs_open_spaced_pad_0(frame);
}

/*
 * chunks_packs_open_moved_in renames a repository's copy to the repository's
 * own name, synced with its parent, under the Spaces lock, when the copy
 * stands and the repository doesn't.
 */
bool
chunks_packs_open_moved_in(HostIoIo *io, const char *copy, size_t copy_len, const char *dir, size_t dir_len,
						   ChunksErrorError *error)
{
	return chunks_packs_open_spaced(io, copy, copy_len, dir, dir_len, chunks_packs_open_moved_in_body_0, error);
}

/* chunks_packs_open_moving_from_body_0 is moving_from's body. */
static void
chunks_packs_open_moving_from_body_0(void *frame_)
{
	chunks_packs_open_spaced_frame_0_t *frame = frame_;
	HostIoIoDoors doors;
	bool		there;
	bool		marked;

	if (!chunks_packs_open_spaced_lock(frame, &doors))
		return;
	std_path_join(frame->dir, frame->dir_len, CHUNKS_PACKS_OPEN_MOVED_FROM, strlen(CHUNKS_PACKS_OPEN_MOVED_FROM),
				  &frame->mark);
	if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->dir, frame->dir_len, &there, frame->error))
		goto done;
	if (there)
	{
		if (!chunks_file_exists(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->mark.bytes.ptr, frame->mark.bytes.len,
								&marked, frame->error))
			goto done;
		if (!marked)
		{
			if (!chunks_file_open_rw(doors, frame->mark.bytes.ptr, frame->mark.bytes.len, true, &frame->file,
									 frame->error))
				goto done;
			frame->file_live = true;
		}
	}
	frame->ok = true;
done:
	chunks_packs_open_spaced_pad_0(frame);
}

/*
 * chunks_packs_open_moving_from writes the mark of a move's finish in the
 * directory the repository moves from, durably, under the Spaces lock, when
 * the directory stands and has no mark.
 */
bool
chunks_packs_open_moving_from(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	return chunks_packs_open_spaced(io, NULL, 0, dir, dir_len, chunks_packs_open_moving_from_body_0, error);
}

/* chunks_packs_open_moved_from_body_0 is moved_from's body. */
static void
chunks_packs_open_moved_from_body_0(void *frame_)
{
	chunks_packs_open_spaced_frame_0_t *frame = frame_;
	HostIoIoDoors doors;
	bool		has_names;
	HostError	host;
	bool		removed;

	if (!chunks_packs_open_spaced_lock(frame, &doors))
		return;
	if (!host_list_dir_if_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->dir, frame->dir_len, &has_names,
								&frame->names, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	if (!has_names)
	{
		frame->ok = true;
		goto done;
	}
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);
		bool		there;
		bool		gone;

		if (name->len == strlen(CHUNKS_PACKS_OPEN_MOVED_FROM) &&
			memcmp(name->ptr, CHUNKS_PACKS_OPEN_MOVED_FROM, name->len) == 0)
			continue;
		std_path_join(frame->dir, frame->dir_len, name->ptr, name->len, &frame->path);
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(doors), frame->path.bytes.ptr, frame->path.bytes.len,
								   &there, frame->error))
			goto done;
		if (there ?
			!host_remove_tree(doors, frame->path.bytes.ptr, frame->path.bytes.len, &removed, &host) ||
			!host_sync_path(doors, frame->dir, frame->dir_len, true, &host) :
			!host_remove_durably_if_there(doors, frame->path.bytes.ptr, frame->path.bytes.len, &gone, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		std_PathBuf_new(&frame->path);
	}
	frame->ok = host_remove_tree(doors, frame->dir, frame->dir_len, &removed, &host);
	if (!frame->ok)
		*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_open_spaced_pad_0(frame);
}

/*
 * chunks_packs_open_moved_from removes the directory a repository moved
 * from, each file but the mark first, a directory of its own whole, and then
 * the directory with the mark, under the Spaces lock.
 */
bool
chunks_packs_open_moved_from(HostIoIo *io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	return chunks_packs_open_spaced(io, NULL, 0, dir, dir_len, chunks_packs_open_moved_from_body_0, error);
}

/*
 * chunks_packs_open_dropping_frame_0_t is dropping's state across its pad:
 * its arguments, its result, and the directory it holds.
 */
typedef struct chunks_packs_open_dropping_frame_0_t
{
	HostIoReadDoors io;
	uint32		repository;
	uint32		space;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
} chunks_packs_open_dropping_frame_0_t;

/* chunks_packs_open_dropping_pad_0 is dropping's pad: the directory. */
static void
chunks_packs_open_dropping_pad_0(void *frame_)
{
	chunks_packs_open_dropping_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_open_dropping_body_0 is dropping's body. */
static void
chunks_packs_open_dropping_body_0(void *frame_)
{
	chunks_packs_open_dropping_frame_0_t *frame = frame_;

	frame->dir = host_tablespace_dir(frame->repository, frame->space);
	frame->ok = chunks_packs_dropped_marked(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, frame->result,
											frame->error);
	chunks_packs_open_dropping_pad_0(frame);
}

/* chunks_packs_open_dropping reports whether a repository's directory in a tablespace carries a drop's mark. */
bool
chunks_packs_open_dropping(HostIoReadDoors io, uint32 repository, uint32 space, bool *result, ChunksErrorError *error)
{
	chunks_packs_open_dropping_frame_0_t frame = {.io = io, .repository = repository, .space = space,
	.result = result, .error = error};

	std_PathBuf_new(&frame.dir);
	std_landing(&frame, chunks_packs_open_dropping_body_0, chunks_packs_open_dropping_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_Packs_create_frame_0_t is create's state across its pad:
 * its arguments, its result, and the directory, path, and lists it holds.
 */
typedef struct chunks_packs_open_Packs_create_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	ChunksPacksBackendPacks *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	dir;
	StdPathBuf	path;
	ChunksPacksViewReadTable table;
	ChunksPacksViewReadRuns forwarding;
} chunks_packs_open_Packs_create_frame_0_t;

/* chunks_packs_open_Packs_create_pad_0 is create's pad: the lists, the path, then the directory. */
static void
chunks_packs_open_Packs_create_pad_0(void *frame_)
{
	chunks_packs_open_Packs_create_frame_0_t *frame = frame_;

	chunks_packs_Runs_drop(&frame->forwarding.list);
	chunks_packs_Runs_Default_default(&frame->forwarding.list);
	chunks_packs_Table_drop(&frame->table.list);
	chunks_packs_Table_Default_default(&frame->table.list);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* chunks_packs_open_Packs_create_body_0 is create's body: the directory, its list files, then the open. */
static void
chunks_packs_open_Packs_create_body_0(void *frame_)
{
	chunks_packs_open_Packs_create_frame_0_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	bool		placing = false;

	frame->dir = host_tablespace_dir(frame->repository, frame->own);
	if (!chunks_packs_open_made_dir(frame->io, frame->dir.bytes.ptr, frame->dir.bytes.len, frame->error))
		goto done;
	chunks_packs_open_joined(&frame->dir, CHUNKS_PACKS_TABLE, &frame->path);
	if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, doors, frame->path.bytes.ptr,
									frame->path.bytes.len, &frame->table.list, &frame->table.read, &placing,
									frame->error))
		goto done;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	chunks_packs_open_joined(&frame->dir, CHUNKS_PACKS_FORWARDING, &frame->path);
	placing = false;
	if (!chunks_packs_lists_written(&CHUNKS_PACKS_VIEW_RUNS_LIST_OPS, doors, frame->path.bytes.ptr,
									frame->path.bytes.len, &frame->forwarding.list, &frame->forwarding.read, &placing,
									frame->error))
		goto done;
	chunks_packs_open_Packs_create_pad_0(frame);
	frame->ok = chunks_packs_open_Packs_open(frame->io, frame->repository, frame->own, frame->result, frame->error);
	return;
done:
	chunks_packs_open_Packs_create_pad_0(frame);
}

/*
 * chunks_packs_open_Packs_create makes the directory of a new repository,
 * writes its list files there, which list no pack and no forward, and opens
 * its packs.
 */
bool
chunks_packs_open_Packs_create(HostIoIo *io, uint32 repository, uint32 own, ChunksPacksBackendPacks *result,
							   ChunksErrorError *error)
{
	chunks_packs_open_Packs_create_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.result = result, .error = error};

	std_PathBuf_new(&frame.dir);
	std_PathBuf_new(&frame.path);
	chunks_packs_Table_Default_default(&frame.table.list);
	chunks_packs_Runs_Default_default(&frame.forwarding.list);
	std_landing(&frame, chunks_packs_open_Packs_create_body_0, chunks_packs_open_Packs_create_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_open_Packs_open_frame_0_t is open's state across its pad: the
 * record it made, the makers' lock's guard, and the record it found.
 */
typedef struct chunks_packs_open_Packs_open_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	ChunksPacksBackendPacks *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedOwned owned;
	bool		owned_live;
	HostSharedGuard making;
	bool		making_live;
	HostSharedHeld held;
	bool		held_live;
} chunks_packs_open_Packs_open_frame_0_t;

/* chunks_packs_open_Packs_open_pad_0 is open's pad: the guard, then the record made, then the record found. */
static void
chunks_packs_open_Packs_open_pad_0(void *frame_)
{
	chunks_packs_open_Packs_open_frame_0_t *frame = frame_;

	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->owned_live)
	{
		frame->owned_live = false;
		host_shared_Owned_drop(&frame->owned);
	}
	if (frame->held_live)
	{
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
	}
}

/*
 * chunks_packs_open_Packs_open_body_0 is open's body: the published record
 * found, or one made from the list files and published under the makers'
 * lock after one more look, made again when a drop or a move's finish left
 * the files meanwhile.
 */
static void
chunks_packs_open_Packs_open_body_0(void *frame_)
{
	chunks_packs_open_Packs_open_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	HostError	host;
	uint64		location;
	bool		found;

	frame->ok = false;
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &makers, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	for (;;)
	{
		uint64		forgotten;

		if (!chunks_packs_open_found(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &found, &frame->held,
									 &location, frame->error))
			goto done;
		if (found)
		{
			frame->held_live = true;
			break;
		}
		forgotten = chunks_packs_shared_load(makers, CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX);
		if (!chunks_packs_open_made(frame->io, frame->repository, frame->own, &frame->owned, frame->error))
			goto done;
		frame->owned_live = true;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-chunks-record-made", &host) ||
			!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SINGLE_MAKING_INDEX,
									 &frame->making, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		frame->making_live = true;
		if (!chunks_packs_open_found(HOST_SHARED_GUARD_WITHIN(&frame->making), frame->repository, &found,
									 &frame->held, &location, frame->error))
			goto done;
		if (found || chunks_packs_shared_load(makers, CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX) != forgotten)
		{
			frame->making_live = false;
			host_shared_Guard_drop(&frame->making);
			frame->owned_live = false;
			host_shared_Owned_drop(&frame->owned);
			if (!found)
				continue;
			frame->held_live = true;
			break;
		}
		frame->owned_live = false;
		location = host_shared_Owned_publish(&frame->owned);
		chunks_trace_emit(frame->repository, chunks_packs_open_record_made, NULL, NULL);
		if (!host_shared_open(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
							  location, &found, &frame->held, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
		if (!found)
		{
			static const char text[] = "a pack record just published isn't there";

			frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
			std_string_from_Checked(&frame->error->u.invariant, text, sizeof(text) - 1);
			goto done;
		}
		frame->held_live = true;
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
		break;
	}
	frame->held_live = false;
	if (!chunks_packs_backend_Packs_over(frame->io, &frame->held, location, frame->result, frame->error))
		goto done;
	if (frame->result->own != frame->own)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0),
			STD_FMT_LIT("'s pack record names tablespace "), STD_FMT_ARG(1),
		STD_FMT_LIT(", and it is opened in tablespace "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&frame->repository, std_u32_Display_fmt}, {&frame->result->own, std_u32_Display_fmt},
		{&frame->own, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, 6, args};

		frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
		chunks_packs_backend_Packs_drop(frame->result);
		goto done;
	}
	frame->ok = true;
done:
	chunks_packs_open_Packs_open_pad_0(frame);
}

/*
 * chunks_packs_open_Packs_open returns a backend's packs of a repository,
 * over its published pack record, or over the one it makes from the list
 * files, so two processes that open one repository make one record.
 */
bool
chunks_packs_open_Packs_open(HostIoIo *io, uint32 repository, uint32 own, ChunksPacksBackendPacks *result,
							 ChunksErrorError *error)
{
	chunks_packs_open_Packs_open_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.result = result, .error = error};

	std_landing(&frame, chunks_packs_open_Packs_open_body_0, chunks_packs_open_Packs_open_pad_0);
	return frame.ok;
}

/* chunks_packs_open_record_dropped is RecordDropped's event. */
static void
chunks_packs_open_record_dropped(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_RECORD_DROPPED;
}

/* chunks_packs_open_repository_removed is RepositoryRemoved's event. */
static void
chunks_packs_open_repository_removed(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED;
}

/*
 * chunks_packs_open_Packs_dropped_frame_0_t is dropped's state across its
 * pad: the entry it makes, the makers' lock's guard, the sweep's pairs, and
 * the record it claims.
 */
typedef struct chunks_packs_open_Packs_dropped_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	HostSharedOwned entry;
	bool		entry_live;
	HostSharedGuard making;
	bool		making_live;
	StdVec		swept;
	bool		swept_live;
	HostSharedOwned claimed;
	bool		claimed_live;
	StdPathBuf	dir;
	bool		dir_live;
} chunks_packs_open_Packs_dropped_frame_0_t;

/* chunks_packs_open_Packs_dropped_pad_0 is dropped's pad: what it holds, newest first. */
static void
chunks_packs_open_Packs_dropped_pad_0(void *frame_)
{
	chunks_packs_open_Packs_dropped_frame_0_t *frame = frame_;

	if (frame->claimed_live)
	{
		frame->claimed_live = false;
		host_shared_Owned_drop(&frame->claimed);
	}
	if (frame->swept_live)
	{
		frame->swept_live = false;
		std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->entry_live)
	{
		frame->entry_live = false;
		host_shared_Owned_drop(&frame->entry);
	}
	if (frame->dir_live)
	{
		frame->dir_live = false;
		std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	}
}

/*
 * chunks_packs_open_Packs_dropped_body_0 is dropped's body: the entry made
 * and the directory marked before anything changes, then under the makers'
 * lock the record claimed, the entry published, and the record freed, or
 * with no record the directories removed here.
 */
static void
chunks_packs_open_Packs_dropped_body_0(void *frame_)
{
	chunks_packs_open_Packs_dropped_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	HostError	host;
	uint64		location = 0;
	bool		there;

	frame->ok = false;
	if (!host_shared_make(&CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS, frame->io, &frame->entry, &host))
		goto failed;
	frame->entry_live = true;
	frame->dir = host_tablespace_dir(frame->repository, frame->own);
	frame->dir_live = true;
	if (!chunks_packs_dropped_mark(HOST_IO_IO_IO_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len,
								   frame->error))
		goto done;
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &makers, &host) ||
		!host_shared_Record_lock(makers, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SINGLE_MAKING_INDEX,
								 &frame->making, &host))
		goto failed;
	frame->making_live = true;
	chunks_packs_shared_add(makers, CHUNKS_PACKS_SINGLE_FORGOTTEN_INDEX, 1);
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &frame->swept, &host))
		goto failed;
	frame->swept_live = true;
	for (size_t k = 0, k_len = frame->swept.len; k < k_len; k++)
	{
		std_TupleU64U64_t pair = STD_VEC_AT(&frame->swept, std_TupleU64U64_t, k);
		bool		claimed;

		if (pair.f1 != frame->repository)
			continue;
		if (!host_shared_claim(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
							   pair.f0, &claimed, &frame->claimed, &host))
			goto failed;
		if (!claimed)
			continue;
		if (chunks_packs_shared_load(host_shared_Owned_record(&frame->claimed), CHUNKS_PACKS_SHARED_REPOSITORY_INDEX) ==
			frame->repository)
		{
			frame->claimed_live = true;
			location = pair.f0;
			break;
		}
		host_shared_Owned_drop(&frame->claimed);
	}
	frame->swept_live = false;
	std_vec_free_Checked(&frame->swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	if (frame->claimed_live)
	{
		HostSharedRecord record = host_shared_Owned_record(&frame->entry);
		bool		freed;

		chunks_packs_shared_add(record, CHUNKS_PACKS_DROPPED_NAMED_INDEX, frame->repository);
		chunks_packs_shared_add(record, CHUNKS_PACKS_DROPPED_SPACE_INDEX, frame->own);
		chunks_packs_shared_add(record, CHUNKS_PACKS_DROPPED_AT_INDEX, location);
		frame->entry_live = false;
		host_shared_Owned_publish(&frame->entry);
		frame->claimed_live = false;
		host_shared_free(&frame->claimed);
		chunks_trace_emit(frame->repository, chunks_packs_open_record_dropped, NULL, NULL);
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
		if (!chunks_packs_dropped_remove_dropped(frame->io, &freed, frame->error))
			goto done;
	}
	else
	{
		StdVec		entries;
		bool		dropping = false;
		bool		freed;
		bool		removed;

		if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making),
							   &CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS, CHUNKS_PACKS_DROPPED_NAMED_INDEX,
							   &entries, &host))
			goto failed;
		for (size_t k = 0, k_len = entries.len; k < k_len && !dropping; k++)
			dropping = STD_VEC_AT(&entries, std_TupleU64U64_t, k).f1 == frame->repository;
		std_vec_free_Checked(&entries, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
		frame->entry_live = false;
		host_shared_Owned_drop(&frame->entry);
		if (dropping)
		{
			if (!chunks_packs_dropped_remove_dropped(frame->io, &freed, frame->error))
				goto done;
		}
		else
		{
			if (!chunks_packs_dropped_remove_directories(frame->io, frame->repository, frame->own, &removed,
														 frame->error))
				goto done;
			chunks_trace_emit(frame->repository, chunks_packs_open_repository_removed, NULL, NULL);
		}
	}
	if (!chunks_file_dir_there(HOST_IO_IO_READ_DOORS(frame->io), frame->dir.bytes.ptr, frame->dir.bytes.len, &there,
							   frame->error))
		goto done;
	*frame->result = !there;
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_open_Packs_dropped_pad_0(frame);
}

/*
 * chunks_packs_open_Packs_dropped drops a repository and reports whether its
 * files are gone: its entry made and its own directory marked, durably,
 * before anything changes, then its record claimed and freed under the
 * makers' lock, or with no record and no earlier drop's entry its
 * directories removed here.
 */
bool
chunks_packs_open_Packs_dropped(HostIoIo *io, uint32 repository, uint32 own, bool *result, ChunksErrorError *error)
{
	chunks_packs_open_Packs_dropped_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.result = result, .error = error};

	std_landing(&frame, chunks_packs_open_Packs_dropped_body_0, chunks_packs_open_Packs_dropped_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_PACKS_OPEN_TESTS_RECORDS is RECORDS: how many pack records the simulated server's pool has, three for each of its 64 processes. */
#define CHUNKS_PACKS_OPEN_TESTS_RECORDS ((uint32) (3 * 64))

/*
 * test_chunks_packs_open_tests_a_list_file_that_is_not_whole_makes_no_record:
 * a list file gone with its previous generation is refused as a settle
 * refuses it, and one cut into is refused, since only the startup process
 * settles one; neither leaves a record made.
 */
void
test_chunks_packs_open_tests_a_list_file_that_is_not_whole_makes_no_record(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	path;
	HostSharedOwned owned;
	StdVec		whole;
	StdIoError	io_error;
	ChunksErrorError error;
	StdString	refusal;
	StdStr		shown;
	bool		refused;
	StdFmtPiece pieces[] = {STD_FMT_LIT("required file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" does not exist")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	chunks_packs_fixtures_home(&dir);
	owned = chunks_packs_lists_tests_repository(io, &dir);
	host_shared_free(&owned);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_FORWARDING, strlen(CHUNKS_PACKS_FORWARDING), &path);
	if (!std_fs_read(path.bytes.ptr, path.bytes.len, &whole, &io_error))
		STD_RESULT_EXPECT_FAILED(431, 42, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (!std_fs_remove_file(path.bytes.ptr, path.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(432, 37, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	shown.ptr = path.bytes.ptr;
	shown.len = path.bytes.len;
	std_fmt_format_Checked(&refusal, &arguments);
	refused = !chunks_packs_open_made(io, CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT, &owned, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_CORRUPT && error.u.corrupt.len == refusal.len &&
			memcmp(error.u.corrupt.ptr, refusal.ptr, refusal.len) == 0;
		chunks_error_Error_drop(&error);
	}
	else
		host_shared_Owned_drop(&owned);
	STD_ASSERT(434, 9, refused,
			   "matches!(made(io, id, host::PG_DEFAULT), Err(Error::Corrupt(message)) if message == refusal)");
	std_vec_free_Checked(&refusal, 1, 1);
	std_range_to_check(whole.len - 3, whole.len, RUST_FILE, 435, 32);
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, whole.ptr, whole.len - 3, &io_error))
		STD_RESULT_EXPECT_FAILED(435, 58, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	refused = !chunks_packs_open_made(io, CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT, &owned, &error);
	if (refused)
		chunks_error_Error_drop(&error);
	else
		host_shared_Owned_drop(&owned);
	STD_ASSERT(436, 9, refused, "made(io, id, host::PG_DEFAULT).is_err()");
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, whole.ptr, whole.len, &io_error))
		STD_RESULT_EXPECT_FAILED(437, 39, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	refused = !chunks_packs_open_made(io, CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT, &owned, &error);
	if (refused)
		chunks_error_Error_drop(&error);
	else
		host_shared_Owned_drop(&owned);
	STD_ASSERT(438, 9, !refused, "made(io, id, host::PG_DEFAULT).is_ok()");
	std_vec_free_Checked(&whole, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_open_tests_a_created_repositorys_packs_are_listed_by_the_next_to_open_it:
 * a new repository's packs list nothing; the packs a backend adds are listed
 * by a backend that opens the repository after it, from the list files.
 */
void
test_chunks_packs_open_tests_a_created_repositorys_packs_are_listed_by_the_next_to_open_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 447, 46);
	ChunksPacksBackendPacks mine;
	ChunksPacksBackendPacks theirs;
	ChunksPackPackWriterInDir writer;
	ChunksPackPack finished;
	ChunksAddressAddress one;
	ChunksTrailerLocator at_one;
	ChunksErrorError error;
	StdVec		bytes;
	static const uint64 first[] = {1};

	if (!chunks_packs_open_Packs_create(io, 7, HOST_PG_DEFAULT, &mine, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 449, 60);
	chunks_packs_tests_numbers_eq(RUST_FILE, &mine, io, &reading, NULL, 0, NULL, 450, 9, 47);
	writer = chunks_packs_tests_writer(RUST_FILE, &mine, io, &reading, 451, 72);
	chunks_packs_fixtures_stored((const uint8 *) "one", 3, &one, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, one, bytes.ptr, bytes.len, false, &at_one, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 452, 59);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &finished, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 453, 42);
	if (!chunks_packs_backend_Packs_add(&mine, io, &reading, &finished, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 454, 42);
	chunks_packs_backend_Packs_drop(&mine);
	chunks_packs_tests_opened(RUST_FILE, io, 7, 456, 0, 60, &theirs);
	chunks_packs_tests_numbers_eq(RUST_FILE, &theirs, io, &reading, first, lengthof(first), NULL, 457, 9, 49);
	chunks_packs_tests_found(RUST_FILE, &theirs, io, &reading, &one, &at_one, 458, 9, 52);
	chunks_packs_backend_Packs_drop(&theirs);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_opener is two_backends_that_open_one_repository_make_one_record's actor: an open of repository 61, which keeps where its record was. */
static bool
chunks_packs_open_tests_opener(void *captures, HostIoIo *io, HostError *error)
{
	uint64	   *at = captures;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;

	if (!chunks_packs_open_Packs_open(io, 61, HOST_PG_DEFAULT, &packs, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	*at = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	return true;
}

/*
 * test_chunks_packs_open_tests_two_backends_that_open_one_repository_make_one_record:
 * two backends that open one repository at once, with no record of it
 * published, make one record: each finds it or makes it, a maker that finds
 * another's under the makers' lock gives its own up, and both hold the same
 * one.
 */
void
test_chunks_packs_open_tests_two_backends_that_open_one_repository_make_one_record(void)
{
	for (uint64 seed = 1; seed <= 24; seed++)
	{
		SimcTestIo	test_io = simc_io();
		HostIoIo   *io = &test_io.io;
		ChunksPacksBackendPacks created;
		ChunksErrorError error;
		uint64		location;
		uint64		opened[2] = {0, 0};
		SimcActor	first = {chunks_packs_open_tests_opener, &opened[0]};
		SimcActor	second = {chunks_packs_open_tests_opener, &opened[1]};
		SimcRun		run;
		SimcReport	report;
		StdFmtPiece seed_pieces[] = {STD_FMT_LIT("seed "), STD_FMT_ARG(0), STD_FMT_LIT(": two records of one repository")};
		StdFmtArg	seed_args[] = {{&seed, std_u64_Display_fmt}};
		StdFmtArguments two_records = {seed_pieces, lengthof(seed_pieces), seed_args};

		if (!chunks_packs_open_Packs_create(io, 61, HOST_PG_DEFAULT, &created, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 471, 67);
		location = chunks_packs_backend_Packs_location(&created);
		chunks_packs_backend_Packs_drop(&created);
		chunks_packs_tests_freed(RUST_FILE, io, location, 474, 79, 88);
		run = simc_Run_new(seed);
		simc_Run_actor(&run, "first", 5, first);
		simc_Run_actor(&run, "second", 6, second);
		report = simc_Run_finish(run);
		if (!chunks_packs_tests_returned(&report.outcome))
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("seed "), STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&seed, std_u64_Display_fmt}, {&report.outcome, simc_Outcome_Debug_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 486, 13, &arguments);
		}
		simc_Report_drop(&report);
		if (opened[0] != opened[1])
			std_assert_failed(RUST_FILE, 493, 13, "==", &opened[0], std_u64_Debug_fmt, &opened[1],
							  std_u64_Debug_fmt, &two_records);
		if (opened[0] == location)
			std_assert_failed(RUST_FILE, 494, 13, "!=", &opened[0], std_u64_Debug_fmt, &location,
							  std_u64_Debug_fmt, NULL);
		simc_TestIo_drop(&test_io);
	}
}

/* chunks_packs_open_tests_deferring returns the packs of a new repository with an id that lists one pack whose add is deferred. */
static void
chunks_packs_open_tests_deferring(HostIoIo *io, uint32 id, ChunksPacksBackendPacks *result)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 500, 46);
	ChunksPackPackWriterInDir writer;
	ChunksPackPack pack;
	ChunksAddressAddress address;
	ChunksTrailerLocator at;
	ChunksErrorError error;
	StdVec		bytes;
	uint8		le[4];

	chunks_packs_fixtures_created(io, id, result);
	writer = chunks_packs_tests_writer(RUST_FILE, result, io, &reading, 502, 77);
	std_u32_to_le_bytes(id, le);
	chunks_packs_fixtures_stored(le, sizeof(le), &address, &bytes);
	if (!chunks_pack_PackWriterInDir_write(&writer, io, address, bytes.ptr, bytes.len, false, &at, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 503, 50);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 504, 38);
	if (!chunks_packs_backend_Packs_add_deferred(result, HOST_IO_IO_TAKES_SHORT(io), &reading, &pack, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 505, 48);
	host_shared_Reader_drop(&reading);
}

/* chunks_packs_open_tests_dropped_created makes and drops the packs of new repositories with ids from `from` up to `to`, not `to`. */
static void
chunks_packs_open_tests_dropped_created(HostIoIo *io, uint32 from, uint32 to)
{
	for (uint32 id = from; id < to; id++)
	{
		ChunksPacksBackendPacks packs;

		chunks_packs_fixtures_created(io, id, &packs);
		chunks_packs_backend_Packs_drop(&packs);
	}
}

/*
 * test_chunks_packs_open_tests_a_full_pool_of_idle_records_makes_room_for_a_new_repository:
 * a pool full of records no process holds makes room for a new repository's
 * with no error, and a repository whose record was reclaimed lists, once it
 * is opened again, what it listed.
 */
void
test_chunks_packs_open_tests_a_full_pool_of_idle_records_makes_room_for_a_new_repository(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 518, 46);
	ChunksPacksBackendPacks first;
	ChunksPacksBackendPacks again;
	chunks_TupleAddressLocator_t one;
	ChunksErrorError error;
	static const uint64 numbers[] = {1};

	chunks_packs_fixtures_created(io, 100, &first);
	one = chunks_packs_fixtures_added(io, &first, (const uint8 *) "one", 3);
	if (!chunks_packs_backend_Packs_unreserve(&first, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 521, 39);
	chunks_packs_backend_Packs_drop(&first);
	chunks_packs_open_tests_dropped_created(io, 101, 108 + CHUNKS_PACKS_OPEN_TESTS_RECORDS);
	chunks_packs_tests_opened(RUST_FILE, io, 100, 526, 58, 86, &again);
	chunks_packs_tests_numbers_eq(RUST_FILE, &again, io, &reading, numbers, lengthof(numbers), NULL, 527, 9, 48);
	chunks_packs_tests_found(RUST_FILE, &again, io, &reading, &one.f0, &one.f1, 528, 9, 51);
#ifdef DOLTGRES_CHUNKS_TRACE
	{
		StdVec		events = chunks_trace_take(100);
		bool		reclaimed = false;

		for (size_t k = 0; k < events.len; k++)
			reclaimed = reclaimed || STD_VEC_AT(&events, ChunksTraceEvent, k).tag == CHUNKS_TRACE_EVENT_RECORD_RECLAIMED;
		std_vec_drop_Checked(&events, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent),
							 (void (*) (void *)) chunks_trace_Event_drop);
		STD_ASSERT_MSG(530, 9, reclaimed, "no reclaim was traced");
	}
#endif
	chunks_packs_backend_Packs_drop(&again);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_bound_of returns the bound the table's file of the repository with an id holds. */
static uint64
chunks_packs_open_tests_bound_of(HostIoIo *io, uint32 id)
{
	StdPathBuf	dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
	StdPathBuf	path;
	ChunksPacksViewReadTable table;
	ChunksErrorError error;
	uint64		bound;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	chunks_packs_Table_Default_default(&table.list);
	if (!chunks_packs_open_read_whole(&CHUNKS_PACKS_VIEW_TABLE_LIST_OPS, HOST_IO_IO_READ_DOORS(io), path.bytes.ptr,
									  path.bytes.len, &table.list, &table.read, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 535, 90);
	bound = table.list.reserved;
	chunks_packs_Table_drop(&table.list);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return bound;
}

/* chunks_packs_open_tests_number_bound_eq asserts at a line that a writer's number and a repository's bound are those given. */
static void
chunks_packs_open_tests_number_bound_eq(uint64 number, uint64 bound, uint64 want_number, uint64 want_bound, uint32 line)
{
	StdTupleRef got = STD_TUPLE(2, STD_ITEM(&number, std_u64_Debug_fmt), STD_ITEM(&bound, std_u64_Debug_fmt));
	StdTupleRef want = STD_TUPLE(2, STD_ITEM(&want_number, std_u64_Debug_fmt),
								 STD_ITEM(&want_bound, std_u64_Debug_fmt));

	if (number != want_number || bound != want_bound)
		std_assert_failed(RUST_FILE, line, 9, "==", &got, std_tuple_Debug_fmt, &want, std_tuple_Debug_fmt, NULL);
}

/*
 * test_chunks_packs_open_tests_a_record_made_again_gives_no_number_given_before:
 * a record made again from the list files, as after a crash, gives no number
 * the record before it gave: a writer's number is under a bound the table's
 * file held first, so the next record starts at that bound, past a pack
 * whose add the files never got and a writer that never added.
 */
void
test_chunks_packs_open_tests_a_record_made_again_gives_no_number_given_before(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 545, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks again;
	ChunksPackPackWriterInDir writer;
	uint64		location;
	uint64		reserved;
	uint64		ahead = CHUNKS_PACKS_SHARED_AHEAD;

	chunks_packs_open_tests_deferring(io, 401, &packs);
	writer = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 547, 56);
	chunks_packs_open_tests_number_bound_eq(chunks_pack_PackWriterInDir_number(&writer),
											chunks_packs_open_tests_bound_of(io, 401), 2, CHUNKS_PACKS_SHARED_AHEAD,
											548);
	chunks_pack_PackWriterInDir_drop(&writer);
	location = chunks_packs_backend_Packs_location(&packs);
	chunks_packs_backend_Packs_drop(&packs);
	chunks_packs_tests_freed(RUST_FILE, io, location, 552, 75, 84);
	chunks_packs_tests_opened(RUST_FILE, io, 401, 553, 58, 86, &again);
	reserved = chunks_packs_backend_Packs_reserved(&again);
	STD_ASSERT_EQ(554, 9, reserved == ahead, &reserved, std_u64_Debug_fmt, &ahead, std_u64_Debug_fmt);
	writer = chunks_packs_tests_writer(RUST_FILE, &again, io, &reading, 555, 50);
	chunks_packs_open_tests_number_bound_eq(chunks_pack_PackWriterInDir_number(&writer),
											chunks_packs_open_tests_bound_of(io, 401), CHUNKS_PACKS_SHARED_AHEAD + 1,
											2 * CHUNKS_PACKS_SHARED_AHEAD, 556);
	chunks_pack_PackWriterInDir_drop(&writer);
	chunks_packs_backend_Packs_drop(&again);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_open_tests_a_full_pool_of_records_with_bounds_ahead_makes_room_by_writing_one_back:
 * a pool full of records no process holds whose bounds are ahead of the
 * numbers they gave makes room by setting one record's bound back in its
 * table's file and freeing it, so that record made again gives the very next
 * number; every other record keeps its bound.
 */
void
test_chunks_packs_open_tests_a_full_pool_of_records_with_bounds_ahead_makes_room_by_writing_one_back(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 566, 46);
	HostSharedOwned owned;
	ChunksPacksBackendPacks again;
	ChunksPackPackWriterInDir writer;
	StdVec		back;
	uint64		number;
	uint64		two = 2;

	for (uint32 id = 501; id <= 500 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
	{
		ChunksPacksBackendPacks packs;

		chunks_packs_fixtures_created(io, id, &packs);
		writer = chunks_packs_tests_writer(RUST_FILE, &packs, io, &reading, 569, 48);
		chunks_pack_PackWriterInDir_drop(&writer);
		chunks_packs_backend_Packs_drop(&packs);
	}
	owned = chunks_packs_tests_made(RUST_FILE, io, 571, 36);
	host_shared_Owned_drop(&owned);
	std_vec_new(&back, sizeof(uint64), _Alignof(uint64));
	for (uint32 id = 501; id <= 500 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
	{
		uint64		bound = chunks_packs_open_tests_bound_of(io, id);
		uint64		ahead = CHUNKS_PACKS_SHARED_AHEAD;
		uint64		at = id;

		if (bound == 1)
			std_vec_push_Checked(&back, &at, sizeof(at), _Alignof(uint64));
		else if (bound != ahead)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&at, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, 576, 26, "==", &bound, std_u64_Debug_fmt, &ahead, std_u64_Debug_fmt,
							  &arguments);
		}
	}
	if (back.len != 1)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&back, chunks_packs_tests_Numbers_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 579, 43, &arguments);
	}
	chunks_packs_tests_opened(RUST_FILE, io, (uint32) STD_VEC_AT(&back, uint64, 0), 580, 58, 86, &again);
	writer = chunks_packs_tests_writer(RUST_FILE, &again, io, &reading, 581, 50);
	number = chunks_pack_PackWriterInDir_number(&writer);
	STD_ASSERT_EQ(581, 9, number == two, &number, std_u64_Debug_fmt, &two, std_u64_Debug_fmt);
	chunks_pack_PackWriterInDir_drop(&writer);
	chunks_packs_backend_Packs_drop(&again);
	std_vec_free_Checked(&back, sizeof(uint64), _Alignof(uint64));
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_make_fails makes a record with a full disk at the next call, which must fail with no internal error, asserted at lines. */
static void
chunks_packs_open_tests_make_fails(HostIoIo *io, uint64 seed, const char *expect, uint32 expect_line,
								   uint32 expect_col, uint32 ne_line)
{
	StdPathBuf	data = simc_data_dir();
	SimcSim		sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, seed);
	HostSharedOwned owned;
	HostError	failed;
	bool		made;

	std_vec_free_Checked(&data.bytes, 1, 1);
	simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + 1);
	made = host_shared_make(&CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, io, &owned, &failed);
	if (made)
	{
		host_shared_Owned_drop(&owned);
		std_option_expect_failed(expect, strlen(expect), RUST_FILE, expect_line, expect_col);
	}
	simc_Sim_stop(&sim);
	simc_Sim_drop(&sim);
	if (memcmp(host_Error_sqlstate(&failed), "XX000", 5) == 0)
	{
		StdBytes	got = {host_Error_sqlstate(&failed), 5};
		StdBytes	internal = {(const uint8 *) "XX000", 5};
		StdStr		message = host_Error_message(&failed);
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&message, std_str_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, ne_line, 9, "!=", &got, std_bytes_Debug_fmt, &internal, std_bytes_Debug_fmt,
						  &arguments);
	}
	host_Error_drop(&failed);
}

/*
 * test_chunks_packs_open_tests_a_full_pool_of_records_that_defer_adds_makes_room_by_appending_ones:
 * a pool full of records no process holds, each deferring an add, makes room
 * by appending one record's deferred adds and freeing it: with that append
 * made to fail the make raises the write's error and the record keeps its
 * adds, and without it the make succeeds, and every repository still lists
 * its pack.
 */
void
test_chunks_packs_open_tests_a_full_pool_of_records_that_defer_adds_makes_room_by_appending_ones(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 591, 46);
	HostSharedOwned owned;
	static const uint64 numbers[] = {1};

	for (uint32 id = 201; id <= 200 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
	{
		ChunksPacksBackendPacks packs;

		chunks_packs_open_tests_deferring(io, id, &packs);
		chunks_packs_backend_Packs_drop(&packs);
	}
	chunks_packs_open_tests_make_fails(io, 23, "a make whose append fails raises", 597, 50, 599);
	owned = chunks_packs_tests_made(RUST_FILE, io, 600, 36);
	host_shared_Owned_drop(&owned);
	for (uint32 id = 201; id <= 200 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
	{
		ChunksPacksBackendPacks packs;
		uint64		repository = id;
		StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&repository, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		chunks_packs_tests_opened(RUST_FILE, io, id, 602, 61, 89, &packs);
		chunks_packs_tests_numbers_eq(RUST_FILE, &packs, io, &reading, numbers, lengthof(numbers), &arguments, 603, 13, 52);
		chunks_packs_backend_Packs_drop(&packs);
	}
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_open_tests_a_closed_repositorys_deferred_adds_are_in_its_list_files:
 * a repository's packs closed with an add deferred leave the add's records
 * in the list files, so a record made again from them lists the pack.
 */
void
test_chunks_packs_open_tests_a_closed_repositorys_deferred_adds_are_in_its_list_files(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 613, 46);
	ChunksPacksBackendPacks packs;
	ChunksPacksBackendPacks again;
	ChunksErrorError error;
	uint64		location;
	uint64		now;
	static const uint64 numbers[] = {1};

	chunks_packs_open_tests_deferring(io, 62, &packs);
	location = chunks_packs_backend_Packs_location(&packs);
	if (!chunks_packs_backend_Packs_close(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 616, 35);
	chunks_packs_tests_freed(RUST_FILE, io, location, 617, 61, 70);
	chunks_packs_tests_opened(RUST_FILE, io, 62, 618, 57, 85, &again);
	now = chunks_packs_backend_Packs_location(&again);
	if (now == location)
		std_assert_failed(RUST_FILE, 619, 9, "!=", &now, std_u64_Debug_fmt, &location, std_u64_Debug_fmt, NULL);
	chunks_packs_tests_numbers_eq(RUST_FILE, &again, io, &reading, numbers, lengthof(numbers), NULL, 620, 9, 48);
	chunks_packs_backend_Packs_drop(&again);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_reclaim_maker is the maker actor: a new repository's create, which makes the pool reclaim. */
static bool
chunks_packs_open_tests_reclaim_maker(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendPacks made;
	ChunksErrorError failed;

	(void) captures;
	if (!chunks_packs_open_Packs_create(io, 300 + CHUNKS_PACKS_OPEN_TESTS_RECORDS, HOST_PG_DEFAULT, &made, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	chunks_packs_backend_Packs_drop(&made);
	return true;
}

/* chunks_packs_open_tests_reclaim_opener is the opener actor: an open of the record at the location the reclaim looks at, and an add through it. */
static bool
chunks_packs_open_tests_reclaim_opener(void *captures, HostIoIo *io, HostError *error)
{
	const uint64 *location = captures;
	HostSharedReader reading;
	HostSharedLook look;
	bool		theirs;
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	chunks_TupleAddressLocator_t one;
	ChunksTrailerLocator found;
	bool		has;
	bool		same;
	bool		ok;

	if (!host_shared_read(io, &reading, error))
		return false;
	if (!host_shared_reclaim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, *location,
							 &look, error))
		goto reading;
	theirs = look.tag == HOST_SHARED_LOOK_THEIRS;
	if (look.tag == HOST_SHARED_LOOK_MINE)
		host_shared_Reclaim_drop(&look.u.mine);
	if (!theirs)
	{
		*error = chunks_packs_tests_failed_text("the reclaim isn't looking at the first repository's record");
		goto reading;
	}
	if (!chunks_packs_open_Packs_open(io, 300, HOST_PG_DEFAULT, &packs, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		goto reading;
	}
	one = chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-opened", error))
		goto packs;
	same = chunks_packs_backend_Packs_location(&packs) == *location;
	ok = chunks_packs_backend_Packs_find(&packs, io, &reading, &one.f0, &has, &found, &failed);
	if (ok && same && has && chunks_trailer_Locator_PartialEq_eq(&found, &one.f1))
	{
		chunks_packs_backend_Packs_drop(&packs);
		host_shared_Reader_drop(&reading);
		return true;
	}
	{
		StdString	message;
		StdOptionRef some = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		ChunksPacksTestsResult result = {ok, ok ? STD_ITEM(&some, std_option_Debug_fmt) :
		STD_ITEM(&failed, chunks_error_Error_Debug_fmt)};
		StdFmtPiece pieces[] = {STD_FMT_LIT("the record is the one made first: "), STD_FMT_ARG(0),
		STD_FMT_LIT("; found "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&same, std_bool_Display_fmt}, {&result, chunks_packs_tests_Result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		if (!ok)
			chunks_error_Error_drop(&failed);
		*error = chunks_packs_tests_failed(&message);
	}
packs:
	chunks_packs_backend_Packs_drop(&packs);
reading:
	host_shared_Reader_drop(&reading);
	return false;
}

/*
 * test_chunks_packs_open_tests_a_backend_opens_a_repository_while_a_reclaim_looks_at_its_record:
 * a backend that opens a repository while a pool's reclaim is looking at its
 * record finds that record, and the reclaim leaves it and frees another: the
 * backend reads through its packs what it added to them.
 */
void
test_chunks_packs_open_tests_a_backend_opens_a_repository_while_a_reclaim_looks_at_its_record(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks first;
	uint64		location;
	SimcActor	maker = {chunks_packs_open_tests_reclaim_maker, NULL};
	SimcActor	opener = {chunks_packs_open_tests_reclaim_opener, &location};
	SimcStep	steps[] = {{"maker", 5, PG_UINT64_MAX}, {"opener", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;
	StdVec		swept;
	HostError	host;
	bool		kept = false;

	chunks_packs_fixtures_created(io, 300, &first);
	location = chunks_packs_backend_Packs_location(&first);
	chunks_packs_backend_Packs_drop(&first);
	chunks_packs_open_tests_dropped_created(io, 301, 300 + CHUNKS_PACKS_OPEN_TESTS_RECORDS);
	run = simc_Run_new(17);
	simc_Run_actor(&run, "maker", 5, maker);
	simc_Run_actor(&run, "opener", 6, opener);
	simc_Run_until(&run, "maker", 5, "doltgres-chunks-reclaim-looked");
	simc_Run_until(&run, "opener", 6, "doltgres-chunks-case-opened");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	chunks_packs_tests_ended(RUST_FILE, &report, 658, 9);
	simc_Report_drop(&report);
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &swept, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 664, 52);
	for (size_t k = 0; k < swept.len; k++)
		kept = kept || (STD_VEC_AT(&swept, std_TupleU64U64_t, k).f0 == location &&
						STD_VEC_AT(&swept, std_TupleU64U64_t, k).f1 == 300);
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	STD_ASSERT_MSG(664, 9, kept, "the record was freed");
#ifdef DOLTGRES_CHUNKS_TRACE
	{
		StdVec		events = chunks_trace_take(300);
		bool		reclaimed = false;

		for (size_t k = 0; k < events.len; k++)
			reclaimed = reclaimed || STD_VEC_AT(&events, ChunksTraceEvent, k).tag == CHUNKS_TRACE_EVENT_RECORD_RECLAIMED;
		std_vec_drop_Checked(&events, sizeof(ChunksTraceEvent), _Alignof(ChunksTraceEvent),
							 (void (*) (void *)) chunks_trace_Event_drop);
		STD_ASSERT_MSG(666, 9, !reclaimed, "the reclaim of a record a backend opened was traced");
	}
#endif
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_manifest returns the path of the manifest of the repository with an id. */
static StdPathBuf
chunks_packs_open_tests_manifest(uint32 id)
{
	StdPathBuf	dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
	StdPathBuf	path;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return path;
}

/* chunks_packs_open_tests_even reports whether a repository's manifest's two generations hold the same bytes. */
static bool
chunks_packs_open_tests_even(HostIoIo *io, uint32 id)
{
	StdPathBuf	path = chunks_packs_open_tests_manifest(id);
	ChunksErrorError error;
	bool		has;
	bool		has_fallback;
	bool		has_previous;
	StdVec		newest;
	StdVec		previous;
	ChunksFileFallback fallback;
	bool		even;

	if (!chunks_file_read_kept(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has, &newest,
							   &has_fallback, &fallback, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 683, 64);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(683, 73);
	if (!chunks_file_read_previous(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has_previous, &previous,
								   &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 684, 55);
	even = has_previous && previous.len == newest.len && memcmp(previous.ptr, newest.ptr, newest.len) == 0;
	if (has_previous)
		std_vec_free_Checked(&previous, 1, 1);
	std_vec_free_Checked(&newest, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return even;
}

/* chunks_packs_open_tests_evened returns the repositories but the first whose manifests' generations hold the same bytes. */
static StdVec
chunks_packs_open_tests_evened(HostIoIo *io)
{
	StdVec		evened;

	std_vec_new(&evened, sizeof(uint64), _Alignof(uint64));
	for (uint32 id = 402; id <= 400 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
		if (chunks_packs_open_tests_even(io, id))
		{
			uint64		at = id;

			std_vec_push_Checked(&evened, &at, sizeof(at), _Alignof(uint64));
		}
	return evened;
}

/*
 * test_chunks_packs_open_tests_a_full_pool_of_records_whose_manifests_differ_makes_room_by_writing_one_once_more:
 * a pool full of records no process holds makes room first by freeing the
 * record of a repository whose manifest was written twice with the same
 * bytes, writing nothing; then, with every other repository's generations
 * differing, by writing one repository's newest manifest once more and
 * freeing its record: with that write made to fail the make raises the
 * write's error, and without it the make succeeds, and only that
 * repository's generations are then the same.
 */
void
test_chunks_packs_open_tests_a_full_pool_of_records_whose_manifests_differ_makes_room_by_writing_one_once_more(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedOwned first;
	HostSharedOwned second;
	ChunksErrorError error;
	StdVec		evened;
	size_t		one = 1;

	for (uint32 id = 401; id <= 400 + CHUNKS_PACKS_OPEN_TESTS_RECORDS; id++)
	{
		ChunksPacksBackendPacks packs;
		const char *again = id == 401 ? "first" : "other";

		chunks_packs_fixtures_created(io, id, &packs);
		if (!chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												   (const uint8 *) "first", 5, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 688, 60);
		if (!chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
												   (const uint8 *) again, 5, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 689, 95);
		chunks_packs_backend_Packs_drop(&packs);
	}
	first = chunks_packs_tests_made(RUST_FILE, io, 691, 43);
	evened = chunks_packs_open_tests_evened(io);
	if (!chunks_packs_open_tests_even(io, 401) || evened.len != 0)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&evened, chunks_packs_tests_Numbers_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 693, 9, &arguments);
	}
	std_vec_free_Checked(&evened, sizeof(uint64), _Alignof(uint64));
	chunks_packs_open_tests_make_fails(io, 29, "a make whose write fails raises", 696, 50, 698);
	second = chunks_packs_tests_made(RUST_FILE, io, 699, 44);
	evened = chunks_packs_open_tests_evened(io);
	if (evened.len != one)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&evened, chunks_packs_tests_Numbers_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, 700, 9, "==", &evened.len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt,
						  &arguments);
	}
	std_vec_free_Checked(&evened, sizeof(uint64), _Alignof(uint64));
	host_shared_Owned_drop(&first);
	host_shared_Owned_drop(&second);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_drop_opener is a_record_made_before_a_drop_is_no_record_of_the_id_made_again's opener: an open whose result it drops. */
static bool
chunks_packs_open_tests_drop_opener(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;

	(void) captures;
	(void) error;
	if (chunks_packs_open_Packs_open(io, 64, HOST_PG_DEFAULT, &packs, &failed))
		chunks_packs_backend_Packs_drop(&packs);
	else
		chunks_error_Error_drop(&failed);
	return true;
}

/* chunks_packs_open_tests_dropper is a_record_made_before_a_drop_is_no_record_of_the_id_made_again's dropper: the drop, then the id made again. */
static bool
chunks_packs_open_tests_dropper(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendPacks again;
	ChunksErrorError failed;
	HostSharedReader reading;
	StdVec		numbers;
	bool		removed;
	bool		ok;

	(void) captures;
	if (!chunks_packs_open_Packs_dropped(io, 64, HOST_PG_DEFAULT, &removed, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-chunks-case-dropped", error))
		return false;
	if (!chunks_packs_open_Packs_create(io, 64, HOST_PG_DEFAULT, &again, &failed))
	{
		*error = chunks_packs_tests_raised(&failed);
		return false;
	}
	if (!host_shared_read(io, &reading, error))
	{
		chunks_packs_backend_Packs_drop(&again);
		return false;
	}
	ok = chunks_packs_backend_Packs_numbers(&again, io, &reading, &numbers, &failed);
	if (!ok || numbers.len != 0)
	{
		StdString	message;
		ChunksPacksTestsResult listed = {ok, ok ? STD_ITEM(&numbers, chunks_packs_tests_Numbers_Debug_fmt) :
		STD_ITEM(&failed, chunks_error_Error_Debug_fmt)};
		StdFmtPiece pieces[] = {STD_FMT_LIT("the repository made again lists "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&listed, chunks_packs_tests_Result_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		if (ok)
			std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
		else
			chunks_error_Error_drop(&failed);
		*error = chunks_packs_tests_failed(&message);
		ok = false;
	}
	else
		std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	host_shared_Reader_drop(&reading);
	chunks_packs_backend_Packs_drop(&again);
	return ok;
}

/*
 * test_chunks_packs_open_tests_a_record_made_before_a_drop_is_no_record_of_the_id_made_again:
 * bugs row 84: an open that made a repository's record from its files before
 * the repository's drop, and publishes it after, gives a repository made
 * again with the dropped id no record of the dropped one's: the new
 * repository lists no pack the dropped one listed.
 */
void
test_chunks_packs_open_tests_a_record_made_before_a_drop_is_no_record_of_the_id_made_again(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 711, 46);
	ChunksPacksBackendPacks packs;
	ChunksErrorError error;
	uint64		location;
	SimcActor	opener = {chunks_packs_open_tests_drop_opener, NULL};
	SimcActor	dropper = {chunks_packs_open_tests_dropper, NULL};
	SimcStep	steps[] = {{"opener", 6, PG_UINT64_MAX}, {"dropper", 7, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;

	chunks_packs_fixtures_created(io, 64, &packs);
	(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "dropped", 7);
	location = chunks_packs_backend_Packs_location(&packs);
	if (!chunks_packs_backend_Packs_close(&packs, io, &reading, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 715, 35);
	chunks_packs_tests_freed(RUST_FILE, io, location, 716, 61, 70);
	run = simc_Run_new(19);
	simc_Run_actor(&run, "opener", 6, opener);
	simc_Run_actor(&run, "dropper", 7, dropper);
	simc_Run_until(&run, "opener", 6, "doltgres-chunks-record-made");
	simc_Run_until(&run, "dropper", 7, "doltgres-chunks-case-dropped");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	chunks_packs_tests_ended(RUST_FILE, &report, 739, 9);
	simc_Report_drop(&report);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_open_tests_listing is a_record_made_again_from_the_list_files_lists_what_the_last_one_did's change: it adds three packs and retires the second. */
static void
chunks_packs_open_tests_listing(ChunksPacksListsChange *change, void *captures)
{
	const ChunksPacksFixturesPacked *packs = captures;
	ChunksErrorError error;
	static const uint64 second[] = {2};

	for (size_t k = 0; k < 3; k++)
		if (!chunks_packs_lists_Change_add(change, &packs[k].entry, false, &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 397, 69);
	if (!chunks_packs_lists_Change_retire(change, second, lengthof(second), &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 398, 33);
}

/*
 * test_chunks_packs_open_tests_a_record_made_again_from_the_list_files_lists_what_the_last_one_did:
 * a record made again from a repository's list files, after changes that
 * appended to them and wrote them whole, carries its repository's id and its
 * own tablespace's, and gives a view the packs and forwards the record
 * before it gave; its numbers begin past the highest added.
 */
void
test_chunks_packs_open_tests_a_record_made_again_from_the_list_files_lists_what_the_last_one_did(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	HostSharedOwned before;
	ChunksPacksFixturesPacked packs[3];
	ChunksPacksViewView mine;
	ChunksPacksViewView fresh;
	StdVec		listed;
	HostSharedOwned again;
	HostSharedRecord record;
	HostSharedGuard state;
	HostError	host;
	StdVec		blob;
	ChunksPacksSharedSnapshot snapshot;
	ChunksErrorError error;
	StdPathBuf	table;
	StdFsMetadata metadata;
	StdIoError	io_error;
	uint64		got;
	uint64		want;
	uint64		appended[2];
	static const uint64 want_appended[] = {4, 0};
	static const uint64 kept[] = {1, 3};

	chunks_packs_fixtures_home(&dir);
	before = chunks_packs_lists_tests_repository(io, &dir);
	for (size_t k = 0; k < lengthof(packs); k++)
		packs[k] = chunks_packs_fixtures_pack(io, &dir, k + 1);
	chunks_packs_fixtures_view(&mine);
	chunks_packs_lists_tests_changed(io, host_shared_Owned_record(&before), &mine, chunks_packs_open_tests_listing,
									 packs);
	chunks_packs_view_View_listed(&mine, &listed);
	host_shared_free(&before);

	if (!chunks_packs_open_made(io, CHUNKS_PACKS_FIXTURES_REPOSITORY, HOST_PG_DEFAULT, &again, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 404, 52);
	record = host_shared_Owned_record(&again);
	got = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_REPOSITORY_INDEX);
	STD_ASSERT_EQ_U64(406, 9, got, CHUNKS_PACKS_FIXTURES_REPOSITORY);
	got = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_OWN_INDEX);
	STD_ASSERT_EQ_U64(407, 9, got, HOST_PG_DEFAULT);
	got = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_RESERVED_INDEX);
	want = chunks_packs_shared_reserved(3, 3);
	STD_ASSERT_EQ_U64(408, 9, got, want);
	got = chunks_packs_shared_load(record, CHUNKS_PACKS_SHARED_GENERATION_INDEX);
	STD_ASSERT_EQ_U64(409, 9, got, 0);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), CHUNKS_PACKS_SHARED_STATE_LOCK_INDEX,
										&state, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 411, 65);
	blob = host_shared_Guard_read_blob(&state, CHUNKS_PACKS_SHARED_SNAPSHOT_BLOB);
	if (!chunks_packs_shared_Snapshot_read(blob.ptr, blob.len, &snapshot, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 411, 104);
	host_shared_Guard_drop(&state);
	std_vec_free_Checked(&blob, 1, 1);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	if (!std_fs_metadata(table.bytes.ptr, table.bytes.len, &metadata, &io_error))
		STD_RESULT_EXPECT_FAILED(412, 80, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	want = std_fs_Metadata_len(&metadata);
	std_fs_Metadata_drop(&metadata);
	std_vec_free_Checked(&table.bytes, 1, 1);
	STD_ASSERT_EQ_U64(412, 9, snapshot.table.tail.end, want);
	appended[0] = snapshot.table.appended;
	appended[1] = snapshot.forwarding.appended;
	if (appended[0] != want_appended[0] || appended[1] != want_appended[1])
	{
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&appended[0], std_u64_Debug_fmt),
									 STD_ITEM(&appended[1], std_u64_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&want_appended[0], std_u64_Debug_fmt),
									  STD_ITEM(&want_appended[1], std_u64_Debug_fmt));

		std_assert_failed(RUST_FILE, 413, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	STD_ASSERT(414, 9, snapshot.deferred.len == 0, "snapshot.deferred.is_empty()");
	chunks_packs_fixtures_view(&fresh);
	chunks_packs_tests_refreshed(RUST_FILE, &fresh, io, record, 416, 35);
	chunks_packs_tests_listed(RUST_FILE, &fresh, listed.ptr, listed.len, 417, 9);
	chunks_packs_tests_keys(RUST_FILE, &fresh, kept, lengthof(kept), NULL, 418, 9);
	chunks_packs_view_View_drop(&fresh);
	chunks_packs_shared_Snapshot_drop(&snapshot);
	host_shared_Owned_drop(&again);
	std_vec_free_Checked(&listed, sizeof(ChunksPacksTableEntry), _Alignof(ChunksPacksTableEntry));
	chunks_packs_view_View_drop(&mine);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
