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
 * Positioned reads and writes, syncs, and atomic file replacement on every
 * platform, over the host's file door, so that every file chunks reads and
 * writes goes through Postgres' file layer in the server.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/simc/simc.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

/* CHUNKS_FILE_TORN_TAIL is a ChunksFileTornTail's opaque storage read as TornTail's layout, which only file.c knows. */
#define CHUNKS_FILE_TORN_TAIL(tail) ((chunks_file_TornTailLayout *) (tail))

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "file.rs"

/*
 * chunks_file_read_at_frame_0_t is read_at's state across its pad: its
 * arguments, its results, and the buffer it holds.
 */
typedef struct chunks_file_read_at_frame_0_t
{
	HostIoReadDoors io;
	const HostFile *file;
	uint64		offset;
	size_t		len;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		buffer;
	bool		buffer_live;
	bool		ok;
} chunks_file_read_at_frame_0_t;

/* chunks_file_read_at_body_0 is read_at's body. */
static void
chunks_file_read_at_body_0(void *frame_)
{
	chunks_file_read_at_frame_0_t *frame = frame_;
	HostError	error;

	std_vec_from_elem_zeroed(&frame->buffer, frame->len, 1, 1);
	frame->buffer_live = true;
	if (!host_read_at(frame->io, frame->file, frame->offset, frame->buffer.ptr, frame->buffer.len, &error))
	{
		*frame->error = chunks_error_Error_FromError_from(&error);
		frame->buffer_live = false;
		std_vec_free_Checked(&frame->buffer, 1, 1);
		return;
	}
	frame->buffer_live = false;
	*frame->result = frame->buffer;
	frame->ok = true;
}

/* chunks_file_read_at_pad_0 is read_at's pad: the buffer, while it holds it. */
static void
chunks_file_read_at_pad_0(void *frame_)
{
	chunks_file_read_at_frame_0_t *frame = frame_;

	if (frame->buffer_live)
		std_vec_free_Checked(&frame->buffer, 1, 1);
}

/* chunks_file_read_at reads exactly `len` bytes at the offset, so readers can share the file. */
bool
chunks_file_read_at(HostIoReadDoors io, const HostFile *file, uint64 offset, size_t len, StdVec *result,
					ChunksErrorError *error)
{
	chunks_file_read_at_frame_0_t frame = {.io = io, .file = file, .offset = offset, .len = len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_read_at_body_0, chunks_file_read_at_pad_0);
	return frame.ok;
}

/*
 * chunks_file_advise tells the operating system that the bytes at the offset
 * will be read soon, where it takes such advice.
 */
void
chunks_file_advise(HostIoReadDoors io, const HostFile *file, uint64 offset, uint64 len)
{
	host_prefetch(file, offset, len);
}

/*
 * CHUNKS_FILE_READ_ONLY is READ_ONLY: whether the backend changes no file,
 * which fsck sets while it checks, so that a check never writes; its
 * BackendLocal header and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicBool process;
}			CHUNKS_FILE_READ_ONLY = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * chunks_file_set_read_only makes every write of a file by the backend
 * refuse, and every open skip what it would settle, while a check runs.
 */
void
chunks_file_set_read_only(bool read_only)
{
	HostLocalLocal local = host_local_BackendLocal_get(&CHUNKS_FILE_READ_ONLY.local, &STD_ATOMIC_BOOL_TYPE);

	std_AtomicBool_store_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local, &STD_ATOMIC_BOOL_TYPE),
								 read_only);
	host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
}

/* chunks_file_read_only reports whether the backend changes no file. */
bool
chunks_file_read_only(void)
{
	HostLocalLocal local = host_local_BackendLocal_get(&CHUNKS_FILE_READ_ONLY.local, &STD_ATOMIC_BOOL_TYPE);
	bool		read_only;

	read_only = std_AtomicBool_load_relaxed((StdAtomicBool *) host_local_Local_Deref_deref(&local,
																						   &STD_ATOMIC_BOOL_TYPE));
	host_local_Local_drop(&local, &STD_ATOMIC_BOOL_TYPE);
	return read_only;
}

/*
 * chunks_file_writable_frame_0_t is writable's state across its pad: its
 * arguments, its error, the action it takes, and the description it makes.
 */
typedef struct chunks_file_writable_frame_0_t
{
	ChunksErrorAction action;
	bool		action_live;
	const char *path;
	size_t		path_len;
	ChunksErrorError *error;
	StdString	described;
	bool		described_live;
	bool		ok;
} chunks_file_writable_frame_0_t;

/* chunks_file_writable_body_0 is writable's body. */
static void
chunks_file_writable_body_0(void *frame_)
{
	chunks_file_writable_frame_0_t *frame = frame_;

	if (chunks_file_read_only())
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("could not "), STD_FMT_ARG(0), STD_FMT_LIT(": fsck changes nothing")};
		StdFmtArg	args[] = {{&frame->described, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		chunks_error_Action_describe(&frame->action, frame->path, frame->path_len, &frame->described);
		frame->described_live = true;
		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_refused(&message);
		frame->described_live = false;
		std_vec_free_Checked(&frame->described, 1, 1);
	}
	else
		frame->ok = true;
	frame->action_live = false;
	chunks_error_Action_drop(&frame->action);
}

/* chunks_file_writable_pad_0 is writable's pad: the description and the action, while it holds them. */
static void
chunks_file_writable_pad_0(void *frame_)
{
	chunks_file_writable_frame_0_t *frame = frame_;

	if (frame->described_live)
		std_vec_free_Checked(&frame->described, 1, 1);
	if (frame->action_live)
		chunks_error_Action_drop(&frame->action);
}

/* chunks_file_writable fails, naming the action, which it takes, and the file, when the backend changes no file. */
bool
chunks_file_writable(ChunksErrorAction *action, const char *path, size_t path_len, ChunksErrorError *error)
{
	chunks_file_writable_frame_0_t frame = {.action = *action, .action_live = true, .path = path,
	.path_len = path_len, .error = error};

	std_landing(&frame, chunks_file_writable_body_0, chunks_file_writable_pad_0);
	return frame.ok;
}

/* chunks_file_write_at writes all of the bytes at the offset. */
bool
chunks_file_write_at(HostIoIoDoors io, const HostFile *file, const char *path, size_t path_len, uint64 offset,
					 const uint8 *bytes, size_t bytes_len, ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	HostError	host_error;

	if (!chunks_file_writable(&action, path, path_len, error))
		return false;
	if (!host_write_at(io, file, offset, bytes, bytes_len, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/*
 * chunks_file_sync makes the file's contents durable as Postgres syncs its
 * own files, through pg_fsync, which fsync = off skips.
 */
bool
chunks_file_sync(HostIoIoDoors io, const HostFile *file, ChunksErrorError *error)
{
	HostError	host_error;

	if (!host_sync(io, file, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/*
 * chunks_file_sync_dir makes the directory's entries durable where the
 * platform allows it, as Postgres syncs its directories.
 */
bool
chunks_file_sync_dir(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	HostError	host_error;

	if (!host_sync_path(io, dir, dir_len, true, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

static bool chunks_file_write_beside(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes,
									 size_t bytes_len, StdPathBuf *result, ChunksErrorError *error);

/*
 * chunks_file_write_file_frame_0_t is write_file's state across its pad: its
 * arguments, its error, and the temporary name it holds.
 */
typedef struct chunks_file_write_file_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const uint8 *bytes;
	size_t		bytes_len;
	ChunksErrorError *error;
	StdPathBuf	temp;
	bool		temp_live;
	bool		ok;
} chunks_file_write_file_frame_0_t;

/* chunks_file_write_file_body_0 is write_file's body. */
static void
chunks_file_write_file_body_0(void *frame_)
{
	chunks_file_write_file_frame_0_t *frame = frame_;
	StdStr		dir;

	if (!chunks_file_write_beside(frame->io, frame->path, frame->path_len, frame->bytes, frame->bytes_len,
								  &frame->temp, frame->error))
		return;
	frame->temp_live = true;
	if (chunks_file_rename(frame->io, frame->temp.bytes.ptr, frame->temp.bytes.len, frame->path, frame->path_len,
						   frame->error))
	{
		dir = host_directory_of(frame->path, frame->path_len);
		frame->ok = chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error);
	}
	frame->temp_live = false;
	std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/* chunks_file_write_file_pad_0 is write_file's pad: the temporary name, while it holds it. */
static void
chunks_file_write_file_pad_0(void *frame_)
{
	chunks_file_write_file_frame_0_t *frame = frame_;

	if (frame->temp_live)
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/*
 * chunks_file_write_file writes a file whole and durably: to a temporary
 * name, synced, renamed over the path, and the directory synced, so a reader
 * sees the old file or the new one and never a part.
 */
bool
chunks_file_write_file(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes, size_t bytes_len,
					   ChunksErrorError *error)
{
	chunks_file_write_file_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .bytes = bytes,
	.bytes_len = bytes_len, .error = error};

	std_landing(&frame, chunks_file_write_file_body_0, chunks_file_write_file_pad_0);
	return frame.ok;
}

static bool chunks_file_emptied(HostIoIoDoors io, const char *path, size_t path_len, HostFile *result_0,
								bool *result_1, ChunksErrorError *error);

/*
 * chunks_file_write_beside_frame_0_t is write_beside's state across its pad:
 * its arguments, its results, and the temporary name and file it holds.
 */
typedef struct chunks_file_write_beside_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const uint8 *bytes;
	size_t		bytes_len;
	StdPathBuf *result;
	ChunksErrorError *error;
	StdPathBuf	temp;
	bool		temp_live;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_write_beside_frame_0_t;

/* chunks_file_write_beside_body_0 is write_beside's body. */
static void
chunks_file_write_beside_body_0(void *frame_)
{
	chunks_file_write_beside_frame_0_t *frame = frame_;
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	bool		created;

	if (!chunks_file_writable(&action, frame->path, frame->path_len, frame->error))
		return;
	std_path_with_extension(frame->path, frame->path_len, "tmp", 3, &frame->temp);
	frame->temp_live = true;
	if (chunks_file_emptied(frame->io, frame->temp.bytes.ptr, frame->temp.bytes.len, &frame->file, &created,
							frame->error))
	{
		frame->file_live = true;
		if (chunks_file_write_at(frame->io, &frame->file, frame->temp.bytes.ptr, frame->temp.bytes.len, 0,
								 frame->bytes, frame->bytes_len, frame->error) &&
			chunks_file_sync(frame->io, &frame->file, frame->error))
			frame->ok = true;
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	frame->temp_live = false;
	if (frame->ok)
		*frame->result = frame->temp;
	else
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/* chunks_file_write_beside_pad_0 is write_beside's pad: the file and the temporary name, while it holds them. */
static void
chunks_file_write_beside_pad_0(void *frame_)
{
	chunks_file_write_beside_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->temp_live)
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/*
 * chunks_file_write_beside writes a file's new bytes durably under a
 * temporary name beside it and returns that name, for a writer that renames
 * it over the file.
 */
static bool
chunks_file_write_beside(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes, size_t bytes_len,
						 StdPathBuf *result, ChunksErrorError *error)
{
	chunks_file_write_beside_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .bytes = bytes,
	.bytes_len = bytes_len, .result = result, .error = error};

	std_landing(&frame, chunks_file_write_beside_body_0, chunks_file_write_beside_pad_0);
	return frame.ok;
}

/*
 * chunks_file_emptied_frame_0_t is emptied's state across its pad: its
 * arguments, its results, and the file it holds.
 */
typedef struct chunks_file_emptied_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	HostFile   *result_0;
	bool	   *result_1;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_emptied_frame_0_t;

/* chunks_file_emptied_body_0 is emptied's body. */
static void
chunks_file_emptied_body_0(void *frame_)
{
	chunks_file_emptied_frame_0_t *frame = frame_;
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_CREATE};
	HostError	host_error;
	bool		created;

	if (!chunks_file_writable(&action, frame->path, frame->path_len, frame->error))
		return;
	if (!host_create_if_absent(frame->io, frame->path, frame->path_len, &created, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (created)
	{
		*frame->result_0 = frame->file;
		*frame->result_1 = true;
		frame->ok = true;
		return;
	}
	if (!host_open(frame->io, frame->path, frame->path_len, HOST_OPEN_READ_WRITE, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	frame->file_live = true;
	if (!host_truncate(frame->io, &frame->file, 0, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		frame->file_live = false;
		host_File_drop(&frame->file);
		return;
	}
	frame->file_live = false;
	*frame->result_0 = frame->file;
	*frame->result_1 = false;
	frame->ok = true;
}

/* chunks_file_emptied_pad_0 is emptied's pad: the file, while it holds it. */
static void
chunks_file_emptied_pad_0(void *frame_)
{
	chunks_file_emptied_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/*
 * chunks_file_emptied opens a file to read and write, creating it or
 * emptying the one of that name, and reports whether it made it.
 */
static bool
chunks_file_emptied(HostIoIoDoors io, const char *path, size_t path_len, HostFile *result_0, bool *result_1,
					ChunksErrorError *error)
{
	chunks_file_emptied_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result_0 = result_0,
	.result_1 = result_1, .error = error};

	std_landing(&frame, chunks_file_emptied_body_0, chunks_file_emptied_pad_0);
	return frame.ok;
}

static bool chunks_file_sync_created(HostIoIoDoors io, const char *path, size_t path_len, bool created,
									 ChunksErrorError *error);

/*
 * chunks_file_create_frame_0_t is create's state across its pad: its
 * arguments, its results, and the file it holds.
 */
typedef struct chunks_file_create_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	HostFile   *result;
	ChunksErrorError *error;
	bool		created;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_create_frame_0_t;

/* chunks_file_create_body_0 is create's body. */
static void
chunks_file_create_body_0(void *frame_)
{
	chunks_file_create_frame_0_t *frame = frame_;

	if (!chunks_file_emptied(frame->io, frame->path, frame->path_len, &frame->file, &frame->created, frame->error))
		return;
	frame->file_live = true;
	if (!chunks_file_sync_created(frame->io, frame->path, frame->path_len, frame->created, frame->error))
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
		return;
	}
	frame->file_live = false;
	*frame->result = frame->file;
	frame->ok = true;
}

/* chunks_file_create_pad_0 is create's pad: the file, while it holds it. */
static void
chunks_file_create_pad_0(void *frame_)
{
	chunks_file_create_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/*
 * chunks_file_create creates a file to read and write, emptying one of that
 * name, for a writer that renames it into place once it is whole.
 */
bool
chunks_file_create(HostIoIoDoors io, const char *path, size_t path_len, HostFile *result, ChunksErrorError *error)
{
	chunks_file_create_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_create_body_0, chunks_file_create_pad_0);
	return frame.ok;
}

/*
 * chunks_file_sync_created makes a file's or directory's entry durable when a
 * door just created it, before the door returns, so that nothing relies on
 * an entry a crash can take back (I9g).
 */
static bool
chunks_file_sync_created(HostIoIoDoors io, const char *path, size_t path_len, bool created,
						 ChunksErrorError *error)
{
	StdStr		dir;

	if (!created)
		return true;
	dir = host_directory_of(path, path_len);
	return chunks_file_sync_dir(io, dir.ptr, dir.len, error);
}

/*
 * chunks_file_open_rw_frame_0_t is open_rw's state across its pad: its
 * arguments, its results, and the file it holds.
 */
typedef struct chunks_file_open_rw_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	bool		create;
	HostFile   *result;
	ChunksErrorError *error;
	bool		created;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_open_rw_frame_0_t;

/* chunks_file_open_rw_body_0 is open_rw's body. */
static void
chunks_file_open_rw_body_0(void *frame_)
{
	chunks_file_open_rw_frame_0_t *frame = frame_;
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_CREATE};
	HostError	host_error;

	if (!frame->create)
	{
		if (!host_open(frame->io, frame->path, frame->path_len, HOST_OPEN_READ_WRITE, frame->result, &host_error))
		{
			*frame->error = chunks_error_Error_FromError_from(&host_error);
			return;
		}
		frame->ok = true;
		return;
	}
	if (!chunks_file_writable(&action, frame->path, frame->path_len, frame->error))
		return;
	if (!host_create_if_absent(frame->io, frame->path, frame->path_len, &frame->created, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (!frame->created &&
		!host_open(frame->io, frame->path, frame->path_len, HOST_OPEN_READ_WRITE, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	frame->file_live = true;
	if (!chunks_file_sync_created(frame->io, frame->path, frame->path_len, frame->created, frame->error))
	{
		frame->file_live = false;
		host_File_drop(&frame->file);
		return;
	}
	frame->file_live = false;
	*frame->result = frame->file;
	frame->ok = true;
}

/* chunks_file_open_rw_pad_0 is open_rw's pad: the file, while it holds it. */
static void
chunks_file_open_rw_pad_0(void *frame_)
{
	chunks_file_open_rw_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/*
 * chunks_file_open_rw opens a file to read and write without emptying it,
 * creating it first when `create` is set and it is missing.
 */
bool
chunks_file_open_rw(HostIoIoDoors io, const char *path, size_t path_len, bool create, HostFile *result,
					ChunksErrorError *error)
{
	chunks_file_open_rw_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .create = create,
	.result = result, .error = error};

	std_landing(&frame, chunks_file_open_rw_body_0, chunks_file_open_rw_pad_0);
	return frame.ok;
}

/*
 * chunks_file_TornTailLayout is TornTail's layout inside a ChunksFileTornTail's
 * opaque storage: its file, where its whole records end, the generation its
 * header names, and whether the storage holds it, which a door that takes it
 * clears.
 */
typedef struct chunks_file_TornTailLayout
{
	StdPathBuf	path;
	uint64		end;
	uint64		generation;
	bool		has;
} chunks_file_TornTailLayout;

StaticAssertDecl(sizeof(chunks_file_TornTailLayout) <= sizeof(ChunksFileTornTail) &&
				 _Alignof(chunks_file_TornTailLayout) <= _Alignof(ChunksFileTornTail),
				 "TornTail's layout fits ChunksFileTornTail's opaque storage");

/*
 * chunks_file_TornTail_held returns the tail its storage holds, which storage
 * a door took it from panics at, at the Rust's use of the tail, as a use of a
 * moved value is refused.
 */
static chunks_file_TornTailLayout *
chunks_file_TornTail_held(const ChunksFileTornTail *tail, uint32 line, uint32 col)
{
	chunks_file_TornTailLayout *held = CHUNKS_FILE_TORN_TAIL(tail);

	if (!held->has)
		STD_OPTION_UNWRAP_FAILED(line, col);
	return held;
}

/*
 * chunks_file_TornTail_take moves the tail its storage holds out of it, before
 * anything that can fail, so that the storage holds none however the taker
 * ends, as a move of the value does.
 */
static ChunksFileTornTail
chunks_file_TornTail_take(ChunksFileTornTail *tail, uint32 line, uint32 col)
{
	ChunksFileTornTail taken = *tail;

	chunks_file_TornTail_held(tail, line, col)->has = false;
	return taken;
}

/* chunks_file_TornTail_put places a tail of a file, whose path it takes, in its storage. */
static void
chunks_file_TornTail_put(ChunksFileTornTail *tail, StdPathBuf *path, uint64 end, uint64 generation)
{
	chunks_file_TornTailLayout *put = CHUNKS_FILE_TORN_TAIL(tail);

	put->path = *path;
	put->end = end;
	put->generation = generation;
	put->has = true;
}

static bool chunks_file_truncate(HostIoIoDoors io, const HostFile *file, const chunks_file_TornTailLayout *tail,
								 ChunksErrorError *error);

/*
 * chunks_file_cut_frame_0_t is cut's state across its pad: its arguments, its
 * results, the tail it takes, and the file it holds.
 */
typedef struct chunks_file_cut_frame_0_t
{
	HostIoIoDoors io;
	ChunksFileTornTail tail;
	bool		tail_live;
	ChunksFileTornTail *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_cut_frame_0_t;

/* chunks_file_cut_body_0 is cut's body. */
static void
chunks_file_cut_body_0(void *frame_)
{
	chunks_file_cut_frame_0_t *frame = frame_;

	if (chunks_file_open_rw(frame->io, CHUNKS_FILE_TORN_TAIL(&frame->tail)->path.bytes.ptr,
							CHUNKS_FILE_TORN_TAIL(&frame->tail)->path.bytes.len, false, &frame->file, frame->error))
	{
		frame->file_live = true;
		if (chunks_file_truncate(frame->io, &frame->file, CHUNKS_FILE_TORN_TAIL(&frame->tail), frame->error) &&
			chunks_file_sync(frame->io, &frame->file, frame->error))
			frame->ok = true;
		frame->file_live = false;
		host_File_drop(&frame->file);
	}
	frame->tail_live = false;
	if (frame->ok)
		*frame->result = frame->tail;
	else
		chunks_file_TornTail_drop(&frame->tail);
}

/* chunks_file_cut_pad_0 is cut's pad: the file and the tail, while it holds them. */
static void
chunks_file_cut_pad_0(void *frame_)
{
	chunks_file_cut_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->tail_live)
		chunks_file_TornTail_drop(&frame->tail);
}

/*
 * chunks_file_cut shortens the file a tail, which it takes, is of durably to
 * where its whole records end, dropping the bytes past it, as an open does
 * to the torn tail a crash left, and returns where the next record goes.
 */
bool
chunks_file_cut(HostIoIoDoors io, ChunksFileTornTail *tail, ChunksFileTornTail *result, ChunksErrorError *error)
{
	chunks_file_cut_frame_0_t frame = {.io = io, .tail = chunks_file_TornTail_take(tail, 149, 35), .tail_live = true,
	.result = result, .error = error};

	std_landing(&frame, chunks_file_cut_body_0, chunks_file_cut_pad_0);
	return frame.ok;
}

/* chunks_file_truncate shortens an open file of a tail to where its whole records end. */
static bool
chunks_file_truncate(HostIoIoDoors io, const HostFile *file, const chunks_file_TornTailLayout *tail,
					 ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_TRUNCATE};
	HostError	host_error;

	if (!chunks_file_writable(&action, tail->path.bytes.ptr, tail->path.bytes.len, error))
		return false;
	if (!host_truncate(io, file, tail->end, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/* chunks_file_exists reports whether a file is there, by opening it to read. */
bool
chunks_file_exists(HostIoReadDoors io, const char *path, size_t path_len, bool *result, ChunksErrorError *error)
{
	HostError	host_error;
	bool		has;
	HostFile	file;

	if (!host_open_if_there(io, path, path_len, &has, &file, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	*result = has;
	if (has)
		host_File_drop(&file);
	return true;
}

/*
 * chunks_file_previous_frame_0_t is previous's state across its pad: its
 * argument and the name it holds.
 */
typedef struct chunks_file_previous_frame_0_t
{
	const char *path;
	size_t		path_len;
	StdPathBuf	name;
	bool		name_live;
} chunks_file_previous_frame_0_t;

/* chunks_file_previous_body_0 is previous's body. */
static void
chunks_file_previous_body_0(void *frame_)
{
	chunks_file_previous_frame_0_t *frame = frame_;

	std_path_to_path_buf(frame->path, frame->path_len, &frame->name);
	frame->name_live = true;
	std_OsString_push(&frame->name, ".prev", 5);
	frame->name_live = false;
}

/* chunks_file_previous_pad_0 is previous's pad: the name, while it holds it. */
static void
chunks_file_previous_pad_0(void *frame_)
{
	chunks_file_previous_frame_0_t *frame = frame_;

	if (frame->name_live)
		std_vec_free_Checked(&frame->name.bytes, 1, 1);
}

/* chunks_file_previous returns the path of the previous generation of a file write_kept writes, `<name>.prev`. */
StdPathBuf
chunks_file_previous(const char *path, size_t path_len)
{
	chunks_file_previous_frame_0_t frame = {.path = path, .path_len = path_len};

	std_landing(&frame, chunks_file_previous_body_0, chunks_file_previous_pad_0);
	return frame.name;
}

/*
 * chunks_file_older_frame_0_t is older's state across its pad: its argument
 * and the name it holds.
 */
typedef struct chunks_file_older_frame_0_t
{
	const char *path;
	size_t		path_len;
	StdPathBuf	name;
	bool		name_live;
} chunks_file_older_frame_0_t;

/* chunks_file_older_body_0 is older's body. */
static void
chunks_file_older_body_0(void *frame_)
{
	chunks_file_older_frame_0_t *frame = frame_;

	std_path_to_path_buf(frame->path, frame->path_len, &frame->name);
	frame->name_live = true;
	std_OsString_push(&frame->name, ".older", 6);
	frame->name_live = false;
}

/* chunks_file_older_pad_0 is older's pad: the name, while it holds it. */
static void
chunks_file_older_pad_0(void *frame_)
{
	chunks_file_older_frame_0_t *frame = frame_;

	if (frame->name_live)
		std_vec_free_Checked(&frame->name.bytes, 1, 1);
}

/*
 * chunks_file_older returns the path replace keeps a file's previous
 * generation under while it rotates the newest into its place,
 * `<name>.older`.
 */
static StdPathBuf
chunks_file_older(const char *path, size_t path_len)
{
	chunks_file_older_frame_0_t frame = {.path = path, .path_len = path_len};

	std_landing(&frame, chunks_file_older_body_0, chunks_file_older_pad_0);
	return frame.name;
}

/*
 * chunks_file_generations_frame_0_t is generations' state across its pad:
 * its arguments, its results, and the two names it holds.
 */
typedef struct chunks_file_generations_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	StdPathBuf *result_0;
	StdPathBuf *result_1;
	ChunksErrorError *error;
	StdPathBuf	prev;
	bool		prev_live;
	StdPathBuf	old;
	bool		old_live;
	bool		ok;
} chunks_file_generations_frame_0_t;

/* chunks_file_generations_body_0 is generations' body. */
static void
chunks_file_generations_body_0(void *frame_)
{
	chunks_file_generations_frame_0_t *frame = frame_;
	bool		old_there;
	bool		path_there;
	bool		prev_there;

	frame->prev = chunks_file_previous(frame->path, frame->path_len);
	frame->prev_live = true;
	frame->old = chunks_file_older(frame->path, frame->path_len);
	frame->old_live = true;
	if (!chunks_file_exists(frame->io, frame->old.bytes.ptr, frame->old.bytes.len, &old_there, frame->error) ||
		!chunks_file_exists(frame->io, frame->path, frame->path_len, &path_there, frame->error) ||
		!chunks_file_exists(frame->io, frame->prev.bytes.ptr, frame->prev.bytes.len, &prev_there, frame->error))
	{
		frame->old_live = false;
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
		frame->prev_live = false;
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
		return;
	}
	if (old_there && path_there && !prev_there)
	{
		std_path_to_path_buf(frame->path, frame->path_len, frame->result_0);
		frame->old_live = false;
		*frame->result_1 = frame->old;
		frame->prev_live = false;
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
	}
	else if (old_there && !path_there)
	{
		frame->prev_live = false;
		*frame->result_0 = frame->prev;
		frame->old_live = false;
		*frame->result_1 = frame->old;
	}
	else
	{
		std_path_to_path_buf(frame->path, frame->path_len, frame->result_0);
		frame->prev_live = false;
		*frame->result_1 = frame->prev;
		frame->old_live = false;
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
	}
	frame->ok = true;
}

/* chunks_file_generations_pad_0 is generations' pad: the two names, while it holds them. */
static void
chunks_file_generations_pad_0(void *frame_)
{
	chunks_file_generations_frame_0_t *frame = frame_;

	if (frame->old_live)
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
	if (frame->prev_live)
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
}

/*
 * chunks_file_generations returns where the newest and previous generations
 * of a file write_kept or write_appended writes are, reading the names a
 * rotation of replace left as a crash at that step implies. It changes no
 * file, so that a reader meeting a rotation in progress reads it as a crash
 * there would leave it.
 */
bool
chunks_file_generations(HostIoReadDoors io, const char *path, size_t path_len, StdPathBuf *result_0,
						StdPathBuf *result_1, ChunksErrorError *error)
{
	chunks_file_generations_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result_0 = result_0,
	.result_1 = result_1, .error = error};

	std_landing(&frame, chunks_file_generations_body_0, chunks_file_generations_pad_0);
	return frame.ok;
}

/*
 * chunks_file_settle_frame_0_t is settle's state across its pad: its
 * arguments, its error, and the names it holds.
 */
typedef struct chunks_file_settle_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	ChunksErrorError *error;
	StdPathBuf	old;
	bool		old_live;
	StdPathBuf	newest;
	bool		newest_live;
	StdPathBuf	prev;
	bool		prev_live;
	StdPathBuf	previous;
	bool		previous_live;
	bool		ok;
} chunks_file_settle_frame_0_t;

/* chunks_file_settle_body_0 is settle's body. */
static void
chunks_file_settle_body_0(void *frame_)
{
	chunks_file_settle_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	bool		there;
	StdStr		dir;
	bool		done = false;

	frame->old = chunks_file_older(frame->path, frame->path_len);
	frame->old_live = true;
	if (!chunks_file_exists(read, frame->old.bytes.ptr, frame->old.bytes.len, &there, frame->error))
		goto drop_old;
	if (!there)
	{
		frame->ok = true;
		goto drop_old;
	}
	dir = host_directory_of(frame->path, frame->path_len);
	if (!chunks_file_generations(read, frame->path, frame->path_len, &frame->newest, &frame->prev, frame->error))
		goto drop_old;
	frame->newest_live = true;
	frame->prev_live = true;
	if (!std_path_PartialEq_eq(frame->newest.bytes.ptr, frame->newest.bytes.len, frame->path, frame->path_len))
	{
		if (!chunks_file_exists(read, frame->newest.bytes.ptr, frame->newest.bytes.len, &there, frame->error))
			goto drop_all;
		if (there &&
			(!chunks_file_rename(frame->io, frame->newest.bytes.ptr, frame->newest.bytes.len, frame->path,
								 frame->path_len, frame->error) ||
			 !chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error)))
			goto drop_all;
	}
	if (std_path_PartialEq_eq(frame->prev.bytes.ptr, frame->prev.bytes.len, frame->old.bytes.ptr, frame->old.bytes.len))
	{
		frame->previous = chunks_file_previous(frame->path, frame->path_len);
		frame->previous_live = true;
		done = chunks_file_rename(frame->io, frame->old.bytes.ptr, frame->old.bytes.len, frame->previous.bytes.ptr,
								  frame->previous.bytes.len, frame->error);
		frame->previous_live = false;
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	}
	else
		done = chunks_file_remove_file(frame->io, frame->old.bytes.ptr, frame->old.bytes.len, frame->error);
	if (done)
		frame->ok = chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error);
drop_all:
	frame->prev_live = false;
	std_vec_free_Checked(&frame->prev.bytes, 1, 1);
	frame->newest_live = false;
	std_vec_free_Checked(&frame->newest.bytes, 1, 1);
drop_old:
	frame->old_live = false;
	std_vec_free_Checked(&frame->old.bytes, 1, 1);
}

/* chunks_file_settle_pad_0 is settle's pad: the names, while it holds them. */
static void
chunks_file_settle_pad_0(void *frame_)
{
	chunks_file_settle_frame_0_t *frame = frame_;

	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->prev_live)
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest.bytes, 1, 1);
	if (frame->old_live)
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
}

/*
 * chunks_file_settle puts the generations of a file back under their own
 * names as generations reads them, dropping the one a finished rotation left
 * under older's name, for a writer of the file before it writes. Each rename
 * is durable before the next takes over the name it vacated, so that no
 * subset of them a crash keeps loses a generation.
 */
static bool
chunks_file_settle(HostIoIoDoors io, const char *path, size_t path_len, ChunksErrorError *error)
{
	chunks_file_settle_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .error = error};

	std_landing(&frame, chunks_file_settle_body_0, chunks_file_settle_pad_0);
	return frame.ok;
}

/*
 * ChunksFileRotated is Rotated: how far a rotation's renames went, which
 * the put-back of one that failed takes back.
 */
typedef struct ChunksFileRotated
{
	/* kept_older is whether the previous generation moved under older's name. */
	bool		kept_older;
	/* moved is whether the newest moved under the previous name. */
	bool		moved;

	/*
	 * placed is whether the new newest took the file's name, after which
	 * nothing is taken back.
	 */
	bool		placed;
} ChunksFileRotated;

/* chunks_file_Rotated_Default_default is Rotated's derived Default: no rename made. */
static ChunksFileRotated
chunks_file_Rotated_Default_default(void)
{
	ChunksFileRotated rotated = {0};

	return rotated;
}

static bool chunks_file_rotate_steps(HostIoIoDoors io, const char *path, size_t path_len, const char *temp,
									 size_t temp_len, bool had_previous, ChunksFileRotated *rotated,
									 ChunksErrorError *error);
static void chunks_file_put_back(HostIoIoDoors io, const char *path, size_t path_len,
								 const ChunksFileRotated *rotated);

/*
 * chunks_file_rename_generations_frame_0_t is rename_generations' state
 * across its pad: its arguments, its error, and the names and error it
 * holds.
 */
typedef struct chunks_file_rename_generations_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const char *temp;
	size_t		temp_len;
	bool		keep;
	bool		aside;
	ChunksErrorError *error;
	StdPathBuf	aside_2;
	bool		aside_2_live;
	StdPathBuf	previous;
	bool		previous_live;
	StdPathBuf	older;
	bool		older_live;
	ChunksErrorError err;
	bool		err_live;
	bool		ok;
} chunks_file_rename_generations_frame_0_t;

/* chunks_file_rename_generations_body_0 is rename_generations' body. */
static void
chunks_file_rename_generations_body_0(void *frame_)
{
	chunks_file_rename_generations_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	StdStr		dir;
	bool		had_previous;
	bool		renamed;
	ChunksFileRotated rotated;

	if (!chunks_file_settle(frame->io, frame->path, frame->path_len, frame->error))
		return;
	if (frame->aside)
	{
		if (!chunks_file_damaged_name(frame->io, frame->path, frame->path_len, &frame->aside_2, frame->error))
			return;
		frame->aside_2_live = true;
		renamed = chunks_file_rename(frame->io, frame->path, frame->path_len, frame->aside_2.bytes.ptr,
									 frame->aside_2.bytes.len, frame->error);
		frame->aside_2_live = false;
		std_vec_free_Checked(&frame->aside_2.bytes, 1, 1);
		if (!renamed)
			return;
	}
	if (!frame->keep)
	{
		if (!chunks_file_rename(frame->io, frame->temp, frame->temp_len, frame->path, frame->path_len, frame->error))
			return;
		dir = host_directory_of(frame->path, frame->path_len);
		frame->ok = chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error);
		return;
	}
	frame->previous = chunks_file_previous(frame->path, frame->path_len);
	frame->previous_live = true;
	renamed = chunks_file_exists(read, frame->previous.bytes.ptr, frame->previous.bytes.len, &had_previous,
								 frame->error);
	frame->previous_live = false;
	std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (!renamed)
		return;
	rotated = chunks_file_Rotated_Default_default();
	if (chunks_file_rotate_steps(frame->io, frame->path, frame->path_len, frame->temp, frame->temp_len, had_previous,
								 &rotated, &frame->err))
	{
		if (!had_previous)
		{
			frame->ok = true;
			return;
		}
		frame->older = chunks_file_older(frame->path, frame->path_len);
		frame->older_live = true;
		frame->ok = chunks_file_remove_file(frame->io, frame->older.bytes.ptr, frame->older.bytes.len, frame->error);
		frame->older_live = false;
		std_vec_free_Checked(&frame->older.bytes, 1, 1);
		return;
	}
	frame->err_live = true;
	if (!rotated.placed)
		chunks_file_put_back(frame->io, frame->path, frame->path_len, &rotated);
	frame->err_live = false;
	*frame->error = frame->err;
}

/* chunks_file_rename_generations_pad_0 is rename_generations' pad: the names and the error, while it holds them. */
static void
chunks_file_rename_generations_pad_0(void *frame_)
{
	chunks_file_rename_generations_frame_0_t *frame = frame_;

	if (frame->err_live)
		chunks_error_Error_drop(&frame->err);
	if (frame->older_live)
		std_vec_free_Checked(&frame->older.bytes, 1, 1);
	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->aside_2_live)
		std_vec_free_Checked(&frame->aside_2.bytes, 1, 1);
}

/*
 * chunks_file_rename_generations puts a new newest written beside a file in
 * its place, keeping the file it replaces as its previous generation when
 * `keep` says, as rotate does once it holds what orders its rotations.
 */
static bool
chunks_file_rename_generations(HostIoIoDoors io, const char *path, size_t path_len, const char *temp,
							   size_t temp_len, bool keep, bool aside, ChunksErrorError *error)
{
	chunks_file_rename_generations_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .temp = temp,
	.temp_len = temp_len, .keep = keep, .aside = aside, .error = error};

	std_landing(&frame, chunks_file_rename_generations_body_0, chunks_file_rename_generations_pad_0);
	return frame.ok;
}

/*
 * chunks_file_rotate_steps_frame_0_t is rotate_steps' state across its pad:
 * its arguments, its error, and the two names it holds.
 */
typedef struct chunks_file_rotate_steps_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const char *temp;
	size_t		temp_len;
	bool		had_previous;
	ChunksFileRotated *rotated;
	ChunksErrorError *error;
	StdPathBuf	prev;
	bool		prev_live;
	StdPathBuf	old;
	bool		old_live;
	bool		ok;
} chunks_file_rotate_steps_frame_0_t;

/* chunks_file_rotate_steps_body_0 is rotate_steps' body. */
static void
chunks_file_rotate_steps_body_0(void *frame_)
{
	chunks_file_rotate_steps_frame_0_t *frame = frame_;
	StdStr		dir;

	frame->prev = chunks_file_previous(frame->path, frame->path_len);
	frame->prev_live = true;
	frame->old = chunks_file_older(frame->path, frame->path_len);
	frame->old_live = true;
	dir = host_directory_of(frame->path, frame->path_len);
	if (frame->had_previous)
	{
		if (!chunks_file_rename(frame->io, frame->prev.bytes.ptr, frame->prev.bytes.len, frame->old.bytes.ptr,
								frame->old.bytes.len, frame->error))
			goto drop;
		frame->rotated->kept_older = true;
		if (!chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error))
			goto drop;
	}
	if (!chunks_file_rename(frame->io, frame->path, frame->path_len, frame->prev.bytes.ptr, frame->prev.bytes.len,
							frame->error))
		goto drop;
	frame->rotated->moved = true;
	if (!chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error))
		goto drop;
	if (!chunks_file_rename(frame->io, frame->temp, frame->temp_len, frame->path, frame->path_len, frame->error))
		goto drop;
	frame->rotated->placed = true;
	frame->ok = chunks_file_sync_dir(frame->io, dir.ptr, dir.len, frame->error);
drop:
	frame->old_live = false;
	std_vec_free_Checked(&frame->old.bytes, 1, 1);
	frame->prev_live = false;
	std_vec_free_Checked(&frame->prev.bytes, 1, 1);
}

/* chunks_file_rotate_steps_pad_0 is rotate_steps' pad: the two names, while it holds them. */
static void
chunks_file_rotate_steps_pad_0(void *frame_)
{
	chunks_file_rotate_steps_frame_0_t *frame = frame_;

	if (frame->old_live)
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
	if (frame->prev_live)
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
}

/*
 * chunks_file_rotate_steps renames a file's generations for a rotation, each
 * rename durable before the next, noting how far they went.
 */
static bool
chunks_file_rotate_steps(HostIoIoDoors io, const char *path, size_t path_len, const char *temp, size_t temp_len,
						 bool had_previous, ChunksFileRotated *rotated, ChunksErrorError *error)
{
	chunks_file_rotate_steps_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .temp = temp,
	.temp_len = temp_len, .had_previous = had_previous, .rotated = rotated, .error = error};

	std_landing(&frame, chunks_file_rotate_steps_body_0, chunks_file_rotate_steps_pad_0);
	return frame.ok;
}

/*
 * chunks_file_put_back_frame_0_t is put_back's state across its pad: its
 * arguments and the two names it holds.
 */
typedef struct chunks_file_put_back_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const ChunksFileRotated *rotated;
	StdPathBuf	prev;
	bool		prev_live;
	StdPathBuf	old;
	bool		old_live;
} chunks_file_put_back_frame_0_t;

/* chunks_file_put_back_body_0 is put_back's body. */
static void
chunks_file_put_back_body_0(void *frame_)
{
	chunks_file_put_back_frame_0_t *frame = frame_;
	bool		back;

	frame->prev = chunks_file_previous(frame->path, frame->path_len);
	frame->prev_live = true;
	frame->old = chunks_file_older(frame->path, frame->path_len);
	frame->old_live = true;
	back = !frame->rotated->moved ||
		host_rename_back(frame->io, frame->prev.bytes.ptr, frame->prev.bytes.len, frame->path, frame->path_len);
	if (back && frame->rotated->kept_older)
		(void) host_rename_back(frame->io, frame->old.bytes.ptr, frame->old.bytes.len, frame->prev.bytes.ptr,
								frame->prev.bytes.len);
	frame->old_live = false;
	std_vec_free_Checked(&frame->old.bytes, 1, 1);
	frame->prev_live = false;
	std_vec_free_Checked(&frame->prev.bytes, 1, 1);
}

/* chunks_file_put_back_pad_0 is put_back's pad: the two names, while it holds them. */
static void
chunks_file_put_back_pad_0(void *frame_)
{
	chunks_file_put_back_frame_0_t *frame = frame_;

	if (frame->old_live)
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
	if (frame->prev_live)
		std_vec_free_Checked(&frame->prev.bytes, 1, 1);
}

/*
 * chunks_file_put_back takes back a failed rotation's renames, each durable
 * before the next, and stops at the first that doesn't take, after which the
 * names read as a crash at that step leaves them, which generations and
 * settle take.
 */
static void
chunks_file_put_back(HostIoIoDoors io, const char *path, size_t path_len, const ChunksFileRotated *rotated)
{
	chunks_file_put_back_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .rotated = rotated};

	std_landing(&frame, chunks_file_put_back_body_0, chunks_file_put_back_pad_0);
}

/* chunks_file_Fallback_Debug_fmt is Fallback's derived Debug. */
bool
chunks_file_Fallback_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksFileFallback *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case CHUNKS_FILE_FALLBACK_MISSING:
			return std_fmt_write_str(f, "Missing", 7);
		case CHUNKS_FILE_FALLBACK_DAMAGED:
			return std_fmt_write_str(f, "Damaged", 7);
		case CHUNKS_FILE_FALLBACK_UNCLEAR:
			return std_fmt_write_str(f, "Unclear", 7);
		case CHUNKS_FILE_FALLBACK_UNREADABLE:
			return std_fmt_write_str(f, "Unreadable", 10);
		case CHUNKS_FILE_FALLBACK_KEPT:
			break;
	}
	t = std_fmt_debug_tuple(f, "Kept", 4);
	std_DebugTuple_field(&t, &self->u.kept, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* chunks_file_Fallback_PartialEq_eq is Fallback's derived PartialEq: the variant, then Kept's offset. */
bool
chunks_file_Fallback_PartialEq_eq(const ChunksFileFallback *self, const ChunksFileFallback *other)
{
	if (self->tag != other->tag)
		return false;
	return self->tag != CHUNKS_FILE_FALLBACK_KEPT || self->u.kept == other->u.kept;
}

/* chunks_file_VecU8s_Debug_fmt is a Vec<Vec<u8>>'s Debug. */
static bool
chunks_file_VecU8s_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *records = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = records->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(records, StdVec, k), chunks_chunk_VecU8_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* chunks_file_Damage_Debug_fmt is Damage's derived Debug. */
bool
chunks_file_Damage_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksFileDamage *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Damage", 6);

	std_DebugStruct_field(&s, "offset", 6, &self->offset, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "after", 5, &self->after, chunks_file_VecU8s_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_file_Damage_Default_default is Damage's derived Default: no offset and no records after it. */
void
chunks_file_Damage_Default_default(ChunksFileDamage *result)
{
	result->offset = 0;
	std_vec_new(&result->after, sizeof(StdVec), _Alignof(StdVec));
}

/* chunks_file_Damage_PartialEq_eq is Damage's derived PartialEq: the offset, then the records after it. */
bool
chunks_file_Damage_PartialEq_eq(const ChunksFileDamage *self, const ChunksFileDamage *other)
{
	if (self->offset != other->offset || self->after.len != other->after.len)
		return false;
	for (size_t k = 0, k_len = self->after.len; k < k_len; k++)
	{
		const StdVec *a = &STD_VEC_AT(&self->after, StdVec, k);
		const StdVec *b = &STD_VEC_AT(&other->after, StdVec, k);

		if (a->len != b->len || (a->len != 0 && memcmp(a->ptr, b->ptr, a->len) != 0))
			return false;
	}
	return true;
}

/*
 * chunks_file_damaged_name_frame_0_t is damaged_name's state across its pad:
 * its arguments, its results, and the name and candidate it holds.
 */
typedef struct chunks_file_damaged_name_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	StdPathBuf *result;
	ChunksErrorError *error;
	StdCowStr	name;
	bool		name_live;
	StdString	file;
	bool		file_live;
	StdPathBuf	kept;
	bool		kept_live;
	bool		ok;
} chunks_file_damaged_name_frame_0_t;

/* chunks_file_damaged_name_body_0 is damaged_name's body. */
static void
chunks_file_damaged_name_body_0(void *frame_)
{
	chunks_file_damaged_name_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	StdStr		file_name = {(const char *) STD_DANGLING, 0};
	StdStr		shown;

	(void) std_path_file_name(frame->path, frame->path_len, &file_name);
	std_path_to_string_lossy(file_name.ptr, file_name.len, &frame->name);
	frame->name_live = true;
	if (frame->name.tag == STD_COW_STR_BORROWED)
		shown = frame->name.u.borrowed;
	else
	{
		shown.ptr = frame->name.u.owned.ptr;
		shown.len = frame->name.u.owned.len;
	}
	for (int32 next = 0;;)
	{
		int32		n = next;
		bool		there;

		next = std_forward_i32_Checked(next, 1);

		if (n == 0)
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(".damaged")};
			StdFmtArg	args[] = {{&shown, std_str_Display_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			std_fmt_format_Checked(&frame->file, &arguments);
		}
		else
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(".damaged."), STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&shown, std_str_Display_fmt}, {&n, std_i32_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};

			std_fmt_format_Checked(&frame->file, &arguments);
		}
		frame->file_live = true;
		std_path_with_file_name(frame->path, frame->path_len, frame->file.ptr, frame->file.len, &frame->kept);
		frame->kept_live = true;
		frame->file_live = false;
		std_vec_free_Checked(&frame->file, 1, 1);
		if (!chunks_file_exists(read, frame->kept.bytes.ptr, frame->kept.bytes.len, &there, frame->error))
		{
			frame->kept_live = false;
			std_vec_free_Checked(&frame->kept.bytes, 1, 1);
			goto drop_name;
		}
		if (!there)
		{
			frame->kept_live = false;
			*frame->result = frame->kept;
			frame->ok = true;
			goto drop_name;
		}
		frame->kept_live = false;
		std_vec_free_Checked(&frame->kept.bytes, 1, 1);
	}
	{
		static const char text[] = "internal error: entered unreachable code: some damaged name is free";

		std_panic(RUST_FILE, 324, 5, text, sizeof(text) - 1);
	}
