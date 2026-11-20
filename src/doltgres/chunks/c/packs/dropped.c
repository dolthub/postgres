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
 * The C port of chunks' packs/dropped.rs: a dropped repository, whose own
 * directory a drop marks durably and whose directories the removal's pass
 * removes once no process holds its pack record, by the entry the drop
 * leaves in a pool of its own.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/dropped.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/packs/single.h"
#include "doltgres/chunks/trace.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/removal.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "dropped.rs"

/* chunks_packs_dropped_DroppedRepository_count returns as many entries as there are pack records. */
static size_t
chunks_packs_dropped_DroppedRepository_count(const HostSharedLimits *limits)
{
	return CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS.count(limits);
}

/* chunks_packs_dropped_DroppedRepository_reclaim runs the pass, and answers whether it freed an entry. */
static bool
chunks_packs_dropped_DroppedRepository_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_packs_dropped_remove_dropped(io, result, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	return true;
}

/* CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS is DroppedRepository's Pool: the entries drops leave. */
const HostSharedPoolOps CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x6368756e6b730006), .atomics = 3},
	.name = {"dropped repository", 18},
	.count = chunks_packs_dropped_DroppedRepository_count,
	.reclaim = chunks_packs_dropped_DroppedRepository_reclaim,
};

/* chunks_packs_dropped_mark_path returns the path of the mark in a repository's own directory. */
static void
chunks_packs_dropped_mark_path(const char *dir, size_t dir_len, StdPathBuf *result)
{
	std_path_join(dir, dir_len, CHUNKS_PACKS_DROPPED_MARK, strlen(CHUNKS_PACKS_DROPPED_MARK), result);
}

/*
 * chunks_packs_dropped_marked_frame_0_t is marked's state across its pad: its
 * arguments, its result, and the mark's path it holds.
 */
typedef struct chunks_packs_dropped_marked_frame_0_t
{
	HostIoReadDoors io;
	const char *dir;
	size_t		dir_len;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	path;
} chunks_packs_dropped_marked_frame_0_t;

/* chunks_packs_dropped_marked_pad_0 is marked's pad: the mark's path. */
static void
chunks_packs_dropped_marked_pad_0(void *frame_)
{
	chunks_packs_dropped_marked_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
}

/* chunks_packs_dropped_marked_body_0 is marked's body. */
static void
chunks_packs_dropped_marked_body_0(void *frame_)
{
	chunks_packs_dropped_marked_frame_0_t *frame = frame_;

	chunks_packs_dropped_mark_path(frame->dir, frame->dir_len, &frame->path);
	frame->ok = chunks_file_exists(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, frame->result,
								   frame->error);
	chunks_packs_dropped_marked_pad_0(frame);
}

/* chunks_packs_dropped_marked reports whether a repository's own directory holds the mark of a dropped repository. */
bool
chunks_packs_dropped_marked(HostIoReadDoors io, const char *dir, size_t dir_len, bool *result, ChunksErrorError *error)
{
	chunks_packs_dropped_marked_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len, .result = result,
	.error = error};

	std_PathBuf_new(&frame.path);
	std_landing(&frame, chunks_packs_dropped_marked_body_0, chunks_packs_dropped_marked_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_dropped_mark_frame_0_t is mark's state across its pad: its
 * arguments, its result, and the mark's path and file it holds.
 */
typedef struct chunks_packs_dropped_mark_frame_0_t
{
	HostIoIoDoors io;
	const char *dir;
	size_t		dir_len;
	ChunksErrorError *error;
	bool		ok;
	StdPathBuf	path;
	HostFile	file;
	bool		file_live;
} chunks_packs_dropped_mark_frame_0_t;

/* chunks_packs_dropped_mark_pad_0 is mark's pad: the mark's file, then its path. */
static void
chunks_packs_dropped_mark_pad_0(void *frame_)
{
	chunks_packs_dropped_mark_frame_0_t *frame = frame_;

	if (frame->file_live)
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
}

/* chunks_packs_dropped_mark_body_0 is mark's body. */
static void
chunks_packs_dropped_mark_body_0(void *frame_)
{
	chunks_packs_dropped_mark_frame_0_t *frame = frame_;
	HostIoReadDoors read = HOST_IO_IO_DOORS_READ_DOORS(frame->io);
	bool		there;
	bool		marked;

	frame->ok = false;
	if (!chunks_file_dir_there(read, frame->dir, frame->dir_len, &there, frame->error))
		return;
	frame->ok = !there;
	if (!there)
		return;
	if (!chunks_packs_dropped_marked(read, frame->dir, frame->dir_len, &marked, frame->error))
		return;
	frame->ok = marked;
	if (marked)
		return;
	chunks_packs_dropped_mark_path(frame->dir, frame->dir_len, &frame->path);
	if (chunks_file_create(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &frame->file, frame->error))
	{
		frame->file_live = true;
		frame->ok = chunks_file_sync(frame->io, &frame->file, frame->error);
	}
	chunks_packs_dropped_mark_pad_0(frame);
	frame->ok = frame->ok && chunks_file_sync_dir(frame->io, frame->dir, frame->dir_len, frame->error);
}

/* chunks_packs_dropped_mark writes the mark into a repository's own directory, durably, if it is there and holds none. */
bool
chunks_packs_dropped_mark(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	chunks_packs_dropped_mark_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len, .error = error};

	std_PathBuf_new(&frame.path);
	std_landing(&frame, chunks_packs_dropped_mark_body_0, chunks_packs_dropped_mark_pad_0);
	return frame.ok;
}

