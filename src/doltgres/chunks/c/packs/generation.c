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
 * The generation a base backup keeps of a repository's list files and
 * manifest (chunks' packs/generation.rs, §III.9, bugs row 178): the
 * backup's checkpoint keeps both generations of each list file and of the
 * manifest in a directory named by the checkpoint, and a server restored
 * from the backup puts them back before it settles.
 */
#include "postgres.h"

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/generation.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/path.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/packs/fixtures.h"
#include "doltgres/chunks/packs/settle.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "packs" STD_SEP "generation.rs"

/* CHUNKS_PACKS_GENERATION_OWN_NAMES is kept_names' files of the repository's own directory. */
static const char *const CHUNKS_PACKS_GENERATION_OWN_NAMES[] = {CHUNKS_PACKS_TABLE, CHUNKS_PACKS_RUNS,
CHUNKS_PACKS_MANIFEST};

/* CHUNKS_PACKS_GENERATION_OTHER_NAMES is kept_names' files of the repository's directory in another tablespace. */
static const char *const CHUNKS_PACKS_GENERATION_OTHER_NAMES[] = {CHUNKS_PACKS_RUNS};

/*
 * chunks_packs_generation_kept_names returns the files a generation keeps
 * from a directory of the repository's in a tablespace, with their count:
 * the pack table's, the runs file's, and the manifest's in its own
 * directory, and the runs file's in any other.
 */
static const char *const *
chunks_packs_generation_kept_names(uint32 space, size_t *count)
{
	if (space == CHUNKS_PACKS_DEFAULT_SPACE)
	{
		*count = lengthof(CHUNKS_PACKS_GENERATION_OWN_NAMES);
		return CHUNKS_PACKS_GENERATION_OWN_NAMES;
	}
	*count = lengthof(CHUNKS_PACKS_GENERATION_OTHER_NAMES);
	return CHUNKS_PACKS_GENERATION_OTHER_NAMES;
}

/*
 * chunks_packs_generation_generation_dir returns the directory that keeps a
 * base backup's generation in one of the repository's directories, named by
 * the backup's checkpoint.
 */
static StdPathBuf
chunks_packs_generation_generation_dir(const char *dir, size_t dir_len, HostLsn checkpoint)
{
	StdPathBuf	backup;
	StdPathBuf	result;
	char		name[17];

	std_PathBuf_new(&backup);
	std_path_join(dir, dir_len, CHUNKS_PACKS_BACKUP, strlen(CHUNKS_PACKS_BACKUP), &backup);
	snprintf(name, sizeof(name), "%016" PRIX64, (uint64) checkpoint);
	std_PathBuf_new(&result);
	std_path_join(backup.bytes.ptr, backup.bytes.len, name, strlen(name), &result);
	std_vec_free_Checked(&backup.bytes, 1, 1);
	return result;
}

/* chunks_packs_generation_joined sets a path to a directory joined with a name, freeing the one it held. */
static void
chunks_packs_generation_joined(StdPathBuf *path, const char *dir, size_t dir_len, const char *name, size_t name_len)
{
	std_vec_free_Checked(&path->bytes, 1, 1);
	std_PathBuf_new(path);
	std_path_join(dir, dir_len, name, name_len, path);
}

/* chunks_packs_generation_replaced sets a path to one it is given, freeing the one it held. */
static void
chunks_packs_generation_replaced(StdPathBuf *path, StdPathBuf given)
{
	std_vec_free_Checked(&path->bytes, 1, 1);
	*path = given;
}

/* chunks_packs_generation_spaced_drop frees a vector of spaced directories. */
static void
chunks_packs_generation_spaced_drop(StdVec *dirs)
{
	for (size_t k = 0, k_len = dirs->len; k < k_len; k++)
		std_vec_free_Checked(&STD_VEC_AT(dirs, chunks_TupleU32PathBuf_t, k).f1.bytes, 1, 1);
	std_vec_free_Checked(dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	std_vec_new(dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
}

/*
 * chunks_packs_generation_keep_frame_0_t is keep's state across its pad: the
 * files' flag and its Io, the lists lock, the directories, and the paths it
 * holds.
 */
typedef struct chunks_packs_generation_keep_frame_0_t
{
	const ChunksPacksBackendPacks *self;
	HostIoIo   *io;
	HostLsn		checkpoint;
	ChunksErrorError *error;
	bool		ok;
	HostSharedFlagGuard rewriting;
	bool		rewriting_live;
	HostSharedFlagIo flagged;
	HostSharedLongGuard long_;
	bool		long_live;
	StdVec		dirs;
	StdPathBuf	into;
	StdPathBuf	path;
	StdPathBuf	to;
} chunks_packs_generation_keep_frame_0_t;

/* chunks_packs_generation_keep_pad_0 is keep's pad: the paths, the directories, the lock, then the flag. */
static void
chunks_packs_generation_keep_pad_0(void *frame_)
{
	chunks_packs_generation_keep_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->to.bytes, 1, 1);
	std_PathBuf_new(&frame->to);
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_free_Checked(&frame->into.bytes, 1, 1);
	std_PathBuf_new(&frame->into);
	chunks_packs_generation_spaced_drop(&frame->dirs);
	if (frame->long_live)
	{
		frame->long_live = false;
		host_shared_LongGuard_drop(&frame->long_);
	}
	if (frame->rewriting_live)
	{
		frame->rewriting_live = false;
		host_shared_FlagGuard_drop(&frame->rewriting);
	}
}

/* chunks_packs_generation_keep_body_0 is keep's body. */
static void
chunks_packs_generation_keep_body_0(void *frame_)
{
	chunks_packs_generation_keep_frame_0_t *frame = frame_;
	const ChunksPacksBackendPacks *self = frame->self;
	HostSharedLongIo held;
	HostIoIoDoors io;
	HostError	host;

	frame->ok = false;
	if (!host_shared_Record_flag(host_shared_Held_record(&self->held), HOST_IO_IO_TAKES_SHORT(frame->io),
								 CHUNKS_PACKS_SHARED_REWRITING_INDEX, &frame->rewriting, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->rewriting_live = true;
	frame->flagged = host_shared_FlagGuard_io(&frame->rewriting);
	if (!host_shared_FlagIo_lock_long(&frame->flagged, CHUNKS_PACKS_SHARED_LISTS_LOCK_INDEX, &frame->long_, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		goto done;
	}
	frame->long_live = true;
	held = host_shared_LongGuard_io(&frame->long_);
	io = HOST_SHARED_LONG_IO_IO_DOORS(&held);
	if (!chunks_packs_open_spaced_directories(HOST_IO_IO_DOORS_READ_DOORS(io), (uint32) self->repository, self->own,
											  &frame->dirs, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
	{
		const chunks_TupleU32PathBuf_t *spaced = &STD_VEC_AT(&frame->dirs, chunks_TupleU32PathBuf_t, k);
		const StdPathBuf *dir = &spaced->f1;
		size_t		count;
		const char *const *names = chunks_packs_generation_kept_names(spaced->f0, &count);
		bool		there;

		chunks_packs_generation_replaced(&frame->into,
										 chunks_packs_generation_generation_dir(dir->bytes.ptr, dir->bytes.len,
																				frame->checkpoint));
		if (!chunks_file_dir_there(HOST_IO_IO_DOORS_READ_DOORS(io), frame->into.bytes.ptr, frame->into.bytes.len,
								   &there, frame->error))
			goto done;
		if (there)
			continue;
		chunks_packs_generation_joined(&frame->path, dir->bytes.ptr, dir->bytes.len, CHUNKS_PACKS_BACKUP,
									   strlen(CHUNKS_PACKS_BACKUP));
		if (!chunks_file_create_dir(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->error) ||
			!chunks_file_create_dir(io, frame->into.bytes.ptr, frame->into.bytes.len, frame->error))
			goto done;
		for (size_t n = 0; n < count; n++)
		{
			for (int older = 0; older < 2; older++)
			{
				bool		exists;

				chunks_packs_generation_joined(&frame->path, dir->bytes.ptr, dir->bytes.len, names[n],
											   strlen(names[n]));
				if (older)
					chunks_packs_generation_replaced(&frame->path,
													 chunks_file_previous(frame->path.bytes.ptr,
																		  frame->path.bytes.len));
				if (!chunks_file_exists(HOST_IO_IO_DOORS_READ_DOORS(io), frame->path.bytes.ptr, frame->path.bytes.len,
										&exists, frame->error))
					goto done;
				if (!exists)
					continue;
				chunks_packs_generation_joined(&frame->to, frame->into.bytes.ptr, frame->into.bytes.len, names[n],
											   strlen(names[n]));
				if (older)
					chunks_packs_generation_replaced(&frame->to,
													 chunks_file_previous(frame->to.bytes.ptr, frame->to.bytes.len));
				if (!chunks_packs_backend_copied_file(io, frame->path.bytes.ptr, frame->path.bytes.len,
													  frame->to.bytes.ptr, frame->to.bytes.len, frame->error))
					goto done;
			}
		}
	}
	frame->ok = true;
done:
	chunks_packs_generation_keep_pad_0(frame);
}

/*
 * chunks_packs_generation_Packs_keep writes the generation of a base backup's
 * checkpoint: both generations of each kept file that exists, in each of the
 * repository's directories, into that directory's generation directory, made
 * first, under the files' flag and the lists lock. A directory that kept the
 * checkpoint's generation already keeps it.
 */
bool
chunks_packs_generation_Packs_keep(const ChunksPacksBackendPacks *self, HostIoIo *io, HostLsn checkpoint,
								   ChunksErrorError *error)
{
	chunks_packs_generation_keep_frame_0_t frame = {.self = self, .io = io, .checkpoint = checkpoint,
	.error = error};

	std_vec_new(&frame.dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	std_PathBuf_new(&frame.into);
	std_PathBuf_new(&frame.path);
	std_PathBuf_new(&frame.to);
	std_landing(&frame, chunks_packs_generation_keep_body_0, chunks_packs_generation_keep_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_generation_restore_frame_0_t is restore_generation's state
 * across its pad: the directories and the paths it holds.
 */
typedef struct chunks_packs_generation_restore_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		own;
	HostLsn		checkpoint;
	ChunksErrorError *error;
	bool		ok;
	StdVec		dirs;
	StdPathBuf	from;
	StdPathBuf	newest;
	StdPathBuf	older;
	StdPathBuf	kept;
	StdPathBuf	kept_older;
} chunks_packs_generation_restore_frame_0_t;

/* chunks_packs_generation_restore_pad_0 is restore_generation's pad: the paths, then the directories. */
static void
chunks_packs_generation_restore_pad_0(void *frame_)
{
	chunks_packs_generation_restore_frame_0_t *frame = frame_;
	StdPathBuf *paths[] = {&frame->kept_older, &frame->kept, &frame->older, &frame->newest, &frame->from};

	for (size_t k = 0; k < lengthof(paths); k++)
	{
		std_vec_free_Checked(&paths[k]->bytes, 1, 1);
		std_PathBuf_new(paths[k]);
	}
	chunks_packs_generation_spaced_drop(&frame->dirs);
}

/* chunks_packs_generation_restore_body_0 is restore_generation's body. */
static void
chunks_packs_generation_restore_body_0(void *frame_)
{
	chunks_packs_generation_restore_frame_0_t *frame = frame_;
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(frame->io);
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(frame->io);

	frame->ok = false;
	if (!chunks_packs_open_spaced_directories(read, frame->repository, frame->own, &frame->dirs, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
	{
		const chunks_TupleU32PathBuf_t *spaced = &STD_VEC_AT(&frame->dirs, chunks_TupleU32PathBuf_t, k);
		const StdPathBuf *dir = &spaced->f1;
		size_t		count;
		const char *const *names = chunks_packs_generation_kept_names(spaced->f0, &count);
		bool		there;

		chunks_packs_generation_replaced(&frame->from,
										 chunks_packs_generation_generation_dir(dir->bytes.ptr, dir->bytes.len,
																				frame->checkpoint));
		if (!chunks_file_dir_there(read, frame->from.bytes.ptr, frame->from.bytes.len, &there, frame->error))
			goto done;
		if (!there)
			continue;
		for (size_t n = 0; n < count; n++)
		{
			bool		kept_older;
			bool		kept;
			bool		older;

			chunks_packs_generation_joined(&frame->newest, dir->bytes.ptr, dir->bytes.len, names[n],
										   strlen(names[n]));
			chunks_packs_generation_replaced(&frame->older,
											 chunks_file_previous(frame->newest.bytes.ptr, frame->newest.bytes.len));
			chunks_packs_generation_joined(&frame->kept, frame->from.bytes.ptr, frame->from.bytes.len, names[n],
										   strlen(names[n]));
			chunks_packs_generation_replaced(&frame->kept_older,
											 chunks_file_previous(frame->kept.bytes.ptr, frame->kept.bytes.len));
			if (!chunks_file_exists(read, frame->kept.bytes.ptr, frame->kept.bytes.len, &kept, frame->error))
				goto done;
			if (kept)
			{
				if (!chunks_file_exists(read, frame->kept_older.bytes.ptr, frame->kept_older.bytes.len, &kept_older,
										frame->error))
					goto done;
				if (!kept_older)
				{
					if (!chunks_file_exists(read, frame->older.bytes.ptr, frame->older.bytes.len, &older, frame->error))
						goto done;
					if (older && !chunks_file_remove_file(io, frame->older.bytes.ptr, frame->older.bytes.len,
														  frame->error))
						goto done;
				}
			}
			if (!chunks_file_exists(read, frame->kept.bytes.ptr, frame->kept.bytes.len, &kept, frame->error))
				goto done;
			if (kept &&
				(!chunks_file_rename(io, frame->kept.bytes.ptr, frame->kept.bytes.len, frame->newest.bytes.ptr,
									 frame->newest.bytes.len, frame->error) ||
				 !chunks_file_sync_dir(io, dir->bytes.ptr, dir->bytes.len, frame->error)))
				goto done;
			if (!chunks_file_exists(read, frame->kept_older.bytes.ptr, frame->kept_older.bytes.len, &kept_older,
									frame->error))
				goto done;
			if (kept_older &&
				(!chunks_file_rename(io, frame->kept_older.bytes.ptr, frame->kept_older.bytes.len,
									 frame->older.bytes.ptr, frame->older.bytes.len, frame->error) ||
				 !chunks_file_sync_dir(io, dir->bytes.ptr, dir->bytes.len, frame->error)))
				goto done;
		}
	}
	frame->ok = true;
done:
	chunks_packs_generation_restore_pad_0(frame);
}

/*
 * chunks_packs_generation_Packs_restore_generation puts back, in place of each
 * of a repository's kept files and its previous generation, the ones a base
 * backup's checkpoint kept, in each of its directories that kept them,
 * before the startup process settles the repository restored from that
 * backup: a kept file's previous generation the checkpoint kept none of is
 * removed. Each step is durable before the next, in an order whose every
 * prefix a start that stops part way finishes when it starts again from the
 * backup.
 */
bool
chunks_packs_generation_Packs_restore_generation(HostIoIo *io, uint32 repository, uint32 own, HostLsn checkpoint,
												 ChunksErrorError *error)
{
	chunks_packs_generation_restore_frame_0_t frame = {.io = io, .repository = repository, .own = own,
	.checkpoint = checkpoint, .error = error};

	std_vec_new(&frame.dirs, sizeof(chunks_TupleU32PathBuf_t), _Alignof(chunks_TupleU32PathBuf_t));
	std_PathBuf_new(&frame.from);
	std_PathBuf_new(&frame.newest);
	std_PathBuf_new(&frame.older);
	std_PathBuf_new(&frame.kept);
	std_PathBuf_new(&frame.kept_older);
	std_landing(&frame, chunks_packs_generation_restore_body_0, chunks_packs_generation_restore_pad_0);
	return frame.ok;
}

/*
 * chunks_packs_generation_drop_frame_0_t is drop_generations' state across
 * its pad: the directories and the path it holds.
 */
typedef struct chunks_packs_generation_drop_frame_0_t
{
	HostIoIoDoors io;
	uint32		repository;
	uint32		own;
	ChunksErrorError *error;
	bool		ok;
	StdVec		dirs;
	StdPathBuf	path;
} chunks_packs_generation_drop_frame_0_t;

/* chunks_packs_generation_drop_pad_0 is drop_generations' pad: the path, then the directories. */
static void
chunks_packs_generation_drop_pad_0(void *frame_)
{
	chunks_packs_generation_drop_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_PathBuf_new(&frame->path);
	std_vec_drop_Checked(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf), chunks_pack_PathBuf_drop);
	std_vec_new(&frame->dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf));
}

/* chunks_packs_generation_drop_body_0 is drop_generations' body. */
static void
chunks_packs_generation_drop_body_0(void *frame_)
{
	chunks_packs_generation_drop_frame_0_t *frame = frame_;
	HostError	host;
	bool		gone;

	frame->ok = false;
	if (!chunks_packs_open_directories(HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->repository, frame->own,
									   &frame->dirs, frame->error))
		goto done;
	for (size_t k = 0, k_len = frame->dirs.len; k < k_len; k++)
	{
		const StdPathBuf *dir = &STD_VEC_AT(&frame->dirs, StdPathBuf, k);

		chunks_packs_generation_joined(&frame->path, dir->bytes.ptr, dir->bytes.len, CHUNKS_PACKS_BACKUP,
									   strlen(CHUNKS_PACKS_BACKUP));
		if (!host_remove_tree(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &gone, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			goto done;
		}
	}
	frame->ok = true;
done:
	chunks_packs_generation_drop_pad_0(frame);
}

/*
 * chunks_packs_generation_Packs_drop_generations removes every generation base
 * backups kept in a repository's directories, which nothing reads again once
 * no backup is counted in, or at a start that restores none.
 */
bool
chunks_packs_generation_Packs_drop_generations(HostIoIoDoors io, uint32 repository, uint32 own,
											   ChunksErrorError *error)
{
	chunks_packs_generation_drop_frame_0_t frame = {.io = io, .repository = repository, .own = own, .error = error};

	std_vec_new(&frame.dirs, sizeof(StdPathBuf), _Alignof(StdPathBuf));
	std_PathBuf_new(&frame.path);
	std_landing(&frame, chunks_packs_generation_drop_body_0, chunks_packs_generation_drop_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_packs_generation_tests_packs returns the names of a directory's pack and address-index files. */
static StdVec
chunks_packs_generation_tests_packs(const StdPathBuf *dir)
{
	StdVec		files;
	StdVec		names;
	StdIoError	io_error;
	bool		entry;

	if (!std_fs_read_dir_names(dir->bytes.ptr, dir->bytes.len, &files, &entry, &io_error))
	{
		if (entry)
			STD_RESULT_EXPECT_FAILED(132, 32, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		STD_RESULT_EXPECT_FAILED(131, 14, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	}
	std_vec_new(&names, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0, k_len = files.len; k < k_len; k++)
	{
		StdString  *name = &STD_VEC_AT(&files, StdString, k);
		bool		pack = name->len >= 5 && memcmp((const char *) name->ptr + name->len - 5, ".pack", 5) == 0;
		bool		idx = name->len >= 4 && memcmp((const char *) name->ptr + name->len - 4, ".idx", 4) == 0;

		if (pack || idx)
			std_vec_push_Checked(&names, name, sizeof(StdString), _Alignof(StdString));
		else
			std_vec_free_Checked(name, 1, 1);
	}
	std_vec_free_Checked(&files, sizeof(StdString), _Alignof(StdString));
	return names;
}

/* chunks_packs_generation_tests_holds reports whether a list of names holds one. */
static bool
chunks_packs_generation_tests_holds(const StdVec *names, const StdString *name)
{
	for (size_t k = 0, k_len = names->len; k < k_len; k++)
	{
		const StdString *held = &STD_VEC_AT(names, StdString, k);

		if (held->len == name->len && memcmp(held->ptr, name->ptr, name->len) == 0)
			return true;
	}
	return false;
}

/* chunks_packs_generation_tests_ok panics with an error at a line and column unless the call was ok, as unwrap does. */
static void
chunks_packs_generation_tests_ok(bool ok, ChunksErrorError *error, uint32 line, uint32 col)
{
	if (!ok)
		chunks_packs_tests_unwrapped(RUST_FILE, error, line, col);
}

/*
 * test_chunks_packs_generation_tests_a_restore_settles_from_the_kept_generation_without_a_later_pack:
 * a backup copies a pack table that names a pack written after the backup
 * listed the pack's directory, whose file the backup lacks, and its settle
 * fails on that pack. Restored from the generation the backup's checkpoint
 * kept, the repository settles, finds the chunk the generation listed, and
 * lacks the later one; the start removes the generation.
 */
void
test_chunks_packs_generation_tests_a_restore_settles_from_the_kept_generation_without_a_later_pack(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 145, 46);
	StdPathBuf	dir = host_tablespace_dir(93, HOST_PG_DEFAULT);
	ChunksPacksBackendPacks packs_of;
	ChunksPacksBackendPacks after;
	chunks_TupleAddressLocator_t one;
	chunks_TupleAddressLocator_t two;
	ChunksPacksSettleSettled settled;
	ChunksErrorError error;
	ChunksTrailerLocator found;
	StdVec		before;
	StdVec		now;
	StdPathBuf	path;
	bool		failed;
	bool		has;

	chunks_packs_fixtures_created(io, 93, &packs_of);
	one = chunks_packs_fixtures_added(io, &packs_of, (const uint8 *) "one", 3);
	before = chunks_packs_generation_tests_packs(&dir);
	chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_keep(&packs_of, io, 0x04000080, &error), &error,
									 150, 40);
	two = chunks_packs_fixtures_added(io, &packs_of, (const uint8 *) "two", 3);
	chunks_packs_fixtures_restarted(io, &packs_of);
	now = chunks_packs_generation_tests_packs(&dir);
	for (size_t k = 0, k_len = now.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&now, StdString, k);
		StdIoError	io_error;

		if (chunks_packs_generation_tests_holds(&before, name))
			continue;
		std_PathBuf_new(&path);
		std_path_join(dir.bytes.ptr, dir.bytes.len, (const char *) name->ptr, name->len, &path);
		if (!std_fs_remove_file(path.bytes.ptr, path.bytes.len, &io_error))
			STD_RESULT_EXPECT_FAILED(154, 50, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		std_vec_free_Checked(&path.bytes, 1, 1);
	}
	failed = !chunks_packs_settle_Packs_settle(io, 93, HOST_PG_DEFAULT, &settled, &error);
	if (failed)
		chunks_error_Error_drop(&error);
	else
		chunks_packs_settle_Settled_drop(&settled);
	STD_ASSERT_MSG(156, 9, failed, "the copied table's later pack was found");

	chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_restore_generation(io, 93, HOST_PG_DEFAULT,
																					  0x04000080, &error),
									 &error, 158, 74);
	chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(io), 93,
																					HOST_PG_DEFAULT, &error),
									 &error, 159, 59);
	std_PathBuf_new(&path);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_BACKUP, strlen(CHUNKS_PACKS_BACKUP), &path);
	STD_ASSERT_MSG(160, 9, !std_path_Path_exists(path.bytes.ptr, path.bytes.len), "the generation was left");
	std_vec_free_Checked(&path.bytes, 1, 1);
	chunks_packs_generation_tests_ok(chunks_packs_settle_Packs_settle(io, 93, HOST_PG_DEFAULT, &settled, &error),
									 &error, 161, 49);
	chunks_packs_settle_Settled_drop(&settled);
	chunks_packs_generation_tests_ok(chunks_packs_open_Packs_open(io, 93, HOST_PG_DEFAULT, &after, &error), &error,
									 162, 62);
	chunks_packs_tests_found(RUST_FILE, &after, io, &reading, &one.f0, &one.f1, 163, 9, 54);
	chunks_packs_generation_tests_ok(chunks_packs_backend_Packs_find(&after, io, &reading, &two.f0, &has, &found,
																	 &error),
									 &error, 164, 54);
	if (has)
	{
		StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef want = {false, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};

		std_assert_failed(RUST_FILE, 164, 9, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
	chunks_packs_backend_Packs_drop(&after);
	std_vec_drop_Checked(&now, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_drop_Checked(&before, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* chunks_packs_generation_tests_Kept is Kept: the bytes of each kept file of a repository's own directory and of its previous generation. */
typedef struct chunks_packs_generation_tests_Kept
{
	bool		has[3][2];
	StdVec		bytes[3][2];
} chunks_packs_generation_tests_Kept;

/* chunks_packs_generation_tests_Kept_drop frees a Kept. */
static void
chunks_packs_generation_tests_Kept_drop(chunks_packs_generation_tests_Kept *self)
{
	for (int n = 0; n < 3; n++)
		for (int g = 0; g < 2; g++)
			if (self->has[n][g])
				std_vec_free_Checked(&self->bytes[n][g], 1, 1);
}

/* chunks_packs_generation_tests_Kept_eq is Kept's ==. */
static bool
chunks_packs_generation_tests_Kept_eq(const chunks_packs_generation_tests_Kept *a,
									  const chunks_packs_generation_tests_Kept *b)
{
	for (int n = 0; n < 3; n++)
		for (int g = 0; g < 2; g++)
		{
			if (a->has[n][g] != b->has[n][g])
				return false;
			if (a->has[n][g] && (a->bytes[n][g].len != b->bytes[n][g].len ||
								 memcmp(a->bytes[n][g].ptr, b->bytes[n][g].ptr, a->bytes[n][g].len) != 0))
				return false;
		}
	return true;
}

/* chunks_packs_generation_tests_bytes_Debug_fmt is Option<Vec<u8>>'s Debug over a Kept's entry. */
static bool
chunks_packs_generation_tests_bytes_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *bytes = value;
	StdBytes	shown = {bytes->ptr, bytes->len};

	return std_bytes_Debug_fmt(&shown, f);
}

/* chunks_packs_generation_tests_Kept_Debug_fmt is Kept's Debug: a list of pairs of options of bytes. */
static bool
chunks_packs_generation_tests_Kept_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_packs_generation_tests_Kept *kept = value;
	StdDebugList l = std_fmt_debug_list(f);
	StdOptionRef options[3][2];
	StdTupleRef pairs[3];

	for (int n = 0; n < 3; n++)
	{
		for (int g = 0; g < 2; g++)
		{
			options[n][g].has = kept->has[n][g];
			options[n][g].some = STD_ITEM(&kept->bytes[n][g], chunks_packs_generation_tests_bytes_Debug_fmt);
		}
		pairs[n].len = 2;
		pairs[n].items[0] = STD_ITEM(&options[n][0], std_option_Debug_fmt);
		pairs[n].items[1] = STD_ITEM(&options[n][1], std_option_Debug_fmt);
		std_DebugList_entry(&l, &pairs[n], std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&l);
}

/* chunks_packs_generation_tests_kept_bytes returns the bytes of each kept file in a directory, and of its previous generation. */
static chunks_packs_generation_tests_Kept
chunks_packs_generation_tests_kept_bytes(const StdPathBuf *from)
{
	chunks_packs_generation_tests_Kept kept = {0};
	size_t		count;
	const char *const *names = chunks_packs_generation_kept_names(CHUNKS_PACKS_DEFAULT_SPACE, &count);

	for (size_t n = 0; n < count; n++)
	{
		StdPathBuf	path;
		StdPathBuf	older;
		StdIoError	io_error;

		std_PathBuf_new(&path);
		std_path_join(from->bytes.ptr, from->bytes.len, names[n], strlen(names[n]), &path);
		older = chunks_file_previous(path.bytes.ptr, path.bytes.len);
		kept.has[n][0] = std_fs_read(path.bytes.ptr, path.bytes.len, &kept.bytes[n][0], &io_error);
		kept.has[n][1] = std_fs_read(older.bytes.ptr, older.bytes.len, &kept.bytes[n][1], &io_error);
		std_vec_free_Checked(&older.bytes, 1, 1);
		std_vec_free_Checked(&path.bytes, 1, 1);
	}
	return kept;
}

/*
 * chunks_packs_generation_tests_backed_up makes a repository with an id as a
 * server restored from a backup of it has it, and returns its own directory,
 * the first pack's chunk and place, and the bytes the generation keeps.
 */
static void
chunks_packs_generation_tests_backed_up(HostIoIo *io, uint32 id, StdPathBuf *dir, chunks_TupleAddressLocator_t *one,
										chunks_packs_generation_tests_Kept *kept)
{
	HostSharedReader reading = chunks_packs_tests_reading(RUST_FILE, io, 175, 54);
	ChunksPacksBackendPacks packs_of;
	ChunksErrorError error;
	StdVec		before;
	StdVec		now;
	StdPathBuf	from;
	bool		older = false;

	*dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
	chunks_packs_fixtures_created(io, id, &packs_of);
	*one = chunks_packs_fixtures_added(io, &packs_of, (const uint8 *) "one", 3);
	chunks_packs_generation_tests_ok(chunks_packs_backend_Packs_compact_lists(&packs_of, io, &reading, &error), &error,
									 178, 46);
	before = chunks_packs_generation_tests_packs(dir);
	chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_keep(&packs_of, io, 0x04000080, &error), &error,
									 180, 40);
	(void) chunks_packs_fixtures_added(io, &packs_of, (const uint8 *) "two", 3);
	chunks_packs_fixtures_restarted(io, &packs_of);
	now = chunks_packs_generation_tests_packs(dir);
	for (size_t k = 0, k_len = now.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&now, StdString, k);
		StdPathBuf	path;
		StdIoError	io_error;

		if (chunks_packs_generation_tests_holds(&before, name))
			continue;
		std_PathBuf_new(&path);
		std_path_join(dir->bytes.ptr, dir->bytes.len, (const char *) name->ptr, name->len, &path);
		if (!std_fs_remove_file(path.bytes.ptr, path.bytes.len, &io_error))
			STD_RESULT_EXPECT_FAILED(184, 50, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
		std_vec_free_Checked(&path.bytes, 1, 1);
	}
	from = chunks_packs_generation_generation_dir(dir->bytes.ptr, dir->bytes.len, 0x04000080);
	*kept = chunks_packs_generation_tests_kept_bytes(&from);
	std_vec_free_Checked(&from.bytes, 1, 1);
	for (int n = 0; n < 3; n++)
		older = older || kept->has[n][1];
	STD_ASSERT_MSG(187, 9, older, "no kept file had a previous generation");
	std_vec_drop_Checked(&now, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_drop_Checked(&before, sizeof(StdString), _Alignof(StdString), std_String_drop);
	host_shared_Reader_drop(&reading);
}

/* chunks_packs_generation_tests_failed returns a chunks error as the host's, as a simulated process's actor returns it. */
static HostError
chunks_packs_generation_tests_failed(ChunksErrorError *error)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	chunks_error_Error_drop(error);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* chunks_packs_generation_tests_start_t is a crash test actor's captures: the repository. */
typedef struct chunks_packs_generation_tests_start_t
{
	uint32		id;
} chunks_packs_generation_tests_start_t;

/* chunks_packs_generation_tests_actor returns an actor over a copy of a crash test's captures, which the run takes. */
static SimcActor
chunks_packs_generation_tests_actor(bool (*body) (void *captures, HostIoIo *io, HostError *error), uint32 id)
{
	chunks_packs_generation_tests_start_t *captures = std_alloc(sizeof(*captures),
																_Alignof(chunks_packs_generation_tests_start_t));
	SimcActor	actor = {body, captures, NULL, sizeof(*captures), _Alignof(chunks_packs_generation_tests_start_t)};

	captures->id = id;
	return actor;
}

/* test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_0 is the durability test's restore. */
static bool
test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const chunks_packs_generation_tests_start_t *start = captures;
	ChunksErrorError failure;

	if (!chunks_packs_generation_Packs_restore_generation(io, start->id, HOST_PG_DEFAULT, 0x04000080, &failure))
	{
		*error = chunks_packs_generation_tests_failed(&failure);
		return false;
	}
	return true;
}

/* test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_1 is the durability test's removal, and the file it makes after it. */
static bool
test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_1(void *captures, HostIoIo *io, HostError *error)
{
	const chunks_packs_generation_tests_start_t *start = captures;
	ChunksErrorError failure;
	StdPathBuf	dir = host_tablespace_dir(start->id, HOST_PG_DEFAULT);
	StdPathBuf	after;
	HostFile	file;
	bool		ok;

	std_PathBuf_new(&after);
	std_path_join(dir.bytes.ptr, dir.bytes.len, "after", 5, &after);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	ok = chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(io), start->id, HOST_PG_DEFAULT,
														&failure) &&
		chunks_file_create(HOST_IO_IO_IO_DOORS(io), after.bytes.ptr, after.bytes.len, &file, &failure);
	std_vec_free_Checked(&after.bytes, 1, 1);
	if (!ok)
	{
		*error = chunks_packs_generation_tests_failed(&failure);
		return false;
	}
	host_File_drop(&file);
	return true;
}

/* test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start_closure_0 is the crash test's start: the restore, then the removal. */
static bool
test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const chunks_packs_generation_tests_start_t *start = captures;
	ChunksErrorError failure;

	if (!chunks_packs_generation_Packs_restore_generation(io, start->id, HOST_PG_DEFAULT, 0x04000080, &failure) ||
		!chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(io), start->id, HOST_PG_DEFAULT,
														&failure))
	{
		*error = chunks_packs_generation_tests_failed(&failure);
		return false;
	}
	return true;
}

/* chunks_packs_generation_tests_kept_eq asserts at a line that a directory's kept files are a generation's, with a message. */
static void
chunks_packs_generation_tests_kept_eq(const StdPathBuf *dir, const chunks_packs_generation_tests_Kept *want,
									  uint32 line, uint32 col, const StdFmtArguments *message)
{
	chunks_packs_generation_tests_Kept got = chunks_packs_generation_tests_kept_bytes(dir);

	if (!chunks_packs_generation_tests_Kept_eq(&got, want))
		std_assert_failed(RUST_FILE, line, col, "==", &got, chunks_packs_generation_tests_Kept_Debug_fmt, want,
						  chunks_packs_generation_tests_Kept_Debug_fmt, message);
	chunks_packs_generation_tests_Kept_drop(&got);
}

/*
 * test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal:
 * a restore's renames are durable once it returns, so a crash after the
 * removal of the generations never takes them back.
 */
void
test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	root;
	chunks_TupleAddressLocator_t one;
	chunks_packs_generation_tests_Kept want;
	SimcSim		sim;
	SimcRun		run;
	SimcReport	report;
	StdIoError	io_error;
	bool		outcome;
	StdFmtPiece pieces[] = {STD_FMT_LIT("a crash after the restore took back its renames")};
	StdFmtArguments message = {pieces, 1, NULL};

	chunks_packs_generation_tests_backed_up(io, 9290, &dir, &one, &want);
	root = simc_data_dir();
	sim = simc_Sim_start(root.bytes.ptr, root.bytes.len, 43);
	std_vec_free_Checked(&root.bytes, 1, 1);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "restore", 7, chunks_packs_generation_tests_actor(test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_0,
																		   9290));
	report = simc_Run_finish(run);
	outcome = report.outcome.tag == SIMC_OUTCOME_ENDED;
	simc_Report_drop(&report);
	STD_ASSERT_MSG(219, 9, outcome, "the restore didn't end");
	simc_Sim_crash_at(&sim, std_wrapping_add_u64(simc_Sim_calls(&sim), 1));
	run = simc_Run_new(1);
	simc_Run_actor(&run, "removal", 7, chunks_packs_generation_tests_actor(test_chunks_packs_generation_tests_a_restored_generation_is_durable_before_its_removal_closure_1,
																		   9290));
	report = simc_Run_finish(run);
	outcome = report.outcome.tag == SIMC_OUTCOME_CRASHED;
	simc_Report_drop(&report);
	STD_ASSERT_MSG(227, 9, outcome, "nothing after the removal crashed");
	if (!simc_Sim_recover(&sim, true, &io_error))
		STD_RESULT_EXPECT_FAILED(228, 27, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	simc_Sim_stop(&sim);
	simc_Sim_drop(&sim);
	chunks_packs_generation_tests_kept_eq(&dir, &want, 230, 9, &message);
	chunks_packs_generation_tests_Kept_drop(&want);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start:
 * a start from a backup that a crash stops at any step of its restore of
 * the kept generation, or of the generations' removal after it, leaves the
 * next start from the backup to finish it.
 */
void
test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		crashes = 0;

	for (uint32 step = 1; step < 200; step++)
	{
		uint32		id = 9300 + step;
		StdPathBuf	dir;
		StdPathBuf	root;
		chunks_TupleAddressLocator_t one;
		chunks_packs_generation_tests_Kept want;
		ChunksPacksBackendPacks packs_of;
		ChunksPacksSettleSettled settled;
		ChunksErrorError error;
		HostSharedReader reading;
		ChunksTrailerLocator found;
		bool		has;
		SimcSim		sim;
		SimcRun		run;
		SimcReport	report;
		bool		crashed;
		StdFmtPiece left_pieces[] = {STD_FMT_LIT("a crash at step "), STD_FMT_ARG(0),
		STD_FMT_LIT(" left other list files than the generation kept")};
		StdFmtPiece after_pieces[] = {STD_FMT_LIT("after a crash at step "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&step, std_u32_Display_fmt}};
		StdFmtArguments left = {left_pieces, 3, args};
		StdFmtArguments after_step = {after_pieces, 2, args};

		chunks_packs_generation_tests_backed_up(io, id, &dir, &one, &want);
		std_vec_free_Checked(&dir.bytes, 1, 1);
		dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
		root = simc_data_dir();
		sim = simc_Sim_start(root.bytes.ptr, root.bytes.len, 41);
		std_vec_free_Checked(&root.bytes, 1, 1);
		simc_Sim_crash_at(&sim, std_wrapping_add_u64(simc_Sim_calls(&sim), step));
		run = simc_Run_new(1);
		simc_Run_actor(&run, "start", 5, chunks_packs_generation_tests_actor(test_chunks_packs_generation_tests_a_restore_cut_short_by_a_crash_at_any_step_is_finished_by_the_next_start_closure_0,
																			 id));
		report = simc_Run_finish(run);
		crashed = report.outcome.tag == SIMC_OUTCOME_CRASHED;
		simc_Report_drop(&report);
		if (crashed)
		{
			StdIoError	io_error;

			if (!simc_Sim_recover(&sim, true, &io_error))
				STD_RESULT_EXPECT_FAILED(255, 35, CHUNKS_PACKS_FIXTURES_UNWRAP, &io_error, std_io_Error_Debug_fmt);
			crashes = std_wrapping_add_u64(crashes, 1);
		}
		simc_Sim_stop(&sim);
		simc_Sim_drop(&sim);
		chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_restore_generation(io, id, HOST_PG_DEFAULT,
																						  0x04000080, &error),
										 &error, 259, 78);
		chunks_packs_generation_tests_ok(chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(io), id,
																						HOST_PG_DEFAULT, &error),
										 &error, 260, 63);
		chunks_packs_generation_tests_kept_eq(&dir, &want, 261, 13, &left);
		chunks_packs_generation_tests_ok(chunks_packs_settle_Packs_settle(io, id, HOST_PG_DEFAULT, &settled, &error),
										 &error, 262, 53);
		chunks_packs_settle_Settled_drop(&settled);
		reading = chunks_packs_tests_reading(RUST_FILE, io, 263, 50);
		chunks_packs_generation_tests_ok(chunks_packs_open_Packs_open(io, id, HOST_PG_DEFAULT, &packs_of, &error),
										 &error, 264, 66);
		chunks_packs_generation_tests_ok(chunks_packs_backend_Packs_find(&packs_of, io, &reading, &one.f0, &has,
																		 &found, &error),
										 &error, 265, 58);
		if (!has || !chunks_trailer_Locator_PartialEq_eq(&found, &one.f1))
		{
			StdOptionRef got = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
			StdOptionRef wanted = {true, STD_ITEM(&one.f1, chunks_trailer_Locator_Debug_fmt)};

			std_assert_failed(RUST_FILE, 265, 13, "==", &got, std_option_Debug_fmt, &wanted, std_option_Debug_fmt,
							  &after_step);
		}
		chunks_packs_backend_Packs_drop(&packs_of);
		host_shared_Reader_drop(&reading);
		chunks_packs_generation_tests_Kept_drop(&want);
		std_vec_free_Checked(&dir.bytes, 1, 1);
		if (!crashed)
			break;
	}
	STD_ASSERT_MSG(270, 9, crashes > 0, "no step crashed");
	simc_TestIo_drop(&test_io);
}

#endif