drop_name:
	frame->name_live = false;
	std_CowStr_drop(&frame->name);
}

/* chunks_file_damaged_name_pad_0 is damaged_name's pad: the candidate, its name, and the file's name. */
static void
chunks_file_damaged_name_pad_0(void *frame_)
{
	chunks_file_damaged_name_frame_0_t *frame = frame_;

	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept.bytes, 1, 1);
	if (frame->file_live)
		std_vec_free_Checked(&frame->file, 1, 1);
	if (frame->name_live)
		std_CowStr_drop(&frame->name);
}

/*
 * chunks_file_damaged_name returns where a damaged file is kept: beside it
 * as `<name>.damaged`, or the first of `<name>.damaged.1`,
 * `<name>.damaged.2`, and so on that is free, so that no damaged copy is
 * ever replaced.
 */
bool
chunks_file_damaged_name(HostIoIoDoors io, const char *path, size_t path_len, StdPathBuf *result,
						 ChunksErrorError *error)
{
	chunks_file_damaged_name_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_damaged_name_body_0, chunks_file_damaged_name_pad_0);
	return frame.ok;
}

/*
 * chunks_file_replace_records_frame_0_t is replace_records' state across its
 * pad: its arguments, its results, and the temporary name it holds.
 */
typedef struct chunks_file_replace_records_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const uint8 *bytes;
	size_t		bytes_len;
	bool		keep;
	ChunksFileTornTail *result;
	ChunksErrorError *error;
	StdPathBuf	temp;
	bool		temp_live;
	bool		ok;
} chunks_file_replace_records_frame_0_t;

/* chunks_file_replace_records_body_0 is replace_records' body. */
static void
chunks_file_replace_records_body_0(void *frame_)
{
	chunks_file_replace_records_frame_0_t *frame = frame_;

	if (!chunks_file_write_beside(frame->io, frame->path, frame->path_len, frame->bytes, frame->bytes_len,
								  &frame->temp, frame->error))
		return;
	frame->temp_live = true;
	if (chunks_file_rename_generations(frame->io, frame->path, frame->path_len, frame->temp.bytes.ptr,
									   frame->temp.bytes.len, frame->keep, false, frame->error))
	{
		StdPathBuf	path;

		std_path_to_path_buf(frame->path, frame->path_len, &path);
		chunks_file_TornTail_put(frame->result, &path, (uint64) frame->bytes_len, 0);
		frame->ok = true;
	}
	frame->temp_live = false;
	std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/* chunks_file_replace_records_pad_0 is replace_records' pad: the temporary name, while it holds it. */
static void
chunks_file_replace_records_pad_0(void *frame_)
{
	chunks_file_replace_records_frame_0_t *frame = frame_;

	if (frame->temp_live)
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
}

/*
 * chunks_file_replace_records writes a list file of records whole as replace
 * does and returns where its records end. It takes neither the file's writer
 * lock nor ROTATION: the packs' lists lock orders a list file's writes and
 * the reads of its generations.
 */
bool
chunks_file_replace_records(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes,
							size_t bytes_len, bool keep, ChunksFileTornTail *result, ChunksErrorError *error)
{
	chunks_file_replace_records_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .bytes = bytes,
	.bytes_len = bytes_len, .keep = keep, .result = result, .error = error};

	std_landing(&frame, chunks_file_replace_records_body_0, chunks_file_replace_records_pad_0);
	return frame.ok;
}

/* ChunksFileRecordsTag is Records' variants. */
typedef enum ChunksFileRecordsTag
{
	CHUNKS_FILE_RECORDS_MISSING,
	CHUNKS_FILE_RECORDS_OTHER,
	CHUNKS_FILE_RECORDS_APPENDED,
} ChunksFileRecordsTag;

/*
 * ChunksFileRecords is Records: what appended_records read of a file, none
 * when it is missing, its bytes when it is of another kind, or its whole
 * records in order with where they end; defined before write_kept, its first
 * user, as C needs.
 */
typedef struct ChunksFileRecords
{
	ChunksFileRecordsTag tag;
	union
	{
		StdVec		other;
		struct
		{
			/* f0 is a Vec<Vec<u8>> of the records. */
			StdVec		f0;
			bool		has_f1;
			ChunksFileWhole f1;
			bool		has_f2;
			ChunksFileDamage f2;
		}			appended;
	}			u;
} ChunksFileRecords;

static bool chunks_file_appended_records(HostIoReadDoors io, const char *path, size_t path_len,
										 ChunksFileRecords *result, ChunksErrorError *error);
static void chunks_file_Records_drop(ChunksFileRecords *self);
static bool chunks_file_verified(const uint8 *kept, size_t kept_len, const uint8 **result, size_t *result_len);

/*
 * chunks_file_write_kept_frame_0_t is write_kept's state across its pad: its
 * arguments, its result, and the bytes, names, and read it holds.
 */
typedef struct chunks_file_write_kept_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const uint8 *bytes;
	size_t		bytes_len;
	bool	   *result;
	ChunksErrorError *error;
	StdVec		kept;
	bool		kept_live;
	StdPathBuf	newest;
	bool		newest_live;
	StdPathBuf	previous;
	bool		previous_live;
	ChunksFileRecords records;
	bool		records_live;
	StdPathBuf	temp;
	bool		temp_live;
	bool		ok;
} chunks_file_write_kept_frame_0_t;

/* chunks_file_write_kept_body_0 is write_kept's body. */
static void
chunks_file_write_kept_body_0(void *frame_)
{
	chunks_file_write_kept_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	uint8		sum[8];
	bool		keep;
	bool		aside;
	bool		same;
	const uint8 *verified;
	size_t		verified_len;

	if (!chunks_file_writable(&action, frame->path, frame->path_len, frame->error))
		return;
	std_vec_with_capacity_Checked(&frame->kept, STD_ADD(usize, frame->bytes_len, 8, 344, 39), 1, 1);
	frame->kept_live = true;
	std_vec_extend_from_slice_Checked(&frame->kept, frame->bytes, frame->bytes_len, 1, 1);
	std_u64_to_le_bytes(XXH3_64bits(frame->bytes, frame->bytes_len), sum);
	std_vec_extend_from_slice_Checked(&frame->kept, sum, 8, 1, 1);
	if (!chunks_file_generations(read, frame->path, frame->path_len, &frame->newest, &frame->previous, frame->error))
		goto drop_kept;
	frame->newest_live = true;
	frame->previous_live = true;
	frame->previous_live = false;
	std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (!chunks_file_appended_records(read, frame->newest.bytes.ptr, frame->newest.bytes.len, &frame->records,
									  frame->error))
		goto drop_newest;
	frame->records_live = true;
	if (frame->records.tag == CHUNKS_FILE_RECORDS_OTHER &&
		chunks_file_verified(frame->records.u.other.ptr, frame->records.u.other.len, &verified, &verified_len))
	{
		keep = true;
		aside = false;
		same = frame->records.u.other.len == frame->kept.len &&
			memcmp(frame->records.u.other.ptr, frame->kept.ptr, frame->kept.len) == 0;
	}
	else if (frame->records.tag == CHUNKS_FILE_RECORDS_MISSING ||
			 (frame->records.tag == CHUNKS_FILE_RECORDS_APPENDED && frame->records.u.appended.has_f1 &&
			  !frame->records.u.appended.has_f2))
	{
		keep = false;
		aside = false;
		same = false;
	}
	else
	{
		keep = false;
		aside = true;
		same = false;
	}
	frame->records_live = false;
	chunks_file_Records_drop(&frame->records);
	if (chunks_file_write_beside(frame->io, frame->path, frame->path_len, frame->kept.ptr, frame->kept.len,
								 &frame->temp, frame->error))
	{
		frame->temp_live = true;
		if (chunks_file_rename_generations(frame->io, frame->path, frame->path_len, frame->temp.bytes.ptr,
										   frame->temp.bytes.len, keep, aside, frame->error))
		{
			*frame->result = same;
			frame->ok = true;
		}
		frame->temp_live = false;
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
	}
drop_newest:
	frame->newest_live = false;
	std_vec_free_Checked(&frame->newest.bytes, 1, 1);
drop_kept:
	frame->kept_live = false;
	std_vec_free_Checked(&frame->kept, 1, 1);
}