/* chunks_packs_dropped_removed is host::remove_tree as chunks' error: whether everything under a path is gone. */
static bool
chunks_packs_dropped_removed(HostIoIoDoors io, const char *path, size_t path_len, bool *result,
							 ChunksErrorError *error)
{
	HostError	host;

	if (!host_remove_tree(io, path, path_len, result, &host))
	{
		*error = chunks_error_Error_FromError_from(&host);
		return false;
	}
	return true;
}

/* chunks_packs_dropped_only_mark reports whether a directory's names hold nothing but the mark. */
static bool
chunks_packs_dropped_only_mark(const StdVec *names)
{
	for (size_t k = 0, k_len = names->len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(names, StdString, k);

		if (name->len != strlen(CHUNKS_PACKS_DROPPED_MARK) ||
			memcmp(name->ptr, CHUNKS_PACKS_DROPPED_MARK, name->len) != 0)
			return false;
	}
	return true;
}

/*
 * chunks_packs_dropped_remove_directories_frame_0_t is remove_directories'
 * state across its pad: its arguments, its result, and the directories,
 * path, and names it holds.
 */
typedef struct chunks_packs_dropped_remove_directories_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		dirs;
	StdPathBuf	path;
	StdVec		names;
} chunks_packs_dropped_remove_directories_frame_0_t;

/* chunks_packs_dropped_remove_directories_pad_0 is remove_directories' pad: the names, the path, then the directories. */
static void
chunks_packs_dropped_remove_directories_pad_0(void *frame_)
{
	chunks_packs_dropped_remove_directories_frame_0_t *frame = frame_;

	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_drop_Checked(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	std_vec_new(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf));
}

/* chunks_packs_dropped_remove_directories_body_0 is remove_directories' body. */
static void
chunks_packs_dropped_remove_directories_body_0(void *frame_)
{
	chunks_packs_dropped_remove_directories_frame_0_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(frame->io);
	const StdPathBuf *dir;
	bool		gone = true;
	bool		removed;
	bool		there;

	frame->ok = false;
	if (!chunks_packs_open_directories(read, frame->repository, frame->own, &frame->dirs, frame->error))
		goto done;
	for (size_t k = 1, k_len = frame->dirs.len; k < k_len; k++)
	{
		dir = &STD_VEC_AT(&frame->dirs, StdPathBuf, k);
		if (!chunks_packs_dropped_removed(doors, dir->bytes.ptr, dir->bytes.len, &removed, frame->error))
			goto done;
		gone = gone && removed;
	}
	dir = &STD_VEC_AT(&frame->dirs, StdPathBuf, 0);
	if (!chunks_file_dir_there(read, dir->bytes.ptr, dir->bytes.len, &there, frame->error))
		goto done;
	if (!there)
	{
		*frame->result = gone;
		frame->ok = true;
		goto done;
	}
	std_path_join(dir->bytes.ptr, dir->bytes.len, CHUNKS_PACKS_DELTA, strlen(CHUNKS_PACKS_DELTA), &frame->path);
	if (!chunks_packs_dropped_removed(doors, frame->path.bytes.ptr, frame->path.bytes.len, &removed, frame->error))
		goto done;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	gone = gone && removed;
	std_path_join(dir->bytes.ptr, dir->bytes.len, CHUNKS_PACKS_BACKUP, strlen(CHUNKS_PACKS_BACKUP), &frame->path);
	if (!chunks_packs_dropped_removed(doors, frame->path.bytes.ptr, frame->path.bytes.len, &removed, frame->error))
		goto done;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	gone = gone && removed;
	if (!chunks_file_file_names(read, dir->bytes.ptr, dir->bytes.len, &frame->names, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&frame->names, StdString, k);

		if (name->len == strlen(CHUNKS_PACKS_DROPPED_MARK) &&
			memcmp(name->ptr, CHUNKS_PACKS_DROPPED_MARK, name->len) == 0)
			continue;
		std_path_join(dir->bytes.ptr, dir->bytes.len, name->ptr, name->len, &frame->path);
		host_remove_leftover(doors, frame->path.bytes.ptr, frame->path.bytes.len);
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
		std_PathBuf_new(&frame->path);
	}
	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
	if (!gone)
	{
		*frame->result = false;
		frame->ok = true;
		goto done;
	}
	if (!chunks_file_file_names(read, dir->bytes.ptr, dir->bytes.len, &frame->names, frame->error))
		goto done;
	if (!chunks_packs_dropped_only_mark(&frame->names))
	{
		*frame->result = false;
		frame->ok = true;
		goto done;
	}
	frame->ok = chunks_packs_dropped_removed(doors, dir->bytes.ptr, dir->bytes.len, frame->result, frame->error);
done:
	chunks_packs_dropped_remove_directories_pad_0(frame);
}

/*
 * chunks_packs_dropped_remove_directories removes a dropped repository's
 * directories and reports whether all of them are gone: those in other
 * tablespaces whole, then in its own directory the checkpoint's runs and
 * the backups' generations whole and each file but the mark, and only when
 * nothing but the mark is left anywhere the mark with the directory.
 */
bool
chunks_packs_dropped_remove_directories(HostIoIo *io, uint32 repository, uint32 own, bool *result,
										ChunksErrorError *error)
{
	chunks_packs_dropped_remove_directories_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.result = result, .error = error};

	std_vec_new(&frame.dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_PathBuf_new(&frame.path);
	std_vec_new(&frame.names, sizeof(StdString), _Alignof(StdString));
	std_landing(&frame, chunks_packs_dropped_remove_directories_body_0, chunks_packs_dropped_remove_directories_pad_0);
	return frame.ok;
}

/* chunks_packs_dropped_repository_removed is RepositoryRemoved's event. */
static void
chunks_packs_dropped_repository_removed(void *captures, ChunksTraceEvent *result)
{
	(void) captures;
	result->tag = CHUNKS_TRACE_EVENT_REPOSITORY_REMOVED;
}

/*
 * chunks_packs_dropped_remove_dropped_frame_0_t is the pass's state across
 * its pad: the entries swept, the makers' lock's guard, and the entry it
 * claimed.
 */
typedef struct chunks_packs_dropped_remove_dropped_frame_0_t
{
	HostIoIo   *io;
	bool	   *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		entries;
	bool		entries_live;
	HostSharedGuard making;
	bool		making_live;
	HostSharedOwned entry;
	bool		entry_live;
} chunks_packs_dropped_remove_dropped_frame_0_t;