/* chunks_file_write_kept_pad_0 is write_kept's pad: the names, the read, and the bytes, while it holds them. */
static void
chunks_file_write_kept_pad_0(void *frame_)
{
	chunks_file_write_kept_frame_0_t *frame = frame_;

	if (frame->temp_live)
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
	if (frame->records_live)
		chunks_file_Records_drop(&frame->records);
	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest.bytes, 1, 1);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, 1, 1);
}

/*
 * chunks_file_write_kept writes a file whole and durably as write_file does,
 * with an xxh3 trailer over its bytes, keeping the file it replaces as its
 * previous generation, which read_kept reads when the newest is missing or
 * damaged. A damaged newest file moves aside under damaged_name rather than
 * becoming the previous generation. It answers whether the newest generation
 * it replaced held the same bytes, which its previous generation then holds.
 */
bool
chunks_file_write_kept(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes, size_t bytes_len,
					   bool *result, ChunksErrorError *error)
{
	chunks_file_write_kept_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .bytes = bytes,
	.bytes_len = bytes_len, .result = result, .error = error};

	std_landing(&frame, chunks_file_write_kept_body_0, chunks_file_write_kept_pad_0);
	return frame.ok;
}

static bool chunks_file_read_if_there(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
									  StdVec *result, ChunksErrorError *error);

/*
 * chunks_file_read_kept_frame_0_t is read_kept's state across its pad: its
 * arguments, its results, and the names and bytes it holds.
 */
typedef struct chunks_file_read_kept_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	StdVec	   *result_0;
	bool	   *has_result_1;
	ChunksFileFallback *result_1;
	ChunksErrorError *error;
	StdPathBuf	newest_path;
	bool		newest_path_live;
	StdPathBuf	previous_path;
	bool		previous_path_live;
	bool		has_newest;
	StdVec		newest;
	bool		newest_live;
	bool		has_previous;
	StdVec		previous;
	bool		previous_live;
	bool		ok;
} chunks_file_read_kept_frame_0_t;

/* chunks_file_read_kept_body_0 is read_kept's body. */
static void
chunks_file_read_kept_body_0(void *frame_)
{
	chunks_file_read_kept_frame_0_t *frame = frame_;
	const uint8 *bytes;
	size_t		bytes_len;
	ChunksFileFallback why;

	if (!chunks_file_generations(frame->io, frame->path, frame->path_len, &frame->newest_path,
								 &frame->previous_path, frame->error))
		return;
	frame->newest_path_live = true;
	frame->previous_path_live = true;
	if (!chunks_file_read_if_there(frame->io, frame->newest_path.bytes.ptr, frame->newest_path.bytes.len,
								   &frame->has_newest, &frame->newest, frame->error))
		goto drop_paths;
	frame->newest_live = frame->has_newest;
	if (frame->has_newest && chunks_file_verified(frame->newest.ptr, frame->newest.len, &bytes, &bytes_len))
	{
		std_slice_to_vec_Checked(frame->result_0, bytes, bytes_len, 1, 1);
		*frame->has_result_1 = !std_path_PartialEq_eq(frame->newest_path.bytes.ptr, frame->newest_path.bytes.len,
													  frame->path, frame->path_len);
		frame->result_1->tag = CHUNKS_FILE_FALLBACK_MISSING;
		*frame->has_result = true;
		frame->ok = true;
		goto drop_newest;
	}
	why.tag = frame->has_newest ? CHUNKS_FILE_FALLBACK_DAMAGED : CHUNKS_FILE_FALLBACK_MISSING;
	if (!chunks_file_read_if_there(frame->io, frame->previous_path.bytes.ptr, frame->previous_path.bytes.len,
								   &frame->has_previous, &frame->previous, frame->error))
		goto drop_newest;
	frame->previous_live = frame->has_previous;
	if (frame->has_previous &&
		chunks_file_verified(frame->previous.ptr, frame->previous.len, &bytes, &bytes_len))
	{
		std_slice_to_vec_Checked(frame->result_0, bytes, bytes_len, 1, 1);
		*frame->has_result_1 = true;
		*frame->result_1 = why;
		*frame->has_result = true;
		frame->ok = true;
	}
	else if (!frame->has_previous && !frame->has_newest)
	{
		*frame->has_result = false;
		frame->ok = true;
	}
	else
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" and its previous generation are damaged")};
		StdStr		shown = {frame->path, frame->path_len};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
	}
	if (frame->previous_live)
	{
		frame->previous_live = false;
		std_vec_free_Checked(&frame->previous, 1, 1);
	}
drop_newest:
	if (frame->newest_live)
	{
		frame->newest_live = false;
		std_vec_free_Checked(&frame->newest, 1, 1);
	}
drop_paths:
	frame->previous_path_live = false;
	std_vec_free_Checked(&frame->previous_path.bytes, 1, 1);
	frame->newest_path_live = false;
	std_vec_free_Checked(&frame->newest_path.bytes, 1, 1);
}

/* chunks_file_read_kept_pad_0 is read_kept's pad: the bytes and the names, while it holds them. */
static void
chunks_file_read_kept_pad_0(void *frame_)
{
	chunks_file_read_kept_frame_0_t *frame = frame_;

	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest, 1, 1);
	if (frame->previous_path_live)
		std_vec_free_Checked(&frame->previous_path.bytes, 1, 1);
	if (frame->newest_path_live)
		std_vec_free_Checked(&frame->newest_path.bytes, 1, 1);
}

/*
 * chunks_file_read_kept returns the bytes of a file write_kept wrote, or of
 * its previous generation when the newest is missing or its trailer does not
 * match, with why when they are the previous generation's, or None when
 * neither exists. Both damaged is corrupt.
 */
bool
chunks_file_read_kept(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result, StdVec *result_0,
					  bool *has_result_1, ChunksFileFallback *result_1, ChunksErrorError *error)
{
	chunks_file_read_kept_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .has_result = has_result,
	.result_0 = result_0, .has_result_1 = has_result_1, .result_1 = result_1, .error = error};

	std_landing(&frame, chunks_file_read_kept_body_0, chunks_file_read_kept_pad_0);
	return frame.ok;
}

/*
 * chunks_file_read_previous_frame_0_t is read_previous' state across its
 * pad: its arguments, its results, and the names and bytes it holds.
 */
typedef struct chunks_file_read_previous_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	StdVec	   *result;
	ChunksErrorError *error;
	StdPathBuf	newest;
	bool		newest_live;
	StdPathBuf	previous;
	bool		previous_live;
	bool		has_read;
	StdVec		read;
	bool		read_live;
	bool		ok;
} chunks_file_read_previous_frame_0_t;

/* chunks_file_read_previous_body_0 is read_previous' body. */
static void
chunks_file_read_previous_body_0(void *frame_)
{
	chunks_file_read_previous_frame_0_t *frame = frame_;
	const uint8 *bytes;
	size_t		bytes_len;

	if (!chunks_file_generations(frame->io, frame->path, frame->path_len, &frame->newest, &frame->previous,
								 frame->error))
		return;
	frame->previous_live = true;
	frame->newest_live = false;
	std_vec_free_Checked(&frame->newest.bytes, 1, 1);
	if (chunks_file_read_if_there(frame->io, frame->previous.bytes.ptr, frame->previous.bytes.len, &frame->has_read,
								  &frame->read, frame->error))
	{
		frame->read_live = frame->has_read;
		*frame->has_result = frame->has_read &&
			chunks_file_verified(frame->read.ptr, frame->read.len, &bytes, &bytes_len);
		if (*frame->has_result)
			std_slice_to_vec_Checked(frame->result, bytes, bytes_len, 1, 1);
		frame->ok = true;
		if (frame->read_live)
		{
			frame->read_live = false;
			std_vec_free_Checked(&frame->read, 1, 1);
		}
	}
	frame->previous_live = false;
	std_vec_free_Checked(&frame->previous.bytes, 1, 1);
}

/* chunks_file_read_previous_pad_0 is read_previous' pad: the bytes and the names, while it holds them. */
static void
chunks_file_read_previous_pad_0(void *frame_)
{
	chunks_file_read_previous_frame_0_t *frame = frame_;

	if (frame->read_live)
		std_vec_free_Checked(&frame->read, 1, 1);
	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest.bytes, 1, 1);
}

/*
 * chunks_file_read_previous returns the bytes of the previous generation of
 * a file write_kept wrote, if it has one whose trailer matches, for a caller
 * that knows no rename of the file's generations comes between its reads.
 */
bool
chunks_file_read_previous(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result, StdVec *result,
						  ChunksErrorError *error)
{
	chunks_file_read_previous_frame_0_t frame = {.io = io, .path = path, .path_len = path_len,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, chunks_file_read_previous_body_0, chunks_file_read_previous_pad_0);
	return frame.ok;
}

/*
 * chunks_file_read_if_there_frame_0_t is read_if_there's state across its
 * pad: its arguments, its results, and the file it holds.
 */
typedef struct chunks_file_read_if_there_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	StdVec	   *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_read_if_there_frame_0_t;

/* chunks_file_read_if_there_body_0 is read_if_there's body. */
static void
chunks_file_read_if_there_body_0(void *frame_)
{
	chunks_file_read_if_there_frame_0_t *frame = frame_;
	HostError	host_error;
	bool		has;
	uint64		len;

	if (!host_open_if_there(frame->io, frame->path, frame->path_len, &has, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (!has)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	frame->file_live = true;
	if (!host_size(frame->io, &frame->file, &len, &host_error))
		*frame->error = chunks_error_Error_FromError_from(&host_error);
	else if (chunks_file_read_at(frame->io, &frame->file, 0, (size_t) len, frame->result, frame->error))
	{
		*frame->has_result = true;
		frame->ok = true;
	}
	frame->file_live = false;
	host_File_drop(&frame->file);
}

/* chunks_file_read_if_there_pad_0 is read_if_there's pad: the file, while it holds it. */
static void
chunks_file_read_if_there_pad_0(void *frame_)
{
	chunks_file_read_if_there_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/*
 * chunks_file_read_if_there returns a file's bytes, or None when the file
 * isn't there, asking the door that answers which.
 */
static bool
chunks_file_read_if_there(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result, StdVec *result,
						  ChunksErrorError *error)
{
	chunks_file_read_if_there_frame_0_t frame = {.io = io, .path = path, .path_len = path_len,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, chunks_file_read_if_there_body_0, chunks_file_read_if_there_pad_0);
	return frame.ok;
}

/* chunks_file_verified returns a file's bytes without their xxh3 trailer, when the trailer matches them. */
static bool
chunks_file_verified(const uint8 *kept, size_t kept_len, const uint8 **result, size_t *result_len)
{
	size_t		mid;
	const void *bytes;
	size_t		bytes_len;
	const void *trailer;
	size_t		trailer_len;
	uint8		sum[8];

	if (!std_checked_sub_usize(kept_len, 8, &mid))
		return false;
	if (!std_slice_split_at_checked(kept, kept_len, mid, 1, 1, &bytes, &bytes_len, &trailer, &trailer_len))
		return false;
	std_u64_to_le_bytes(XXH3_64bits(bytes, bytes_len), sum);
	if (trailer_len != 8 || memcmp(sum, trailer, 8) != 0)
		return false;
	*result = bytes;
	*result_len = bytes_len;
	return true;
}

/* CHUNKS_FILE_APPENDED begins a file of appended records, which tells it from a file write_kept wrote. */
const uint8 CHUNKS_FILE_APPENDED[8] = {'D', 'G', 'R', 'E', 'C', 'S', '0', '1'};

/* chunks_file_TornTail_end returns where the whole records end. */
uint64
chunks_file_TornTail_end(const ChunksFileTornTail *self)
{
	return chunks_file_TornTail_held(self, 436, 16)->end;
}

/* chunks_file_TornTail_path returns the file. */
const char *
chunks_file_TornTail_path(const ChunksFileTornTail *self, size_t *result_len)
{
	chunks_file_TornTailLayout *held = chunks_file_TornTail_held(self, 441, 17);

	*result_len = held->path.bytes.len;
	return held->path.bytes.ptr;
}

/* chunks_file_TornTail_generation returns the generation the file's header names. */
uint64
chunks_file_TornTail_generation(const ChunksFileTornTail *self)
{
	return chunks_file_TornTail_held(self, 446, 23)->generation;
}

/*
 * chunks_file_TornTail_of_generation returns the tail, which it takes, of a
 * file whose header names the generation, as its writer wrote it.
 */
ChunksFileTornTail
chunks_file_TornTail_of_generation(ChunksFileTornTail *self, uint64 generation)
{
	ChunksFileTornTail tail = chunks_file_TornTail_take(self, 451, 26);

	CHUNKS_FILE_TORN_TAIL(&tail)->generation = generation;
	return tail;
}

/* chunks_file_Whole_of_generation returns where the whole records of a file whose header names the generation end. */
ChunksFileWhole
chunks_file_Whole_of_generation(ChunksFileWhole self, uint64 generation)
{
	self.generation = generation;
	return self;
}

/*
 * chunks_file_tail returns the tail of a file whose whole records a read of
 * it found end where `whole` says, the one tail of the file, which only the
 * file's owner takes, in its open, and holds for the file's life.
 */
ChunksFileTornTail
chunks_file_tail(const char *path, size_t path_len, ChunksFileWhole whole)
{
	ChunksFileTornTail tail;
	StdPathBuf	tail_path;

	std_path_to_path_buf(path, path_len, &tail_path);
	chunks_file_TornTail_put(&tail, &tail_path, whole.end, whole.generation);
	return tail;
}

/* chunks_file_Synced_cuts reports whether whole records ending where a tail does fall short of the length, in its generation. */
bool
chunks_file_Synced_cuts(const ChunksFileSynced *self, const ChunksFileWhole *whole)
{
	return self->generation == whole->generation && whole->end < self->len;
}

/*
 * chunks_file_generation_header_frame_0_t is generation_header's state
 * across its pad: its arguments, its results, and the header it holds.
 */
typedef struct chunks_file_generation_header_frame_0_t
{
	const uint8 *magic;
	StdVec	   *out_0;
	uint64	   *out_1;
	StdVec		header;
	bool		header_live;
} chunks_file_generation_header_frame_0_t;

/* chunks_file_generation_header_body_0 is generation_header's body. */
static void
chunks_file_generation_header_body_0(void *frame_)
{
	chunks_file_generation_header_frame_0_t *frame = frame_;
	ChunksPackPackId id = chunks_pack_PackId_new();
	uint64		generation;
	uint8		generation_bytes[8];
	uint8		sum[8];
	StdStr		parts[2];
	size_t		taken = 8;

	std_range_to_check(taken, sizeof(id.f0), RUST_FILE, 501, 63);
	if (taken != sizeof(generation_bytes))
		STD_RESULT_EXPECT_FAILED(501, 80, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	generation = std_u64_from_le_bytes(id.f0);
	std_u64_to_le_bytes(generation, generation_bytes);
	parts[0].ptr = (const char *) frame->magic;
	parts[0].len = 8;
	parts[1].ptr = (const char *) generation_bytes;
	parts[1].len = 8;
	std_slice_concat(parts, 2, sizeof(StdStr), 1, 1, &frame->header);
	frame->header_live = true;
	std_u64_to_le_bytes(XXH3_64bits(frame->header.ptr, frame->header.len), sum);
	std_vec_extend_from_slice_Checked(&frame->header, sum, 8, 1, 1);
	frame->header_live = false;
	*frame->out_0 = frame->header;
	*frame->out_1 = generation;
}

/* chunks_file_generation_header_pad_0 is generation_header's pad: the header, while it holds it. */
static void
chunks_file_generation_header_pad_0(void *frame_)
{
	chunks_file_generation_header_frame_0_t *frame = frame_;

	if (frame->header_live)
		std_vec_free_Checked(&frame->header, 1, 1);
}

/*
 * chunks_file_generation_header returns the header that begins a file of a
 * kind written whole, with a generation new to the write, and the
 * generation.
 */
void
chunks_file_generation_header(const uint8 *magic, StdVec *out_0, uint64 *out_1)
{
	chunks_file_generation_header_frame_0_t frame = {.magic = magic, .out_0 = out_0, .out_1 = out_1};

	std_landing(&frame, chunks_file_generation_header_body_0, chunks_file_generation_header_pad_0);
}

/*
 * chunks_file_read_generation returns the generation a file's header names,
 * when its bytes begin with the kind's magic and the header's checksum
 * matches.
 */
bool
chunks_file_read_generation(const uint8 *bytes, size_t bytes_len, const uint8 *magic, uint64 *result)
{
	uint8		sum[8];
	size_t		header_len = CHUNKS_FILE_GENERATION_HEADER;
	bool		whole;

	if (bytes_len < CHUNKS_FILE_GENERATION_HEADER)
		return false;
	whole = header_len >= 8 && memcmp(bytes, magic, 8) == 0;
	if (whole)
	{
		std_range_to_check(16, header_len, RUST_FILE, 511, 80);
		std_u64_to_le_bytes(XXH3_64bits(bytes, 16), sum);
		std_range_from_check(16, header_len, RUST_FILE, 511, 111);
		whole = std_wrapping_sub_usize(header_len, 16) == sizeof(sum) && memcmp(sum, bytes + 16, sizeof(sum)) == 0;
	}
	if (!whole)
		return false;
	std_range_check(8, 16, header_len, RUST_FILE, 512, 44);
	if (std_wrapping_sub_usize(16, 8) != sizeof(uint64))
		STD_RESULT_EXPECT_FAILED(512, 63, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	*result = std_u64_from_le_bytes(bytes + 8);
	return true;
}

#ifdef FRONTEND

/* chunks_file_ReadIo_Interruptible_check_interrupt is ReadIo's check_interrupt: none, a frontend program's signals ending it. */
static bool
chunks_file_ReadIo_Interruptible_check_interrupt(void *self, HostError *error)
{
	return true;
}

/* CHUNKS_FILE_READ_IO_INTERRUPTIBLE_OPS is ReadIo's tables of Sealed, ReadDoors, and Interruptible. */
const ChunksFileInterruptibleOps CHUNKS_FILE_READ_IO_INTERRUPTIBLE_OPS = {
	.read_doors = {
		.sealed = {host_io_ReadIo_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_io_ReadIo_Deref_deref}, host_io_ReadIo_DerefMut_deref_mut},
	},
	.check_interrupt = chunks_file_ReadIo_Interruptible_check_interrupt,
};
#endif

/* chunks_file_Io_Interruptible_check_interrupt is Io's check_interrupt: its own check. */
static bool
chunks_file_Io_Interruptible_check_interrupt(void *self, HostError *error)
{
	return host_io_Io_check(self, error);
}

/* CHUNKS_FILE_IO_INTERRUPTIBLE_OPS is Io's tables of Sealed, ReadDoors, and Interruptible. */
const ChunksFileInterruptibleOps CHUNKS_FILE_IO_INTERRUPTIBLE_OPS = {
	.read_doors = {
		.sealed = {host_io_Io_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_io_Io_Deref_deref}, host_io_Io_DerefMut_deref_mut},
	},
	.check_interrupt = chunks_file_Io_Interruptible_check_interrupt,
};

/* chunks_file_FlagIo_Interruptible_check_interrupt is FlagIo's check_interrupt: its own check. */
static bool
chunks_file_FlagIo_Interruptible_check_interrupt(void *self, HostError *error)
{
	return host_shared_FlagIo_check(self, error);
}

/* CHUNKS_FILE_FLAG_IO_INTERRUPTIBLE_OPS is FlagIo's tables of Sealed, ReadDoors, and Interruptible. */
const ChunksFileInterruptibleOps CHUNKS_FILE_FLAG_IO_INTERRUPTIBLE_OPS = {
	.read_doors = {
		.sealed = {host_shared_FlagIo_Sealed_parts, host_io_sealed_Sealed_pushed},
		.deref_mut = {{host_shared_FlagIo_Deref_deref}, host_shared_FlagIo_DerefMut_deref_mut},
	},
	.check_interrupt = chunks_file_FlagIo_Interruptible_check_interrupt,
};

static bool chunks_file_scan_with(HostIoReadDoors io, const char *path, size_t path_len, uint64 from, size_t stride,
								  bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at,
												  size_t *result),
								  void *record_captures, void (*record_drop) (void *),
								  bool (*check) (void *captures, HostIoReadDoors io, HostError *error),
								  void *check_captures, void (*check_drop) (void *), ChunksFileScan *result,
								  ChunksErrorError *error);

/* host_io_Io_check_Fn_call is the fn item Io::check's Fn shim, as scan passes it for scan_with's check. */
static bool
host_io_Io_check_Fn_call(void *captures, HostIoReadDoors io, HostError *error)
{
	return host_io_Io_check(io.self, error);
}

/*
 * chunks_file_scan reads a file from the place `from` on, which must be where
 * a record starts, a missing file as empty, and its records in the bytes
 * read, each as long as `record` says a whole one at a place in them is.
 * After a record that fails it looks for the next whole one every `stride`
 * bytes on. It checks for an interrupt at each place it tries.
 */
bool
chunks_file_scan(HostIoIo *io, const char *path, size_t path_len, uint64 from, size_t stride,
				 bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at, size_t *result),
				 void *record_captures, void (*record_drop) (void *), ChunksFileScan *result,
				 ChunksErrorError *error)
{
	return chunks_file_scan_with(HOST_IO_IO_READ_DOORS(io), path, path_len, from, stride, record, record_captures,
								 record_drop, host_io_Io_check_Fn_call, NULL, NULL, result, error);
}

/* chunks_file_scan_locked_closure_0 is scan_locked's |_| Ok(()): no check for an interrupt. */
static bool
chunks_file_scan_locked_closure_0(void *captures, HostIoReadDoors io, HostError *error)
{
	return true;
}

/*
 * chunks_file_scan_locked is scan under a lock that holds interrupts, as the
 * packs' lists lock does, so it checks for none; the list files it reads are
 * bounded by what collections keep.
 */
bool
chunks_file_scan_locked(HostIoReadDoors io, const char *path, size_t path_len, uint64 from, size_t stride,
						bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at,
										size_t *result),
						void *record_captures, void (*record_drop) (void *), ChunksFileScan *result,
						ChunksErrorError *error)
{
	return chunks_file_scan_with(io, path, path_len, from, stride, record, record_captures, record_drop,
								 chunks_file_scan_locked_closure_0, NULL, NULL, result, error);
}

/*
 * chunks_file_scan_with_frame_0_t is scan_with's state across its pad: its
 * arguments, its results, the closures it takes, and the file, bytes, and
 * places it holds.
 */
typedef struct chunks_file_scan_with_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	uint64		from;
	size_t		stride;
	bool		(*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at, size_t *result);
	void	   *record_captures;
	void		(*record_drop) (void *);
	bool		record_live;
	bool		(*check) (void *captures, HostIoReadDoors io, HostError *error);
	void	   *check_captures;
	void		(*check_drop) (void *);
	bool		check_live;
	ChunksFileScan *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		records;
	bool		records_live;
	StdVec		damaged;
	bool		damaged_live;
	bool		ok;
} chunks_file_scan_with_frame_0_t;

/* chunks_file_scan_with_closure_0 is scan_with's |at| record(&bytes, at), over the frame's bytes. */
static bool
chunks_file_scan_with_closure_0(const chunks_file_scan_with_frame_0_t *frame, size_t at, size_t *result)
{
	return frame->record(frame->record_captures, frame->bytes.ptr, frame->bytes.len, at, result);
}

/* chunks_file_scan_with_body_0 is scan_with's body. */
static void
chunks_file_scan_with_body_0(void *frame_)
{
	chunks_file_scan_with_frame_0_t *frame = frame_;
	HostError	host_error;
	bool		has;
	uint64		from_3;
	size_t		at = 0;
	bool		has_end = false;
	size_t		end = 0;
	size_t		end_2;
	uint64		whole_end;

	if (!host_open_if_there(frame->io, frame->path, frame->path_len, &has, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		goto drop_closures;
	}
	if (!has)
	{
		frame->bytes = (StdVec) STD_VEC_NEW;
		frame->bytes_live = true;
		from_3 = 0;
	}
	else
	{
		uint64		len;
		uint64		from_2;
		bool		read;

		frame->file_live = true;
		if (!host_size(frame->io, &frame->file, &len, &host_error))
		{
			*frame->error = chunks_error_Error_FromError_from(&host_error);
			frame->file_live = false;
			host_File_drop(&frame->file);
			goto drop_closures;
		}
		from_2 = std_min_u64(frame->from, len);
		read = chunks_file_read_at(frame->io, &frame->file, from_2, (size_t) STD_SUB(u64, len, from_2, 596, 40),
								   &frame->bytes, frame->error);
		frame->file_live = false;
		host_File_drop(&frame->file);
		if (!read)
			goto drop_closures;
		frame->bytes_live = true;
		from_3 = from_2;
	}
	frame->records = (StdVec) STD_VEC_NEW;
	frame->records_live = true;
	frame->damaged = (StdVec) STD_VEC_NEW;
	frame->damaged_live = true;
	while (at < frame->bytes.len)
	{
		size_t		len;
		size_t		start;
		size_t		remaining;
		size_t		next;
		bool		found = false;
		size_t		found_at = 0;

		if (!frame->check(frame->check_captures, frame->io, &host_error))
		{
			*frame->error = chunks_error_Error_FromError_from(&host_error);
			goto drop_all;
		}
		if (chunks_file_scan_with_closure_0(frame, at, &len))
		{
			StdRangeUsize place;

			CHUNKS_CONTRACT(604, 13, len > 0, "a whole record takes at least a byte");
			place.start = at;
			place.end = STD_ADD(usize, at, len, 605, 30);
			std_vec_push_Checked(&frame->records, &place, sizeof(place), _Alignof(StdRangeUsize));
			at = STD_ADD(usize, at, len, 606, 13);
			continue;
		}
		if (!has_end)
		{
			has_end = true;
			end = at;
		}
		start = STD_ADD(usize, at, frame->stride, 610, 16);
		remaining = std_iter_step_by_range_usize(start, frame->bytes.len, frame->stride);
		next = start;
		while (remaining > 0)
		{
			size_t		val = next;
			size_t		ignored;

			next = std_wrapping_add_usize(val, frame->stride);
			remaining = std_wrapping_sub_usize(remaining, 1);
			if (chunks_file_scan_with_closure_0(frame, val, &ignored))
			{
				found = true;
				found_at = val;
				break;
			}
		}
		if (!found)
			break;
		{
			StdRangeUsize place = {at, found_at};

			std_vec_push_Checked(&frame->damaged, &place, sizeof(place), _Alignof(StdRangeUsize));
			at = found_at;
		}
	}
	end_2 = has_end ? end : at;
	frame->result->has_torn = frame->damaged.len == 0;
	whole_end = STD_ADD(u64, from_3, (uint64) end_2, 619, 58);
	if (frame->result->has_torn)
	{
		frame->result->torn.end = whole_end;
		frame->result->torn.generation = 0;
	}
	frame->bytes_live = false;
	frame->result->bytes = frame->bytes;
	frame->records_live = false;
	frame->result->records = frame->records;
	frame->result->end = end_2;
	frame->damaged_live = false;
	frame->result->damaged = frame->damaged;
	frame->ok = true;
	goto drop_closures;
drop_all:
	frame->damaged_live = false;
	std_vec_free_Checked(&frame->damaged, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
	frame->records_live = false;
	std_vec_free_Checked(&frame->records, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
drop_closures:
	frame->check_live = false;
	if (frame->check_drop != NULL)
		frame->check_drop(frame->check_captures);
	frame->record_live = false;
	if (frame->record_drop != NULL)
		frame->record_drop(frame->record_captures);
}

/* chunks_file_scan_with_pad_0 is scan_with's pad: the places, the bytes, the file, and the closures it holds. */
static void
chunks_file_scan_with_pad_0(void *frame_)
{
	chunks_file_scan_with_frame_0_t *frame = frame_;

	if (frame->damaged_live)
		std_vec_free_Checked(&frame->damaged, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
	if (frame->records_live)
		std_vec_free_Checked(&frame->records, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->check_live && frame->check_drop != NULL)
		frame->check_drop(frame->check_captures);
	if (frame->record_live && frame->record_drop != NULL)
		frame->record_drop(frame->record_captures);
}

/* chunks_file_scan_with is scan with its check for an interrupt, both closures taken. */
static bool
chunks_file_scan_with(HostIoReadDoors io, const char *path, size_t path_len, uint64 from, size_t stride,
					  bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at,
									  size_t *result),
					  void *record_captures, void (*record_drop) (void *),
					  bool (*check) (void *captures, HostIoReadDoors io, HostError *error), void *check_captures,
					  void (*check_drop) (void *), ChunksFileScan *result, ChunksErrorError *error)
{
	chunks_file_scan_with_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .from = from,
		.stride = stride, .record = record, .record_captures = record_captures, .record_drop = record_drop,
		.record_live = true, .check = check, .check_captures = check_captures, .check_drop = check_drop,
	.check_live = true, .result = result, .error = error};

	std_landing(&frame, chunks_file_scan_with_body_0, chunks_file_scan_with_pad_0);
	return frame.ok;
}

/*
 * chunks_file_appended_record_frame_0_t is appended_record's state across its
 * pad: its argument, and the record it holds.
 */
typedef struct chunks_file_appended_record_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	StdVec		record;
	bool		record_live;
} chunks_file_appended_record_frame_0_t;

/* chunks_file_appended_record_body_0 is appended_record's body. */
static void
chunks_file_appended_record_body_0(void *frame_)
{
	chunks_file_appended_record_frame_0_t *frame = frame_;
	uint8		len[4];
	uint8		sum[8];

	std_vec_with_capacity_Checked(&frame->record, STD_ADD(usize, frame->bytes_len, 12, 626, 41), 1, 1);
	frame->record_live = true;
	std_u32_to_le_bytes((uint32) frame->bytes_len, len);
	std_vec_extend_from_slice_Checked(&frame->record, len, 4, 1, 1);
	std_vec_extend_from_slice_Checked(&frame->record, frame->bytes, frame->bytes_len, 1, 1);
	std_u64_to_le_bytes(XXH3_64bits(frame->record.ptr, frame->record.len), sum);
	std_vec_extend_from_slice_Checked(&frame->record, sum, 8, 1, 1);
	frame->record_live = false;
}

/* chunks_file_appended_record_pad_0 is appended_record's pad: the record, while it holds it. */
static void
chunks_file_appended_record_pad_0(void *frame_)
{
	chunks_file_appended_record_frame_0_t *frame = frame_;

	if (frame->record_live)
		std_vec_free_Checked(&frame->record, 1, 1);
}

/*
 * chunks_file_appended_record returns a record as a file of appended records
 * stores it: its length, its bytes, and an xxh3 of both.
 */
static StdVec
chunks_file_appended_record(const uint8 *bytes, size_t bytes_len)
{
	chunks_file_appended_record_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len};

	std_landing(&frame, chunks_file_appended_record_body_0, chunks_file_appended_record_pad_0);
	return frame.record;
}

/*
 * chunks_file_whole_record returns the length of the record that starts at a
 * place in a file of appended records' bytes, or None when no whole record
 * with a matching checksum starts there.
 */
static bool
chunks_file_whole_record(const uint8 *bytes, size_t bytes_len, size_t at, size_t *result)
{
	size_t		head_end = STD_ADD(usize, at, 4, 636, 48);
	size_t		len;
	size_t		end;
	const void *body;
	size_t		body_len;
	const void *sum_part;
	size_t		sum_len;
	uint8		sum[8];

	if (!(at <= head_end && head_end <= bytes_len))
		return false;
	if (std_wrapping_sub_usize(head_end, at) != 4)
		STD_RESULT_EXPECT_FAILED(636, 68, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	len = (size_t) std_u32_from_le_bytes(bytes + at);
	end = STD_ADD(usize, STD_ADD(usize, STD_ADD(usize, at, 4, 637, 32), len, 637, 32), 8, 637, 32);
	if (!(at <= end && end <= bytes_len))
		return false;
	std_slice_split_at_Checked(bytes + at, std_wrapping_sub_usize(end, at), STD_ADD(usize, 4, len, 638, 39), 1, 1,
							   &body, &body_len, &sum_part, &sum_len, RUST_FILE, 638, 30);
	std_u64_to_le_bytes(XXH3_64bits(body, body_len), sum);
	if (sum_len != 8 || memcmp(sum, sum_part, 8) != 0)
		return false;
	*result = std_wrapping_sub_usize(end, at);
	return true;
}

/* chunks_file_VecU8_drop is a Vec<u8>'s drop glue, an element of a Vec<Vec<u8>>. */
static void
chunks_file_VecU8_drop(void *elem)
{
	std_vec_free_Checked(elem, 1, 1);
}

/*
 * chunks_file_appended_between_frame_0_t is appended_between's state across
 * its pad: its arguments, its results, and the file, bytes, records, body,
 * and message it holds.
 */
typedef struct chunks_file_appended_between_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	uint64		from;
	uint64		to;
	bool	   *has_result;
	StdVec	   *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		records;
	bool		records_live;
	StdVec		body;
	bool		body_live;
	StdString	message;
	bool		message_live;
	bool		ok;
} chunks_file_appended_between_frame_0_t;

/* chunks_file_appended_between_body_0 is appended_between's body. */
static void
chunks_file_appended_between_body_0(void *frame_)
{
	chunks_file_appended_between_frame_0_t *frame = frame_;
	HostError	host_error;
	uint64		len;
	bool		has;
	bool		read;
	size_t		at = 0;
	StdStr		shown = {frame->path, frame->path_len};

	if (!std_checked_sub_u64(frame->to, frame->from, &len))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a read of "), STD_FMT_ARG(0), STD_FMT_LIT(" ends before it starts")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
		return;
	}
	if (!host_open_if_there(frame->io, frame->path, frame->path_len, &has, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (!has)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	frame->file_live = true;
	read = chunks_file_read_at(frame->io, &frame->file, frame->from, (size_t) len, &frame->bytes, frame->error);
	if (!read)
		goto drop_file;
	frame->bytes_live = true;
	frame->records = (StdVec) STD_VEC_NEW;
	frame->records_live = true;
	while (at < frame->bytes.len)
	{
		size_t		len_2;
		size_t		start;
		size_t		end;

		if (!chunks_file_whole_record(frame->bytes.ptr, frame->bytes.len, at, &len_2))
		{
			uint64		offset = STD_ADD(u64, frame->from, (uint64) at, 655, 26);
			StdFmtPiece pieces[] = {STD_FMT_LIT("file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" is damaged at offset "),
			STD_FMT_ARG(1), STD_FMT_LIT(", within the records a reader was told of")};
			StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&offset, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, 5, args};

			std_fmt_format_Checked(&frame->message, &arguments);
			frame->message_live = true;
			frame->error->tag = CHUNKS_ERROR_ERROR_DAMAGED;
			std_path_to_path_buf(frame->path, frame->path_len, &frame->error->u.damaged.path);
			frame->error->u.damaged.offset = offset;
			frame->error->u.damaged.message = frame->message;
			frame->message_live = false;
			goto drop_records;
		}
		start = STD_ADD(usize, at, 4, 662, 28);
		end = STD_SUB(usize, STD_ADD(usize, at, len_2, 662, 36), 8, 662, 36);
		std_range_check(start, end, frame->bytes.len, RUST_FILE, 662, 27);
		std_slice_to_vec_Checked(&frame->body, STD_PTR_AT(frame->bytes.ptr, start, 1), std_wrapping_sub_usize(end, start),
								 1, 1);
		frame->body_live = true;
		std_vec_push_Checked(&frame->records, &frame->body, sizeof(StdVec), _Alignof(StdVec));
		frame->body_live = false;
		at = STD_ADD(usize, at, len_2, 663, 9);
	}
	frame->records_live = false;
	*frame->result = frame->records;
	*frame->has_result = true;
	frame->ok = true;
	goto drop_bytes;
drop_records:
	frame->records_live = false;
	std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
drop_bytes:
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
drop_file:
	frame->file_live = false;
	host_File_drop(&frame->file);
}

/*
 * chunks_file_appended_between_pad_0 is appended_between's pad: the message
 * and the body it holds until the error or the records take them, the
 * records, the bytes, and the file.
 */
static void
chunks_file_appended_between_pad_0(void *frame_)
{
	chunks_file_appended_between_frame_0_t *frame = frame_;

	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->records_live)
		std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->file_live)
		host_File_drop(&frame->file);
}

/*
 * chunks_file_appended_between returns the records of a file of appended
 * records between two places where records end, in order, for a reader that
 * has read the file up to the first and is told the records now end at the
 * second. Bytes there that aren't whole records are damage. It answers
 * nothing when no file has the path.
 */
bool
chunks_file_appended_between(HostIoReadDoors io, const char *path, size_t path_len, uint64 from, uint64 to,
							 bool *has_result, StdVec *result, ChunksErrorError *error)
{
	chunks_file_appended_between_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .from = from,
	.to = to, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, chunks_file_appended_between_body_0, chunks_file_appended_between_pad_0);
	return frame.ok;
}

/* chunks_file_Records_drop is Records' drop glue: its bytes, or its records and damage. */
static void
chunks_file_Records_drop(ChunksFileRecords *self)
{
	switch (self->tag)
	{
		case CHUNKS_FILE_RECORDS_MISSING:
			break;
		case CHUNKS_FILE_RECORDS_OTHER:
			std_vec_free_Checked(&self->u.other, 1, 1);
			break;
		case CHUNKS_FILE_RECORDS_APPENDED:
			std_vec_drop_Checked(&self->u.appended.f0, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
			if (self->u.appended.has_f2)
				chunks_file_Damage_drop(&self->u.appended.f2);
			break;
	}
}

/*
 * chunks_file_previous_damaged_frame_0_t is previous_damaged's state across
 * its pad: its arguments, its results, and the names and read it holds.
 */
typedef struct chunks_file_previous_damaged_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool	   *result;
	ChunksErrorError *error;
	StdPathBuf	newest;
	bool		newest_live;
	StdPathBuf	previous;
	bool		previous_live;
	ChunksFileRecords records;
	bool		records_live;
	bool		ok;
} chunks_file_previous_damaged_frame_0_t;

/* chunks_file_previous_damaged_body_0 is previous_damaged's body. */
static void
chunks_file_previous_damaged_body_0(void *frame_)
{
	chunks_file_previous_damaged_frame_0_t *frame = frame_;
	const uint8 *verified;
	size_t		verified_len;

	if (!chunks_file_generations(frame->io, frame->path, frame->path_len, &frame->newest, &frame->previous,
								 frame->error))
		return;
	frame->previous_live = true;
	frame->newest_live = false;
	std_vec_free_Checked(&frame->newest.bytes, 1, 1);
	if (chunks_file_appended_records(frame->io, frame->previous.bytes.ptr, frame->previous.bytes.len,
									 &frame->records, frame->error))
	{
		frame->records_live = true;
		switch (frame->records.tag)
		{
			case CHUNKS_FILE_RECORDS_MISSING:
				*frame->result = false;
				break;
			case CHUNKS_FILE_RECORDS_OTHER:
				*frame->result = !chunks_file_verified(frame->records.u.other.ptr, frame->records.u.other.len,
													   &verified, &verified_len);
				break;
			case CHUNKS_FILE_RECORDS_APPENDED:
				*frame->result = frame->records.u.appended.has_f2;
				break;
		}
		frame->ok = true;
		frame->records_live = false;
		chunks_file_Records_drop(&frame->records);
	}
	frame->previous_live = false;
	std_vec_free_Checked(&frame->previous.bytes, 1, 1);
}

/* chunks_file_previous_damaged_pad_0 is previous_damaged's pad: the read and the names, while it holds them. */
static void
chunks_file_previous_damaged_pad_0(void *frame_)
{
	chunks_file_previous_damaged_frame_0_t *frame = frame_;

	if (frame->records_live)
		chunks_file_Records_drop(&frame->records);
	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest.bytes, 1, 1);
}

/*
 * chunks_file_previous_damaged reports whether the previous generation
 * write_kept or write_appended kept of a file, when there is one, is
 * damaged, as a read that falls back to it would find it.
 */
bool
chunks_file_previous_damaged(HostIoReadDoors io, const char *path, size_t path_len, bool *result,
							 ChunksErrorError *error)
{
	chunks_file_previous_damaged_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_previous_damaged_body_0, chunks_file_previous_damaged_pad_0);
	return frame.ok;
}

/*
 * chunks_file_appended_records_closure_0 is appended_records' whole: the
 * header's length at the start, and a whole record's anywhere else.
 */
static bool
chunks_file_appended_records_closure_0(void *captures, const uint8 *bytes, size_t bytes_len, size_t at,
									   size_t *result)
{
	if (at == 0)
	{
		if (!(bytes_len >= sizeof(CHUNKS_FILE_APPENDED) &&
			  memcmp(bytes, CHUNKS_FILE_APPENDED, sizeof(CHUNKS_FILE_APPENDED)) == 0))
			return false;
		*result = sizeof(CHUNKS_FILE_APPENDED);
		return true;
	}
	return chunks_file_whole_record(bytes, bytes_len, at, result);
}

/*
 * chunks_file_appended_records_closure_1 is appended_records' body: the
 * bytes of a record the scan found, past its length and before its checksum.
 */
static StdVec
chunks_file_appended_records_closure_1(const StdVec *bytes, const StdRangeUsize *record)
{
	size_t		start = STD_ADD(usize, record->start, 4, 703, 54);
	size_t		end = STD_SUB(usize, record->end, 8, 703, 72);
	StdVec		body;

	std_range_check(start, end, bytes->len, RUST_FILE, 703, 53);
	std_slice_to_vec_Checked(&body, STD_PTR_AT(bytes->ptr, start, 1), std_wrapping_sub_usize(end, start), 1, 1);
	return body;
}

/*
 * chunks_file_appended_records_frame_0_t is appended_records' state across
 * its pad: its arguments, its results, and the scan, records, and body it
 * holds.
 */
typedef struct chunks_file_appended_records_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	ChunksFileRecords *result;
	ChunksErrorError *error;
	ChunksFileScan scanned;
	bool		scanned_live;
	StdVec		records;
	bool		records_live;
	StdVec		after;
	bool		after_live;
	StdVec		body;
	bool		body_live;
	bool		ok;
} chunks_file_appended_records_frame_0_t;

/* chunks_file_appended_records_body_0 is appended_records' body. */
static void
chunks_file_appended_records_body_0(void *frame_)
{
	chunks_file_appended_records_frame_0_t *frame = frame_;
	const StdRangeUsize *found;
	size_t		skipped;
	const StdRangeUsize *kept;
	bool		there;
	bool		has_torn;
	bool		first = true;

	if (!chunks_file_scan_locked(frame->io, frame->path, frame->path_len, 0, 1,
								 chunks_file_appended_records_closure_0, NULL, NULL, &frame->scanned, frame->error))
		return;
	frame->scanned_live = true;
	if (frame->scanned.bytes.len == 0)
	{
		if (!chunks_file_exists(frame->io, frame->path, frame->path_len, &there, frame->error))
			goto drop_scanned;
		if (!there)
		{
			frame->result->tag = CHUNKS_FILE_RECORDS_MISSING;
			frame->ok = true;
			goto drop_scanned;
		}
	}
	if (!(frame->scanned.bytes.len >= sizeof(CHUNKS_FILE_APPENDED) &&
		  memcmp(frame->scanned.bytes.ptr, CHUNKS_FILE_APPENDED, sizeof(CHUNKS_FILE_APPENDED)) == 0))
	{
		frame->result->tag = CHUNKS_FILE_RECORDS_OTHER;
		frame->scanned_live = false;
		frame->result->u.other = frame->scanned.bytes;
		std_vec_free_Checked(&frame->scanned.records, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
		std_vec_free_Checked(&frame->scanned.damaged, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
		frame->ok = true;
		return;
	}
	found = frame->scanned.records.ptr;
	frame->records = (StdVec) STD_VEC_NEW;
	frame->records_live = true;
	skipped = std_min_usize(frame->scanned.records.len, 1);
	kept = STD_PTR_AT(found, skipped, sizeof(StdRangeUsize));
	for (size_t i = 0, i_len = std_wrapping_sub_usize(frame->scanned.records.len, skipped); i < i_len; i++)
	{
		const StdRangeUsize *record = STD_PTR_AT(kept, i, sizeof(StdRangeUsize));

		if (!(record->end <= frame->scanned.end))
			break;
		frame->body = chunks_file_appended_records_closure_1(&frame->scanned.bytes, record);
		frame->body_live = true;
		if (first)
			std_vec_collect_first(&frame->records, &frame->body, 0, sizeof(StdVec), _Alignof(StdVec));
		else
			std_vec_extend_one(&frame->records, &frame->body, 0, sizeof(StdVec), _Alignof(StdVec));
		frame->body_live = false;
		first = false;
	}
	has_torn = frame->scanned.has_torn &&
		(frame->records.len > 0 || frame->scanned.end == frame->scanned.bytes.len);
	frame->result->u.appended.has_f2 = !has_torn;
	if (!has_torn)
	{
		const StdRangeUsize *damaged = frame->scanned.damaged.ptr;
		size_t		past = frame->scanned.damaged.len > 0 ? damaged->end : frame->scanned.bytes.len;

		frame->after = (StdVec) STD_VEC_NEW;
		frame->after_live = true;
		first = true;
		for (size_t i = 0, i_len = frame->scanned.records.len; i < i_len; i++)
		{
			const StdRangeUsize *record = STD_PTR_AT(found, i, sizeof(StdRangeUsize));

			if (!(record->start >= past))
				continue;
			frame->body = chunks_file_appended_records_closure_1(&frame->scanned.bytes, record);
			frame->body_live = true;
			if (first)
				std_vec_collect_first(&frame->after, &frame->body, 0, sizeof(StdVec), _Alignof(StdVec));
			else
				std_vec_extend_one(&frame->after, &frame->body, 0, sizeof(StdVec), _Alignof(StdVec));
			frame->body_live = false;
			first = false;
		}
		frame->result->u.appended.f2.offset = (uint64) frame->scanned.end;
		frame->after_live = false;
		frame->result->u.appended.f2.after = frame->after;
	}
	frame->result->tag = CHUNKS_FILE_RECORDS_APPENDED;
	frame->records_live = false;
	frame->result->u.appended.f0 = frame->records;
	frame->result->u.appended.has_f1 = has_torn;
	frame->result->u.appended.f1 = frame->scanned.torn;
	frame->ok = true;
drop_scanned:
	frame->scanned_live = false;
	chunks_file_Scan_drop(&frame->scanned);
}

/*
 * chunks_file_appended_records_pad_0 is appended_records' pad: the body, the
 * records, and the scan, while it holds them.
 */
static void
chunks_file_appended_records_pad_0(void *frame_)
{
	chunks_file_appended_records_frame_0_t *frame = frame_;

	if (frame->body_live)
		std_vec_free_Checked(&frame->body, 1, 1);
	if (frame->after_live)
		std_vec_drop_Checked(&frame->after, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->records_live)
		std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->scanned_live)
		chunks_file_Scan_drop(&frame->scanned);
}

/*
 * chunks_file_appended_records reads the whole records of a file of appended
 * records in order, stopping at the first record a crash cut short or
 * damaged, with where they end unless the file is damaged rather than cut
 * short, which its first record failing or a whole record after the one that
 * failed shows, since only the last record can be cut short.
 */
static bool
chunks_file_appended_records(HostIoReadDoors io, const char *path, size_t path_len, ChunksFileRecords *result,
							 ChunksErrorError *error)
{
	chunks_file_appended_records_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_appended_records_body_0, chunks_file_appended_records_pad_0);
	return frame.ok;
}

/*
 * chunks_file_read_appended_closure_0_frame_0_t is read_appended's read's
 * state across its pad: the Records it takes, its results, and the bytes it
 * makes a record of.
 */
typedef struct chunks_file_read_appended_closure_0_frame_0_t
{
	ChunksFileRecords records;
	bool		records_live;
	StdVec	   *out_0;
	bool	   *has_out_1;
	ChunksFileWhole *out_1;
	bool	   *out_2;
	StdVec		record;
	bool		record_live;
	bool		some;
} chunks_file_read_appended_closure_0_frame_0_t;

/* chunks_file_read_appended_closure_0_body_0 is read's body. */
static void
chunks_file_read_appended_closure_0_body_0(void *frame_)
{
	chunks_file_read_appended_closure_0_frame_0_t *frame = frame_;
	const uint8 *bytes;
	size_t		bytes_len;

	if (frame->records.tag == CHUNKS_FILE_RECORDS_APPENDED && frame->records.u.appended.has_f1)
	{
		frame->records_live = false;
		*frame->out_0 = frame->records.u.appended.f0;
		*frame->has_out_1 = true;
		*frame->out_1 = frame->records.u.appended.f1;
		*frame->out_2 = false;
		if (frame->records.u.appended.has_f2)
			chunks_file_Damage_drop(&frame->records.u.appended.f2);
		frame->some = true;
		return;
	}
	if (frame->records.tag == CHUNKS_FILE_RECORDS_OTHER &&
		chunks_file_verified(frame->records.u.other.ptr, frame->records.u.other.len, &bytes, &bytes_len))
	{
		StdVec	   *boxed;

		std_slice_to_vec_Checked(&frame->record, bytes, bytes_len, 1, 1);
		frame->record_live = true;
		boxed = std_box_new_uninit_layout_Checked(sizeof(StdVec), _Alignof(StdVec));
		STD_CHECK_ALIGNED(boxed, _Alignof(StdVec), 725, 64);
		STD_CHECK_NULL_DEREF(boxed, 725, 64);
		frame->record_live = false;
		boxed[0] = frame->record;
		std_box_slice_into_vec_Checked(frame->out_0, boxed, 1, sizeof(StdVec));
		*frame->has_out_1 = false;
		*frame->out_2 = true;
		frame->some = true;
	}
	frame->records_live = false;
	chunks_file_Records_drop(&frame->records);
}

/* chunks_file_read_appended_closure_0_pad_0 is read's pad: the record and the Records, while it holds them. */
static void
chunks_file_read_appended_closure_0_pad_0(void *frame_)
{
	chunks_file_read_appended_closure_0_frame_0_t *frame = frame_;

	if (frame->record_live)
		std_vec_free_Checked(&frame->record, 1, 1);
	if (frame->records_live)
		chunks_file_Records_drop(&frame->records);
}

/*
 * chunks_file_read_appended_closure_0 is read_appended's read, taking a
 * Records: the records of one whose end is known, or a file write_kept wrote
 * as one record, or None.
 */
static bool
chunks_file_read_appended_closure_0(ChunksFileRecords *records, StdVec *out_0, bool *has_out_1,
									ChunksFileWhole *out_1, bool *out_2)
{
	chunks_file_read_appended_closure_0_frame_0_t frame = {.records = *records, .records_live = true,
	.out_0 = out_0, .has_out_1 = has_out_1, .out_1 = out_1, .out_2 = out_2};

	std_landing(&frame, chunks_file_read_appended_closure_0_body_0, chunks_file_read_appended_closure_0_pad_0);
	return frame.some;
}

/* chunks_file_Appended_Default_default is Appended's derived Default: no records, no end, and no fallback. */
static ChunksFileAppended
chunks_file_Appended_Default_default(void)
{
	ChunksFileAppended appended = {.records = STD_VEC_NEW};

	return appended;
}

/*
 * chunks_file_read_appended_frame_0_t is read_appended's state across its
 * pad: its arguments, its results, and the names, read, and message it
 * holds.
 */
typedef struct chunks_file_read_appended_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	ChunksFileAppended *result;
	ChunksErrorError *error;
	StdPathBuf	newest_path;
	bool		newest_path_live;
	StdPathBuf	previous_path;
	bool		previous_path_live;
	ChunksFileRecords newest;
	bool		newest_live;
	bool		ok;
	StdString	message;
	bool		message_live;
} chunks_file_read_appended_frame_0_t;