/* chunks_packs_dropped_remove_dropped_pad_0 is the pass's pad: the entry, the guard, then the entries. */
static void
chunks_packs_dropped_remove_dropped_pad_0(void *frame_)
{
	chunks_packs_dropped_remove_dropped_frame_0_t *frame = frame_;

	if (frame->entry_live)
	{
		frame->entry_live = false;
		host_shared_Owned_drop(&frame->entry);
	}
	if (frame->making_live)
	{
		frame->making_live = false;
		host_shared_Guard_drop(&frame->making);
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* chunks_packs_dropped_remove_dropped_lock takes the makers' lock, shared. */
static bool
chunks_packs_dropped_remove_dropped_lock(chunks_packs_dropped_remove_dropped_frame_0_t *frame,
										 HostSharedRecord makers, HostError *host)
{
	if (!host_shared_Record_lock_shared(makers, HOST_IO_IO_TAKES_SHORT(frame->io), CHUNKS_PACKS_SINGLE_MAKING_INDEX,
										&frame->making, host))
		return false;
	frame->making_live = true;
	return true;
}

/* chunks_packs_dropped_remove_dropped_unlock lets the makers' lock go. */
static void
chunks_packs_dropped_remove_dropped_unlock(chunks_packs_dropped_remove_dropped_frame_0_t *frame)
{
	frame->making_live = false;
	host_shared_Guard_drop(&frame->making);
}

/*
 * chunks_packs_dropped_remove_dropped_body_0 is the pass's body: each entry
 * claimed and asked about under the makers' lock, shared, and given back
 * there while its record is held, and otherwise its repository's directories
 * removed once and the entry freed.
 */
static void
chunks_packs_dropped_remove_dropped_body_0(void *frame_)
{
	chunks_packs_dropped_remove_dropped_frame_0_t *frame = frame_;
	HostSharedRecord makers;
	HostError	host;
	bool		freed = false;

	frame->ok = false;
	if (!host_shared_single(&CHUNKS_PACKS_SINGLE_CHUNKS_STATE_SINGLE_OPS, &makers, &host) ||
		!chunks_packs_dropped_remove_dropped_lock(frame, makers, &host))
		goto failed;
	if (!host_shared_sweep(HOST_SHARED_GUARD_WITHIN(&frame->making), &CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS,
						   CHUNKS_PACKS_DROPPED_NAMED_INDEX, &frame->entries, &host))
		goto failed;
	frame->entries_live = true;
	chunks_packs_dropped_remove_dropped_unlock(frame);
	for (size_t k = 0, k_len = frame->entries.len; k < k_len; k++)
	{
		uint64		location = STD_VEC_AT(&frame->entries, std_TupleU64U64_t, k).f0;
		bool		has_entry;
		bool		gone;
		HostSharedRecord record;
		uint64		repository;
		uint64		own;
		bool		removed;

		if (!chunks_packs_dropped_remove_dropped_lock(frame, makers, &host))
			goto failed;
		if (!host_shared_claim(HOST_SHARED_GUARD_WITHIN(&frame->making),
							   &CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS, location, &has_entry, &frame->entry,
							   &host))
			goto failed;
		if (!has_entry)
		{
			chunks_packs_dropped_remove_dropped_unlock(frame);
			continue;
		}
		frame->entry_live = true;
		record = host_shared_Owned_record(&frame->entry);
		if (!host_shared_gone(&CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
							  chunks_packs_shared_load(record, CHUNKS_PACKS_DROPPED_AT_INDEX), &gone, &host))
			goto failed;
		if (!gone)
		{
			frame->entry_live = false;
			host_shared_Owned_drop(&frame->entry);
			chunks_packs_dropped_remove_dropped_unlock(frame);
			continue;
		}
		repository = chunks_packs_shared_load(record, CHUNKS_PACKS_DROPPED_NAMED_INDEX);
		own = chunks_packs_shared_load(record, CHUNKS_PACKS_DROPPED_SPACE_INDEX);
		chunks_packs_dropped_remove_dropped_unlock(frame);
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-chunks-drop-claimed", &host))
			goto failed;
		if (!chunks_packs_dropped_remove_directories(frame->io, (uint32) repository, (uint32) own, &removed,
													 frame->error))
			goto done;
		frame->entry_live = false;
		host_shared_free(&frame->entry);
		chunks_trace_emit(repository, chunks_packs_dropped_repository_removed, NULL, NULL);
		freed = true;
	}
	*frame->result = freed;
	frame->ok = true;
	goto done;

failed:
	*frame->error = chunks_error_Error_FromError_from(&host);
done:
	chunks_packs_dropped_remove_dropped_pad_0(frame);
}

/*
 * chunks_packs_dropped_remove_dropped is the pass over the dropped
 * repositories' entries, which reports whether it freed one.
 */
bool
chunks_packs_dropped_remove_dropped(HostIoIo *io, bool *result, ChunksErrorError *error)
{
	chunks_packs_dropped_remove_dropped_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, chunks_packs_dropped_remove_dropped_body_0, chunks_packs_dropped_remove_dropped_pad_0);
	return frame.ok;
}

/* chunks_packs_dropped_refused returns the error of an open of a repository that is dropped. */
ChunksErrorError
chunks_packs_dropped_refused(uint32 repository)
{
	ChunksErrorError error = {.tag = CHUNKS_ERROR_ERROR_INVARIANT};
	StdFmtPiece pieces[] = {STD_FMT_LIT("repository "), STD_FMT_ARG(0),
	STD_FMT_LIT("'s packs are opened after the repository was dropped")};
	StdFmtArg	args[] = {{&repository, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	std_fmt_format_Checked(&error.u.invariant, &arguments);
	return error;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_dropped_tests_id returns a repository's id, which unwraps a NonZeroU32. */
static uint32
chunks_packs_dropped_tests_id(uint32 repository)
{
	if (repository == 0)
		STD_OPTION_UNWRAP_FAILED(180, 37);
	return repository;
}

/* chunks_packs_dropped_tests_home returns a repository's own directory, in the default tablespace. */
static StdPathBuf
chunks_packs_dropped_tests_home(uint32 repository)
{
	return host_tablespace_dir(repository, HOST_PG_DEFAULT);
}

/* chunks_packs_dropped_tests_entries returns how many dropped repositories' entries are published. */
static size_t
chunks_packs_dropped_tests_entries(HostIoIo *io)
{
	StdVec		swept;
	HostError	host;
	size_t		len;

	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_DROPPED_DROPPED_REPOSITORY_POOL_OPS,
						   CHUNKS_PACKS_DROPPED_NAMED_INDEX, &swept, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 190, 47);
	len = swept.len;
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	return len;
}

/*
 * test_chunks_packs_dropped_tests_a_dropped_repositorys_files_are_gone_at_once_when_no_process_holds_it:
 * a drop of a repository no process holds removes its directories, in every
 * tablespace, before it returns, frees its record, and leaves no entry; so
 * does a drop of a repository that has no record.
 */
void
test_chunks_packs_dropped_tests_a_dropped_repositorys_files_are_gone_at_once_when_no_process_holds_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	static const bool cases[] = {true, false};

	for (size_t k = 0; k < lengthof(cases); k++)
	{
		bool		recorded = cases[k];
		ChunksPacksBackendPacks packs;
		uint64		location;
		StdPathBuf	other = host_tablespace_dir(671, 16400);
		StdPathBuf	file;
		StdPathBuf	home;
		StdIoError	io_error;
		ChunksErrorError error;
		HostSharedHeld held;
		HostError	host;
		bool		removed;
		bool		has;
		size_t		entries;
		size_t		none = 0;
		StdFmtPiece pieces[] = {STD_FMT_LIT("recorded: "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&recorded, std_bool_Display_fmt}};
		StdFmtArguments shown = {pieces, lengthof(pieces), args};

		chunks_packs_fixtures_created(io, 671, &packs);
		(void) chunks_packs_fixtures_added(io, &packs, (const uint8 *) "one", 3);
		location = chunks_packs_backend_Packs_location(&packs);
		if (!std_fs_create_dir_all(other.bytes.ptr, other.bytes.len, &io_error))
			STD_RESULT_EXPECT_FAILED(213, 45, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		std_path_join(other.bytes.ptr, other.bytes.len, "file", 4, &file);
		if (!std_fs_write(file.bytes.ptr, file.bytes.len, (const uint8 *) "a file", 6, &io_error))
			STD_RESULT_EXPECT_FAILED(214, 59, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		std_vec_free_Checked(&file.bytes, 1, 1);
		chunks_packs_backend_Packs_drop(&packs);
		if (!recorded)
			chunks_packs_tests_freed(RUST_FILE, io, location, 217, 69, 78);
		if (!chunks_packs_open_Packs_dropped(io, chunks_packs_dropped_tests_id(671), HOST_PG_DEFAULT, &removed,
											 &error))
			chunks_packs_tests_unwrapped(RUST_FILE, &error, 219, 67);
		if (!removed)
			std_panic_fmt(RUST_FILE, 219, 13, &shown);
		home = chunks_packs_dropped_tests_home(671);
		if (std_path_Path_exists(home.bytes.ptr, home.bytes.len) ||
			std_path_Path_exists(other.bytes.ptr, other.bytes.len))
			std_panic_fmt(RUST_FILE, 220, 13, &shown);
		std_vec_free_Checked(&home.bytes, 1, 1);
		if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has, &held,
							  &host))
			chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 221, 53);
		if (has)
			host_shared_Held_drop(&held);
		STD_ASSERT(221, 13, !has, "open::<PackState>(io, location).unwrap().is_none()");
		entries = chunks_packs_dropped_tests_entries(io);
		if (entries != none)
			std_assert_failed(RUST_FILE, 222, 13, "==", &entries, std_usize_Debug_fmt, &none, std_usize_Debug_fmt,
							  NULL);
		std_vec_free_Checked(&other.bytes, 1, 1);
	}
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_dropped_tests_drop answers a drop of the repository with an id, whether its files are gone, which unwraps at a line and column. */
static bool
chunks_packs_dropped_tests_drop(HostIoIo *io, uint32 repository, uint32 line, uint32 col)
{
	ChunksErrorError error;
	bool		removed;

	if (!chunks_packs_open_Packs_dropped(io, chunks_packs_dropped_tests_id(repository), HOST_PG_DEFAULT, &removed,
										 &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
	return removed;
}

/*
 * chunks_packs_dropped_tests_held_then_dropped drops a new repository with
 * an id while its packs are held, lets the hold go, and leaves its entry due
 * for the next pass.
 */
static void
chunks_packs_dropped_tests_held_then_dropped(HostIoIo *io, uint32 repository)
{
	ChunksPacksBackendPacks held;

	chunks_packs_fixtures_created(io, repository, &held);
	(void) chunks_packs_fixtures_added(io, &held, (const uint8 *) "one", 3);
	(void) chunks_packs_fixtures_added(io, &held, (const uint8 *) "two", 3);
	STD_ASSERT(199, 9, !chunks_packs_dropped_tests_drop(io, repository, 199, 71),
			   "!Packs::dropped(io, id(repository), host::PG_DEFAULT).unwrap()");
	chunks_packs_backend_Packs_drop(&held);
}

/* chunks_packs_dropped_tests_there reports whether the repository with an id has its own directory, or a file in it by a name. */
static bool
chunks_packs_dropped_tests_there(uint32 repository, const char *name)
{
	StdPathBuf	home = chunks_packs_dropped_tests_home(repository);
	StdPathBuf	file;
	bool		there;

	if (name == NULL)
		there = std_path_Path_exists(home.bytes.ptr, home.bytes.len);
	else
	{
		std_path_join(home.bytes.ptr, home.bytes.len, name, strlen(name), &file);
		there = std_path_Path_exists(file.bytes.ptr, file.bytes.len);
		std_vec_free_Checked(&file.bytes, 1, 1);
	}
	std_vec_free_Checked(&home.bytes, 1, 1);
	return there;
}

/* chunks_packs_dropped_tests_removed removes what is due, which unwraps at a line and a column. */
static void
chunks_packs_dropped_tests_removed(HostIoIo *io, uint32 line, uint32 col)
{
	ChunksErrorError error;

	if (!chunks_packs_removal_remove_due(io, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, line, col);
}

/* chunks_packs_dropped_tests_entries_eq asserts at a line and column that as many dropped repositories' entries as given are published. */
static void
chunks_packs_dropped_tests_entries_eq(HostIoIo *io, size_t want, const StdFmtArguments *message, uint32 line,
									  uint32 col)
{
	size_t		entries = chunks_packs_dropped_tests_entries(io);

	if (entries != want)
		std_assert_failed(RUST_FILE, line, col, "==", &entries, std_usize_Debug_fmt, &want, std_usize_Debug_fmt,
						  message);
}

/* chunks_packs_dropped_tests_Pairs_Debug_fmt is a Vec<(u64, u64)>'s Debug. */
static bool
chunks_packs_dropped_tests_Pairs_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *pairs = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = pairs->len; k < k_len; k++)
	{
		const std_TupleU64U64_t *pair = &STD_VEC_AT(pairs, std_TupleU64U64_t, k);
		StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&pair->f0, std_u64_Debug_fmt), STD_ITEM(&pair->f1, std_u64_Debug_fmt));

		std_DebugList_entry(&l, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&l);
}

/*
 * test_chunks_packs_dropped_tests_a_dropped_repositorys_files_wait_for_the_last_process_that_holds_it:
 * a drop of a repository another process holds marks it, frees its record,
 * and answers that files remain: the holder still reads, no open finds or
 * makes a record of it, a pass that meets its entry leaves it, and the first
 * pass after the holder's close removes the directories and frees the entry;
 * no other repository is touched.
 */
void
test_chunks_packs_dropped_tests_a_dropped_repositorys_files_wait_for_the_last_process_that_holds_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 234, 46);
	ChunksPacksBackendPacks held;
	ChunksPacksBackendPacks kept;
	ChunksPacksBackendPacks opened;
	chunks_TupleAddressLocator_t one;
	ChunksErrorError error;
	HostError	host;
	StdVec		swept;
	bool		refused;

	chunks_packs_fixtures_created(io, 672, &held);
	chunks_packs_fixtures_created(io, 673, &kept);
	one = chunks_packs_fixtures_added(io, &held, (const uint8 *) "one", 3);
	STD_ASSERT(237, 9, !chunks_packs_dropped_tests_drop(io, 672, 237, 64),
			   "!Packs::dropped(io, id(672), host::PG_DEFAULT).unwrap()");
	STD_ASSERT(238, 9, chunks_packs_dropped_tests_there(672, CHUNKS_PACKS_DROPPED_MARK), "home(672).join(MARK).exists()");
	chunks_packs_dropped_tests_entries_eq(io, 1, NULL, 239, 9);
	chunks_packs_tests_found(RUST_FILE, &held, io, &reading, &one.f0, &one.f1, 240, 9, 50);
	refused = !chunks_packs_open_Packs_open(io, chunks_packs_dropped_tests_id(672), HOST_PG_DEFAULT, &opened, &error);
	if (refused)
	{
		refused = error.tag == CHUNKS_ERROR_ERROR_INVARIANT;
		chunks_error_Error_drop(&error);
	}
	else
		chunks_packs_backend_Packs_drop(&opened);
	STD_ASSERT(241, 9, refused, "matches!(Packs::open(io, id(672), host::PG_DEFAULT), Err(Error::Invariant(_)))");
	chunks_packs_dropped_tests_removed(io, 242, 24);
	STD_ASSERT_MSG(243, 9, chunks_packs_dropped_tests_there(672, NULL) && chunks_packs_dropped_tests_entries(io) == 1,
				   "a pass removed a repository a process still holds");
	STD_ASSERT(244, 9, !chunks_packs_dropped_tests_drop(io, 672, 244, 64),
			   "!Packs::dropped(io, id(672), host::PG_DEFAULT).unwrap()");
	STD_ASSERT_MSG(245, 9, chunks_packs_dropped_tests_there(672, NULL) && chunks_packs_dropped_tests_entries(io) == 1,
				   "a second drop removed a repository a process still holds");
	chunks_packs_tests_found(RUST_FILE, &held, io, &reading, &one.f0, &one.f1, 246, 9, 50);
	chunks_packs_backend_Packs_drop(&held);
	chunks_packs_dropped_tests_removed(io, 248, 24);
	STD_ASSERT(249, 9, !chunks_packs_dropped_tests_there(672, NULL) && chunks_packs_dropped_tests_entries(io) == 0,
			   "!home(672).exists() && entries(io) == 0");
	if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS,
						   CHUNKS_PACKS_SHARED_REPOSITORY_INDEX, &swept, &host))
		chunks_packs_tests_host_unwrapped(RUST_FILE, &host, 250, 55);
	if (swept.len != 1 || STD_VEC_AT(&swept, std_TupleU64U64_t, 0).f0 != chunks_packs_backend_Packs_location(&kept) ||
		STD_VEC_AT(&swept, std_TupleU64U64_t, 0).f1 != 673)
	{
		std_TupleU64U64_t entry = {chunks_packs_backend_Packs_location(&kept), 673};
		StdVec		want;

		std_slice_to_vec_Checked(&want, &entry, 1, sizeof(entry), _Alignof(std_TupleU64U64_t));
		std_assert_failed(RUST_FILE, 250, 9, "==", &swept, chunks_packs_dropped_tests_Pairs_Debug_fmt, &want,
						  chunks_packs_dropped_tests_Pairs_Debug_fmt, NULL);
	}
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	STD_ASSERT(251, 9, chunks_packs_dropped_tests_there(673, NULL), "home(673).exists()");
	chunks_packs_backend_Packs_drop(&kept);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_dropped_tests_a_removal_cut_short_leaves_the_mark_for_the_next_settle:
 * a removal cut short at any of its files frees the entry all the same and
 * leaves the mark, by which the next start's settle removes what is left and
 * reports the repository dropped, opening nothing.
 */
void
test_chunks_packs_dropped_tests_a_removal_cut_short_leaves_the_mark_for_the_next_settle(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		short_ = 0;

	for (uint32 repository = 674, fault = 1; repository < 682; repository++, fault++)
	{
		StdPathBuf	data = simc_data_dir();
		SimcSim		sim = simc_Sim_start(data.bytes.ptr, data.bytes.len, 43);
		uint64		at = fault;
		StdFmtPiece waits_pieces[] = {STD_FMT_LIT("fault "), STD_FMT_ARG(0),
		STD_FMT_LIT(": the entry waits for a removal to succeed")};
		StdFmtPiece took_pieces[] = {STD_FMT_LIT("fault "), STD_FMT_ARG(0),
		STD_FMT_LIT(": a removal cut short took the mark")};
		StdFmtPiece fault_pieces[] = {STD_FMT_LIT("fault "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&at, std_u64_Display_fmt}};
		StdFmtArguments waits = {waits_pieces, lengthof(waits_pieces), args};
		StdFmtArguments took = {took_pieces, lengthof(took_pieces), args};
		StdFmtArguments faulted = {fault_pieces, lengthof(fault_pieces), args};

		std_vec_free_Checked(&data.bytes, 1, 1);
		chunks_packs_dropped_tests_held_then_dropped(io, repository);
		simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + fault);
		chunks_packs_dropped_tests_removed(io, 265, 28);
		simc_Sim_stop(&sim);
		chunks_packs_dropped_tests_entries_eq(io, 0, &waits, 267, 13);
		if (chunks_packs_dropped_tests_there(repository, NULL))
		{
			ChunksPacksSettleSettled settled;
			ChunksErrorError error;

			short_ = STD_ADD(u64, short_, 1, 269, 17);
			if (!chunks_packs_dropped_tests_there(repository, CHUNKS_PACKS_DROPPED_MARK))
				std_panic_fmt(RUST_FILE, 270, 17, &took);
			if (!chunks_packs_settle_Packs_settle(io, chunks_packs_dropped_tests_id(repository), HOST_PG_DEFAULT,
												  &settled, &error))
				chunks_packs_tests_unwrapped(RUST_FILE, &error, 271, 83);
			if (!settled.dropped || chunks_packs_dropped_tests_there(repository, NULL))
				std_panic_fmt(RUST_FILE, 272, 17, &faulted);
			chunks_packs_settle_Settled_drop(&settled);
		}
		simc_Sim_drop(&sim);
	}
	STD_ASSERT_MSG(275, 9, short_ > 0, "no fault cut a removal short");
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_dropped_tests_a_settle_removes_a_repository_dropped_before_a_crash:
 * a settle of a repository that was dropped while a process held it, with
 * no pass run since, removes its directories by the mark and reports it
 * dropped.
 */