/* chunks_file_read_appended_body_0 is read_appended's body. */
static void
chunks_file_read_appended_body_0(void *frame_)
{
	chunks_file_read_appended_frame_0_t *frame = frame_;
	StdStr		shown = {frame->path, frame->path_len};
	bool		missing;
	StdVec		records;
	bool		has_end;
	ChunksFileWhole end;
	bool		kept;
	ChunksFileFallback why;
	ChunksFileRecords previous;
	bool		there;

	if (!chunks_file_generations(frame->io, frame->path, frame->path_len, &frame->newest_path,
								 &frame->previous_path, frame->error))
		return;
	frame->newest_path_live = true;
	frame->previous_path_live = true;
	if (!chunks_file_appended_records(frame->io, frame->newest_path.bytes.ptr, frame->newest_path.bytes.len,
									  &frame->newest, frame->error))
		goto drop_paths;
	frame->newest_live = true;
	if (frame->newest.tag == CHUNKS_FILE_RECORDS_APPENDED && !frame->newest.u.appended.has_f1 &&
		frame->newest.u.appended.has_f2)
	{
		if (frame->newest.u.appended.f0.len == 0)
		{
			uint64		offset = frame->newest.u.appended.f2.offset;
			StdFmtPiece pieces[] = {STD_FMT_LIT("file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" is damaged at offset "),
			STD_FMT_ARG(1), STD_FMT_LIT(" in its first record, which no WAL record restores")};
			StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&offset, std_u64_Display_fmt}};
			StdFmtArguments arguments = {pieces, 5, args};

			std_fmt_format_Checked(&frame->message, &arguments);
			frame->message_live = true;
			frame->error->tag = CHUNKS_ERROR_ERROR_DAMAGED;
			std_path_to_path_buf(frame->path, frame->path_len, &frame->error->u.damaged.path);
			frame->error->u.damaged.offset = offset;
			frame->error->u.damaged.message = frame->message;
			frame->message_live = false;
			frame->newest_live = false;
			chunks_file_Records_drop(&frame->newest);
			goto drop_paths;
		}
		*frame->result = chunks_file_Appended_Default_default();
		frame->newest_live = false;
		frame->result->records = frame->newest.u.appended.f0;
		frame->result->has_damage = true;
		frame->result->damage = frame->newest.u.appended.f2;
		*frame->has_result = true;
		frame->ok = true;
		goto drop_paths;
	}
	missing = frame->newest.tag == CHUNKS_FILE_RECORDS_MISSING;
	frame->newest_live = false;
	if (chunks_file_read_appended_closure_0(&frame->newest, &records, &has_end, &end, &kept))
	{
		*frame->result = chunks_file_Appended_Default_default();
		frame->result->records = records;
		frame->result->kept = kept;
		if (std_path_PartialEq_eq(frame->newest_path.bytes.ptr, frame->newest_path.bytes.len, frame->path,
								  frame->path_len))
		{
			frame->result->has_end = has_end;
			frame->result->end = end;
		}
		else
		{
			frame->result->has_fallback = true;
			frame->result->fallback.tag = CHUNKS_FILE_FALLBACK_MISSING;
		}
		*frame->has_result = true;
		frame->ok = true;
		goto drop_paths;
	}
	why.tag = missing ? CHUNKS_FILE_FALLBACK_MISSING : CHUNKS_FILE_FALLBACK_DAMAGED;
	if (!chunks_file_appended_records(frame->io, frame->previous_path.bytes.ptr, frame->previous_path.bytes.len,
									  &previous, frame->error))
		goto drop_paths;
	if (chunks_file_read_appended_closure_0(&previous, &records, &has_end, &end, &kept))
	{
		*frame->result = chunks_file_Appended_Default_default();
		frame->result->records = records;
		frame->result->has_fallback = true;
		frame->result->fallback = why;
		frame->result->kept = kept;
		*frame->has_result = true;
		frame->ok = true;
		goto drop_paths;
	}
	if (missing)
	{
		if (!chunks_file_exists(frame->io, frame->previous_path.bytes.ptr, frame->previous_path.bytes.len, &there,
								frame->error))
			goto drop_paths;
		if (!there)
		{
			*frame->has_result = false;
			frame->ok = true;
			goto drop_paths;
		}
	}
	{
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" and its previous generation are damaged")};
		StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 2, args};

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
	}
drop_paths:
	frame->previous_path_live = false;
	std_vec_free_Checked(&frame->previous_path.bytes, 1, 1);
	frame->newest_path_live = false;
	std_vec_free_Checked(&frame->newest_path.bytes, 1, 1);
}

/*
 * chunks_file_read_appended_pad_0 is read_appended's pad: the message, the
 * newest's read, and the names, while it holds them.
 */
static void
chunks_file_read_appended_pad_0(void *frame_)
{
	chunks_file_read_appended_frame_0_t *frame = frame_;

	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
	if (frame->newest_live)
		chunks_file_Records_drop(&frame->newest);
	if (frame->previous_path_live)
		std_vec_free_Checked(&frame->previous_path.bytes, 1, 1);
	if (frame->newest_path_live)
		std_vec_free_Checked(&frame->newest_path.bytes, 1, 1);
}

/*
 * chunks_file_read_appended returns the records of a file write_appended and
 * append wrote, up to one a crash cut short, or those of its previous
 * generation when the newest is missing or, as a file write_kept wrote,
 * damaged, with why, or None when neither exists. A newest file damaged
 * past its first record reads as its records before the damage, with the
 * damage, and one whose first record is damaged is refused.
 */
bool
chunks_file_read_appended(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
						  ChunksFileAppended *result, ChunksErrorError *error)
{
	chunks_file_read_appended_frame_0_t frame = {.io = io, .path = path, .path_len = path_len,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, chunks_file_read_appended_body_0, chunks_file_read_appended_pad_0);
	return frame.ok;
}

/*
 * chunks_file_write_appended_frame_0_t is write_appended's state across its
 * pad: its arguments, its results, and the names, read, and bytes it holds.
 */
typedef struct chunks_file_write_appended_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const uint8 *record;
	size_t		record_len;
	bool	   *placing;
	ChunksFileTornTail *result;
	ChunksErrorError *error;
	StdPathBuf	newest;
	bool		newest_live;
	StdPathBuf	previous;
	bool		previous_live;
	ChunksFileRecords records;
	bool		records_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		appended;
	bool		appended_live;
	StdPathBuf	temp;
	bool		temp_live;
	bool		ok;
} chunks_file_write_appended_frame_0_t;

/* chunks_file_write_appended_body_0 is write_appended's body. */
static void
chunks_file_write_appended_body_0(void *frame_)
{
	chunks_file_write_appended_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	const uint8 *verified;
	size_t		verified_len;
	bool		readable = false;

	if (!chunks_file_writable(&action, frame->path, frame->path_len, frame->error))
		return;
	if (!chunks_file_generations(read, frame->path, frame->path_len, &frame->newest, &frame->previous, frame->error))
		return;
	frame->newest_live = true;
	frame->previous_live = false;
	std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (!chunks_file_appended_records(read, frame->newest.bytes.ptr, frame->newest.bytes.len, &frame->records,
									  frame->error))
		goto drop_newest;
	frame->records_live = true;
	switch (frame->records.tag)
	{
		case CHUNKS_FILE_RECORDS_MISSING:
			readable = false;
			break;
		case CHUNKS_FILE_RECORDS_OTHER:
			readable = chunks_file_verified(frame->records.u.other.ptr, frame->records.u.other.len, &verified,
											&verified_len);
			break;
		case CHUNKS_FILE_RECORDS_APPENDED:
			readable = frame->records.u.appended.has_f1;
			break;
	}
	frame->records_live = false;
	chunks_file_Records_drop(&frame->records);
	std_slice_to_vec_Checked(&frame->bytes, CHUNKS_FILE_APPENDED, sizeof(CHUNKS_FILE_APPENDED), 1, 1);
	frame->bytes_live = true;
	frame->appended = chunks_file_appended_record(frame->record, frame->record_len);
	frame->appended_live = true;
	std_vec_extend_from_slice_Checked(&frame->bytes, frame->appended.ptr, frame->appended.len, 1, 1);
	frame->appended_live = false;
	std_vec_free_Checked(&frame->appended, 1, 1);
	if (chunks_file_write_beside(frame->io, frame->path, frame->path_len, frame->bytes.ptr, frame->bytes.len,
								 &frame->temp, frame->error))
	{
		frame->temp_live = true;
		*frame->placing = true;
		if (chunks_file_rename_generations(frame->io, frame->path, frame->path_len, frame->temp.bytes.ptr,
										   frame->temp.bytes.len, readable, false, frame->error))
		{
			StdPathBuf	path;

			std_path_to_path_buf(frame->path, frame->path_len, &path);
			chunks_file_TornTail_put(frame->result, &path, (uint64) frame->bytes.len, 0);
			frame->ok = true;
		}
		frame->temp_live = false;
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
	}
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
drop_newest:
	frame->newest_live = false;
	std_vec_free_Checked(&frame->newest.bytes, 1, 1);
}

/* chunks_file_write_appended_pad_0 is write_appended's pad: the names, the read, and the bytes it holds. */
static void
chunks_file_write_appended_pad_0(void *frame_)
{
	chunks_file_write_appended_frame_0_t *frame = frame_;

	if (frame->temp_live)
		std_vec_free_Checked(&frame->temp.bytes, 1, 1);
	if (frame->appended_live)
		std_vec_free_Checked(&frame->appended, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->records_live)
		chunks_file_Records_drop(&frame->records);
	if (frame->previous_live)
		std_vec_free_Checked(&frame->previous.bytes, 1, 1);
	if (frame->newest_live)
		std_vec_free_Checked(&frame->newest.bytes, 1, 1);
}

/*
 * chunks_file_write_appended writes a list file of appended records whole
 * and durably as write_file does, with one record, keeping the file it
 * replaces as its previous generation when that file reads, and returns
 * where the next record goes. It sets `placing` once the new bytes are
 * durable beside the file and its renames begin.
 */
bool
chunks_file_write_appended(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *record,
						   size_t record_len, bool *placing, ChunksFileTornTail *result, ChunksErrorError *error)
{
	chunks_file_write_appended_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .record = record,
	.record_len = record_len, .placing = placing, .result = result, .error = error};

	std_landing(&frame, chunks_file_write_appended_body_0, chunks_file_write_appended_pad_0);
	return frame.ok;
}

/*
 * chunks_file_append_frame_0_t is append's state across its pad: its
 * arguments, its results, the tail it takes, and the record it holds.
 */
typedef struct chunks_file_append_frame_0_t
{
	HostIoIoDoors io;
	ChunksFileTornTail end;
	bool		end_live;
	const uint8 *record;
	size_t		record_len;
	ChunksFileTornTail *result;
	ChunksErrorError *error;
	StdVec		appended;
	bool		appended_live;
	bool		ok;
} chunks_file_append_frame_0_t;

/* chunks_file_append_body_0 is append's body. */
static void
chunks_file_append_body_0(void *frame_)
{
	chunks_file_append_frame_0_t *frame = frame_;

	frame->appended = chunks_file_appended_record(frame->record, frame->record_len);
	frame->appended_live = true;
	frame->end_live = false;
	frame->ok = chunks_file_write_end(frame->io, &frame->end, frame->appended.ptr, frame->appended.len, false,
									  frame->result, frame->error);
	frame->appended_live = false;
	std_vec_free_Checked(&frame->appended, 1, 1);
}

/* chunks_file_append_pad_0 is append's pad: the record and the tail, while it holds them. */
static void
chunks_file_append_pad_0(void *frame_)
{
	chunks_file_append_frame_0_t *frame = frame_;

	if (frame->appended_live)
		std_vec_free_Checked(&frame->appended, 1, 1);
	if (frame->end_live)
		chunks_file_TornTail_drop(&frame->end);
}

/*
 * chunks_file_append adds a record durably to a file write_appended wrote,
 * where its whole records end, writing over whatever a crash left past them,
 * and returns where the next record goes.
 */
bool
chunks_file_append(HostIoIoDoors io, ChunksFileTornTail *end, const uint8 *record, size_t record_len,
				   ChunksFileTornTail *result, ChunksErrorError *error)
{
	chunks_file_append_frame_0_t frame = {.io = io, .end = chunks_file_TornTail_take(end, 784, 38), .end_live = true,
	.record = record, .record_len = record_len, .result = result, .error = error};

	std_landing(&frame, chunks_file_append_body_0, chunks_file_append_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS
static void chunks_file_run_append_hook(HostIoIoDoors io, const char *path, size_t path_len);
#endif

/*
 * chunks_file_write_end_frame_0_t is write_end's state across its pad: its
 * arguments, its results, the path it takes, the tail it makes, and the
 * file it holds.
 */
typedef struct chunks_file_write_end_frame_0_t
{
	HostIoIoDoors io;
	StdPathBuf	path;
	bool		path_live;
	uint64		end_2;
	uint64		generation;
	const uint8 *bytes;
	size_t		bytes_len;
	bool		create;
	ChunksFileTornTail *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	ChunksFileTornTail end_3;
	bool		end_3_live;
	bool		ok;
} chunks_file_write_end_frame_0_t;

/* chunks_file_write_end_body_0 is write_end's body. */
static void
chunks_file_write_end_body_0(void *frame_)
{
	chunks_file_write_end_frame_0_t *frame = frame_;
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_WRITE};
	uint64		end_3;

	if (!chunks_file_writable(&action, frame->path.bytes.ptr, frame->path.bytes.len, frame->error))
		goto drop_path;
	if (!chunks_file_open_rw(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, frame->create, &frame->file,
							 frame->error))
		goto drop_path;
	frame->file_live = true;
	if (!chunks_file_write_at(frame->io, &frame->file, frame->path.bytes.ptr, frame->path.bytes.len, frame->end_2,
							  frame->bytes, frame->bytes_len, frame->error))
		goto drop_file;
	end_3 = STD_ADD(u64, frame->end_2, (uint64) frame->bytes_len, 795, 37);
	frame->path_live = false;
	chunks_file_TornTail_put(&frame->end_3, &frame->path, end_3, frame->generation);
	frame->end_3_live = true;
	if (!chunks_file_truncate(frame->io, &frame->file, CHUNKS_FILE_TORN_TAIL(&frame->end_3), frame->error))
		goto drop_end;
#ifdef DOLTGRES_TEST_CHUNKS
	chunks_file_run_append_hook(frame->io, CHUNKS_FILE_TORN_TAIL(&frame->end_3)->path.bytes.ptr,
								CHUNKS_FILE_TORN_TAIL(&frame->end_3)->path.bytes.len);
#endif
	if (!chunks_file_sync(frame->io, &frame->file, frame->error))
		goto drop_end;
	frame->end_3_live = false;
	*frame->result = frame->end_3;
	frame->ok = true;
	goto drop_file;
drop_end:
	frame->end_3_live = false;
	chunks_file_TornTail_drop(&frame->end_3);
drop_file:
	frame->file_live = false;
	host_File_drop(&frame->file);
drop_path:
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
}

/* chunks_file_write_end_pad_0 is write_end's pad: the tail, the file, and the path, while it holds them. */
static void
chunks_file_write_end_pad_0(void *frame_)
{
	chunks_file_write_end_frame_0_t *frame = frame_;

	if (frame->end_3_live)
		chunks_file_TornTail_drop(&frame->end_3);
	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
}

/*
 * chunks_file_write_end writes bytes durably where a file's whole records
 * end, taking its tail, writing over whatever a crash left past them, and
 * returns where the next bytes go. A missing file is created, and its
 * directory synced, when `create` is set.
 */
bool
chunks_file_write_end(HostIoIoDoors io, ChunksFileTornTail *end, const uint8 *bytes, size_t bytes_len, bool create,
					  ChunksFileTornTail *result, ChunksErrorError *error)
{
	ChunksFileTornTail taken = chunks_file_TornTail_take(end, 790, 41);
	chunks_file_write_end_frame_0_t frame = {.io = io, .path = CHUNKS_FILE_TORN_TAIL(&taken)->path, .path_live = true,
		.end_2 = CHUNKS_FILE_TORN_TAIL(&taken)->end, .generation = CHUNKS_FILE_TORN_TAIL(&taken)->generation,
	.bytes = bytes, .bytes_len = bytes_len, .create = create, .result = result, .error = error};

	std_landing(&frame, chunks_file_write_end_body_0, chunks_file_write_end_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * ChunksFileAppendHook is AppendHook, a Box<dyn FnOnce(&mut Held) + Send +
 * Sync>: a hook that runs inside an append, before its fsync, for the tests
 * of what an append holds up; its call, which consumes its captures, the
 * captures, and their drop glue, size, and alignment from its vtable.
 */
typedef struct ChunksFileAppendHook
{
	void		(*call_once) (void *captures, HostIoHeld *held);
	void	   *captures;
	void		(*drop) (void *captures);
	size_t		size;
	size_t		align;
} ChunksFileAppendHook;

/* chunks_TuplePathBufAppendHook_t is the tuple (PathBuf, AppendHook): a hook and the file it is for. */
typedef struct chunks_TuplePathBufAppendHook_t
{
	StdPathBuf	f0;
	ChunksFileAppendHook f1;
} chunks_TuplePathBufAppendHook_t;

/* chunks_file_AppendHook_drop is AppendHook's drop glue, a Box<dyn FnOnce>'s: its captures dropped, then freed. */
static void
chunks_file_AppendHook_drop(ChunksFileAppendHook *self)
{
	if (self->drop != NULL)
		self->drop(self->captures);
	if (self->size != 0)
		std_global_dealloc(self->captures, self->size, self->align);
}

/* chunks_file_TuplePathBufAppendHook_drop is (PathBuf, AppendHook)'s drop glue, a vector's element's. */
static void
chunks_file_TuplePathBufAppendHook_drop(void *elem)
{
	chunks_TuplePathBufAppendHook_t *self = elem;

	std_vec_free_Checked(&self->f0.bytes, 1, 1);
	chunks_file_AppendHook_drop(&self->f1);
}

/* chunks_file_VecTuplePathBufAppendHook_drop is Vec<(PathBuf, AppendHook)>'s drop glue. */
static void
chunks_file_VecTuplePathBufAppendHook_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(chunks_TuplePathBufAppendHook_t), _Alignof(chunks_TuplePathBufAppendHook_t),
						 chunks_file_TuplePathBufAppendHook_drop);
}

/* taken at Vec<(PathBuf, AppendHook)> and IoMutex<Vec<(PathBuf, AppendHook)>>, which file.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecTuplePathBufAppendHook
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(chunks_TuplePathBufAppendHook_t), _Alignof(chunks_TuplePathBufAppendHook_t))
#define HOST_LOCK_TAKEN_DROP(place) chunks_file_VecTuplePathBufAppendHook_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_FILE_APPEND_HOOKS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecTuplePathBufAppendHook
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(chunks_TuplePathBufAppendHook_t), _Alignof(chunks_TuplePathBufAppendHook_t))
#include "doltgres/host/lock_template.h"

/* chunks_file_IoMutexVecTuplePathBufAppendHook_drop is IoMutex<Vec<(PathBuf, AppendHook)>>' drop glue: its value's. */
static void
chunks_file_IoMutexVecTuplePathBufAppendHook_drop(void *value)
{
	HostLockIoMutexVecTuplePathBufAppendHook *self = value;

	chunks_file_VecTuplePathBufAppendHook_drop(&self->value);
}

/* CHUNKS_FILE_APPEND_HOOKS_TYPE is IoMutex<Vec<(PathBuf, AppendHook)>>' size, Default, and drop. */
static const StdDefaultType CHUNKS_FILE_APPEND_HOOKS_TYPE = {sizeof(HostLockIoMutexVecTuplePathBufAppendHook),
	_Alignof(HostLockIoMutexVecTuplePathBufAppendHook), host_lock_IoMutexVecTuplePathBufAppendHook_Default_default,
chunks_file_IoMutexVecTuplePathBufAppendHook_drop};

/*
 * CHUNKS_FILE_APPEND_HOOKS is APPEND_HOOKS: the hooks the backend's next
 * appends to files run, by the file's path; its BackendLocal header and the
 * process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecTuplePathBufAppendHook process;
}			CHUNKS_FILE_APPEND_HOOKS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

static void chunks_file_run_path_hook(HostLockIoMutex *hooks, HostIoHeld *held, const char *path, size_t path_len);

/*
 * chunks_file_run_append_hook_frame_0_t is run_append_hook's state across its
 * pad: its arguments and the backend's hooks it holds.
 */
typedef struct chunks_file_run_append_hook_frame_0_t
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	HostLocalLocal hooks;
	bool		hooks_live;
} chunks_file_run_append_hook_frame_0_t;

/* chunks_file_run_append_hook_body_0 is run_append_hook's body. */
static void
chunks_file_run_append_hook_body_0(void *frame_)
{
	chunks_file_run_append_hook_frame_0_t *frame = frame_;
	HostIoHeld *held;

	frame->hooks = host_local_BackendLocal_get(&CHUNKS_FILE_APPEND_HOOKS.local, &CHUNKS_FILE_APPEND_HOOKS_TYPE);
	frame->hooks_live = true;
	held = frame->io.ops->read_doors.deref_mut.deref_mut(frame->io.self);
	chunks_file_run_path_hook((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->hooks,
																			   &CHUNKS_FILE_APPEND_HOOKS_TYPE),
							  held, frame->path, frame->path_len);
	frame->hooks_live = false;
	host_local_Local_drop(&frame->hooks, &CHUNKS_FILE_APPEND_HOOKS_TYPE);
}

/* chunks_file_run_append_hook_pad_0 is run_append_hook's pad: the backend's hooks, while it holds them. */
static void
chunks_file_run_append_hook_pad_0(void *frame_)
{
	chunks_file_run_append_hook_frame_0_t *frame = frame_;

	if (frame->hooks_live)
		host_local_Local_drop(&frame->hooks, &CHUNKS_FILE_APPEND_HOOKS_TYPE);
}

/* chunks_file_run_append_hook runs and drops the first hook for an append to the file. */
static void
chunks_file_run_append_hook(HostIoIoDoors io, const char *path, size_t path_len)
{
	chunks_file_run_append_hook_frame_0_t frame = {.io = io, .path = path, .path_len = path_len};

	std_landing(&frame, chunks_file_run_append_hook_body_0, chunks_file_run_append_hook_pad_0);
}

/*
 * chunks_file_run_path_hook_frame_0_t is run_path_hook's state across its
 * pad: its arguments, the guard of the hooks it holds, and the hook it takes,
 * whose box it frees should the hook's call unwind.
 */
typedef struct chunks_file_run_path_hook_frame_0_t
{
	HostLockIoMutex *hooks;
	HostIoHeld *held;
	const char *path;
	size_t		path_len;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	ChunksFileAppendHook hook;
	bool		hook_live;
} chunks_file_run_path_hook_frame_0_t;

/* chunks_file_run_path_hook_body_0 is run_path_hook's body. */
static void
chunks_file_run_path_hook_body_0(void *frame_)
{
	chunks_file_run_path_hook_frame_0_t *frame = frame_;
	StdVec	   *list;
	bool		has_hook = false;

	frame->guard = host_lock_IoMutex_lock(frame->hooks, &CHUNKS_FILE_APPEND_HOOKS_LOCK_TYPE, frame->held);
	frame->guard_live = true;
	list = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	for (size_t i = 0, i_len = list->len; i < i_len; i++)
	{
		const chunks_TuplePathBufAppendHook_t *entry = STD_PTR_AT(list->ptr, i,
																  sizeof(chunks_TuplePathBufAppendHook_t));

		if (std_path_PartialEq_eq(entry->f0.bytes.ptr, entry->f0.bytes.len, frame->path, frame->path_len))
		{
			chunks_TuplePathBufAppendHook_t removed;

			std_vec_remove(list, i, &removed, sizeof(removed), _Alignof(chunks_TuplePathBufAppendHook_t),
						   RUST_FILE, 822, 72);
			frame->hook = removed.f1;
			std_vec_free_Checked(&removed.f0.bytes, 1, 1);
			has_hook = true;
			break;
		}
	}
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (!has_hook)
		return;
	frame->hook_live = true;
	frame->hook.call_once(frame->hook.captures, frame->held);
	frame->hook_live = false;
	if (frame->hook.size != 0)
		std_global_dealloc(frame->hook.captures, frame->hook.size, frame->hook.align);
}

/* chunks_file_run_path_hook_pad_0 is run_path_hook's pad: the guard, and the hook's box its unwound call leaves. */
static void
chunks_file_run_path_hook_pad_0(void *frame_)
{
	chunks_file_run_path_hook_frame_0_t *frame = frame_;

	if (frame->hook_live && frame->hook.size != 0)
		std_global_dealloc(frame->hook.captures, frame->hook.size, frame->hook.align);
	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
}

/* chunks_file_run_path_hook runs and drops the first hook of a list for the file. */
static void
chunks_file_run_path_hook(HostLockIoMutex *hooks, HostIoHeld *held, const char *path, size_t path_len)
{
	chunks_file_run_path_hook_frame_0_t frame = {.hooks = hooks, .held = held, .path = path, .path_len = path_len};

	std_landing(&frame, chunks_file_run_path_hook_body_0, chunks_file_run_path_hook_pad_0);
}
#endif

/* chunks_file_rename renames a file or directory over the target, durably once its directory's sync is. */
bool
chunks_file_rename(HostIoIoDoors io, const char *from, size_t from_len, const char *to, size_t to_len,
				   ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_RENAME};
	HostError	host_error;

	std_path_to_path_buf(to, to_len, &action.u.rename);
	if (!chunks_file_writable(&action, from, from_len, error))
		return false;
	if (!host_rename(io, from, from_len, to, to_len, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/*
 * chunks_file_file_names returns the names of a directory's entries in
 * order, so that what they drive happens in one order.
 */
bool
chunks_file_file_names(HostIoReadDoors io, const char *dir, size_t dir_len, StdVec *result, ChunksErrorError *error)
{
	HostError	host_error;

	if (!host_list_dir(io, dir, dir_len, result, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/* chunks_file_dir_there reports whether a directory is there. */
bool
chunks_file_dir_there(HostIoReadDoors io, const char *dir, size_t dir_len, bool *result, ChunksErrorError *error)
{
	HostError	host_error;
	bool		has;
	StdVec		names;

	if (!host_list_dir_if_there(io, dir, dir_len, &has, &names, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	*result = has;
	if (has)
		std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	return true;
}

/*
 * chunks_file_read_file_frame_0_t is read_file's state across its pad: its
 * arguments, its results, and the file it holds.
 */
typedef struct chunks_file_read_file_frame_0_t
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	StdVec	   *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	bool		ok;
} chunks_file_read_file_frame_0_t;

/* chunks_file_read_file_body_0 is read_file's body. */
static void
chunks_file_read_file_body_0(void *frame_)
{
	chunks_file_read_file_frame_0_t *frame = frame_;
	HostError	host_error;
	uint64		len;
	size_t		len_2;

	if (!host_open_to_read(frame->io, frame->path, frame->path_len, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	frame->file_live = true;
	if (!host_size(frame->io, &frame->file, &len, &host_error))
		*frame->error = chunks_error_Error_FromError_from(&host_error);
	else
	{
		len_2 = std_usize_try_from_u64(len);
		frame->ok = chunks_file_read_at(frame->io, &frame->file, 0, len_2, frame->result, frame->error);
	}
	frame->file_live = false;
	host_File_drop(&frame->file);
}

/* chunks_file_read_file_pad_0 is read_file's pad: the file, while it holds it. */
static void
chunks_file_read_file_pad_0(void *frame_)
{
	chunks_file_read_file_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/* chunks_file_read_file returns a file's bytes. */
bool
chunks_file_read_file(HostIoReadDoors io, const char *path, size_t path_len, StdVec *result, ChunksErrorError *error)
{
	chunks_file_read_file_frame_0_t frame = {.io = io, .path = path, .path_len = path_len, .result = result,
	.error = error};

	std_landing(&frame, chunks_file_read_file_body_0, chunks_file_read_file_pad_0);
	return frame.ok;
}

/*
 * chunks_file_create_dir_all_frame_0_t is create_dir_all's state across its
 * pad: its arguments, its error, and the directories above it holds.
 */
typedef struct chunks_file_create_dir_all_frame_0_t
{
	HostIoIoDoors io;
	const char *dir;
	size_t		dir_len;
	ChunksErrorError *error;
	StdVec		above;
	bool		above_live;
	bool		ok;
} chunks_file_create_dir_all_frame_0_t;

/* chunks_file_create_dir_all_body_0 is create_dir_all's body. */
static void
chunks_file_create_dir_all_body_0(void *frame_)
{
	chunks_file_create_dir_all_frame_0_t *frame = frame_;
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_CREATE_DIR};
	StdStr		next = {frame->dir, frame->dir_len};
	bool		has_next = true;
	bool		first = true;
	const StdStr *made;

	if (!chunks_file_writable(&action, frame->dir, frame->dir_len, frame->error))
		return;
	frame->above = (StdVec) STD_VEC_NEW;
	frame->above_live = true;
	while (has_next)
	{
		StdStr		above = next;

		has_next = std_path_parent(above.ptr, above.len, &next);
		if (above.len == 0)
			break;
		if (first)
			std_vec_collect_first(&frame->above, &above, 0, sizeof(StdStr), _Alignof(StdStr));
		else
			std_vec_extend_one(&frame->above, &above, 0, sizeof(StdStr), _Alignof(StdStr));
		first = false;
	}
	std_slice_reverse(frame->above.ptr, frame->above.len, sizeof(StdStr), _Alignof(StdStr));
	made = frame->above.ptr;
	frame->ok = true;
	for (size_t i = 0, i_len = frame->above.len; i < i_len; i++)
	{
		if (!chunks_file_create_dir(frame->io, made[i].ptr, made[i].len, frame->error))
		{
			frame->ok = false;
			break;
		}
	}
	frame->above_live = false;
	std_vec_free_Checked(&frame->above, sizeof(StdStr), _Alignof(StdStr));
}

/* chunks_file_create_dir_all_pad_0 is create_dir_all's pad: the directories above, while it holds them. */
static void
chunks_file_create_dir_all_pad_0(void *frame_)
{
	chunks_file_create_dir_all_frame_0_t *frame = frame_;

	if (frame->above_live)
		std_vec_free_Checked(&frame->above, sizeof(StdStr), _Alignof(StdStr));
}

/*
 * chunks_file_create_dir_all creates a directory and the directories above
 * it that are missing, each durable in its parent.
 */
bool
chunks_file_create_dir_all(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	chunks_file_create_dir_all_frame_0_t frame = {.io = io, .dir = dir, .dir_len = dir_len, .error = error};

	std_landing(&frame, chunks_file_create_dir_all_body_0, chunks_file_create_dir_all_pad_0);
	return frame.ok;
}

/*
 * chunks_file_create_dir creates a directory in one that exists, unless it
 * exists already, failing when the one it goes in is missing, and makes its
 * entry durable.
 */
bool
chunks_file_create_dir(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_CREATE_DIR};
	HostError	host_error;

	if (!chunks_file_writable(&action, dir, dir_len, error))
		return false;
	if (!host_make_dir(io, dir, dir_len, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return chunks_file_sync_created(io, dir, dir_len, true, error);
}

/* chunks_file_remove_file removes a file, durably at once, as Postgres removes its own. */
bool
chunks_file_remove_file(HostIoIoDoors io, const char *path, size_t path_len, ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_REMOVE};
	HostError	host_error;

	if (!chunks_file_writable(&action, path, path_len, error))
		return false;
	if (!host_remove_durably(io, path, path_len, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/* chunks_file_remove_if_there removes a file, durably at once, and reports whether one was there. */
bool
chunks_file_remove_if_there(HostIoIoDoors io, const char *path, size_t path_len, bool *result,
							ChunksErrorError *error)
{
	ChunksErrorAction action = {.tag = CHUNKS_ERROR_ACTION_REMOVE};
	HostError	host_error;

	if (!chunks_file_writable(&action, path, path_len, error))
		return false;
	if (!host_remove_durably_if_there(io, path, path_len, result, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	return true;
}

/* chunks_file_Appended_drop is Appended's drop glue: its records, and its damage. */
void
chunks_file_Appended_drop(void *self_)
{
	ChunksFileAppended *self = self_;

	std_vec_drop_Checked(&self->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (self->has_damage)
		chunks_file_Damage_drop(&self->damage);
}

/* chunks_file_Damage_drop is Damage's drop glue: the records after it. */
void
chunks_file_Damage_drop(void *self_)
{
	ChunksFileDamage *self = self_;

	std_vec_drop_Checked(&self->after, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
}

/* chunks_file_TornTail_drop is TornTail's drop glue: its path, if its storage holds it, which then holds none. */
void
chunks_file_TornTail_drop(void *self_)
{
	chunks_file_TornTailLayout *self = CHUNKS_FILE_TORN_TAIL(self_);

	if (self->has)
		std_vec_free_Checked(&self->path.bytes, 1, 1);
	self->has = false;
}

/* chunks_file_Scan_drop is Scan's drop glue: its bytes and its places. */
void
chunks_file_Scan_drop(void *self_)
{
	ChunksFileScan *self = self_;

	std_vec_free_Checked(&self->bytes, 1, 1);
	std_vec_free_Checked(&self->records, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
	std_vec_free_Checked(&self->damaged, sizeof(StdRangeUsize), _Alignof(StdRangeUsize));
}

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t
 * is the checker's captures: both flags and the directory's path, moved.
 */
typedef struct test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t
{
	StdArcInner *checking;
	StdArcInner *written;
	StdPathBuf	path;
} test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t;

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_drop is the captures' drop glue. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_drop(void *captures)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t *self = captures;

	std_arc_drop(self->checking, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	std_arc_drop(self->written, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t
 * is the checker's state across its pad: its captures, its capability, its
 * error's place, its answer, and the path it joins.
 */
typedef struct test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t *captures;
	bool		captures_live;
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	StdPathBuf	checked;
	bool		checked_live;
} test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t;

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_body_0 is the checker's body. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_body_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t *frame = frame_;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t *captures = frame->captures;
	ChunksErrorError error;
	bool		refused;

	chunks_file_set_read_only(true);
	std_AtomicBool_store_relaxed(std_arc_Deref_deref(captures->checking, _Alignof(StdAtomicBool)), true);
	while (!std_AtomicBool_load_relaxed(std_arc_Deref_deref(captures->written, _Alignof(StdAtomicBool))))
		(void) host_unique();
	std_path_join(captures->path.bytes.ptr, captures->path.bytes.len, "checked", 7, &frame->checked);
	frame->checked_live = true;
	refused = !chunks_file_write_file(HOST_IO_IO_IO_DOORS(frame->io), frame->checked.bytes.ptr,
									  frame->checked.bytes.len, (const uint8 *) "checked", 7, &error);
	if (refused)
		chunks_error_Error_drop(&error);
	frame->checked_live = false;
	std_vec_free_Checked(&frame->checked.bytes, 1, 1);
	chunks_file_set_read_only(false);
	if (refused)
		frame->ok = true;
	else
	{
		StdString	message;

		std_string_from_Checked(&message, "a read-only backend wrote a file", 32);
		*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
	}
	frame->captures_live = false;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_drop(captures);
}

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_pad_0 is the checker's pad. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_pad_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t *frame = frame_;

	if (frame->checked_live)
		std_vec_free_Checked(&frame->checked.bytes, 1, 1);
	if (frame->captures_live)
		test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_drop(frame->captures);
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0
 * is the checker: it turns its backend's read-only mode on, says so, waits
 * for the writer's write, and succeeds when its own write is refused.
 */
static bool
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0(void *captures,
																								  HostIoIo *io,
																								  HostError *error)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_frame_0_t frame = {
		.captures = captures, .captures_live = true, .io = io, .error = error
	};

	std_landing(&frame, test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_body_0,
				test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_pad_0);
	return frame.ok;
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t
 * is the writer's captures: both flags and the directory's path, moved.
 */
typedef struct test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t
{
	StdArcInner *checking;
	StdArcInner *written;
	StdPathBuf	path;
} test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t;

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_drop is the writer's captures' drop glue. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_drop(void *captures)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t *self = captures;

	std_arc_drop(self->checking, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	std_arc_drop(self->written, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	std_vec_free_Checked(&self->path.bytes, 1, 1);
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t
 * is the writer's state across its pad: its captures, its capability, its
 * error's place, its answer, the path it joins, and its write's error.
 */
typedef struct test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t *captures;
	bool		captures_live;
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	StdPathBuf	written;
	bool		written_live;
	ChunksErrorError wrote;
	bool		wrote_live;
} test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t;

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_body_0 is the writer's body. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_body_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t *frame = frame_;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t *captures = frame->captures;
	bool		ok;

	while (!std_AtomicBool_load_relaxed(std_arc_Deref_deref(captures->checking, _Alignof(StdAtomicBool))))
		(void) host_unique();
	std_path_join(captures->path.bytes.ptr, captures->path.bytes.len, "written", 7, &frame->written);
	frame->written_live = true;
	ok = chunks_file_write_file(HOST_IO_IO_IO_DOORS(frame->io), frame->written.bytes.ptr, frame->written.bytes.len,
								(const uint8 *) "written", 7, &frame->wrote);
	frame->wrote_live = !ok;
	frame->written_live = false;
	std_vec_free_Checked(&frame->written.bytes, 1, 1);
	std_AtomicBool_store_relaxed(std_arc_Deref_deref(captures->written, _Alignof(StdAtomicBool)), true);
	if (ok)
		frame->ok = true;
	else
	{
		StdString	message;

		std_to_string(&frame->wrote, chunks_error_Error_Display_fmt, &message);
		*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
		frame->wrote_live = false;
		chunks_error_Error_drop(&frame->wrote);
	}
	frame->captures_live = false;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_drop(captures);
}

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_pad_0 is the writer's pad. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_pad_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t *frame = frame_;

	if (frame->wrote_live)
		chunks_error_Error_drop(&frame->wrote);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written.bytes, 1, 1);
	if (frame->captures_live)
		test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_drop(frame->captures);
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1
 * is the writer: it waits for the checker's mode, writes, and says so.
 */
static bool
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1(void *captures,
																								  HostIoIo *io,
																								  HostError *error)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_frame_0_t frame = {
		.captures = captures, .captures_live = true, .io = io, .error = error
	};

	std_landing(&frame, test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_body_0,
				test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_pad_0);
	return frame.ok;
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t
 * is the test's state across its pad: its capability, directory, flags, run,
 * and report, and the paths it asks about.
 */
typedef struct test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdArcInner *flags[2];
	size_t		flags_made;
	StdArcInner *checking;
	bool		checking_live;
	StdArcInner *written;
	bool		written_live;
	SimcRun		run;
	bool		run_live;
	StdPathBuf	path;
	bool		path_live;
	StdArcInner *checking_2;
	bool		checking_2_live;
	StdArcInner *written_2;
	bool		written_2_live;
	StdPathBuf	path_2;
	bool		path_2_live;
	SimcReport	report;
	bool		report_live;
	StdPathBuf	written_join;
	bool		written_join_live;
	StdPathBuf	checked_join;
	bool		checked_join_live;
} test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t;

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_body_0 is the test's body. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_body_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t *frame = frame_;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t *captures;
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t *captures_1;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	SimcActor	actor;
	bool		ended;
	bool		exists;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(895, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	frame->flags[0] = std_arc_default(&STD_ATOMIC_BOOL_TYPE);
	frame->flags_made = 1;
	frame->flags[1] = std_arc_default(&STD_ATOMIC_BOOL_TYPE);
	frame->flags_made = 2;
	frame->checking = std_arc_clone(frame->flags[0]);
	frame->checking_live = true;
	frame->written = std_arc_clone(frame->flags[1]);
	frame->written_live = true;
	frame->run = simc_Run_new(51);
	frame->run_live = true;
	std_path_to_path_buf(dir_path, dir_path_len, &frame->path);
	frame->path_live = true;
	captures = std_alloc(sizeof(*captures), _Alignof(test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t));
	frame->checking_live = false;
	captures->checking = frame->checking;
	frame->written_live = false;
	captures->written = frame->written;
	frame->path_live = false;
	captures->path = frame->path;
	actor = (SimcActor)
	{
		test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0, captures,
			test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_drop,
			sizeof(*captures), _Alignof(test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_0_t)
	};
	simc_Run_actor(&frame->run, "checker", 7, actor);
	frame->flags_made = 0;
	frame->checking_2 = frame->flags[0];
	frame->checking_2_live = true;
	frame->written_2 = frame->flags[1];
	frame->written_2_live = true;
	std_path_to_path_buf(dir_path, dir_path_len, &frame->path_2);
	frame->path_2_live = true;
	captures_1 = std_alloc(sizeof(*captures_1), _Alignof(test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t));
	frame->checking_2_live = false;
	captures_1->checking = frame->checking_2;
	frame->written_2_live = false;
	captures_1->written = frame->written_2;
	frame->path_2_live = false;
	captures_1->path = frame->path_2;
	actor = (SimcActor)
	{
		test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1, captures_1,
			test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_drop,
			sizeof(*captures_1), _Alignof(test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_closure_1_t)
	};
	simc_Run_actor(&frame->run, "writer", 6, actor);
	frame->run_live = false;
	frame->report = simc_Run_finish(frame->run);
	frame->report_live = true;
	ended = frame->report.outcome.tag == SIMC_OUTCOME_ENDED;
	if (ended)
	{
		SimcEnding	ok = {.tag = SIMC_ENDING_RETURNED};
		const StdVec *endings = &frame->report.outcome.u.ended;

		for (size_t i = 0, i_len = endings->len; i < i_len; i++)
		{
			const simc_TupleStringEnding_t *ending = STD_PTR_AT(endings->ptr, i, sizeof(simc_TupleStringEnding_t));

			if (!simc_Ending_PartialEq_eq(&ending->f1, &ok))
			{
				ended = false;
				break;
			}
		}
	}
	if (!ended)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 924, 9, &arguments);
	}
	std_path_join(dir_path, dir_path_len, "written", 7, &frame->written_join);
	frame->written_join_live = true;
	exists = std_path_Path_exists(frame->written_join.bytes.ptr, frame->written_join.bytes.len);
	if (exists)
	{
		std_path_join(dir_path, dir_path_len, "checked", 7, &frame->checked_join);
		frame->checked_join_live = true;
		exists = !std_path_Path_exists(frame->checked_join.bytes.ptr, frame->checked_join.bytes.len);
	}
	STD_ASSERT(930, 9, exists, "dir.path().join(\"written\").exists() && !dir.path().join(\"checked\").exists()");
	if (frame->checked_join_live)
	{
		frame->checked_join_live = false;
		std_vec_free_Checked(&frame->checked_join.bytes, 1, 1);
	}
	frame->written_join_live = false;
	std_vec_free_Checked(&frame->written_join.bytes, 1, 1);
	frame->report_live = false;
	simc_Report_drop(&frame->report);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_pad_0 is the test's pad. */
static void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_pad_0(void *frame_)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t *frame = frame_;

	if (frame->checked_join_live)
		std_vec_free_Checked(&frame->checked_join.bytes, 1, 1);
	if (frame->written_join_live)
		std_vec_free_Checked(&frame->written_join.bytes, 1, 1);
	if (frame->report_live)
		simc_Report_drop(&frame->report);
	if (frame->path_2_live)
		std_vec_free_Checked(&frame->path_2.bytes, 1, 1);
	if (frame->written_2_live)
		std_arc_drop(frame->written_2, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	if (frame->checking_2_live)
		std_arc_drop(frame->checking_2, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->run_live)
		simc_Run_drop(&frame->run);
	if (frame->written_live)
		std_arc_drop(frame->written, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	if (frame->checking_live)
		std_arc_drop(frame->checking, sizeof(StdAtomicBool), _Alignof(StdAtomicBool), NULL);
	for (size_t i = 0, i_len = frame->flags_made; i < i_len; i++)
		std_arc_drop(*(StdArcInner **) STD_PTR_AT(frame->flags, i, sizeof(StdArcInner *)), sizeof(StdAtomicBool),
					 _Alignof(StdAtomicBool), NULL);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone:
 * a backend's read-only mode refuses its own writes and leaves another
 * backend's writes alone.
 */
void
test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone(void)
{
	test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_body_0,
				test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone_pad_0);
}

/*
 * test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t
 * is the test's state across its pad: its capability, directory, path, each
 * read's answer of its statement, and the bytes it damages.
 */
typedef struct test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		kept_942;
	bool		kept_942_live;
	StdVec		kept_945;
	bool		kept_945_live;
	StdVec		kept_949;
	bool		kept_949_live;
	StdVec		kept_953;
	bool		kept_953_live;
	StdVec		kept_956;
	bool		kept_956_live;
	StdVec		kept_961;
	bool		kept_961_live;
	StdVec		previous_945;
	bool		previous_945_live;
	StdVec		previous_956;
	bool		previous_956_live;
	ChunksErrorError error;
	bool		error_live;
	StdVec		damaged;
	bool		damaged_live;
	StdPathBuf	previous_963;
	bool		previous_963_live;
	StdPathBuf	damaged_name;
	bool		damaged_name_live;
	StdVec		want_944;
	bool		want_944_live;
	StdVec		want_949;
	bool		want_949_live;
	StdVec		want_953;
	bool		want_953_live;
	StdVec		want_955;
	bool		want_955_live;
	StdVec		want_955_1;
	bool		want_955_1_live;
	StdVec		want_961;
	bool		want_961_live;
} test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t;

/* test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_body_0 is the test's body. */
static void
test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_body_0(void *frame_)
{
	test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	HostIoReadDoors read_io;
	StdIoError	io_error;
	ChunksErrorError error;
	const char *dir_path;
	size_t		dir_path_len;
	bool		kept_942_has;
	bool		kept_942_has_fallback;
	ChunksFileFallback kept_942_fallback;
	bool		kept_945_has;
	bool		kept_945_has_fallback;
	ChunksFileFallback kept_945_fallback;
	bool		kept_949_has;
	bool		kept_949_has_fallback;
	ChunksFileFallback kept_949_fallback;
	bool		kept_953_has;
	bool		kept_953_has_fallback;
	ChunksFileFallback kept_953_fallback;
	bool		kept_956_has;
	bool		kept_956_has_fallback;
	ChunksFileFallback kept_956_fallback;
	bool		kept_961_has;
	bool		kept_961_has_fallback;
	ChunksFileFallback kept_961_fallback;
	bool		previous_945_has;
	bool		previous_956_has;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read_io = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(940, 34, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "manifest", 8, &frame->path);
	frame->path_live = true;
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_942_has, &frame->kept_942,
								   &kept_942_has_fallback, &kept_942_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(942, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_942_live = kept_942_has;
	}
	{
		StdBytes	left_bytes = {frame->kept_942.ptr, frame->kept_942.len};
		StdOptionRef left_fallback = {kept_942_has_fallback, STD_ITEM(&kept_942_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left_bytes, std_bytes_Debug_fmt),
										   STD_ITEM(&left_fallback, std_option_Debug_fmt));
		StdOptionRef left = {kept_942_has, STD_ITEM(&left_tuple, std_tuple_Debug_fmt)};
		ChunksFileFallback right_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_MISSING};
		StdBytes	right_bytes = {(const uint8 *) "", 0};
		StdOptionRef right_fallback = {false, STD_ITEM(&right_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right_bytes, std_bytes_Debug_fmt),
											STD_ITEM(&right_fallback, std_option_Debug_fmt));
		StdOptionRef right = {false, STD_ITEM(&right_tuple, std_tuple_Debug_fmt)};

		STD_ASSERT_EQ(942, 9, !kept_942_has, &left, std_option_Debug_fmt, &right,
					  std_option_Debug_fmt);
	}
	if (frame->kept_942_live)
	{
		frame->kept_942_live = false;
		std_vec_free_Checked(&frame->kept_942, 1, 1);
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "first", 5,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(943, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
	}
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_945_has, &frame->kept_945,
								   &kept_945_has_fallback, &kept_945_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(945, 35, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_945_live = kept_945_has;
	}
	{
		if (!chunks_file_read_previous(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &previous_945_has,
									   &frame->previous_945, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(945, 70, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->previous_945_live = previous_945_has;
	}
	std_slice_to_vec_Checked(&frame->want_944, "first", 5, 1, 1);
	frame->want_944_live = true;
	{
		StdBytes	left_0_bytes = {frame->kept_945.ptr, frame->kept_945.len};
		StdOptionRef left_0_fallback = {kept_945_has_fallback, STD_ITEM(&kept_945_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_0_tuple = STD_TUPLE(2, STD_ITEM(&left_0_bytes, std_bytes_Debug_fmt),
											 STD_ITEM(&left_0_fallback, std_option_Debug_fmt));
		StdOptionRef left_0 = {kept_945_has, STD_ITEM(&left_0_tuple, std_tuple_Debug_fmt)};
		StdBytes	left_1_bytes = {frame->previous_945.ptr, frame->previous_945.len};
		StdOptionRef left_1 = {previous_945_has, STD_ITEM(&left_1_bytes, std_bytes_Debug_fmt)};
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&left_0, std_option_Debug_fmt), STD_ITEM(&left_1, std_option_Debug_fmt));
		ChunksFileFallback right_0_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_MISSING};
		StdBytes	right_0_bytes = {frame->want_944.ptr, frame->want_944.len};
		StdOptionRef right_0_fallback = {false, STD_ITEM(&right_0_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_0_tuple = STD_TUPLE(2, STD_ITEM(&right_0_bytes, std_bytes_Debug_fmt),
											  STD_ITEM(&right_0_fallback, std_option_Debug_fmt));
		StdOptionRef right_0 = {true, STD_ITEM(&right_0_tuple, std_tuple_Debug_fmt)};
		StdBytes	right_1_bytes = {(const uint8 *) "", 0};
		StdOptionRef right_1 = {false, STD_ITEM(&right_1_bytes, std_bytes_Debug_fmt)};
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_option_Debug_fmt), STD_ITEM(&right_1, std_option_Debug_fmt));

		STD_ASSERT_EQ(944, 9, (kept_945_has && frame->kept_945.len == frame->want_944.len && memcmp(frame->kept_945.ptr, frame->want_944.ptr, frame->want_944.len) == 0 && !kept_945_has_fallback) && (!previous_945_has), &left, std_tuple_Debug_fmt,
					  &right, std_tuple_Debug_fmt);
	}
	if (frame->want_944_live)
	{
		frame->want_944_live = false;
		std_vec_free_Checked(&frame->want_944, 1, 1);
	}
	if (frame->previous_945_live)
	{
		frame->previous_945_live = false;
		std_vec_free_Checked(&frame->previous_945, 1, 1);
	}
	if (frame->kept_945_live)
	{
		frame->kept_945_live = false;
		std_vec_free_Checked(&frame->kept_945, 1, 1);
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "second", 6,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(948, 42, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
	}
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_949_has, &frame->kept_949,
								   &kept_949_has_fallback, &kept_949_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(949, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_949_live = kept_949_has;
	}
	std_slice_to_vec_Checked(&frame->want_949, "second", 6, 1, 1);
	frame->want_949_live = true;
	{
		StdBytes	left_bytes = {frame->kept_949.ptr, frame->kept_949.len};
		StdOptionRef left_fallback = {kept_949_has_fallback, STD_ITEM(&kept_949_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left_bytes, std_bytes_Debug_fmt),
										   STD_ITEM(&left_fallback, std_option_Debug_fmt));
		StdOptionRef left = {kept_949_has, STD_ITEM(&left_tuple, std_tuple_Debug_fmt)};
		ChunksFileFallback right_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_MISSING};
		StdBytes	right_bytes = {frame->want_949.ptr, frame->want_949.len};
		StdOptionRef right_fallback = {false, STD_ITEM(&right_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right_bytes, std_bytes_Debug_fmt),
											STD_ITEM(&right_fallback, std_option_Debug_fmt));
		StdOptionRef right = {true, STD_ITEM(&right_tuple, std_tuple_Debug_fmt)};

		STD_ASSERT_EQ(949, 9, kept_949_has && frame->kept_949.len == frame->want_949.len && memcmp(frame->kept_949.ptr, frame->want_949.ptr, frame->want_949.len) == 0 && !kept_949_has_fallback, &left, std_option_Debug_fmt, &right,
					  std_option_Debug_fmt);
	}
	if (frame->want_949_live)
	{
		frame->want_949_live = false;
		std_vec_free_Checked(&frame->want_949, 1, 1);
	}
	if (frame->kept_949_live)
	{
		frame->kept_949_live = false;
		std_vec_free_Checked(&frame->kept_949, 1, 1);
	}
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->damaged, &io_error))
		STD_RESULT_EXPECT_FAILED(950, 48, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	frame->damaged_live = true;
	std_index_check(0, frame->damaged.len, RUST_FILE, 951, 9);
	*(uint8 *) STD_PTR_AT(frame->damaged.ptr, 0, 1) ^= 0xff;
	frame->damaged_live = false;
	if (!std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->damaged.ptr, frame->damaged.len, &io_error))
	{
		std_vec_free_Checked(&frame->damaged, 1, 1);
		STD_RESULT_EXPECT_FAILED(952, 40, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	}
	std_vec_free_Checked(&frame->damaged, 1, 1);
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_953_has, &frame->kept_953,
								   &kept_953_has_fallback, &kept_953_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(953, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_953_live = kept_953_has;
	}
	std_slice_to_vec_Checked(&frame->want_953, "first", 5, 1, 1);
	frame->want_953_live = true;
	{
		StdBytes	left_bytes = {frame->kept_953.ptr, frame->kept_953.len};
		StdOptionRef left_fallback = {kept_953_has_fallback, STD_ITEM(&kept_953_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left_bytes, std_bytes_Debug_fmt),
										   STD_ITEM(&left_fallback, std_option_Debug_fmt));
		StdOptionRef left = {kept_953_has, STD_ITEM(&left_tuple, std_tuple_Debug_fmt)};
		ChunksFileFallback right_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_DAMAGED};
		StdBytes	right_bytes = {frame->want_953.ptr, frame->want_953.len};
		StdOptionRef right_fallback = {true, STD_ITEM(&right_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right_bytes, std_bytes_Debug_fmt),
											STD_ITEM(&right_fallback, std_option_Debug_fmt));
		StdOptionRef right = {true, STD_ITEM(&right_tuple, std_tuple_Debug_fmt)};

		STD_ASSERT_EQ(953, 9, kept_953_has && frame->kept_953.len == frame->want_953.len && memcmp(frame->kept_953.ptr, frame->want_953.ptr, frame->want_953.len) == 0 && kept_953_has_fallback && chunks_file_Fallback_PartialEq_eq(&kept_953_fallback, &right_fallback_value), &left, std_option_Debug_fmt, &right,
					  std_option_Debug_fmt);
	}
	if (frame->want_953_live)
	{
		frame->want_953_live = false;
		std_vec_free_Checked(&frame->want_953, 1, 1);
	}
	if (frame->kept_953_live)
	{
		frame->kept_953_live = false;
		std_vec_free_Checked(&frame->kept_953, 1, 1);
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "third", 5,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(954, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
	}
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_956_has, &frame->kept_956,
								   &kept_956_has_fallback, &kept_956_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(956, 35, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_956_live = kept_956_has;
	}
	{
		if (!chunks_file_read_previous(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &previous_956_has,
									   &frame->previous_956, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(956, 70, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->previous_956_live = previous_956_has;
	}
	std_slice_to_vec_Checked(&frame->want_955, "third", 5, 1, 1);
	frame->want_955_live = true;
	std_slice_to_vec_Checked(&frame->want_955_1, "first", 5, 1, 1);
	frame->want_955_1_live = true;
	{
		StdBytes	left_0_bytes = {frame->kept_956.ptr, frame->kept_956.len};
		StdOptionRef left_0_fallback = {kept_956_has_fallback, STD_ITEM(&kept_956_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_0_tuple = STD_TUPLE(2, STD_ITEM(&left_0_bytes, std_bytes_Debug_fmt),
											 STD_ITEM(&left_0_fallback, std_option_Debug_fmt));
		StdOptionRef left_0 = {kept_956_has, STD_ITEM(&left_0_tuple, std_tuple_Debug_fmt)};
		StdBytes	left_1_bytes = {frame->previous_956.ptr, frame->previous_956.len};
		StdOptionRef left_1 = {previous_956_has, STD_ITEM(&left_1_bytes, std_bytes_Debug_fmt)};
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&left_0, std_option_Debug_fmt), STD_ITEM(&left_1, std_option_Debug_fmt));
		ChunksFileFallback right_0_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_MISSING};
		StdBytes	right_0_bytes = {frame->want_955.ptr, frame->want_955.len};
		StdOptionRef right_0_fallback = {false, STD_ITEM(&right_0_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_0_tuple = STD_TUPLE(2, STD_ITEM(&right_0_bytes, std_bytes_Debug_fmt),
											  STD_ITEM(&right_0_fallback, std_option_Debug_fmt));
		StdOptionRef right_0 = {true, STD_ITEM(&right_0_tuple, std_tuple_Debug_fmt)};
		StdBytes	right_1_bytes = {frame->want_955_1.ptr, frame->want_955_1.len};
		StdOptionRef right_1 = {true, STD_ITEM(&right_1_bytes, std_bytes_Debug_fmt)};
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_option_Debug_fmt), STD_ITEM(&right_1, std_option_Debug_fmt));

		STD_ASSERT_EQ(955, 9, (kept_956_has && frame->kept_956.len == frame->want_955.len && memcmp(frame->kept_956.ptr, frame->want_955.ptr, frame->want_955.len) == 0 && !kept_956_has_fallback) && (previous_956_has && frame->previous_956.len == frame->want_955_1.len && memcmp(frame->previous_956.ptr, frame->want_955_1.ptr, frame->want_955_1.len) == 0), &left, std_tuple_Debug_fmt,
					  &right, std_tuple_Debug_fmt);
	}
	if (frame->want_955_1_live)
	{
		frame->want_955_1_live = false;
		std_vec_free_Checked(&frame->want_955_1, 1, 1);
	}
	if (frame->want_955_live)
	{
		frame->want_955_live = false;
		std_vec_free_Checked(&frame->want_955, 1, 1);
	}
	if (frame->previous_956_live)
	{
		frame->previous_956_live = false;
		std_vec_free_Checked(&frame->previous_956, 1, 1);
	}
	if (frame->kept_956_live)
	{
		frame->kept_956_live = false;
		std_vec_free_Checked(&frame->kept_956, 1, 1);
	}
	std_path_join(dir_path, dir_path_len, "manifest.damaged", 16, &frame->damaged_name);
	frame->damaged_name_live = true;
	STD_ASSERT(959, 9, std_path_Path_exists(frame->damaged_name.bytes.ptr, frame->damaged_name.bytes.len),
			   "dir.path().join(\"manifest.damaged\").exists()");
	frame->damaged_name_live = false;
	std_vec_free_Checked(&frame->damaged_name.bytes, 1, 1);
	if (!std_fs_remove_file(frame->path.bytes.ptr, frame->path.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(960, 37, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	{
		if (!chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &kept_961_has, &frame->kept_961,
								   &kept_961_has_fallback, &kept_961_fallback, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(961, 41, "called `Result::unwrap()` on an `Err` value", &frame->error, chunks_error_Error_Debug_fmt);
		}
		frame->kept_961_live = kept_961_has;
	}
	std_slice_to_vec_Checked(&frame->want_961, "first", 5, 1, 1);
	frame->want_961_live = true;
	{
		StdBytes	left_bytes = {frame->kept_961.ptr, frame->kept_961.len};
		StdOptionRef left_fallback = {kept_961_has_fallback, STD_ITEM(&kept_961_fallback, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef left_tuple = STD_TUPLE(2, STD_ITEM(&left_bytes, std_bytes_Debug_fmt),
										   STD_ITEM(&left_fallback, std_option_Debug_fmt));
		StdOptionRef left = {kept_961_has, STD_ITEM(&left_tuple, std_tuple_Debug_fmt)};
		ChunksFileFallback right_fallback_value = {.tag = CHUNKS_FILE_FALLBACK_MISSING};
		StdBytes	right_bytes = {frame->want_961.ptr, frame->want_961.len};
		StdOptionRef right_fallback = {true, STD_ITEM(&right_fallback_value, chunks_file_Fallback_Debug_fmt)};
		StdTupleRef right_tuple = STD_TUPLE(2, STD_ITEM(&right_bytes, std_bytes_Debug_fmt),
											STD_ITEM(&right_fallback, std_option_Debug_fmt));
		StdOptionRef right = {true, STD_ITEM(&right_tuple, std_tuple_Debug_fmt)};

		STD_ASSERT_EQ(961, 9, kept_961_has && frame->kept_961.len == frame->want_961.len && memcmp(frame->kept_961.ptr, frame->want_961.ptr, frame->want_961.len) == 0 && kept_961_has_fallback && chunks_file_Fallback_PartialEq_eq(&kept_961_fallback, &right_fallback_value), &left, std_option_Debug_fmt, &right,
					  std_option_Debug_fmt);
	}
	if (frame->want_961_live)
	{
		frame->want_961_live = false;
		std_vec_free_Checked(&frame->want_961, 1, 1);
	}
	if (frame->kept_961_live)
	{
		frame->kept_961_live = false;
		std_vec_free_Checked(&frame->kept_961, 1, 1);
	}
	if (!std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "x", 1, &io_error))
		STD_RESULT_EXPECT_FAILED(962, 37, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	frame->previous_963 = chunks_file_previous(frame->path.bytes.ptr, frame->path.bytes.len);
	frame->previous_963_live = true;
	if (!std_fs_write(frame->previous_963.bytes.ptr, frame->previous_963.bytes.len, (const uint8 *) "y", 1, &io_error))
		STD_RESULT_EXPECT_FAILED(963, 47, "called `Result::unwrap()` on an `Err` value", &io_error, std_io_Error_Debug_fmt);
	frame->previous_963_live = false;
	std_vec_free_Checked(&frame->previous_963.bytes, 1, 1);
	{
		bool		has;
		StdVec		bytes;
		bool		has_fallback;
		ChunksFileFallback fallback;
		bool		corrupt;

		if (chunks_file_read_kept(read_io, frame->path.bytes.ptr, frame->path.bytes.len, &has, &bytes, &has_fallback,
								  &fallback, &error))
		{
			if (has)
				std_vec_free_Checked(&bytes, 1, 1);
			corrupt = false;
		}
		else
		{
			corrupt = error.tag == CHUNKS_ERROR_ERROR_CORRUPT;
			chunks_error_Error_drop(&error);
		}
		STD_ASSERT(964, 9, corrupt, "matches!(read_kept(io, &path), Err(Error::Corrupt(_)))");
	}
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_pad_0(void *frame_)
{
	test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->want_961_live)
		std_vec_free_Checked(&frame->want_961, 1, 1);
	if (frame->want_955_1_live)
		std_vec_free_Checked(&frame->want_955_1, 1, 1);
	if (frame->want_955_live)
		std_vec_free_Checked(&frame->want_955, 1, 1);
	if (frame->want_953_live)
		std_vec_free_Checked(&frame->want_953, 1, 1);
	if (frame->want_949_live)
		std_vec_free_Checked(&frame->want_949, 1, 1);
	if (frame->want_944_live)
		std_vec_free_Checked(&frame->want_944, 1, 1);
	if (frame->damaged_name_live)
		std_vec_free_Checked(&frame->damaged_name.bytes, 1, 1);
	if (frame->previous_963_live)
		std_vec_free_Checked(&frame->previous_963.bytes, 1, 1);
	if (frame->damaged_live)
		std_vec_free_Checked(&frame->damaged, 1, 1);
	if (frame->previous_956_live)
		std_vec_free_Checked(&frame->previous_956, 1, 1);
	if (frame->previous_945_live)
		std_vec_free_Checked(&frame->previous_945, 1, 1);
	if (frame->kept_961_live)
		std_vec_free_Checked(&frame->kept_961, 1, 1);
	if (frame->kept_956_live)
		std_vec_free_Checked(&frame->kept_956, 1, 1);
	if (frame->kept_953_live)
		std_vec_free_Checked(&frame->kept_953, 1, 1);
	if (frame->kept_949_live)
		std_vec_free_Checked(&frame->kept_949, 1, 1);
	if (frame->kept_945_live)
		std_vec_free_Checked(&frame->kept_945, 1, 1);
	if (frame->kept_942_live)
		std_vec_free_Checked(&frame->kept_942, 1, 1);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write:
 * a kept file reads as its newest generation; with the newest damaged or
 * missing it reads as its previous one, with why; a write over a damaged
 * newest keeps the good previous generation and moves the damaged file
 * aside; and with both damaged the read fails.
 */
void
test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write(void)
{
	test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_body_0, test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write_pad_0);
}

/* chunks_file_tests_SliceVecU8_Debug_fmt is [Vec<u8>]'s Debug, which a Vec's and an array's call: a list of byte lists. */
static bool
chunks_file_tests_SliceVecU8_Debug_fmt(const StdVec *items, size_t len, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = len; i < i_len; i++)
	{
		const StdVec *item = STD_PTR_AT(items, i, sizeof(StdVec));
		StdBytes	bytes = {item->ptr, item->len};

		std_DebugList_entry(&builder, &bytes, std_bytes_Debug_fmt);
	}
	return std_DebugList_finish(&builder);
}

/* chunks_file_tests_VecVecU8_Debug_fmt is Vec<Vec<u8>>'s Debug, its slice's. */
static bool
chunks_file_tests_VecVecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;

	return chunks_file_tests_SliceVecU8_Debug_fmt(self->ptr, self->len, f);
}

/* chunks_file_tests_Array1VecU8_Debug_fmt is [Vec<u8>; 1]'s Debug, its slice's. */
static bool
chunks_file_tests_Array1VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_file_tests_SliceVecU8_Debug_fmt(value, 1, f);
}

/* chunks_file_tests_Array2VecU8_Debug_fmt is [Vec<u8>; 2]'s Debug, its slice's. */
static bool
chunks_file_tests_Array2VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_file_tests_SliceVecU8_Debug_fmt(value, 2, f);
}

/*
 * chunks_file_tests_SliceVecU8_PartialEqSliceVecU8_eq is [Vec<u8>]'s
 * PartialEq, which a Vec's with an array's calls: the same length, and each
 * pair of byte lists equal.
 */
static bool
chunks_file_tests_SliceVecU8_PartialEqSliceVecU8_eq(const StdVec *self, size_t self_len, const StdVec *other,
													size_t other_len)
{
	if (self_len != other_len)
		return false;
	for (size_t i = 0, i_len = self_len; i < i_len; i++)
	{
		const StdVec *a = STD_PTR_AT(self, i, sizeof(StdVec));
		const StdVec *b = STD_PTR_AT(other, i, sizeof(StdVec));

		if (!(a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0)))
			return false;
	}
	return true;
}

/* chunks_file_tests_VecVecU8_PartialEqArray1VecU8_eq is Vec<Vec<u8>>'s PartialEq<[Vec<u8>; 1]>, its slices'. */
static bool
chunks_file_tests_VecVecU8_PartialEqArray1VecU8_eq(const StdVec *self, const StdVec *other)
{
	return chunks_file_tests_SliceVecU8_PartialEqSliceVecU8_eq(self->ptr, self->len, other, 1);
}

/* chunks_file_tests_VecVecU8_PartialEqArray2VecU8_eq is Vec<Vec<u8>>'s PartialEq<[Vec<u8>; 2]>, its slices'. */
static bool
chunks_file_tests_VecVecU8_PartialEqArray2VecU8_eq(const StdVec *self, const StdVec *other)
{
	return chunks_file_tests_SliceVecU8_PartialEqSliceVecU8_eq(self->ptr, self->len, other, 2);
}

/* chunks_file_tests_Array1VecU8_drop is [Vec<u8>; 1]'s drop glue: each byte list's. */
static void
chunks_file_tests_Array1VecU8_drop(void *value)
{
	std_vec_free_Checked(value, 1, 1);
}

/* chunks_file_tests_Array2VecU8_drop is [Vec<u8>; 2]'s drop glue: each byte list's, in order. */
static void
chunks_file_tests_Array2VecU8_drop(void *value)
{
	std_vec_free_Checked(STD_PTR_AT(value, 0, sizeof(StdVec)), 1, 1);
	std_vec_free_Checked(STD_PTR_AT(value, 1, sizeof(StdVec)), 1, 1);
}

/*
 * test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t
 * is the test's state across its pad: its capability, directory, path,
 * tails, the file it appends to by hand, the records it reads, the
 * expectation it compares them with, and an unwrapped error.
 */
typedef struct test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksFileTornTail tail;
	bool		tail_live;
	StdFsOpenOptions options;
	bool		options_live;
	StdFsFile	file;
	bool		file_live;
	ChunksFileTornTail tail_2;
	bool		tail_2_live;
	StdFsMetadata metadata;
	bool		metadata_live;
	StdVec		records;
	bool		records_live;
	StdVec		want[2];
	size_t		want_made;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t;

/* test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_body_0 is the test's body. */
static void
test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_body_0(void *frame_)
{
	test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t *frame = frame_;
	static const uint8 first_bytes[] = {'f', 'i', 'r', 's', 't'};
	static const uint8 second_bytes[] = {'s', 'e', 'c', 'o', 'n', 'd'};
	static const uint8 torn[] = {'t', 'o', 'r', 'n'};
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	bool		placing = false;
	bool		has;
	ChunksFileTornTail first;
	ChunksFileAppended appended;
	uint64		len;
	uint64		end;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(972, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "records", 7, &frame->path);
	frame->path_live = true;
	if (!chunks_file_write_appended(io, frame->path.bytes.ptr, frame->path.bytes.len, first_bytes,
									sizeof(first_bytes), &placing, &first, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(974, 69, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!chunks_file_append(io, &first, second_bytes, sizeof(second_bytes), &frame->tail, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(975, 49, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->tail_live = true;
	frame->options = std_fs_OpenOptions_new();
	frame->options_live = true;
	if (!std_fs_OpenOptions_open(std_fs_OpenOptions_append(&frame->options, true), frame->path.bytes.ptr,
								 frame->path.bytes.len, &frame->file, &io_error))
		STD_RESULT_EXPECT_FAILED(976, 93, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->file_live = true;
	if (!std_fs_File_Write_write_all(&frame->file, torn, sizeof(torn), &io_error))
		STD_RESULT_EXPECT_FAILED(976, 112, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->file_live = false;
	std_fs_File_drop(&frame->file);
	frame->options_live = false;
	std_fs_OpenOptions_drop(&frame->options);
	frame->tail_live = false;
	if (!chunks_file_cut(io, &frame->tail, &frame->tail_2, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(977, 34, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->tail_2_live = true;
	if (!std_fs_metadata(frame->path.bytes.ptr, frame->path.bytes.len, &frame->metadata, &io_error))
		STD_RESULT_EXPECT_FAILED(978, 45, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->metadata_live = true;
	len = std_fs_Metadata_len(&frame->metadata);
	end = chunks_file_TornTail_end(&frame->tail_2);
	STD_ASSERT_EQ(978, 9, len == end, &len, std_u64_Debug_fmt, &end, std_u64_Debug_fmt);
	frame->metadata_live = false;
	std_fs_Metadata_drop(&frame->metadata);
	if (!chunks_file_read_appended(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->path.bytes.ptr,
								   frame->path.bytes.len, &has, &appended, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(979, 48, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!has)
		STD_OPTION_UNWRAP_FAILED(979, 57);
	frame->records = appended.records;
	frame->records_live = true;
	if (appended.has_damage)
		chunks_file_Damage_drop(&appended.damage);
	std_slice_to_vec_Checked(&frame->want[0], first_bytes, sizeof(first_bytes), 1, 1);
	frame->want_made = 1;
	std_slice_to_vec_Checked(&frame->want[1], second_bytes, sizeof(second_bytes), 1, 1);
	frame->want_made = 2;
	STD_ASSERT_EQ(980, 9, chunks_file_tests_VecVecU8_PartialEqArray2VecU8_eq(&frame->records, frame->want),
				  &frame->records, chunks_file_tests_VecVecU8_Debug_fmt, frame->want,
				  chunks_file_tests_Array2VecU8_Debug_fmt);
	frame->want_made = 0;
	chunks_file_tests_Array2VecU8_drop(frame->want);
	frame->records_live = false;
	std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	frame->tail_2_live = false;
	chunks_file_TornTail_drop(&frame->tail_2);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_pad_0(void *frame_)
{
	test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	for (size_t i = 0, i_len = frame->want_made; i < i_len; i++)
		std_vec_free_Checked(STD_PTR_AT(frame->want, i, sizeof(StdVec)), 1, 1);
	if (frame->records_live)
		std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->metadata_live)
		std_fs_Metadata_drop(&frame->metadata);
	if (frame->tail_2_live)
		chunks_file_TornTail_drop(&frame->tail_2);
	if (frame->file_live)
		std_fs_File_drop(&frame->file);
	if (frame->options_live)
		std_fs_OpenOptions_drop(&frame->options);
	if (frame->tail_live)
		chunks_file_TornTail_drop(&frame->tail);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote:
 * a cut by the tail an append returns keeps what the append wrote and drops
 * what a crash left past it.
 */
void
test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote(void)
{
	test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_body_0,
				test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote_pad_0);
}

/*
 * test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_closure_0
 * is the test's |bytes, at| (at < bytes.len()).then_some(1): a record of a
 * byte at every place within the bytes.
 */
static bool
test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_closure_0(void *captures,
																											const uint8 *bytes,
																											size_t bytes_len,
																											size_t at,
																											size_t *result)
{
	if (!(at < bytes_len))
		return false;
	*result = 1;
	return true;
}

/*
 * test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t
 * is the test's state across its pad: its capability, directory, path, and
 * the scan's answer.
 */
typedef struct test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	bool		scanned_ok;
	ChunksFileScan scanned;
	ChunksErrorError scanned_error;
	bool		scanned_live;
} test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t;

/* test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_body_0 is the test's body. */
static void
test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_body_0(void *frame_)
{
	test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t *frame = frame_;
	static const uint8 ones[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(988, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "records", 7, &frame->path);
	frame->path_live = true;
	if (!std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, ones, sizeof(ones), &io_error))
		STD_RESULT_EXPECT_FAILED(990, 42, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	simc_interrupt_at(3);
	frame->scanned_ok = chunks_file_scan(&frame->test_io.io, frame->path.bytes.ptr, frame->path.bytes.len, 0, 1,
										 test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_closure_0,
										 NULL, NULL, &frame->scanned, &frame->scanned_error);
	frame->scanned_live = true;
	STD_ASSERT_MSG(993, 9, !frame->scanned_ok && frame->scanned_error.tag == CHUNKS_ERROR_ERROR_HOST &&
				   memcmp(host_Error_sqlstate(&frame->scanned_error.u.host), "57014", 5) == 0,
				   "a scan went on past a cancel");
	frame->scanned_live = false;
	if (frame->scanned_ok)
		chunks_file_Scan_drop(&frame->scanned);
	else
		chunks_error_Error_drop(&frame->scanned_error);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_pad_0(void *frame_)
{
	test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t *frame = frame_;

	if (frame->scanned_live)
	{
		if (frame->scanned_ok)
			chunks_file_Scan_drop(&frame->scanned);
		else
			chunks_error_Error_drop(&frame->scanned_error);
	}
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending:
 * a scan of a file of records stops within a record once a cancel is
 * pending.
 */
void
test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending(void)
{
	test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_body_0,
				test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending_pad_0);
}

/* test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_closure_0 is the test's |_, _| None. */
static bool
test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_closure_0(void *captures, const uint8 *bytes,
																				  size_t bytes_len, size_t at,
																				  size_t *result)
{
	return false;
}

/*
 * test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t
 * is the test's state across its pad: its capability, directory, the
 * missing file's path, and the scan.
 */
typedef struct test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	missing;
	bool		missing_live;
	ChunksFileScan scanned;
	bool		scanned_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t;

/* test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_body_0 is the test's body. */
static void
test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_body_0(void *frame_)
{
	test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t *frame = frame_;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	bool		scanned;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1004, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "missing", 7, &frame->missing);
	frame->missing_live = true;
	scanned = chunks_file_scan(&frame->test_io.io, frame->missing.bytes.ptr, frame->missing.bytes.len, 0, 1,
							   test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_closure_0, NULL,
							   NULL, &frame->scanned, &frame->error);
	if (!scanned)
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1005, 80, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->scanned_live = true;
	frame->missing_live = false;
	std_vec_free_Checked(&frame->missing.bytes, 1, 1);
	STD_ASSERT(1006, 9, frame->scanned.bytes.len == 0 && frame->scanned.records.len == 0,
			   "scanned.bytes.is_empty() && scanned.records.is_empty()");
	frame->scanned_live = false;
	chunks_file_Scan_drop(&frame->scanned);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_pad_0(void *frame_)
{
	test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->scanned_live)
		chunks_file_Scan_drop(&frame->scanned);
	if (frame->missing_live)
		std_vec_free_Checked(&frame->missing.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing:
 * a scan of a file that isn't there finds no bytes and no record, and raises
 * nothing.
 */
void
test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing(void)
{
	test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_body_0,
				test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing_pad_0);
}

/*
 * test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t
 * is the test's state across its pad: its capability, and one step's
 * directory, simulation, path, write, and records.
 */
typedef struct test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	SimcSim		sim;
	bool		sim_live;
	StdPathBuf	path;
	bool		path_live;
	bool		wrote_ok;
	ChunksFileTornTail wrote;
	ChunksErrorError wrote_error;
	bool		wrote_live;
	StdVec		records;
	bool		records_live;
	StdVec		want[1];
	size_t		want_made;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t;

/* test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_body_0 is the test's body. */
static void
test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_body_0(void *frame_)
{
	test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t *frame = frame_;
	static const uint8 first_bytes[] = {'f', 'i', 'r', 's', 't'};
	static const uint8 second_bytes[] = {'s', 'e', 'c', 'o', 'n', 'd'};
	static const uint8 third_bytes[] = {'t', 'h', 'i', 'r', 'd'};
	static const bool second_has_values[3] = {false, true, true};
	static const uint64 second_values[3] = {0, 1, 2};
	HostIoIoDoors io;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	for (size_t i = 0, i_len = 3; i < i_len; i++)
	{
		bool		second_has = *(const bool *) STD_PTR_AT(second_has_values, i, sizeof(bool));
		uint64		second_value = *(const uint64 *) STD_PTR_AT(second_values, i, sizeof(uint64));

		for (uint64 next = 1;;)
		{
			uint64		step = next;
			StdIoError	io_error;
			const char *dir_path;
			size_t		dir_path_len;
			bool		placing = false;
			ChunksFileTornTail tail;
			uint64		before;
			bool		has;
			ChunksFileAppended appended;

			next = std_forward_u64_Checked(next, 1);
			STD_ASSERT_MSG(1018, 17, step < 100, "a whole write never finished");
			if (!simc_tempdir(&frame->dir, &io_error))
				STD_RESULT_EXPECT_FAILED(1019, 42, "called `Result::unwrap()` on an `Err` value", &io_error,
										 std_io_Error_Debug_fmt);
			frame->dir_live = true;
			dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
			frame->sim = simc_Sim_start(dir_path, dir_path_len, 17);
			frame->sim_live = true;
			std_path_join(dir_path, dir_path_len, "records", 7, &frame->path);
			frame->path_live = true;
			if (!chunks_file_write_appended(io, frame->path.bytes.ptr, frame->path.bytes.len, first_bytes,
											sizeof(first_bytes), &placing, &tail, &frame->error))
			{
				frame->error_live = true;
				STD_RESULT_EXPECT_FAILED(1022, 65, "called `Result::unwrap()` on an `Err` value", &frame->error,
										 chunks_error_Error_Debug_fmt);
			}
			chunks_file_TornTail_drop(&tail);
			placing = false;
			if (!chunks_file_write_appended(io, frame->path.bytes.ptr, frame->path.bytes.len, second_bytes,
											sizeof(second_bytes), &placing, &tail, &frame->error))
			{
				frame->error_live = true;
				STD_RESULT_EXPECT_FAILED(1023, 66, "called `Result::unwrap()` on an `Err` value", &frame->error,
										 chunks_error_Error_Debug_fmt);
			}
			chunks_file_TornTail_drop(&tail);
			before = simc_Sim_calls(&frame->sim);
			simc_Sim_full_at(&frame->sim, STD_ADD(u64, before, step, 1025, 29));
			if (second_has)
				simc_Sim_full_at(&frame->sim,
								 STD_ADD(u64, STD_ADD(u64, before, step, 1027, 33), second_value, 1027, 33));
			placing = false;
			frame->wrote_ok = chunks_file_write_appended(io, frame->path.bytes.ptr, frame->path.bytes.len, third_bytes,
														 sizeof(third_bytes), &placing, &frame->wrote,
														 &frame->wrote_error);
			frame->wrote_live = true;
			if (!chunks_file_read_appended(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->path.bytes.ptr,
										   frame->path.bytes.len, &has, &appended, &frame->error))
			{
				frame->error_live = true;
				STD_RESULT_EXPECT_FAILED(1030, 56, "called `Result::unwrap()` on an `Err` value", &frame->error,
										 chunks_error_Error_Debug_fmt);
			}
			if (!has)
				STD_OPTION_UNWRAP_FAILED(1030, 65);
			frame->records = appended.records;
			frame->records_live = true;
			if (appended.has_damage)
				chunks_file_Damage_drop(&appended.damage);
			if (!(second_has || std_path_Path_exists(frame->path.bytes.ptr, frame->path.bytes.len)))
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("a write that failed at its step "), STD_FMT_ARG(0),
				STD_FMT_LIT(" left no newest file")};
				StdFmtArg	args[] = {{&step, std_u64_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				std_panic_fmt(RUST_FILE, 1031, 17, &arguments);
			}
			simc_Sim_stop(&frame->sim);
			if (frame->wrote_ok)
			{
				std_slice_to_vec_Checked(&frame->want[0], third_bytes, sizeof(third_bytes), 1, 1);
				frame->want_made = 1;
				STD_ASSERT_EQ(1037, 21, chunks_file_tests_VecVecU8_PartialEqArray1VecU8_eq(&frame->records, frame->want),
							  &frame->records, chunks_file_tests_VecVecU8_Debug_fmt, frame->want,
							  chunks_file_tests_Array1VecU8_Debug_fmt);
				frame->want_made = 0;
				chunks_file_tests_Array1VecU8_drop(frame->want);
			}
			else
			{
				bool		left;

				std_slice_to_vec_Checked(&frame->want[0], second_bytes, sizeof(second_bytes), 1, 1);
				frame->want_made = 1;
				left = chunks_file_tests_VecVecU8_PartialEqArray1VecU8_eq(&frame->records, frame->want);
				frame->want_made = 0;
				chunks_file_tests_Array1VecU8_drop(frame->want);
				if (!left)
				{
					std_slice_to_vec_Checked(&frame->want[0], third_bytes, sizeof(third_bytes), 1, 1);
					frame->want_made = 1;
					left = chunks_file_tests_VecVecU8_PartialEqArray1VecU8_eq(&frame->records, frame->want);
					frame->want_made = 0;
					chunks_file_tests_Array1VecU8_drop(frame->want);
				}
				if (!left)
				{
					StdOptionRef second = {second_has, STD_ITEM(&second_value, std_u64_Debug_fmt)};
					StdFmtPiece pieces[] = {STD_FMT_LIT("a write that failed at its step "), STD_FMT_ARG(0),
						STD_FMT_LIT(" (and "), STD_FMT_ARG(1), STD_FMT_LIT(" after) left "),
					STD_FMT_ARG(2)};
					StdFmtArg	args[] = {{&step, std_u64_Display_fmt}, {&second, std_option_Debug_fmt},
					{&frame->records, chunks_file_tests_VecVecU8_Debug_fmt}};
					StdFmtArguments arguments = {pieces, lengthof(pieces), args};

					std_panic_fmt(RUST_FILE, 1040, 17, &arguments);
				}
			}
			frame->records_live = false;
			std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
			frame->wrote_live = false;
			if (frame->wrote_ok)
				chunks_file_TornTail_drop(&frame->wrote);
			else
				chunks_error_Error_drop(&frame->wrote_error);
			frame->path_live = false;
			std_vec_free_Checked(&frame->path.bytes, 1, 1);
			frame->sim_live = false;
			simc_Sim_drop(&frame->sim);
			frame->dir_live = false;
			simc_TempDir_drop(&frame->dir);
			if (frame->wrote_ok)
				break;
		}
	}
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_pad_0(void *frame_)
{
	test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	for (size_t i = 0, i_len = frame->want_made; i < i_len; i++)
		std_vec_free_Checked(STD_PTR_AT(frame->want, i, sizeof(StdVec)), 1, 1);
	if (frame->records_live)
		std_vec_drop_Checked(&frame->records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->wrote_live)
	{
		if (frame->wrote_ok)
			chunks_file_TornTail_drop(&frame->wrote);
		else
			chunks_error_Error_drop(&frame->wrote_error);
	}
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->sim_live)
		simc_Sim_drop(&frame->sim);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new:
 * a whole write of a list file that fails at any of its steps returns that
 * step's error and leaves a file that reads as the records before it or the
 * new ones, whether the renames it had made were taken back or a second
 * failure cut the taking back short; with no second failure, the newest file
 * is back under its own name.
 */
void
test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new(void)
{
	test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_body_0,
				test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new_pad_0);
}

/*
 * test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t
 * is the test's state across its pad: its capability, directory, path,
 * tail, reads, the expectation it compares them with, bytes, refusal, and an
 * unwrapped error or a refused read's Ok.
 */
typedef struct test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksFileTornTail third;
	bool		third_live;
	StdVec		read;
	bool		read_live;
	StdVec		want[2];
	size_t		want_made;
	StdVec		bytes;
	bool		bytes_live;
	ChunksErrorError refused;
	bool		refused_live;
	ChunksErrorError error;
	bool		error_live;
	bool		ok_has;
	StdVec		ok;
	bool		ok_live;
} test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t;

/* test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_body_0 is the test's body. */
static void
test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_body_0(void *frame_)
{
	test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t *frame = frame_;
	static const uint8 second_bytes[] = {'s', 'e', 'c', 'o', 'n', 'd'};
	static const uint8 third_bytes[] = {'t', 'h', 'i', 'r', 'd'};
	HostIoIoDoors io;
	HostIoReadDoors read_io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	bool		placing = false;
	bool		has;
	ChunksFileTornTail first;
	ChunksFileTornTail second;
	uint64		from;
	size_t		damaged;
	uint64		third_starts;
	bool		empty;
	bool		invariant;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read_io = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1054, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "records", 7, &frame->path);
	frame->path_live = true;
	if (!chunks_file_write_appended(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "first", 5,
									&placing, &first, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1056, 69, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	from = chunks_file_TornTail_end(&first);
	if (!chunks_file_append(io, &first, second_bytes, sizeof(second_bytes), &second, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1058, 51, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!chunks_file_append(io, &second, third_bytes, sizeof(third_bytes), &frame->third, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1059, 50, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->third_live = true;
	if (!chunks_file_appended_between(read_io, frame->path.bytes.ptr, frame->path.bytes.len, from,
									  chunks_file_TornTail_end(&frame->third), &has, &frame->read, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1060, 67, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!has)
		STD_OPTION_UNWRAP_FAILED(1060, 76);
	frame->read_live = true;
	std_slice_to_vec_Checked(&frame->want[0], second_bytes, sizeof(second_bytes), 1, 1);
	frame->want_made = 1;
	std_slice_to_vec_Checked(&frame->want[1], third_bytes, sizeof(third_bytes), 1, 1);
	frame->want_made = 2;
	STD_ASSERT_EQ(1061, 9, chunks_file_tests_VecVecU8_PartialEqArray2VecU8_eq(&frame->read, frame->want),
				  &frame->read, chunks_file_tests_VecVecU8_Debug_fmt, frame->want,
				  chunks_file_tests_Array2VecU8_Debug_fmt);
	frame->want_made = 0;
	chunks_file_tests_Array2VecU8_drop(frame->want);
	if (!chunks_file_appended_between(read_io, frame->path.bytes.ptr, frame->path.bytes.len, from, from, &has,
									  &frame->ok, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1062, 57, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!has)
		STD_OPTION_UNWRAP_FAILED(1062, 66);
	empty = frame->ok.len == 0;
	std_vec_drop_Checked(&frame->ok, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	STD_ASSERT(1062, 9, empty, "appended_between(io, &path, from, from).unwrap().unwrap().is_empty()");
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1063, 46, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	damaged = STD_SUB(usize, frame->bytes.len, 1, 1064, 23);
	std_index_check(damaged, frame->bytes.len, RUST_FILE, 1065, 9);
	*(uint8 *) STD_PTR_AT(frame->bytes.ptr, damaged, 1) ^= 1;
	frame->bytes_live = false;
	if (!std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->bytes.ptr, frame->bytes.len, &io_error))
	{
		std_vec_free_Checked(&frame->bytes, 1, 1);
		STD_RESULT_EXPECT_FAILED(1066, 38, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	}
	std_vec_free_Checked(&frame->bytes, 1, 1);
	if (chunks_file_appended_between(read_io, frame->path.bytes.ptr, frame->path.bytes.len, from,
									 chunks_file_TornTail_end(&frame->third), &frame->ok_has, &frame->ok,
									 &frame->refused))
	{
		StdOptionRef ok = {frame->ok_has, STD_ITEM(&frame->ok, chunks_file_tests_VecVecU8_Debug_fmt)};

		frame->ok_live = frame->ok_has;
		STD_RESULT_EXPECT_FAILED(1067, 70, "called `Result::unwrap_err()` on an `Ok` value", &ok,
								 std_option_Debug_fmt);
	}
	frame->refused_live = true;
	third_starts = STD_SUB(u64, chunks_file_TornTail_end(&frame->third), 4 + 5 + 8, 1068, 28);
	if (!(frame->refused.tag == CHUNKS_ERROR_ERROR_DAMAGED && frame->refused.u.damaged.offset == third_starts))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->refused, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1069, 9, &arguments);
	}
	if (chunks_file_appended_between(read_io, frame->path.bytes.ptr, frame->path.bytes.len,
									 chunks_file_TornTail_end(&frame->third), from, &has, &frame->ok, &frame->error))
	{
		invariant = false;
		if (has)
			std_vec_drop_Checked(&frame->ok, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	}
	else
	{
		invariant = frame->error.tag == CHUNKS_ERROR_ERROR_INVARIANT;
		chunks_error_Error_drop(&frame->error);
	}
	STD_ASSERT(1070, 9, invariant,
			   "matches!(appended_between(io, &path, third.end(), from), Err(Error::Invariant(_)))");
	frame->refused_live = false;
	chunks_error_Error_drop(&frame->refused);
	frame->read_live = false;
	std_vec_drop_Checked(&frame->read, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	frame->third_live = false;
	chunks_file_TornTail_drop(&frame->third);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_pad_0(void *frame_)
{
	test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t *frame = frame_;

	if (frame->ok_live)
		std_vec_drop_Checked(&frame->ok, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->refused_live)
		chunks_error_Error_drop(&frame->refused);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	for (size_t i = 0, i_len = frame->want_made; i < i_len; i++)
		std_vec_free_Checked(STD_PTR_AT(frame->want, i, sizeof(StdVec)), 1, 1);
	if (frame->read_live)
		std_vec_drop_Checked(&frame->read, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	if (frame->third_live)
		chunks_file_TornTail_drop(&frame->third);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records:
 * a read of the records between two places gives each whole record there,
 * and refuses as damage bytes that aren't whole records, at the place they
 * start.
 */
void
test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records(void)
{
	test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_body_0,
				test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records_pad_0);
}

/*
 * test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t
 * is the test's state across its pad: its capability, directory, path, and
 * the bytes it damages.
 */
typedef struct test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		damaged;
	bool		damaged_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t;

/* test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_body_0 is the test's body. */
static void
test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_body_0(void *frame_)
{
	test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1079, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "manifest", 8, &frame->path);
	frame->path_live = true;
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "first", 5,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1081, 50, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT(1081, 9, !same, "!write_kept(io, &path, b\"first\").unwrap()");
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "first", 5,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1082, 49, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT(1082, 9, same, "write_kept(io, &path, b\"first\").unwrap()");
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "second", 6,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1083, 51, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT(1083, 9, !same, "!write_kept(io, &path, b\"second\").unwrap()");
	}
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "second", 6,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1084, 50, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT(1084, 9, same, "write_kept(io, &path, b\"second\").unwrap()");
	}
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->damaged, &io_error))
		STD_RESULT_EXPECT_FAILED(1085, 48, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->damaged_live = true;
	std_index_check(0, frame->damaged.len, RUST_FILE, 1086, 9);
	*(uint8 *) STD_PTR_AT(frame->damaged.ptr, 0, 1) ^= 0xff;
	frame->damaged_live = false;
	if (!std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->damaged.ptr, frame->damaged.len,
					  &io_error))
	{
		std_vec_free_Checked(&frame->damaged, 1, 1);
		STD_RESULT_EXPECT_FAILED(1087, 40, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	}
	std_vec_free_Checked(&frame->damaged, 1, 1);
	{
		bool		same;

		if (!chunks_file_write_kept(io, frame->path.bytes.ptr, frame->path.bytes.len, (const uint8 *) "second", 6,
									&same, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1088, 51, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		STD_ASSERT(1088, 9, !same, "!write_kept(io, &path, b\"second\").unwrap()");
	}
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_pad_0(void *frame_)
{
	test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->damaged_live)
		std_vec_free_Checked(&frame->damaged, 1, 1);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same:
 * a write of a kept file answers that the newest generation it replaced held
 * the same bytes only when it did: not for the first write, a write of other
 * bytes, or a write over a damaged newest.
 */
void
test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same(void)
{
	test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_body_0,
				test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same_pad_0);
}

/* chunks_file_tests_VecString_Debug_fmt is Vec<String>'s Debug, its slice's: a list of quoted strings. */
static bool
chunks_file_tests_VecString_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = self->len; i < i_len; i++)
	{
		const StdString *item = STD_PTR_AT(self->ptr, i, sizeof(StdString));
		StdStr text = {item->ptr, item->len};

		std_DebugList_entry(&builder, &text, std_str_Debug_fmt);
	}
	return std_DebugList_finish(&builder);
}

/* chunks_file_tests_SliceRefStr_Debug_fmt is [&str]'s Debug, which an array's calls: a list of quoted strings. */
static bool
chunks_file_tests_SliceRefStr_Debug_fmt(const StdStr *items, size_t len, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = len; i < i_len; i++)
		std_DebugList_entry(&builder, STD_PTR_AT(items, i, sizeof(StdStr)), std_str_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* chunks_file_tests_Array1RefStr_Debug_fmt is [&str; 1]'s Debug, its slice's. */
static bool
chunks_file_tests_Array1RefStr_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_file_tests_SliceRefStr_Debug_fmt(value, 1, f);
}

/* chunks_file_tests_Array2RefStr_Debug_fmt is [&str; 2]'s Debug, its slice's. */
static bool
chunks_file_tests_Array2RefStr_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_file_tests_SliceRefStr_Debug_fmt(value, 2, f);
}

/*
 * chunks_file_tests_SliceString_PartialEqSliceRefStr_eq is [String]'s
 * PartialEq<[&str]>, which a Vec's with an array's calls: the same length,
 * and each pair of strings equal.
 */
static bool
chunks_file_tests_SliceString_PartialEqSliceRefStr_eq(const StdString *self, size_t self_len, const StdStr *other,
													  size_t other_len)
{
	if (self_len != other_len)
		return false;
	for (size_t i = 0, i_len = self_len; i < i_len; i++)
	{
		const StdString *a = STD_PTR_AT(self, i, sizeof(StdString));
		const StdStr *b = STD_PTR_AT(other, i, sizeof(StdStr));

		if (!(a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0)))
			return false;
	}
	return true;
}

/* chunks_file_tests_VecString_PartialEqArray1RefStr_eq is Vec<String>'s PartialEq<[&str; 1]>, its slices'. */
static bool
chunks_file_tests_VecString_PartialEqArray1RefStr_eq(const StdVec *self, const StdStr *other)
{
	return chunks_file_tests_SliceString_PartialEqSliceRefStr_eq(self->ptr, self->len, other, 1);
}

/* chunks_file_tests_VecString_PartialEqArray2RefStr_eq is Vec<String>'s PartialEq<[&str; 2]>, its slices'. */
static bool
chunks_file_tests_VecString_PartialEqArray2RefStr_eq(const StdVec *self, const StdStr *other)
{
	return chunks_file_tests_SliceString_PartialEqSliceRefStr_eq(self->ptr, self->len, other, 2);
}

/*
 * test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t
 * is the test's state across its pad: its capability, simulations,
 * directory, the made path, and the traces it compares.
 */
typedef struct test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcSim		relative;
	bool		relative_live;
	StdVec		relative_trace;
	bool		relative_trace_live;
	SimcTempDir dir;
	bool		dir_live;
	SimcSim		absolute;
	bool		absolute_live;
	StdPathBuf	made;
	bool		made_live;
	StdVec		absolute_trace;
	bool		absolute_trace_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t;

/* test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_body_0 is the test's body. */
static void
test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_body_0(void *frame_)
{
	test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	frame->relative = simc_Sim_start("", 0, 61);
	frame->relative_live = true;
	if (!chunks_file_sync_created(io, "pg_versioned", 12, true, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1098, 59, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	if (!chunks_file_sync_created(io, "pg_versioned/7", 14, true, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1099, 61, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	simc_Sim_stop(&frame->relative);
	frame->relative_trace = simc_Sim_trace(&frame->relative);
	frame->relative_trace_live = true;
	{
		StdStr		want[2] = {STD_STR("sync dir ."), STD_STR("sync dir pg_versioned")};

		STD_ASSERT_EQ(1101, 9, chunks_file_tests_VecString_PartialEqArray2RefStr_eq(&frame->relative_trace, want),
					  &frame->relative_trace, chunks_file_tests_VecString_Debug_fmt, want,
					  chunks_file_tests_Array2RefStr_Debug_fmt);
	}
	frame->relative_trace_live = false;
	std_vec_drop_Checked(&frame->relative_trace, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1102, 34, "called `Result::unwrap()` on an `Err` value", &io_error,
								 std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	frame->absolute = simc_Sim_start(dir_path, dir_path_len, 62);
	frame->absolute_live = true;
	std_path_join(dir_path, dir_path_len, "made", 4, &frame->made);
	frame->made_live = true;
	if (!chunks_file_sync_created(io, frame->made.bytes.ptr, frame->made.bytes.len, true, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1104, 58, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->made_live = false;
	std_vec_free_Checked(&frame->made.bytes, 1, 1);
	simc_Sim_stop(&frame->absolute);
	frame->absolute_trace = simc_Sim_trace(&frame->absolute);
	frame->absolute_trace_live = true;
	{
		StdStr		want[1] = {STD_STR("sync dir ")};

		STD_ASSERT_EQ(1106, 9, chunks_file_tests_VecString_PartialEqArray1RefStr_eq(&frame->absolute_trace, want),
					  &frame->absolute_trace, chunks_file_tests_VecString_Debug_fmt, want,
					  chunks_file_tests_Array1RefStr_Debug_fmt);
	}
	frame->absolute_trace_live = false;
	std_vec_drop_Checked(&frame->absolute_trace, sizeof(StdString), _Alignof(StdString), std_String_drop);
	frame->absolute_live = false;
	simc_Sim_drop(&frame->absolute);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->relative_live = false;
	simc_Sim_drop(&frame->relative);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_pad_0 is the test's pad. */
static void
test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_pad_0(void *frame_)
{
	test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->absolute_trace_live)
		std_vec_drop_Checked(&frame->absolute_trace, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (frame->made_live)
		std_vec_free_Checked(&frame->made.bytes, 1, 1);
	if (frame->absolute_live)
		simc_Sim_drop(&frame->absolute);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->relative_trace_live)
		std_vec_drop_Checked(&frame->relative_trace, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (frame->relative_live)
		simc_Sim_drop(&frame->relative);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in:
 * a new folder or file of a one-part relative path, as pg_versioned is in the
 * data directory the server runs in, syncs `.`, the directory it is in, as a
 * nested one syncs its parent and an absolute one the folder it is in.
 */
void
test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in(void)
{
	test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_body_0,
				test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in_pad_0);
}

/* chunks_file_tests_oom is the leak cases' allocation error hook: a failed allocation panics, as pg's hook unwinds. */
static void
chunks_file_tests_oom(size_t size, size_t align)
{
	std_panic(__FILE__, __LINE__, 0, "out of memory", sizeof("out of memory") - 1);
}

/* CHUNKS_FILE_TESTS_OOM_HOOK is the leak cases' hook, set while a case fails an allocation. */
static const StdAllocErrorHook CHUNKS_FILE_TESTS_OOM_HOOK = {chunks_file_tests_oom};

/* ChunksFileTestsRead is a leak case's read: of the whole file, or between two places. */
typedef struct ChunksFileTestsRead
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	bool		between;
	uint64		from;
	uint64		to;
} ChunksFileTestsRead;

/* chunks_file_tests_read runs a leak case's read and drops what it answers, its records or its error. */
static void
chunks_file_tests_read(void *captures)
{
	ChunksFileTestsRead *r = captures;
	ChunksErrorError error;
	bool		has;

	if (r->between)
	{
		StdVec		records;

		if (!chunks_file_appended_between(r->io, r->path, r->path_len, r->from, r->to, &has, &records, &error))
			chunks_error_Error_drop(&error);
		else if (has)
			std_vec_drop_Checked(&records, sizeof(StdVec), _Alignof(StdVec), chunks_file_VecU8_drop);
	}
	else
	{
		ChunksFileAppended appended;

		if (!chunks_file_read_appended(r->io, r->path, r->path_len, &has, &appended, &error))
			chunks_error_Error_drop(&error);
		else if (has)
			chunks_file_Appended_drop(&appended);
	}
}

/*
 * chunks_file_tests_leaks_nothing fails each allocation of a read in turn,
 * the first, then the second, until the read runs whole, and checks that
 * every failure, which panics, gives back every byte the read took, as
 * Rust's unwinding drops everything a failed allocation leaves held. The
 * test's output capture is taken away meanwhile, since the panic hook's
 * report would grow its buffer.
 */
static void
chunks_file_tests_leaks_nothing(ChunksFileTestsRead *read)
{
	StdOutputCapture *capture = std_io_set_output_capture(NULL);

	for (uint64 nth = 1;; nth++)
	{
		uint64		before = test_std_alloc_tests_in_use();
		StdCaught	caught;
		bool		ok;

		std_alloc_set_alloc_error_hook(&CHUNKS_FILE_TESTS_OOM_HOOK);
		test_std_alloc_tests_fail_allocation(nth);
		ok = std_catch_unwind(chunks_file_tests_read, read, &caught);
		test_std_alloc_tests_fail_allocation(0);
		std_alloc_set_alloc_error_hook(NULL);
		if (!ok)
			std_Caught_drop(&caught);
		if (test_std_alloc_tests_in_use() != before)
			std_panic(__FILE__, __LINE__, 0, "a failed allocation of the read leaked",
					  sizeof("a failed allocation of the read leaked") - 1);
		if (ok)
			break;
	}
	(void) std_io_set_output_capture(capture);
}

/*
 * test_chunks_file_tests_a_failed_allocation_in_a_read_of_appended_records_leaks_nothing
 * reads a file of three appended records whole, between two places, and
 * between two places that end inside a record, and then a file whose first
 * record is damaged, each with every one of its allocations failing in turn:
 * no failure leaks a record's body, the records, or a damaged error's
 * message.
 */
void
test_chunks_file_tests_a_failed_allocation_in_a_read_of_appended_records_leaks_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	io_error;
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	ChunksErrorError error;
	ChunksFileTornTail first;
	ChunksFileTornTail second;
	ChunksFileTornTail third;
	StdPathBuf	path;
	StdVec		bytes;
	const char *dir_path;
	size_t		dir_path_len;
	bool		placing = false;
	uint64		from;
	uint64		end;
	ChunksFileTestsRead read;

	if (!simc_tempdir(&dir, &io_error))
		std_panic(__FILE__, __LINE__, 0, "no temporary directory", sizeof("no temporary directory") - 1);
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "records", 7, &path);
	if (!chunks_file_write_appended(io, path.bytes.ptr, path.bytes.len, (const uint8 *) "first", 5, &placing, &first,
									&error))
		std_panic(__FILE__, __LINE__, 0, "the records weren't written", sizeof("the records weren't written") - 1);
	from = chunks_file_TornTail_end(&first);
	if (!chunks_file_append(io, &first, (const uint8 *) "second", 6, &second, &error) ||
		!chunks_file_append(io, &second, (const uint8 *) "third", 5, &third, &error))
		std_panic(__FILE__, __LINE__, 0, "the records weren't written", sizeof("the records weren't written") - 1);
	end = chunks_file_TornTail_end(&third);
	read = (ChunksFileTestsRead)
	{
		HOST_IO_IO_READ_DOORS(&test_io.io), path.bytes.ptr, path.bytes.len
	};
	chunks_file_tests_leaks_nothing(&read);
	read.between = true;
	read.from = from;
	read.to = end;
	chunks_file_tests_leaks_nothing(&read);
	read.to = end - 2;
	chunks_file_tests_leaks_nothing(&read);
	if (!std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error))
		std_panic(__FILE__, __LINE__, 0, "the file wasn't read", sizeof("the file wasn't read") - 1);
	((uint8 *) bytes.ptr)[from - 2] ^= 0xFF;
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error))
		std_panic(__FILE__, __LINE__, 0, "the file wasn't written", sizeof("the file wasn't written") - 1);
	std_vec_free_Checked(&bytes, 1, 1);
	read.between = false;
	chunks_file_tests_leaks_nothing(&read);
	chunks_file_TornTail_drop(&third);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * ChunksFileTestsReuse is what a reuse of a taken tail needs: the doors, the
 * tail's storage, and whether the reuse appends or reads its end.
 */
typedef struct ChunksFileTestsReuse
{
	HostIoIoDoors io;
	ChunksFileTornTail *tail;
	bool		append;
} ChunksFileTestsReuse;

/* chunks_file_tests_reuse uses a tail's storage again, which panics once a door took the tail. */
static void
chunks_file_tests_reuse(void *captures)
{
	ChunksFileTestsReuse *reuse = captures;
	ChunksFileTornTail next;
	ChunksErrorError error;

	if (reuse->append)
	{
		if (chunks_file_append(reuse->io, reuse->tail, (const uint8 *) "again", 5, &next, &error))
			chunks_file_TornTail_drop(&next);
		else
			chunks_error_Error_drop(&error);
	}
	else
		(void) chunks_file_TornTail_end(reuse->tail);
}

/* chunks_file_tests_panics_as_a_moved_use reports whether a reuse panics as an unwrap of None does. */
static bool
chunks_file_tests_panics_as_a_moved_use(ChunksFileTestsReuse *reuse)
{
	static const char unwrapped[] = "called `Option::unwrap()` on a `None` value";
	StdCaught	caught;
	const char *message;
	size_t		message_len;
	bool		panicked;

	if (std_catch_unwind(chunks_file_tests_reuse, reuse, &caught))
		return false;
	panicked = std_Caught_downcast_str(&caught, &message, &message_len) &&
		message_len == sizeof(unwrapped) - 1 && memcmp(message, unwrapped, message_len) == 0;
	std_Caught_drop_Checked(&caught);
	return panicked;
}

/*
 * test_chunks_file_tests_a_torn_tail_a_door_took_panics_at_its_next_use_and_a_failed_door_still_takes_it:
 * the C's move-out of a torn tail, which the Rust's moves refuse at compile
 * time (chunks/fail/torn_tail_reused): a tail an append took panics at its
 * next read and at its next append, an append that fails still takes its
 * tail, and a taken tail's drop frees nothing twice.
 */
void
test_chunks_file_tests_a_torn_tail_a_door_took_panics_at_its_next_use_and_a_failed_door_still_takes_it(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	io_error;
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	ChunksErrorError error;
	ChunksFileTornTail first;
	ChunksFileTornTail second;
	ChunksFileTornTail third;
	ChunksFileTestsReuse reuse = {.io = io};
	StdPathBuf	path;
	const char *dir_path;
	size_t		dir_path_len;
	bool		placing = false;
	SimcSim		sim;

	if (!simc_tempdir(&dir, &io_error))
		std_panic(__FILE__, __LINE__, 0, "no temporary directory", sizeof("no temporary directory") - 1);
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "records", 7, &path);
	if (!chunks_file_write_appended(io, path.bytes.ptr, path.bytes.len, (const uint8 *) "first", 5, &placing, &first,
									&error) ||
		!chunks_file_append(io, &first, (const uint8 *) "second", 6, &second, &error))
		std_panic(__FILE__, __LINE__, 0, "the records weren't written", sizeof("the records weren't written") - 1);
	reuse.tail = &first;
	STD_ASSERT(__LINE__, 5, chunks_file_tests_panics_as_a_moved_use(&reuse), "a read of a taken tail panics");
	reuse.append = true;
	STD_ASSERT(__LINE__, 5, chunks_file_tests_panics_as_a_moved_use(&reuse), "an append of a taken tail panics");
	chunks_file_TornTail_drop(&first);
	sim = simc_Sim_start(dir_path, dir_path_len, 5);
	simc_Sim_full_at(&sim, simc_Sim_calls(&sim) + 1);
	STD_ASSERT(__LINE__, 5, !chunks_file_append(io, &second, (const uint8 *) "third", 5, &third, &error),
			   "the append finds the disk full");
	chunks_error_Error_drop(&error);
	simc_Sim_stop(&sim);
	simc_Sim_drop(&sim);
	reuse.tail = &second;
	reuse.append = false;
	STD_ASSERT(__LINE__, 5, chunks_file_tests_panics_as_a_moved_use(&reuse),
			   "the failed append took its tail all the same");
	chunks_file_TornTail_drop(&second);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}
#endif