void
test_chunks_packs_dropped_tests_a_settle_removes_a_repository_dropped_before_a_crash(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksSettleSettled settled;
	ChunksErrorError error;

	chunks_packs_dropped_tests_held_then_dropped(io, 683);
	if (!chunks_packs_settle_Packs_settle(io, chunks_packs_dropped_tests_id(683), HOST_PG_DEFAULT, &settled, &error))
		chunks_packs_tests_unwrapped(RUST_FILE, &error, 285, 68);
	STD_ASSERT(286, 9, settled.dropped && !chunks_packs_dropped_tests_there(683, NULL),
			   "settled.dropped && !home(683).exists()");
	chunks_packs_settle_Settled_drop(&settled);
	chunks_packs_dropped_tests_removed(io, 287, 24);
	chunks_packs_dropped_tests_entries_eq(io, 0, NULL, 288, 9);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_dropped_tests_pass is a pass actor: what is due removed, or failed as its actor does. */
static bool
chunks_packs_dropped_tests_pass(void *captures, HostIoIo *io, HostError *error)
{
	ChunksErrorError failed;

	(void) captures;
	if (chunks_packs_removal_remove_due(io, &failed))
		return true;
	*error = chunks_packs_tests_raised(&failed);
	return false;
}

/*
 * test_chunks_packs_dropped_tests_two_passes_act_on_a_dropped_repositorys_entry_one_at_a_time:
 * two passes at once over one due entry remove its directories once and free
 * it once: the pass that claimed the entry holds it while the other finds
 * none.
 */
void
test_chunks_packs_dropped_tests_two_passes_act_on_a_dropped_repositorys_entry_one_at_a_time(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	first = {chunks_packs_dropped_tests_pass, NULL};
	SimcActor	second = {chunks_packs_dropped_tests_pass, NULL};
	SimcStep	steps[] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;

	chunks_packs_dropped_tests_held_then_dropped(io, 684);
	run = simc_Run_new(47);
	simc_Run_actor(&run, "first", 5, first);
	simc_Run_actor(&run, "second", 6, second);
	simc_Run_until(&run, "first", 5, "doltgres-chunks-drop-claimed");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	chunks_packs_tests_ended(RUST_FILE, &report, 305, 9);
	simc_Report_drop(&report);
	STD_ASSERT(311, 9, !chunks_packs_dropped_tests_there(684, NULL) && chunks_packs_dropped_tests_entries(io) == 0,
			   "!home(684).exists() && entries(io) == 0");
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_dropped_tests_a_pass_terminated_at_any_point_leaves_its_entry_for_the_next:
 * a pass terminated at any of its yield points, under its claim of an entry
 * among them, leaves the entry for the next pass or has finished it: the next
 * pass leaves no directory and no entry.
 */
void
test_chunks_packs_dropped_tests_a_pass_terminated_at_any_point_leaves_its_entry_for_the_next(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	size_t		terminated = 0;

	for (uint32 repository = 685, yields = 1; repository < 725; repository++, yields++)
	{
		SimcActor	pass = {chunks_packs_dropped_tests_pass, NULL};
		SimcRun		run;
		SimcReport	report;
		SimcEnding	ended = {SIMC_ENDING_TERMINATED};
		uint64		at = yields;
		StdFmtPiece pieces[] = {STD_FMT_LIT("terminated after "), STD_FMT_ARG(0), STD_FMT_LIT(" yield points")};
		StdFmtArg	args[] = {{&at, std_u64_Display_fmt}};
		StdFmtArguments after = {pieces, lengthof(pieces), args};

		chunks_packs_dropped_tests_held_then_dropped(io, repository);
		run = simc_Run_new(53);
		simc_Run_actor(&run, "pass", 4, pass);
		simc_Run_terminate_at(&run, "pass", 4, yields);
		report = simc_Run_finish(run);
		if (report.outcome.tag != SIMC_OUTCOME_ENDED)
		{
			StdFmtPiece outcome_pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	outcome_args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
			StdFmtArguments outcome = {outcome_pieces, lengthof(outcome_pieces), outcome_args};

			std_panic_fmt(RUST_FILE, 327, 71, &outcome);
		}
		std_index_check(0, report.outcome.u.ended.len, RUST_FILE, 328, 39);
		if (simc_Ending_PartialEq_eq(&STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, 0).f1, &ended))
			terminated = STD_ADD(usize, terminated, 1, 328, 13);
		simc_Report_drop(&report);
		chunks_packs_dropped_tests_removed(io, 329, 28);
		if (chunks_packs_dropped_tests_there(repository, NULL) || chunks_packs_dropped_tests_entries(io) != 0)
			std_panic_fmt(RUST_FILE, 330, 13, &after);
	}
	STD_ASSERT_MSG(332, 9, terminated > 0, "no pass was terminated");
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
