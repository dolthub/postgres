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
 * The server's host (design item 14): host's seams over Postgres, through
 * pg's wrappers. Every entry installs it before a storage crate runs, and
 * hands its capability down as host's. Files go through Postgres' fd.c, so
 * they share its descriptor cache and are as durable as the WAL, and the
 * fork's records go into Postgres' WAL under its resource manager.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "utils/snapshot.h"

#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/pg/zstd.h"
#include "doltgres/pghost/delta.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres/shmem/collection.h"
#include "doltgres/shmem/delta.h"
#include "doltgres/shmem/keys.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/time.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_PGHOST
#include "doltgres/std/test.h"
#endif
#include "pg_calls.h"

#define RUST_FILE "pghost" STD_SEP "src" STD_SEP "lib.rs"

/* PGHOST_POSTGRES_EPOCH is Postgres' epoch, 2000-01-01, in seconds after the Unix epoch. */
#define PGHOST_POSTGRES_EPOCH ((uint64) 946684800)

/* PGHOST_FILE is the kind path_stat gives a file. */
#define PGHOST_FILE ((uint64) 1)

/* PGHOST_DIRECTORY is the kind path_stat gives a directory. */
#define PGHOST_DIRECTORY ((uint64) 2)

/* PghostPgHost is the server's host. */
typedef struct PghostPgHost
{
	char		unused;
} PghostPgHost;

/* PGHOST_HOST is the process's server host. */
static PghostPgHost PGHOST_HOST;

/* PGHOST_NEXT is the next of the process's unique numbers. */
static StdAtomicU64 PGHOST_NEXT = STD_ATOMIC_U64_INIT(0);

/*
 * PGHOST_IN_WINDOW is whether the backend holds a checkpoint window open,
 * inside whose critical section a pause runs the injection point the
 * window's open loaded, from the backend's cache.
 */
static StdAtomicBool PGHOST_IN_WINDOW = STD_ATOMIC_BOOL_INIT(false);

StaticAssertDecl(HOST_CACHE_KEY == SHMEM_CACHE_KEY, "the node cache's key is one length in host and in shmem");

StaticAssertDecl((size_t) HOST_STRIPES == SHMEM_KEYS_STRIPES,
				 "the key lock's stripes host names are the ones the shared region holds");

/* PGHOST_READ is the index of the file door's wait event for reads in shmem's file waits. */
#define PGHOST_READ ((size_t) 0)

/* PGHOST_WRITE is the index of the file door's wait event for writes in shmem's file waits. */
#define PGHOST_WRITE ((size_t) 1)

/* PGHOST_SYNC is the index of the file door's wait event for syncs in shmem's file waits. */
#define PGHOST_SYNC ((size_t) 2)

/* PGHOST_INVALID_COMMAND is Postgres' InvalidCommandId, a command id heap's record of a row's command ids leaves unset. */
#define PGHOST_INVALID_COMMAND ((uint32) PG_UINT32_MAX)

static void pghost_names(const uint8 *listing, size_t listing_len, StdVec *result);
static bool pghost_c_path(const char *path, size_t path_len, StdCString *result, HostError *error);
static bool pghost_canonical(const char *path, size_t path_len, StdCString *result);
static bool pghost_signed(uint64 value, int64 *result, HostError *error);
static StdSystemTime pghost_since_postgres_epoch(int64 micros);
static const PgZstdDecompressDictionary *pghost_prepared_DecompressDictionary(StdAny dictionary);
static const PgZstdCompressDictionary *pghost_prepared_CompressDictionary(StdAny dictionary);
static bool pghost_cache_of(HostCachePlacement placement, ShmemCacheCache *result);

/* pghost_PgHost_ReadHost_open_to_read_frame_0_t is open_to_read's state across its pad: its arguments, its path, and its result. */
typedef struct pghost_PgHost_ReadHost_open_to_read_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	int32	   *result;
	HostError  *error;
	bool		done;
	StdCString	c_path;
	bool		c_path_live;
} pghost_PgHost_ReadHost_open_to_read_frame_0_t;

/* pghost_PgHost_ReadHost_open_to_read_body_0 is open_to_read's body. */
static void
pghost_PgHost_ReadHost_open_to_read_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_open_to_read_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->c_path, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->c_path_live = true;
	frame->done = pg_calls_file_open(raise, std_CString_as_ptr(&frame->c_path), 0, frame->result, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->c_path_live = false;
	std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_open_to_read_pad_0 is open_to_read's pad: its path while it is live. */
static void
pghost_PgHost_ReadHost_open_to_read_pad_0(void *frame_)
{
	pghost_PgHost_ReadHost_open_to_read_frame_0_t *frame = frame_;

	if (frame->c_path_live)
		std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_open_to_read opens a file to read through fd.c. */
static bool
pghost_PgHost_ReadHost_open_to_read(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
									int32 *result, HostError *error)
{
	pghost_PgHost_ReadHost_open_to_read_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len,
	.result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_open_to_read_body_0, pghost_PgHost_ReadHost_open_to_read_pad_0);
	return frame.done;
}

/* pghost_PgHost_ReadHost_open_if_there_frame_0_t is open_if_there's state across its pad: its arguments, its path, and its result. */
typedef struct pghost_PgHost_ReadHost_open_if_there_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	int32	   *result;
	HostError  *error;
	bool		done;
	StdCString	c_path;
	bool		c_path_live;
} pghost_PgHost_ReadHost_open_if_there_frame_0_t;

/* pghost_PgHost_ReadHost_open_if_there_body_0 is open_if_there's body. */
static void
pghost_PgHost_ReadHost_open_if_there_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_open_if_there_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;
	int32		file;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->c_path, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->c_path_live = true;
	frame->done = pg_calls_file_open_if_there(raise, std_CString_as_ptr(&frame->c_path), &file, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->c_path_live = false;
	std_CString_drop(&frame->c_path);
	if (!frame->done)
		return;
	*frame->has_result = file >= 0;
	if (*frame->has_result)
		*frame->result = file;
}

/* pghost_PgHost_ReadHost_open_if_there_pad_0 is open_if_there's pad: its path while it is live. */
static void
pghost_PgHost_ReadHost_open_if_there_pad_0(void *frame_)
{
	pghost_PgHost_ReadHost_open_if_there_frame_0_t *frame = frame_;

	if (frame->c_path_live)
		std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_open_if_there opens a file to read, or answers nothing for a file that isn't there. */
static bool
pghost_PgHost_ReadHost_open_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
									 bool *has_result, int32 *result, HostError *error)
{
	pghost_PgHost_ReadHost_open_if_there_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len,
	.has_result = has_result, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_open_if_there_body_0, pghost_PgHost_ReadHost_open_if_there_pad_0);
	return frame.done;
}

/* pghost_PgHost_ReadHost_read_at reads a file's bytes at an offset into the buffer, all of them. */
static bool
pghost_PgHost_ReadHost_read_at(void *self, const HostMayRaise *raise, int32 file, uint64 offset, uint8 *buffer,
							   size_t buffer_len, HostError *error)
{
	uint32		waits[3];
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	int64		signed_;
	PgPgError	error_;

	(void) self;
	if (!pghost_signed(offset, &signed_, error))
		return false;
	shmem_file_waits(waits);
	if (!pg_calls_file_read(pg_raise, file, buffer, buffer_len, signed_, waits[PGHOST_READ], &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_ReadHost_size returns a file's size. */
static bool
pghost_PgHost_ReadHost_size(void *self, const HostMayRaise *raise, int32 file, uint64 *result, HostError *error)
{
	int64		size;
	PgPgError	error_;

	(void) self;
	if (!pg_calls_file_size(pghost_pg_raise(raise), file, &size, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*result = std_i64_unsigned_abs(size);
	return true;
}

/* pghost_PgHost_ReadHost_prefetch asks the system to read a file's range ahead, when both fit Postgres' signed range. */
static void
pghost_PgHost_ReadHost_prefetch(void *self, int32 file, uint64 offset, uint64 len)
{
	uint32		waits[3];
	int64		offset_;
	int64		len_;
	StdTryFromIntError tried;
	bool		has_offset = std_i64_try_from_u64(offset, &offset_, &tried);
	bool		has_len = std_i64_try_from_u64(len, &len_, &tried);

	(void) self;
	if (has_offset && has_len)
	{
		shmem_file_waits(waits);
		pg_calls_file_prefetch(file, offset_, len_, waits[PGHOST_READ]);
	}
}

/* pghost_PgHost_ReadHost_close closes a file. */
static void
pghost_PgHost_ReadHost_close(void *self, int32 file)
{
	(void) self;
	pg_calls_file_close(file);
}

/* pghost_PgHost_ReadHost_tablespace_root_frame_0_t is tablespace_root's state across its pad: its text while it is live. */
typedef struct pghost_PgHost_ReadHost_tablespace_root_frame_0_t
{
	uint32		tablespace;
	StdPathBuf *result;
	StdCowStr	lossy;
	bool		lossy_live;
} pghost_PgHost_ReadHost_tablespace_root_frame_0_t;

/* pghost_PgHost_ReadHost_tablespace_root_body_0 is tablespace_root's body. */
static void
pghost_PgHost_ReadHost_tablespace_root_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_tablespace_root_frame_0_t *frame = frame_;
	uint8		path[64] = {0};
	size_t		len;
	size_t		end;
	StdString	owned;

	if (frame->tablespace == HOST_PG_DEFAULT)
	{
		std_PathBuf_new(frame->result);
		return;
	}
	len = (size_t) std_i32_unsigned_abs(pg_calls_tablespace_version_dir(frame->tablespace, path, sizeof(path)));
	end = std_min_usize(len, sizeof(path));
	std_range_to_check(end, sizeof(path), RUST_FILE, 115, 52);
	std_string_from_utf8_lossy(path, end, &frame->lossy);
	frame->lossy_live = true;
	frame->lossy_live = false;
	std_CowStr_into_owned(&frame->lossy, &owned);
	std_PathBuf_from_String(&owned, frame->result);
}

/* pghost_PgHost_ReadHost_tablespace_root_pad_0 is tablespace_root's pad: the lossy text while it is live. */
static void
pghost_PgHost_ReadHost_tablespace_root_pad_0(void *frame_)
{
	pghost_PgHost_ReadHost_tablespace_root_frame_0_t *frame = frame_;

	if (frame->lossy_live)
		std_CowStr_drop(&frame->lossy);
}

/* pghost_PgHost_ReadHost_tablespace_root returns a tablespace's version directory, the empty path for the default one. */
static StdPathBuf
pghost_PgHost_ReadHost_tablespace_root(void *self, uint32 tablespace)
{
	StdPathBuf	result;
	pghost_PgHost_ReadHost_tablespace_root_frame_0_t frame = {.tablespace = tablespace, .result = &result};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_tablespace_root_body_0, pghost_PgHost_ReadHost_tablespace_root_pad_0);
	return result;
}

/* pghost_PgHost_ReadHost_list_dir_frame_0_t is list_dir's (and list_dir_if_there's) state across its pad: its arguments, its path, and its listing. */
typedef struct pghost_PgHost_ReadHost_list_dir_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool		if_there;
	bool	   *has_result;
	StdVec	   *result;
	HostError  *error;
	bool		done;
	StdCString	c_path;
	bool		c_path_live;
	StdVec		listing;
	bool		listing_live;
} pghost_PgHost_ReadHost_list_dir_frame_0_t;

/* pghost_PgHost_ReadHost_list_dir_body_0 is list_dir's body. */
static void
pghost_PgHost_ReadHost_list_dir_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_list_dir_frame_0_t *frame = frame_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->c_path, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->c_path_live = true;
	frame->listing = (StdVec) STD_VEC_NEW;
	frame->listing_live = true;
	for (;;)
	{
		size_t		used;
		PgPgError	error_;
		uint8		zero = 0;

		if (!pg_calls_list_dir(pghost_pg_raise(frame->raise), std_CString_as_ptr(&frame->c_path), frame->listing.ptr,
							   frame->listing.len, &used, &error_))
		{
			*frame->error = pghost_held(&error_);
			frame->done = false;
			break;
		}
		if (used <= frame->listing.len)
		{
			std_vec_truncate(&frame->listing, used, 1, NULL);
			pghost_names(frame->listing.ptr, frame->listing.len, frame->result);
			frame->done = true;
			break;
		}
		std_vec_resize(&frame->listing, used, &zero, 1, 1);
	}
	frame->listing_live = false;
	std_vec_free_Checked(&frame->listing, 1, 1);
	frame->c_path_live = false;
	std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_list_dir_pad_0 is list_dir's pad: its listing and its path, while each is live. */
static void
pghost_PgHost_ReadHost_list_dir_pad_0(void *frame_)
{
	pghost_PgHost_ReadHost_list_dir_frame_0_t *frame = frame_;

	if (frame->listing_live)
		std_vec_free_Checked(&frame->listing, 1, 1);
	if (frame->c_path_live)
		std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_list_dir returns a directory's names, in byte order. */
static bool
pghost_PgHost_ReadHost_list_dir(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
								StdVec *result, HostError *error)
{
	pghost_PgHost_ReadHost_list_dir_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len,
	.result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_list_dir_body_0, pghost_PgHost_ReadHost_list_dir_pad_0);
	return frame.done;
}

/* pghost_PgHost_ReadHost_list_dir_if_there_body_0 is list_dir_if_there's body. */
static void
pghost_PgHost_ReadHost_list_dir_if_there_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_list_dir_frame_0_t *frame = frame_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->c_path, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->c_path_live = true;
	frame->listing = (StdVec) STD_VEC_NEW;
	frame->listing_live = true;
	for (;;)
	{
		size_t		used;
		PgPgError	error_;
		uint8		zero = 0;

		if (!pg_calls_list_dir_if_there(pghost_pg_raise(frame->raise), std_CString_as_ptr(&frame->c_path),
										frame->listing.ptr, frame->listing.len, &used, &error_))
		{
			*frame->error = pghost_held(&error_);
			frame->done = false;
			break;
		}
		if (used == SIZE_MAX)
		{
			*frame->has_result = false;
			frame->done = true;
			break;
		}
		if (used <= frame->listing.len)
		{
			std_vec_truncate(&frame->listing, used, 1, NULL);
			pghost_names(frame->listing.ptr, frame->listing.len, frame->result);
			*frame->has_result = true;
			frame->done = true;
			break;
		}
		std_vec_resize(&frame->listing, used, &zero, 1, 1);
	}
	frame->listing_live = false;
	std_vec_free_Checked(&frame->listing, 1, 1);
	frame->c_path_live = false;
	std_CString_drop(&frame->c_path);
}

/* pghost_PgHost_ReadHost_list_dir_if_there returns a directory's names, in byte order, or nothing for one that isn't there. */
static bool
pghost_PgHost_ReadHost_list_dir_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
										 bool *has_result, StdVec *result, HostError *error)
{
	pghost_PgHost_ReadHost_list_dir_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len,
	.has_result = has_result, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_list_dir_if_there_body_0, pghost_PgHost_ReadHost_list_dir_pad_0);
	return frame.done;
}

/* pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t is prepare_decompressing's state across its pad: the dictionary before its box holds it. */
typedef struct pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	HostDecompressDictionary *result;
	HostError  *error;
	bool		done;
	PgZstdDecompressDictionary prepared;
	bool		prepared_live;
} pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t;

/* pghost_PgHost_ReadHost_prepare_decompressing_body_0 is prepare_decompressing's body. */
static void
pghost_PgHost_ReadHost_prepare_decompressing_body_0(void *frame_)
{
	pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t *frame = frame_;
	PgZstdDecompressDictionary *boxed;
	StdAny		any;

	if (!pg_zstd_DecompressDictionary_new(frame->bytes, frame->bytes_len, &frame->prepared))
	{
		*frame->error = host_Error_out_of_memory();
		frame->done = false;
		return;
	}
	frame->prepared_live = true;
	boxed = std_box_new_uninit_Checked(sizeof(PgZstdDecompressDictionary), _Alignof(PgZstdDecompressDictionary));
	frame->prepared_live = false;
	*boxed = frame->prepared;
	any = (StdAny)
	{
		&PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID, boxed
	};
	*frame->result = host_DecompressDictionary_new(&any);
	frame->done = true;
}

/* pghost_PgHost_ReadHost_prepare_decompressing_pad_0 is prepare_decompressing's pad: the dictionary while it is live. */
static void
pghost_PgHost_ReadHost_prepare_decompressing_pad_0(void *frame_)
{
	pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t *frame = frame_;

	if (frame->prepared_live)
		pg_zstd_DecompressDictionary_drop(&frame->prepared);
}

/* pghost_PgHost_ReadHost_prepare_decompressing prepares a dictionary for decompressing, in pg's form. */
static bool
pghost_PgHost_ReadHost_prepare_decompressing(void *self, const uint8 *bytes, size_t bytes_len,
											 HostDecompressDictionary *result, HostError *error)
{
	pghost_PgHost_ReadHost_prepare_decompressing_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len,
	.result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_ReadHost_prepare_decompressing_body_0,
				pghost_PgHost_ReadHost_prepare_decompressing_pad_0);
	return frame.done;
}

/* pghost_PgHost_ReadHost_frame_size returns the length a frame records of its bytes. */
static bool
pghost_PgHost_ReadHost_frame_size(void *self, const uint8 *frame, size_t frame_len, uint64 *result)
{
	(void) self;
	return pg_zstd_frame_size(frame, frame_len, result);
}

/* pghost_PgHost_ReadHost_decompress returns a frame's bytes, nothing for a damaged frame, or an error when libzstd can't allocate. */
static bool
pghost_PgHost_ReadHost_decompress(void *self, const uint8 *frame, size_t frame_len, size_t size,
								  const HostDecompressDictionary *dictionary, bool *has_result, StdVec *result,
								  HostError *error)
{
	const PgZstdDecompressDictionary *prepared = NULL;
	PgZstdFailed failed;

	(void) self;
	if (dictionary != NULL)
		prepared = pghost_prepared_DecompressDictionary(host_DecompressDictionary_prepared(dictionary));
	if (pg_zstd_decompress(frame, frame_len, size, prepared, result, &failed))
	{
		*has_result = true;
		return true;
	}
	switch (failed)
	{
		case PG_ZSTD_FAILED_DAMAGED:
			*has_result = false;
			return true;
		case PG_ZSTD_FAILED_OUT_OF_MEMORY:
			*error = host_Error_out_of_memory();
			return false;
	}
	pg_unreachable();
}

/* pghost_PgHost_Host_now returns the time of day, Postgres' current timestamp. */
static StdSystemTime
pghost_PgHost_Host_now(void *self)
{
	(void) self;
	return pghost_since_postgres_epoch(pg_calls_current_timestamp());
}

/* pghost_PgHost_Host_instant returns the monotonic clock's reading. */
static StdInstant
pghost_PgHost_Host_instant(void *self)
{
	(void) self;
	return std_Instant_now();
}

/* pghost_PgHost_Host_work_mem returns work_mem in bytes, read anew at each call. */
static uint64
pghost_PgHost_Host_work_mem(void *self)
{
	(void) self;
	return STD_MUL(u64, (uint64) std_i32_unsigned_abs(pg_calls_work_mem()), 1024, 1, 1);
}

/* pghost_PgHost_Host_process_id returns the backend's process id. */
static uint32
pghost_PgHost_Host_process_id(void *self)
{
	(void) self;
	return std_i32_unsigned_abs(pg_calls_my_proc_pid());
}

/* pghost_PgHost_Host_session_database returns the session's database and its tablespace, or nothing for a process with no database. */
static bool
pghost_PgHost_Host_session_database(void *self, uint32 *out_0, uint32 *out_1)
{
	uint32		database = pg_calls_my_database_id();

	(void) self;
	if (database == 0)
		return false;
	*out_0 = database;
	*out_1 = pg_calls_my_database_tablespace();
	return true;
}

/* pghost_PgHost_Host_exiting reports whether the process is exiting. */
static bool
pghost_PgHost_Host_exiting(void *self)
{
	(void) self;
	return pg_calls_proc_exit_inprogress();
}

/* pghost_PgHost_Host_unique returns the next of the process's unique numbers. */
static uint64
pghost_PgHost_Host_unique(void *self)
{
	(void) self;
	return std_AtomicU64_fetch_add_relaxed(&PGHOST_NEXT, 1);
}

/* pghost_PgHost_Host_hold_interrupts runs Postgres' HOLD_INTERRUPTS. */
static void
pghost_PgHost_Host_hold_interrupts(void *self)
{
	(void) self;
	pg_calls_hold_interrupts();
}

/* pghost_PgHost_Host_resume_interrupts runs Postgres' RESUME_INTERRUPTS. */
static void
pghost_PgHost_Host_resume_interrupts(void *self)
{
	(void) self;
	pg_calls_resume_interrupts();
}

/* pghost_PgHost_Host_check_interrupts runs Postgres' CHECK_FOR_INTERRUPTS. */
static bool
pghost_PgHost_Host_check_interrupts(void *self, const HostMayRaise *raise, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_check_interrupts(pghost_pg_raise(raise), &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_open_frame_0_t is open's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_open_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	HostOpen	how;
	int32	   *result;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_open_frame_0_t;

/* pghost_PgHost_Host_open_body_0 is open's body. */
static void
pghost_PgHost_Host_open_body_0(void *frame_)
{
	pghost_PgHost_Host_open_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;
	int32		how = 0;

	switch (frame->how)
	{
		case HOST_OPEN_READ_WRITE:
			how = 1;
			break;
		case HOST_OPEN_CREATE_NEW:
			how = 2;
			break;
	}
	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_file_open(raise, std_CString_as_ptr(&frame->path_c), how, frame->result, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_open_pad_0 is open's pad: its paths while each is live. */
static void
pghost_PgHost_Host_open_pad_0(void *frame_)
{
	pghost_PgHost_Host_open_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_open opens a file to read and write, or creates a new one, through fd.c. */
static bool
pghost_PgHost_Host_open(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostOpen how, int32 *result, HostError *error)
{
	pghost_PgHost_Host_open_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .how = how, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_open_body_0, pghost_PgHost_Host_open_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_create_if_absent_frame_0_t is create_if_absent's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_create_if_absent_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool	   *has_result;
	int32	   *result;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_create_if_absent_frame_0_t;

/* pghost_PgHost_Host_create_if_absent_body_0 is create_if_absent's body. */
static void
pghost_PgHost_Host_create_if_absent_body_0(void *frame_)
{
	pghost_PgHost_Host_create_if_absent_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;
	int32		file;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_file_create_if_absent(raise, std_CString_as_ptr(&frame->path_c), &file, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
	if (!frame->done)
		return;
	*frame->has_result = file >= 0;
	if (*frame->has_result)
		*frame->result = file;
}

/* pghost_PgHost_Host_create_if_absent_pad_0 is create_if_absent's pad: its paths while each is live. */
static void
pghost_PgHost_Host_create_if_absent_pad_0(void *frame_)
{
	pghost_PgHost_Host_create_if_absent_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_create_if_absent creates a file, or answers nothing for a file that is there. */
static bool
pghost_PgHost_Host_create_if_absent(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *has_result, int32 *result, HostError *error)
{
	pghost_PgHost_Host_create_if_absent_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .has_result = has_result, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_create_if_absent_body_0, pghost_PgHost_Host_create_if_absent_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_open_scratch opens a scratch file, which Postgres removes at the transaction's end. */
static bool
pghost_PgHost_Host_open_scratch(void *self, const HostMayRaise *raise, int32 *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_file_open_scratch(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_write_at writes bytes into a file at an offset, all of them. */
static bool
pghost_PgHost_Host_write_at(void *self, const HostMayRaise *raise, int32 file, uint64 offset, const uint8 *bytes,
							size_t bytes_len, HostError *error)
{
	uint32		waits[3];
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	int64		signed_;
	PgPgError	error_;

	(void) self;
	if (!pghost_signed(offset, &signed_, error))
		return false;
	shmem_file_waits(waits);
	if (!pg_calls_file_write(pg_raise, file, bytes, bytes_len, signed_, waits[PGHOST_WRITE], &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_truncate cuts a file to a length. */
static bool
pghost_PgHost_Host_truncate(void *self, const HostMayRaise *raise, int32 file, uint64 len, HostError *error)
{
	uint32		waits[3];
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	int64		signed_;
	PgPgError	error_;

	(void) self;
	if (!pghost_signed(len, &signed_, error))
		return false;
	shmem_file_waits(waits);
	if (!pg_calls_file_truncate(pg_raise, file, signed_, waits[PGHOST_WRITE], &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_sync syncs a file. */
static bool
pghost_PgHost_Host_sync(void *self, const HostMayRaise *raise, int32 file, HostError *error)
{
	uint32		waits[3];
	PgPgError	error_;

	(void) self;
	shmem_file_waits(waits);
	if (!pg_calls_file_sync(pghost_pg_raise(raise), file, waits[PGHOST_SYNC], &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_rename_frame_0_t is rename's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_rename_frame_0_t
{
	const HostMayRaise *raise;
	const char *from;
	size_t		from_len;
	const char *to;
	size_t		to_len;
	HostError  *error;
	bool		done;
	StdCString	from_c;
	bool		from_c_live;
	StdCString	to_c;
	bool		to_c_live;
} pghost_PgHost_Host_rename_frame_0_t;

/* pghost_PgHost_Host_rename_body_0 is rename's body. */
static void
pghost_PgHost_Host_rename_body_0(void *frame_)
{
	pghost_PgHost_Host_rename_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->from, frame->from_len, &frame->from_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->from_c_live = true;
	if (!pghost_c_path(frame->to, frame->to_len, &frame->to_c, frame->error))
	{
		frame->done = false;
		frame->from_c_live = false;
		std_CString_drop(&frame->from_c);
		return;
	}
	frame->to_c_live = true;
	frame->done = pg_calls_rename_file(raise, std_CString_as_ptr(&frame->from_c), std_CString_as_ptr(&frame->to_c), &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->to_c_live = false;
	std_CString_drop(&frame->to_c);
	frame->from_c_live = false;
	std_CString_drop(&frame->from_c);
}

/* pghost_PgHost_Host_rename_pad_0 is rename's pad: its paths while each is live. */
static void
pghost_PgHost_Host_rename_pad_0(void *frame_)
{
	pghost_PgHost_Host_rename_frame_0_t *frame = frame_;

	if (frame->to_c_live)
		std_CString_drop(&frame->to_c);
	if (frame->from_c_live)
		std_CString_drop(&frame->from_c);
}

/* pghost_PgHost_Host_rename renames a file. */
static bool
pghost_PgHost_Host_rename(void *self, const HostMayRaise *raise, const char *from, size_t from_len, const char *to, size_t to_len, HostError *error)
{
	pghost_PgHost_Host_rename_frame_0_t frame = {.raise = raise, .from = from, .from_len = from_len, .to = to, .to_len = to_len, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_rename_body_0, pghost_PgHost_Host_rename_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_rename_durably_frame_0_t is rename_durably's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_rename_durably_frame_0_t
{
	const HostMayRaise *raise;
	const char *from;
	size_t		from_len;
	const char *to;
	size_t		to_len;
	HostError  *error;
	bool		done;
	StdCString	from_c;
	bool		from_c_live;
	StdCString	to_c;
	bool		to_c_live;
} pghost_PgHost_Host_rename_durably_frame_0_t;

/* pghost_PgHost_Host_rename_durably_body_0 is rename_durably's body. */
static void
pghost_PgHost_Host_rename_durably_body_0(void *frame_)
{
	pghost_PgHost_Host_rename_durably_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->from, frame->from_len, &frame->from_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->from_c_live = true;
	if (!pghost_c_path(frame->to, frame->to_len, &frame->to_c, frame->error))
	{
		frame->done = false;
		frame->from_c_live = false;
		std_CString_drop(&frame->from_c);
		return;
	}
	frame->to_c_live = true;
	frame->done = pg_calls_durable_rename(raise, std_CString_as_ptr(&frame->from_c), std_CString_as_ptr(&frame->to_c), &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->to_c_live = false;
	std_CString_drop(&frame->to_c);
	frame->from_c_live = false;
	std_CString_drop(&frame->from_c);
}

/* pghost_PgHost_Host_rename_durably_pad_0 is rename_durably's pad: its paths while each is live. */
static void
pghost_PgHost_Host_rename_durably_pad_0(void *frame_)
{
	pghost_PgHost_Host_rename_durably_frame_0_t *frame = frame_;

	if (frame->to_c_live)
		std_CString_drop(&frame->to_c);
	if (frame->from_c_live)
		std_CString_drop(&frame->from_c);
}

/* pghost_PgHost_Host_rename_durably renames a file durably, as Postgres' durable_rename does. */
static bool
pghost_PgHost_Host_rename_durably(void *self, const HostMayRaise *raise, const char *from, size_t from_len, const char *to, size_t to_len, HostError *error)
{
	pghost_PgHost_Host_rename_durably_frame_0_t frame = {.raise = raise, .from = from, .from_len = from_len, .to = to, .to_len = to_len, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_rename_durably_body_0, pghost_PgHost_Host_rename_durably_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_remove_durably_frame_0_t is remove_durably's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_remove_durably_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_remove_durably_frame_0_t;

/* pghost_PgHost_Host_remove_durably_body_0 is remove_durably's body. */
static void
pghost_PgHost_Host_remove_durably_body_0(void *frame_)
{
	pghost_PgHost_Host_remove_durably_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_durable_unlink(raise, std_CString_as_ptr(&frame->path_c), &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_durably_pad_0 is remove_durably's pad: its paths while each is live. */
static void
pghost_PgHost_Host_remove_durably_pad_0(void *frame_)
{
	pghost_PgHost_Host_remove_durably_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_durably removes a file durably. */
static bool
pghost_PgHost_Host_remove_durably(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostError *error)
{
	pghost_PgHost_Host_remove_durably_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_remove_durably_body_0, pghost_PgHost_Host_remove_durably_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_remove_durably_if_there_frame_0_t is remove_durably_if_there's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_remove_durably_if_there_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool	   *result;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_remove_durably_if_there_frame_0_t;

/* pghost_PgHost_Host_remove_durably_if_there_body_0 is remove_durably_if_there's body. */
static void
pghost_PgHost_Host_remove_durably_if_there_body_0(void *frame_)
{
	pghost_PgHost_Host_remove_durably_if_there_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_remove_durably_if_there(raise, std_CString_as_ptr(&frame->path_c), frame->result, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_durably_if_there_pad_0 is remove_durably_if_there's pad: its paths while each is live. */
static void
pghost_PgHost_Host_remove_durably_if_there_pad_0(void *frame_)
{
	pghost_PgHost_Host_remove_durably_if_there_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_durably_if_there removes a file durably, reporting whether it was there. */
static bool
pghost_PgHost_Host_remove_durably_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *result, HostError *error)
{
	pghost_PgHost_Host_remove_durably_if_there_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_remove_durably_if_there_body_0, pghost_PgHost_Host_remove_durably_if_there_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_remove_tree_frame_0_t is remove_tree's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_remove_tree_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool	   *result;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_remove_tree_frame_0_t;

/* pghost_PgHost_Host_remove_tree_body_0 is remove_tree's body. */
static void
pghost_PgHost_Host_remove_tree_body_0(void *frame_)
{
	pghost_PgHost_Host_remove_tree_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_remove_tree(raise, std_CString_as_ptr(&frame->path_c), frame->result, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_tree_pad_0 is remove_tree's pad: its paths while each is live. */
static void
pghost_PgHost_Host_remove_tree_pad_0(void *frame_)
{
	pghost_PgHost_Host_remove_tree_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_remove_tree removes a directory and everything under it, reporting whether it was there. */
static bool
pghost_PgHost_Host_remove_tree(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *result, HostError *error)
{
	pghost_PgHost_Host_remove_tree_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_remove_tree_body_0, pghost_PgHost_Host_remove_tree_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_remove_leftover removes a leftover file, if its path names one. */
static void
pghost_PgHost_Host_remove_leftover(void *self, const char *path, size_t path_len)
{
	StdCString	canonical;

	(void) self;
	if (pghost_canonical(path, path_len, &canonical))
	{
		pg_calls_remove_leftover(std_CString_as_ptr(&canonical));
		std_CString_drop(&canonical);
	}
}

/* pghost_PgHost_Host_rename_back_frame_0_t is rename_back's state across its pad: the paths while each is live. */
typedef struct pghost_PgHost_Host_rename_back_frame_0_t
{
	const char *from;
	size_t		from_len;
	const char *to;
	size_t		to_len;
	bool		done;
	bool		has_from;
	StdCString	from_c;
	bool		has_to;
	StdCString	to_c;
} pghost_PgHost_Host_rename_back_frame_0_t;

/* pghost_PgHost_Host_rename_back_body_0 is rename_back's body. */
static void
pghost_PgHost_Host_rename_back_body_0(void *frame_)
{
	pghost_PgHost_Host_rename_back_frame_0_t *frame = frame_;

	frame->has_from = pghost_canonical(frame->from, frame->from_len, &frame->from_c);
	frame->has_to = pghost_canonical(frame->to, frame->to_len, &frame->to_c);
	if (frame->has_from && frame->has_to)
		frame->done = pg_calls_durable_rename_back(std_CString_as_ptr(&frame->from_c),
												   std_CString_as_ptr(&frame->to_c));
	else
		frame->done = false;
	if (frame->has_to)
	{
		frame->has_to = false;
		std_CString_drop(&frame->to_c);
	}
	if (frame->has_from)
	{
		frame->has_from = false;
		std_CString_drop(&frame->from_c);
	}
}

/* pghost_PgHost_Host_rename_back_pad_0 is rename_back's pad: the paths while each is live. */
static void
pghost_PgHost_Host_rename_back_pad_0(void *frame_)
{
	pghost_PgHost_Host_rename_back_frame_0_t *frame = frame_;

	if (frame->has_to)
		std_CString_drop(&frame->to_c);
	if (frame->has_from)
		std_CString_drop(&frame->from_c);
}

/* pghost_PgHost_Host_rename_back renames a file back durably, reporting whether it did, from a recovery that can't raise. */
static bool
pghost_PgHost_Host_rename_back(void *self, const char *from, size_t from_len, const char *to, size_t to_len)
{
	pghost_PgHost_Host_rename_back_frame_0_t frame = {.from = from, .from_len = from_len, .to = to, .to_len = to_len};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_rename_back_body_0, pghost_PgHost_Host_rename_back_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_sync_path_frame_0_t is sync_path's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_sync_path_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	bool		directory;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_sync_path_frame_0_t;

/* pghost_PgHost_Host_sync_path_body_0 is sync_path's body. */
static void
pghost_PgHost_Host_sync_path_body_0(void *frame_)
{
	pghost_PgHost_Host_sync_path_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_fsync_fname(raise, std_CString_as_ptr(&frame->path_c), frame->directory, &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_sync_path_pad_0 is sync_path's pad: its paths while each is live. */
static void
pghost_PgHost_Host_sync_path_pad_0(void *frame_)
{
	pghost_PgHost_Host_sync_path_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_sync_path syncs a file or a directory. */
static bool
pghost_PgHost_Host_sync_path(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool directory, HostError *error)
{
	pghost_PgHost_Host_sync_path_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .directory = directory, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_sync_path_body_0, pghost_PgHost_Host_sync_path_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_make_dir_frame_0_t is make_dir's state across its pad: its arguments, its paths, and its result. */
typedef struct pghost_PgHost_Host_make_dir_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_make_dir_frame_0_t;

/* pghost_PgHost_Host_make_dir_body_0 is make_dir's body. */
static void
pghost_PgHost_Host_make_dir_body_0(void *frame_)
{
	pghost_PgHost_Host_make_dir_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pghost_pg_raise(frame->raise);
	PgPgError	error_;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_make_directory(raise, std_CString_as_ptr(&frame->path_c), &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_make_dir_pad_0 is make_dir's pad: its paths while each is live. */
static void
pghost_PgHost_Host_make_dir_pad_0(void *frame_)
{
	pghost_PgHost_Host_make_dir_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_make_dir makes a directory. */
static bool
pghost_PgHost_Host_make_dir(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostError *error)
{
	pghost_PgHost_Host_make_dir_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_make_dir_body_0, pghost_PgHost_Host_make_dir_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_disk_space_frame_0_t is disk_space's state across its pad: its arguments, its path, and its result. */
typedef struct pghost_PgHost_Host_disk_space_frame_0_t
{
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	uint64	   *result_0;
	uint64	   *result_1;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
} pghost_PgHost_Host_disk_space_frame_0_t;

/* pghost_PgHost_Host_disk_space_body_0 is disk_space's body. */
static void
pghost_PgHost_Host_disk_space_body_0(void *frame_)
{
	pghost_PgHost_Host_disk_space_frame_0_t *frame = frame_;
	uint8		sizes[16] = {0};
	PgPgError	error_;
	uint8		free_[8];
	uint8		total[8];
	const void *free_part;
	size_t		free_len;
	const void *total_part;
	size_t		total_len;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	if (!pg_calls_disk_space(pghost_pg_raise(frame->raise), std_CString_as_ptr(&frame->path_c), sizes, sizeof(sizes),
							 &error_))
	{
		*frame->error = pghost_held(&error_);
		frame->done = false;
		frame->path_c_live = false;
		std_CString_drop(&frame->path_c);
		return;
	}
	std_slice_split_at_Checked(sizes, sizeof(sizes), 8, 1, 1, &free_part, &free_len, &total_part, &total_len,
							   RUST_FILE, 274, 35);
	if (free_len != 8)
		STD_RESULT_EXPECT_FAILED(276, 48, "8 bytes", STD_DANGLING, std_TryFromSliceError_Debug_fmt);
	memcpy(free_, free_part, 8);
	*frame->result_0 = std_u64_from_ne_bytes(free_);
	if (total_len != 8)
		STD_RESULT_EXPECT_FAILED(277, 49, "8 bytes", STD_DANGLING, std_TryFromSliceError_Debug_fmt);
	memcpy(total, total_part, 8);
	*frame->result_1 = std_u64_from_ne_bytes(total);
	frame->done = true;
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_disk_space_pad_0 is disk_space's pad: its path while it is live. */
static void
pghost_PgHost_Host_disk_space_pad_0(void *frame_)
{
	pghost_PgHost_Host_disk_space_frame_0_t *frame = frame_;

	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_disk_space returns the bytes free and the bytes in all of the file system that holds a path. */
static bool
pghost_PgHost_Host_disk_space(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
							  uint64 *result_0, uint64 *result_1, HostError *error)
{
	pghost_PgHost_Host_disk_space_frame_0_t frame = {.raise = raise, .path = path, .path_len = path_len,
	.result_0 = result_0, .result_1 = result_1, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_disk_space_body_0, pghost_PgHost_Host_disk_space_pad_0);
	return frame.done;
}

static bool pghost_PgHost_Host_tree_bytes(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
										  uint64 *result, HostError *error);

/* pghost_PgHost_Host_tree_bytes_frame_0_t is tree_bytes' state across its pad: its arguments, its path, the names, and the joined path. */
typedef struct pghost_PgHost_Host_tree_bytes_frame_0_t
{
	void	   *self;
	const HostMayRaise *raise;
	const char *path;
	size_t		path_len;
	uint64	   *result;
	HostError  *error;
	bool		done;
	StdCString	path_c;
	bool		path_c_live;
	StdVecIntoIter names;
	bool		names_live;
	StdString	name;
	bool		name_live;
	StdPathBuf	joined;
	bool		joined_live;
} pghost_PgHost_Host_tree_bytes_frame_0_t;

/* pghost_PgHost_Host_tree_bytes_body_0 is tree_bytes' body. */
static void
pghost_PgHost_Host_tree_bytes_body_0(void *frame_)
{
	pghost_PgHost_Host_tree_bytes_frame_0_t *frame = frame_;
	uint8		found[16] = {0};
	PgPgError	error_;
	uint8		kind_bytes[8];
	uint8		size_bytes[8];
	const void *kind_part;
	size_t		kind_len;
	const void *size_part;
	size_t		size_len;
	uint64		kind;

	if (!pghost_c_path(frame->path, frame->path_len, &frame->path_c, frame->error))
	{
		frame->done = false;
		return;
	}
	frame->path_c_live = true;
	frame->done = pg_calls_path_stat(pghost_pg_raise(frame->raise), std_CString_as_ptr(&frame->path_c), found,
									 sizeof(found), &error_);
	if (!frame->done)
		*frame->error = pghost_held(&error_);
	frame->path_c_live = false;
	std_CString_drop(&frame->path_c);
	if (!frame->done)
		return;
	std_slice_split_at_Checked(found, sizeof(found), 8, 1, 1, &kind_part, &kind_len, &size_part, &size_len, RUST_FILE,
							   284, 34);
	if (kind_len != 8)
		STD_RESULT_EXPECT_FAILED(285, 50, "8 bytes", STD_DANGLING, std_TryFromSliceError_Debug_fmt);
	memcpy(kind_bytes, kind_part, 8);
	kind = std_u64_from_ne_bytes(kind_bytes);
	if (kind == PGHOST_DIRECTORY)
	{
		uint64		total = 0;
		bool		has_names;
		StdVec		names;

		if (!pghost_PgHost_ReadHost_list_dir_if_there(frame->self, frame->raise, frame->path, frame->path_len,
													  &has_names, &names, frame->error))
		{
			frame->done = false;
			return;
		}
		if (has_names)
		{
			frame->names = std_Vec_IntoIterator_into_iter(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
			frame->names_live = true;
			while (std_VecIntoIter_Iterator_next(&frame->names, &frame->name))
			{
				uint64		bytes;

				frame->name_live = true;
				std_path_join(frame->path, frame->path_len, (const char *) frame->name.ptr, frame->name.len,
							  &frame->joined);
				frame->name_live = false;
				std_vec_free_Checked(&frame->name, 1, 1);
				frame->joined_live = true;
				if (!pghost_PgHost_Host_tree_bytes(frame->self, frame->raise, (const char *) frame->joined.bytes.ptr,
												   frame->joined.bytes.len, &bytes, frame->error))
				{
					frame->done = false;
					frame->joined_live = false;
					std_vec_free_Checked(&frame->joined.bytes, 1, 1);
					frame->names_live = false;
					std_VecIntoIter_drop(&frame->names);
					return;
				}
				frame->joined_live = false;
				std_vec_free_Checked(&frame->joined.bytes, 1, 1);
				total = STD_ADD(u64, total, bytes, 289, 21);
			}
			frame->names_live = false;
			std_VecIntoIter_drop(&frame->names);
		}
		*frame->result = total;
		frame->done = true;
		return;
	}
	if (kind == PGHOST_FILE)
	{
		if (size_len != 8)
			STD_RESULT_EXPECT_FAILED(293, 59, "8 bytes", STD_DANGLING, std_TryFromSliceError_Debug_fmt);
		memcpy(size_bytes, size_part, 8);
		*frame->result = std_u64_from_ne_bytes(size_bytes);
		frame->done = true;
		return;
	}
	*frame->result = 0;
	frame->done = true;
}

/* pghost_PgHost_Host_tree_bytes_pad_0 is tree_bytes' pad: the joined path, the names, and the path, while each is live. */
static void
pghost_PgHost_Host_tree_bytes_pad_0(void *frame_)
{
	pghost_PgHost_Host_tree_bytes_frame_0_t *frame = frame_;

	if (frame->joined_live)
		std_vec_free_Checked(&frame->joined.bytes, 1, 1);
	if (frame->name_live)
		std_vec_free_Checked(&frame->name, 1, 1);
	if (frame->names_live)
		std_VecIntoIter_drop(&frame->names);
	if (frame->path_c_live)
		std_CString_drop(&frame->path_c);
}

/* pghost_PgHost_Host_tree_bytes returns the bytes of the files under a path, a file's own size for a file. */
static bool
pghost_PgHost_Host_tree_bytes(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
							  uint64 *result, HostError *error)
{
	pghost_PgHost_Host_tree_bytes_frame_0_t frame = {.self = self, .raise = raise, .path = path,
	.path_len = path_len, .result = result, .error = error};

	std_landing(&frame, pghost_PgHost_Host_tree_bytes_body_0, pghost_PgHost_Host_tree_bytes_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_nap sleeps on the backend's latch for a duration, waking for interrupts. */
static bool
pghost_PgHost_Host_nap(void *self, const HostMayRaise *raise, StdDuration duration, HostError *error)
{
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	int64		ms;
	StdTryFromIntError tried;
	PgPgError	error_;

	(void) self;
	if (!std_i64_try_from_u128(std_Duration_as_millis(&duration), &ms, &tried))
		ms = PG_INT64_MAX;
	if (!pg_calls_latch_sleep(pg_raise, ms, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_wal_insert inserts one of the fork's records into the WAL under its resource manager. */
static bool
pghost_PgHost_Host_wal_insert(void *self, const HostMayRaise *raise, HostWalKind kind, const uint8 *body,
							  size_t body_len, HostLsn *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_wal_insert(pghost_pg_raise(raise), host_WalKind_value(kind), body, body_len, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_wal_new_cid logs a row's command ids for logical decoding, as heap's log_heap_new_cid does. */
static bool
pghost_PgHost_Host_wal_new_cid(void *self, const HostMayRaise *raise, const HostStorage *storage, HostRow tid,
							   HostNewCid cid, HostLsn *result, HostError *error)
{
	uint32		tablespace = storage->tablespace;
	uint32		database = storage->database;
	uint32		relation = storage->relation;
	uint32		cmin = cid.has_cmin ? cid.cmin.f0 : PGHOST_INVALID_COMMAND;
	uint32		cmax = cid.has_cmax ? cid.cmax.f0 : PGHOST_INVALID_COMMAND;
	uint32		combo = cid.has_combo ? cid.combo.f0 : PGHOST_INVALID_COMMAND;
	DgStorage	pg_storage = {tablespace, database, relation};
	PgPgError	error_;

	(void) self;
	if (!pg_calls_log_new_cid(pghost_pg_raise(raise), pg_storage, tid.f0, cmin, cmax, combo, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_wal_flush flushes the WAL up to a position. */
static bool
pghost_PgHost_Host_wal_flush(void *self, const HostMayRaise *raise, HostLsn lsn, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_wal_flush(pghost_pg_raise(raise), lsn, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_fold_point returns the WAL position a fold's redo starts from. */
static bool
pghost_PgHost_Host_fold_point(void *self, const HostMayRaise *raise, HostLsn *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_fold_point(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_checkpoint_now asks for an immediate checkpoint and waits for it. */
static bool
pghost_PgHost_Host_checkpoint_now(void *self, const HostMayRaise *raise, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_checkpoint_now(pghost_pg_raise(raise), &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_quiet_database ends the sessions of a database and keeps new ones out. */
static bool
pghost_PgHost_Host_quiet_database(void *self, const HostMayRaise *raise, uint32 database, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_quiet_database(pghost_pg_raise(raise), database, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_unquiet_database lets a quieted database's sessions in again. */
static bool
pghost_PgHost_Host_unquiet_database(void *self, const HostMayRaise *raise, uint32 database, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_unquiet_database(pghost_pg_raise(raise), database, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_last_redo returns the redo position of the last checkpoint. */
static bool
pghost_PgHost_Host_last_redo(void *self, const HostMayRaise *raise, HostLsn *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_last_redo(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_wal_stand returns where the WAL stands: recovery, its timelines, and its flushed and replayed positions. */
static HostWalStand
pghost_PgHost_Host_wal_stand(void *self)
{
	bool		recovering = pg_calls_recovery_in_progress();
	uint64		replayed;
	uint32		replayed_timeline;
	HostWalStand result;

	(void) self;
	pg_replayed(&replayed, &replayed_timeline);
	result.recovering = recovering;
	result.insertion_timeline = pg_calls_insertion_timeline_if_set();
	result.flushed = recovering ? 0 : pg_calls_flushed();
	result.replayed = replayed;
	result.replayed_timeline = replayed_timeline;
	result.target_timeline = pg_calls_recovery_target_timeline();
	return result;
}

/* pghost_PgHost_Host_warn_kept_wal warns that a range of WAL a fallback would replay is kept. */
static bool
pghost_PgHost_Host_warn_kept_wal(void *self, const HostMayRaise *raise, HostLsn from, HostLsn to, uint32 timeline,
								 HostLsn upto, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_warn_kept_wal(pghost_pg_raise(raise), from, to, timeline, upto, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_next_fork_record_closure_0 is word: the native-endian word of the buffer at an offset, zero when it falls short. */
static uint64
pghost_PgHost_Host_next_fork_record_closure_0(const StdVec *buffer, size_t at)
{
	size_t		end = STD_ADD(usize, at, 8, 367, 66);
	uint8		bytes[8] = {0};

	std_range_check(at, end, buffer->len, RUST_FILE, 367, 61);
	if (std_wrapping_sub_usize(end, at) == 8)
		memcpy(bytes, STD_PTR_AT(buffer->ptr, at, 1), 8);
	return std_u64_from_ne_bytes(bytes);
}

/* pghost_PgHost_Host_next_fork_record_frame_0_t is next_fork_record's state across its pad: its arguments, its result, and its buffer. */
typedef struct pghost_PgHost_Host_next_fork_record_frame_0_t
{
	const HostMayRaise *raise;
	HostLsn		from;
	uint32		kinds;
	bool		received;
	bool	   *has_result;
	HostForkRecord *result;
	HostError  *error;
	bool		done;
	StdVec		buffer;
	bool		buffer_live;
} pghost_PgHost_Host_next_fork_record_frame_0_t;

/* pghost_PgHost_Host_next_fork_record_body_0 is next_fork_record's body. */
static void
pghost_PgHost_Host_next_fork_record_body_0(void *frame_)
{
	pghost_PgHost_Host_next_fork_record_frame_0_t *frame = frame_;

	std_vec_u8_from_elem(&frame->buffer, 0, 4096);
	frame->buffer_live = true;
	for (;;)
	{
		uint64		needed;
		size_t		needed_;
		PgPgError	error_;
		size_t		length;
		size_t		end;
		uint8		kind;
		uint64		at;
		uint8		zero = 0;

		if (!pg_calls_next_fork_record(pghost_pg_raise(frame->raise), frame->from, frame->kinds, frame->received,
									   frame->buffer.ptr, frame->buffer.len, &needed, &error_))
		{
			*frame->error = pghost_held(&error_);
			frame->done = false;
			break;
		}
		if (needed == 0)
		{
			*frame->has_result = false;
			frame->done = true;
			break;
		}
		needed_ = std_usize_try_from_u64(needed);
		if (needed_ > frame->buffer.len)
		{
			std_vec_resize(&frame->buffer, needed_, &zero, 1, 1);
			continue;
		}
		length = std_usize_try_from_u64(pghost_PgHost_Host_next_fork_record_closure_0(&frame->buffer, 9));
		std_index_check(0, frame->buffer.len, RUST_FILE, 369, 29);
		kind = STD_VEC_AT(&frame->buffer, uint8, 0);
		at = pghost_PgHost_Host_next_fork_record_closure_0(&frame->buffer, 1);
		end = STD_ADD(usize, 17, length, 369, 60);
		std_range_check(17, end, frame->buffer.len, RUST_FILE, 369, 55);
		frame->result->f0 = kind;
		frame->result->f1 = at;
		std_slice_to_vec_Checked(&frame->result->f2, STD_PTR_AT(frame->buffer.ptr, 17, 1), std_wrapping_sub_usize(end, 17), 1, 1);
		*frame->has_result = true;
		frame->done = true;
		break;
	}
	frame->buffer_live = false;
	std_vec_free_Checked(&frame->buffer, 1, 1);
}

/* pghost_PgHost_Host_next_fork_record_pad_0 is next_fork_record's pad: its buffer while it is live. */
static void
pghost_PgHost_Host_next_fork_record_pad_0(void *frame_)
{
	pghost_PgHost_Host_next_fork_record_frame_0_t *frame = frame_;

	if (frame->buffer_live)
		std_vec_free_Checked(&frame->buffer, 1, 1);
}

/* pghost_PgHost_Host_next_fork_record returns the next of the fork's WAL records of some kinds from a position: its kind, its end, and its body. */
static bool
pghost_PgHost_Host_next_fork_record(void *self, const HostMayRaise *raise, HostLsn from, uint32 kinds,
									bool received, bool *has_result, HostForkRecord *result, HostError *error)
{
	pghost_PgHost_Host_next_fork_record_frame_0_t frame = {.raise = raise, .from = from, .kinds = kinds,
	.received = received, .has_result = has_result, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_next_fork_record_body_0, pghost_PgHost_Host_next_fork_record_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_warn reports an error at WARNING with a context, for a seam that goes on past it; it takes the error. */
static bool
pghost_PgHost_Host_warn(void *self, const HostMayRaise *raise, HostError *error_, const char *context,
						HostError *error)
{
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	PgPgError	raised = pghost_raised(error_);
	PgPgError	warned;

	(void) self;
	if (!pg_warn_error(pg_raise, &raised, context, &warned))
	{
		*error = pghost_held(&warned);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_log reports a message at LOG. */
static bool
pghost_PgHost_Host_log(void *self, const HostMayRaise *raise, const char *message, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_report_log(pghost_pg_raise(raise), message, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_refuse_recovery records why the delta can't be recovered, which the next start reports. */
static void
pghost_PgHost_Host_refuse_recovery(void *self, const char *detail, size_t detail_len)
{
	(void) self;
	shmem_delta_refuse(detail, detail_len);
}

/*
 * pghost_PgHost_Host_load_pauses loads pauses' callbacks, as
 * INJECTION_POINT_LOAD does, and each one's failing point, which host_pause
 * looks up inside the window (bugs row 392), and does nothing in a build
 * without injection points.
 */
static bool
pghost_PgHost_Host_load_pauses(void *self, const HostMayRaise *raise, const char *const *pauses, size_t pauses_len,
							   HostError *error)
{
	(void) self;
#ifdef USE_INJECTION_POINTS
	for (size_t i = 0, i_len = pauses_len; i < i_len; i++)
	{
		PgPgError	error_;
		char		fails[HOST_PAUSE_NAME];

		snprintf(fails, sizeof(fails), "%s-fails", pauses[i]);
		if (!pg_calls_injection_point_load(pghost_pg_raise(raise), pauses[i], &error_) ||
			!pg_calls_injection_point_load(pghost_pg_raise(raise), fails, &error_))
		{
			*error = pghost_held(&error_);
			return false;
		}
	}
#else
	(void) raise;
	(void) pauses;
	(void) pauses_len;
	(void) error;
#endif
	return true;
}

/* pghost_PgHost_Host_delay_checkpoints holds checkpoints off, loading first the pauses its window runs inside its critical section. */
static bool
pghost_PgHost_Host_delay_checkpoints(void *self, const HostMayRaise *raise, const char *const *pauses,
									 size_t pauses_len, HostError *error)
{
	PgPgError	error_;

	if (!pghost_PgHost_Host_load_pauses(self, raise, pauses, pauses_len, error))
		return false;
	if (!pg_calls_delay_checkpoints(pghost_pg_raise(raise), &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	std_AtomicBool_store_relaxed(&PGHOST_IN_WINDOW, true);
	return true;
}

/* pghost_PgHost_Host_resume_checkpoints lets checkpoints start again. */
static void
pghost_PgHost_Host_resume_checkpoints(void *self, bool finished)
{
	(void) self;
	std_AtomicBool_store_relaxed(&PGHOST_IN_WINDOW, false);
	pg_calls_resume_checkpoints(finished);
}

/*
 * pghost_PgHost_Host_fail_checkpoints stops the server with the error that
 * ended a window, a Postgres error by its own data, which it reads without
 * taking (bugs row 392).
 */
static void
pghost_PgHost_Host_fail_checkpoints(void *self, const HostError *error)
{
	const PgPgError *raised = error->cause.tag == HOST_CAUSE_POSTGRES ?
		std_any_downcast(error->cause.u.postgres, &PG_PG_ERROR_TYPE_ID) : NULL;
	StdStr		message = host_Error_message(error);
	StdStr		hint = host_Error_hint(error);
	StdCString	message_c;
	StdCString	hint_c;
	StdNulError nul;

	(void) self;
	if (!std_CString_new_bytes((const uint8 *) message.ptr, message.len, &message_c, &nul))
	{
		std_NulError_drop(&nul);
		std_CString_default(&message_c);
	}
	if (!std_CString_new_bytes((const uint8 *) hint.ptr, hint.len, &hint_c, &nul))
	{
		std_NulError_drop(&nul);
		std_CString_default(&hint_c);
	}
	std_AtomicBool_store_relaxed(&PGHOST_IN_WINDOW, false);
	pg_calls_fail_checkpoint_delay(raised != NULL ? raised->raw.data : NULL, host_Error_sqlstate(error), 5,
								   std_CString_as_ptr(&message_c), std_CString_as_ptr(&hint_c));
}

/* pghost_PgHost_Host_xid_status returns a transaction's status in the commit log. */
static bool
pghost_PgHost_Host_xid_status(void *self, const HostMayRaise *raise, HostXid xid, HostStatus *result,
							  HostError *error)
{
	int32		status;
	PgPgError	error_;

	(void) self;
	if (!pg_calls_xid_status(pghost_pg_raise(raise), xid.f0, &status, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	switch (status)
	{
		case 0:
			*result = HOST_STATUS_IN_PROGRESS;
			break;
		case 1:
			*result = HOST_STATUS_COMMITTED;
			break;
		default:
			*result = HOST_STATUS_ABORTED;
			break;
	}
	return true;
}

/* pghost_PgHost_Host_xid_in_progress reports whether a transaction is running. */
static bool
pghost_PgHost_Host_xid_in_progress(void *self, const HostMayRaise *raise, HostXid xid, bool *result,
								   HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_xid_in_progress(pghost_pg_raise(raise), xid.f0, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_xids_answered reports whether the backend can answer transaction statuses. */
static bool
pghost_PgHost_Host_xids_answered(void *self)
{
	(void) self;
	return pg_calls_xids_answered();
}

/* pghost_PgHost_Host_removable_in reports whether a transaction's rows are removable under a snapshot of pg's. */
static bool
pghost_PgHost_Host_removable_in(void *self, const HostMayRaise *raise, StdAny snapshot, HostXid xid, bool *result,
								HostError *error)
{
	const PgSnapshot *pg_snapshot = std_any_downcast(snapshot, &PG_SNAPSHOT_TYPE_ID);
	PgPgError	error_;

	(void) self;
	if (pg_snapshot == NULL)
		STD_OPTION_EXPECT_FAILED(419, 64, "pghost makes a snapshot only from pg's");
	if (!pg_calls_snapshot_removable(pghost_pg_raise(raise), pg_snapshot, xid.f0, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_current_writer returns the transaction the backend writes as and its top transaction. */
static bool
pghost_PgHost_Host_current_writer(void *self, const HostMayRaise *raise, HostXid *result_0, HostXid *result_1,
								  HostError *error)
{
	uint64		writer;
	uint64		top;
	PgPgError	error_;

	(void) self;
	if (!pg_calls_current_xid(pghost_pg_raise(raise), &writer, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	if (!pg_calls_top_xid(pghost_pg_raise(raise), &top, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*result_0 = (HostXid)
	{
		writer
	};
	*result_1 = (HostXid)
	{
		top
	};
	return true;
}

/* pghost_PgHost_Host_current_command returns the backend's command id, marking it used when the command writes. */
static bool
pghost_PgHost_Host_current_command(void *self, const HostMayRaise *raise, bool used, HostCommand *result,
								   HostError *error)
{
	uint32		command;
	PgPgError	error_;

	(void) self;
	if (!pg_calls_current_command(pghost_pg_raise(raise), used, &command, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*result = (HostCommand)
	{
		command
	};
	return true;
}

/* pghost_PgHost_Host_xid_is_current reports whether a transaction is one of the backend's own. */
static bool
pghost_PgHost_Host_xid_is_current(void *self, HostXid xid)
{
	(void) self;
	return pg_calls_is_current_xid(xid.f0);
}

/* pghost_PgHost_Host_combo_command returns the combo command id of a writer's cmin and cmax. */
static bool
pghost_PgHost_Host_combo_command(void *self, const HostMayRaise *raise, HostXid writer, HostCommand cmin,
								 HostCommand cmax, HostCommand *result, HostError *error)
{
	uint32		command;
	PgPgError	error_;

	(void) self;
	if (!pg_calls_combo_command(pghost_pg_raise(raise), writer.f0, cmin.f0, cmax.f0, &command, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*result = (HostCommand)
	{
		command
	};
	return true;
}

/* pghost_PgHost_Host_actor returns nothing: the server's backends are no simulation's actors. */
static bool
pghost_PgHost_Host_actor(void *self, uint64 *result)
{
	(void) self;
	(void) result;
	return false;
}

/* pghost_PgHost_Host_yield_point does nothing: the server's backends yield to no simulation. */
static void
pghost_PgHost_Host_yield_point(void *self)
{
	(void) self;
}

/* pghost_PgHost_Host_pause runs an injection point, the cached one inside a checkpoint window's critical section. */
static bool
pghost_PgHost_Host_pause(void *self, const HostMayRaise *raise, const char *point, HostError *error)
{
	PgPgError	error_;

	(void) self;
#ifdef USE_INJECTION_POINTS
	if (std_AtomicBool_load_relaxed(&PGHOST_IN_WINDOW))
	{
		if (!pg_calls_injection_point_cached(pghost_pg_raise(raise), point, &error_))
		{
			*error = pghost_held(&error_);
			return false;
		}
		return true;
	}
#endif
	if (!pg_calls_injection_point(pghost_pg_raise(raise), point, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_injected reports whether an injection point is attached. */
static bool
pghost_PgHost_Host_injected(void *self, const HostMayRaise *raise, const char *point, bool *result,
							HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_injection_point_attached(pghost_pg_raise(raise), point, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_at_exit lists an exit work of the process. */
static bool
pghost_PgHost_Host_at_exit(void *self, const StdTypeId *work, bool (*run) (HostIoIo *io, HostError *error),
						   HostError *error)
{
	(void) self;
	return pghost_shared_at_exit(work, run, error);
}

/* pghost_PgHost_Host_shared_make takes a free record of a kind's pool for this process. */
static bool
pghost_PgHost_Host_shared_make(void *self, const HostMayRaise *raise, const HostSharedKind *kind, bool *has_result,
							   uint64 *result, HostError *error)
{
	(void) self;
	return pghost_shared_make(raise, kind->id, has_result, result, error);
}

/* pghost_PgHost_Host_shared_publish publishes a record this process owns and returns its location. */
static uint64
pghost_PgHost_Host_shared_publish(void *self, uint64 place)
{
	(void) self;
	return pghost_shared_publish(place);
}

/* pghost_PgHost_Host_shared_claim takes the ownership of a kind's published record at a location. */
static bool
pghost_PgHost_Host_shared_claim(void *self, const HostSharedKind *kind, uint64 location, bool *has_result,
								uint64 *result, HostError *error)
{
	(void) self;
	return pghost_shared_claim(kind->id, location, has_result, result, error);
}

/* pghost_PgHost_Host_shared_free frees a record this process owns. */
static void
pghost_PgHost_Host_shared_free(void *self, uint64 place)
{
	(void) self;
	pghost_shared_free(place);
}

/* pghost_PgHost_Host_shared_give_up gives up a record this process still owns. */
static void
pghost_PgHost_Host_shared_give_up(void *self, uint64 place)
{
	(void) self;
	pghost_shared_give_up(place);
}

/* pghost_PgHost_Host_shared_open opens a kind's published record at a location. */
static bool
pghost_PgHost_Host_shared_open(void *self, const HostMayRaise *raise, const HostSharedKind *kind, uint64 location,
							   bool *has_result, uint64 *result, HostError *error)
{
	(void) self;
	return pghost_shared_open(raise, kind->id, location, has_result, result, error);
}

/* pghost_PgHost_Host_shared_close closes one of this process's handles of a record it opened. */
static void
pghost_PgHost_Host_shared_close(void *self, uint64 place)
{
	(void) self;
	pghost_shared_close(place);
}

/* pghost_PgHost_Host_shared_reclaim looks at a kind's published record at a location for a reclaim. */
static bool
pghost_PgHost_Host_shared_reclaim(void *self, const HostSharedKind *kind, uint64 location, HostSharedLooked *result,
								  HostError *error)
{
	(void) self;
	return pghost_shared_reclaim(kind->id, location, result, error);
}

/* pghost_PgHost_Host_shared_reclaimed frees a record this process's reclaim took, if no process opened it since. */
static bool
pghost_PgHost_Host_shared_reclaimed(void *self, uint64 place)
{
	(void) self;
	return pghost_shared_reclaimed(place);
}

/* pghost_PgHost_Host_shared_unreclaim gives up a record this process's reclaim still has. */
static void
pghost_PgHost_Host_shared_unreclaim(void *self, uint64 place)
{
	(void) self;
	pghost_shared_unreclaim(place);
}

/* pghost_PgHost_Host_shared_sweep lists a kind's published records, each with one of its atomics. */
static bool
pghost_PgHost_Host_shared_sweep(void *self, const HostMayRaise *raise, const HostSharedKind *kind, size_t atomic,
								StdVec *result, HostError *error)
{
	(void) self;
	return pghost_shared_sweep(raise, kind->id, atomic, result, error);
}

/* pghost_PgHost_Host_shared_index_find answers the location a pool's index holds for a key, or nothing (bugs row 405). */
static bool
pghost_PgHost_Host_shared_index_find(void *self, const HostMayRaise *raise, const HostSharedKind *kind,
									 const uint8 *key, bool *has, uint64 *location, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (shmem_records_index_find(pghost_pg_raise(raise), kind->id, key, has, location, &error_))
		return true;
	*error = pghost_held(&error_);
	return false;
}

/* pghost_PgHost_Host_shared_index_put sets the location a pool's index holds for a key (bugs row 405). */
static bool
pghost_PgHost_Host_shared_index_put(void *self, const HostMayRaise *raise, const HostSharedKind *kind,
									const uint8 *key, uint64 location, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (shmem_records_index_put(pghost_pg_raise(raise), kind->id, key, location, &error_))
		return true;
	*error = pghost_held(&error_);
	return false;
}

/* pghost_PgHost_Host_shared_index_remove takes a key out of a pool's index when it holds that location (bugs row 405). */
static bool
pghost_PgHost_Host_shared_index_remove(void *self, const HostMayRaise *raise, const HostSharedKind *kind,
									   const uint8 *key, uint64 location, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (shmem_records_index_remove(pghost_pg_raise(raise), kind->id, key, location, &error_))
		return true;
	*error = pghost_held(&error_);
	return false;
}

/* pghost_PgHost_Host_shared_single returns the place of the server's one record of a kind. */
static bool
pghost_PgHost_Host_shared_single(void *self, const HostSharedKind *kind, uint64 *result, HostError *error)
{
	(void) self;
	return pghost_shared_single(kind->id, result, error);
}

/* pghost_PgHost_Host_shared_ready reports whether the fork's region is mapped. */
static bool
pghost_PgHost_Host_shared_ready(void *self)
{
	(void) self;
	return !(pg_calls_shmem_base() == NULL);
}

/* pghost_PgHost_Host_shared_lock takes one of a record's locks. */
static bool
pghost_PgHost_Host_shared_lock(void *self, const HostMayRaise *raise, uint64 place, size_t lock, bool exclusive,
							   HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_lock(pghost_pg_raise(raise), kind, index, lock, exclusive, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_shared_unlock releases one of a record's locks. */
static void
pghost_PgHost_Host_shared_unlock(void *self, uint64 place, size_t lock)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_unlock(kind, index, lock);
}

/* pghost_PgHost_Host_key_lock takes the key lock's stripes exclusively, in the order given. */
static bool
pghost_PgHost_Host_key_lock(void *self, const HostMayRaise *raise, const uint32 *stripes, size_t stripes_len,
							HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_keys_lock(pghost_pg_raise(raise), stripes, stripes_len, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_key_unlock releases the key lock's stripes, the last taken first. */
static void
pghost_PgHost_Host_key_unlock(void *self, const uint32 *stripes, size_t stripes_len, bool pushed)
{
	(void) self;
	shmem_keys_unlock(stripes, stripes_len, pushed);
}

/* pghost_PgHost_Host_key_count returns a stripe's push count. */
static uint64
pghost_PgHost_Host_key_count(void *self, uint32 stripe)
{
	(void) self;
	return shmem_keys_count(stripe);
}

/* pghost_PgHost_Host_key_hold holds a stripe shared without waiting, reporting whether it did. */
static bool
pghost_PgHost_Host_key_hold(void *self, uint32 stripe)
{
	(void) self;
	return shmem_keys_hold(stripe);
}

/* pghost_PgHost_Host_key_release releases a stripe the process holds shared. */
static void
pghost_PgHost_Host_key_release(void *self, uint32 stripe)
{
	(void) self;
	shmem_keys_release(stripe);
}

/* pghost_PgHost_Host_key_held_wait waits for a stripe's exclusive holders to release it. */
static bool
pghost_PgHost_Host_key_held_wait(void *self, const HostMayRaise *raise, uint32 stripe, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_keys_held_wait(pghost_pg_raise(raise), stripe, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_xact_wait waits for a transaction to end, with the catalog capability the Io carries, as heap's lock waits do. */
static bool
pghost_PgHost_Host_xact_wait(void *self, const HostMayRaise *raise, HostXid xid, bool has_on, HostOperation on,
							 bool wait, bool *result, HostError *error)
{
	const PgCatalogCaps *caps = std_any_downcast(host_MayRaise_inner(raise), &PG_CATALOG_CAPS_TYPE_ID);
	HostOperation operation;
	PgPgError	error_;

	(void) self;
	if (caps == NULL)
	{
		StdString	message;

		std_string_from_Checked(&message, "a wait for a transaction was asked with no catalog access",
								sizeof("a wait for a transaction was asked with no catalog access") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	operation = has_on ? on : (HostOperation)
	{
		0, 0, 0
	};
	if (!pg_calls_xact_wait(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), (uint32) xid.f0,
							operation.relation, operation.tid, operation.oper, wait, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_speculative_wait waits for a speculative insertion's token to be released. */
static bool
pghost_PgHost_Host_speculative_wait(void *self, const HostMayRaise *raise, HostXid xid, uint32 token,
									HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_speculative_wait(pghost_pg_raise(raise), (uint32) xid.f0, token, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_lock_tuple takes a row's tuple lock, reporting whether it did. */
static bool
pghost_PgHost_Host_lock_tuple(void *self, const HostMayRaise *raise, HostTupleLock tuple, bool wait, bool *result,
							  HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_lock_tuple(pghost_pg_raise(raise), tuple.database, tuple.relation, tuple.tid, (int32) tuple.mode,
							 wait, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_unlock_tuple releases a row's tuple lock. */
static void
pghost_PgHost_Host_unlock_tuple(void *self, HostTupleLock tuple)
{
	(void) self;
	pg_calls_unlock_tuple(tuple.database, tuple.relation, tuple.tid, (int32) tuple.mode);
}

/* pghost_PgHost_Host_flag_take takes one of a record's flags. */
static bool
pghost_PgHost_Host_flag_take(void *self, const HostMayRaise *raise, uint64 place, size_t flag, HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_flag_take(pghost_pg_raise(raise), kind, index, flag, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_flag_try takes one of a record's flags when no process holds it, answering whether it did. */
static bool
pghost_PgHost_Host_flag_try(void *self, const HostMayRaise *raise, uint64 place, size_t flag, bool *result,
							HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_flag_try(pghost_pg_raise(raise), kind, index, flag, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_flag_clear clears one of a record's flags. */
static void
pghost_PgHost_Host_flag_clear(void *self, uint64 place, size_t flag)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_flag_clear(kind, index, flag);
}

/* pghost_PgHost_Host_shared_atomics returns the number of a record's atomics. */
static size_t
pghost_PgHost_Host_shared_atomics(void *self, uint64 place)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	return shmem_records_atomics(kind);
}

/* pghost_PgHost_Host_atomic does an operation on one of a record's atomics and returns the value it read. */
static uint64
pghost_PgHost_Host_atomic(void *self, uint64 place, size_t atomic, HostSharedOp op)
{
	ShmemRecordsOp records_op = {0};
	size_t		kind;
	uint32		index;

	(void) self;
	switch (op.tag)
	{
		case HOST_SHARED_OP_LOAD:
			records_op.tag = SHMEM_RECORDS_OP_LOAD;
			break;
		case HOST_SHARED_OP_ADD:
			records_op.tag = SHMEM_RECORDS_OP_ADD;
			records_op.u.add = op.u.add;
			break;
		case HOST_SHARED_OP_EXCHANGE:
			records_op.tag = SHMEM_RECORDS_OP_EXCHANGE;
			records_op.u.exchange.f0 = op.u.exchange.f0;
			records_op.u.exchange.f1 = op.u.exchange.f1;
			break;
		case HOST_SHARED_OP_LOAD_ACQUIRE:
			records_op.tag = SHMEM_RECORDS_OP_LOAD_ACQUIRE;
			break;
	}
	pghost_shared_placed(place, &kind, &index);
	return shmem_records_atomic(kind, index, atomic, records_op);
}

/* pghost_PgHost_Host_blob_read returns one of a record's blobs. */
static StdVec
pghost_PgHost_Host_blob_read(void *self, uint64 place, size_t blob)
{
	size_t		kind;
	uint32		index;
	StdVec		result;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_blob_read(kind, index, blob, &result);
	return result;
}

/* pghost_PgHost_Host_blob_write writes one of a record's blobs. */
static bool
pghost_PgHost_Host_blob_write(void *self, uint64 place, size_t blob, const uint8 *bytes, size_t bytes_len,
							  HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_blob_write(kind, index, blob, bytes, bytes_len, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_entry_read returns an entry of one of a record's arrays. */
static bool
pghost_PgHost_Host_entry_read(void *self, uint64 place, size_t array, size_t entry, StdVec *result,
							  HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_entry_read(kind, index, array, entry, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_entry_write writes an entry of one of a record's arrays. */
static bool
pghost_PgHost_Host_entry_write(void *self, uint64 place, size_t array, size_t entry, const uint8 *bytes,
							   size_t bytes_len, HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_entry_write(kind, index, array, entry, bytes, bytes_len, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_shared_location returns the location of the record at a place. */
static uint64
pghost_PgHost_Host_shared_location(void *self, uint64 place)
{
	(void) self;
	return pghost_shared_location(place);
}

/* pghost_PgHost_Host_shared_live reports whether the pooled record at a location still has its slot. */
static bool
pghost_PgHost_Host_shared_live(void *self, uint64 location)
{
	(void) self;
	return pghost_shared_live(location);
}

/* pghost_PgHost_Host_shared_gone reports whether the record of a kind a location named no longer has its slot. */
static bool
pghost_PgHost_Host_shared_gone(void *self, const HostSharedKind *kind, uint64 location, bool *result,
							   HostError *error)
{
	(void) self;
	return pghost_shared_gone(kind->id, location, result, error);
}

/* pghost_PgHost_Host_slots_name returns the name of one of a record's slots arrays. */
static uint64
pghost_PgHost_Host_slots_name(void *self, uint64 place, size_t array)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	return shmem_records_slots_name(kind, index, array);
}

/* pghost_PgHost_Host_slots_rename renames one of a record's slots arrays from the name it had. */
static bool
pghost_PgHost_Host_slots_rename(void *self, uint64 place, size_t array, uint64 loaded, uint64 name)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	return shmem_records_slots_rename(kind, index, array, loaded, name);
}

/* pghost_PgHost_Host_slots_empty empties one of a record's slots arrays. */
static void
pghost_PgHost_Host_slots_empty(void *self, uint64 place, size_t array)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_slots_empty(kind, index, array);
}

/* pghost_PgHost_Host_slots_claim claims an entry of one of a record's slots arrays for bytes, reporting whether one was free. */
static bool
pghost_PgHost_Host_slots_claim(void *self, const HostMayRaise *raise, uint64 place, size_t array,
							   const uint8 *bytes, size_t bytes_len, bool *result, HostError *error)
{
	size_t		kind;
	uint32		index;
	PgPgError	error_;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	if (!shmem_records_slots_claim(pghost_pg_raise(raise), kind, index, array, bytes, bytes_len, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_slots_read returns the claimed entries of one of a record's slots arrays. */
static StdVec
pghost_PgHost_Host_slots_read(void *self, uint64 place, size_t array)
{
	size_t		kind;
	uint32		index;
	StdVec		result;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_slots_read(kind, index, array, &result);
	return result;
}

/* pghost_PgHost_Host_words_mine returns the place of this process's slot of a kind of words. */
static bool
pghost_PgHost_Host_words_mine(void *self, const HostSharedKind *kind, uint64 *result, HostError *error)
{
	(void) self;
	return pghost_shared_mine(kind->id, result, error);
}

/* pghost_PgHost_Host_word_store stores a word of a process's slot. */
static void
pghost_PgHost_Host_word_store(void *self, uint64 place, size_t word, uint64 value)
{
	size_t		kind;
	uint32		index;

	(void) self;
	pghost_shared_placed(place, &kind, &index);
	shmem_records_word_store(kind, word, value);
}

/* pghost_PgHost_Host_word_store_own stores a word of this process's own slot of the kind at an index. */
static void
pghost_PgHost_Host_word_store_own(void *self, size_t index, const HostSharedKind *kind, size_t word, uint64 value)
{
	(void) self;
	(void) kind;
	shmem_records_word_store(index, word, value);
}

/* pghost_PgHost_Host_word_load loads a word of a process's slot of a kind of words. */
static bool
pghost_PgHost_Host_word_load(void *self, const HostSharedKind *kind, uint32 proc, size_t word, uint64 *result,
							 HostError *error)
{
	(void) self;
	return pghost_shared_load(kind->id, proc, word, result, error);
}

/* pghost_PgHost_Host_words_place returns the place of a process's slot of a kind of words. */
static bool
pghost_PgHost_Host_words_place(void *self, const HostSharedKind *kind, uint32 proc, uint64 *result,
							   HostError *error)
{
	(void) self;
	return pghost_shared_place_of(kind->id, proc, result, error);
}

/* pghost_PgHost_Host_words_processes returns the processes whose slot of a kind of words can hold a word. */
static bool
pghost_PgHost_Host_words_processes(void *self, const HostSharedKind *kind, StdVec *result, HostError *error)
{
	(void) self;
	return pghost_shared_processes(kind->id, result, error);
}

/* pghost_PgHost_Host_epochs_lock takes the epoch lock exclusively. */
static bool
pghost_PgHost_Host_epochs_lock(void *self, const HostMayRaise *raise, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_records_epochs_lock(pghost_pg_raise(raise), &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_epochs_unlock releases the epoch lock. */
static void
pghost_PgHost_Host_epochs_unlock(void *self)
{
	(void) self;
	shmem_records_epochs_unlock();
}

/* pghost_PgHost_Host_epochs_advance advances the epoch under the epoch lock and returns it. */
static uint64
pghost_PgHost_Host_epochs_advance(void *self)
{
	(void) self;
	return shmem_records_epochs_advance();
}

/* pghost_PgHost_Host_epochs_oldest returns the oldest epoch a live reader holds. */
static uint64
pghost_PgHost_Host_epochs_oldest(void *self)
{
	(void) self;
	return shmem_records_epochs_oldest();
}

/* pghost_PgHost_Host_epochs_read starts a reader of this process at the current epoch. */
static bool
pghost_PgHost_Host_epochs_read(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_records_epochs_read(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_epochs_end ends a reader of this process. */
static void
pghost_PgHost_Host_epochs_end(void *self, uint64 epoch)
{
	(void) self;
	pghost_delta_ended(epoch);
}

/* pghost_PgHost_Host_proc_number returns the backend's proc number. */
static uint32
pghost_PgHost_Host_proc_number(void *self)
{
	(void) self;
	return shmem_records_proc_number();
}

/* pghost_PgHost_Host_delta_hold holds the delta at the current epoch. */
static bool
pghost_PgHost_Host_delta_hold(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	(void) self;
	return pghost_delta_hold(raise, result, error);
}

/* pghost_PgHost_Host_delta_release releases a hold of the delta. */
static void
pghost_PgHost_Host_delta_release(void *self, uint64 epoch)
{
	(void) self;
	pghost_delta_release(epoch);
}

/* pghost_PgHost_Host_delta_open opens a page of the delta's directory at an epoch. */
static bool
pghost_PgHost_Host_delta_open(void *self, HostDeltaPagePage name, uint64 epoch, HostDeltaDirectoryOpened *result,
							  HostError *error)
{
	(void) self;
	return pghost_delta_open(name, epoch, result, error);
}

/* pghost_PgHost_Host_delta_bytes returns bytes of an opened page. */
static bool
pghost_PgHost_Host_delta_bytes(void *self, const HostDeltaDirectoryOpened *opened, uint64 offset, uint64 len,
							   HostCacheBytes *result, HostError *error)
{
	(void) self;
	return pghost_delta_bytes_of(opened, offset, len, result, error);
}

/* pghost_PgHost_Host_delta_room returns the room an opened page has. */
static uint64
pghost_PgHost_Host_delta_room(void *self, const HostDeltaDirectoryOpened *opened)
{
	(void) self;
	return pghost_delta_room(opened);
}

/* pghost_PgHost_Host_delta_filled returns the bytes an opened page holds. */
static uint64
pghost_PgHost_Host_delta_filled(void *self, const HostDeltaDirectoryOpened *opened)
{
	(void) self;
	return pghost_delta_filled(opened);
}

/* pghost_PgHost_Host_delta_word does an operation on a word of a block's body. */
static uint64
pghost_PgHost_Host_delta_word(void *self, uint64 body, uint64 index, HostDeltaWordOp op)
{
	(void) self;
	return pghost_delta_word(body, index, op);
}

/* pghost_PgHost_Host_delta_alloc allocates blocks of the delta's sizes, naming their pages. */
static bool
pghost_PgHost_Host_delta_alloc(void *self, const HostMayRaise *raise, const HostDeltaPageSize *sizes,
							   size_t sizes_len, StdVec *names, bool *has_result, uint64 *result, HostError *error)
{
	(void) self;
	return pghost_delta_alloc(raise, sizes, sizes_len, names, has_result, result, error);
}

/* pghost_PgHost_Host_delta_alloc_replacing allocates a block in a page's place. */
static bool
pghost_PgHost_Host_delta_alloc_replacing(void *self, const HostMayRaise *raise, HostDeltaPageSize size,
										 HostDeltaPagePage replaced, StdVec *names, bool *has_result,
										 uint64 *result, HostError *error)
{
	(void) self;
	return pghost_delta_alloc_replacing(raise, size, replaced, names, has_result, result, error);
}

/* pghost_PgHost_Host_delta_give_back gives back a page this process allocated. */
static void
pghost_PgHost_Host_delta_give_back(void *self, HostDeltaPagePage name)
{
	(void) self;
	pghost_delta_give_back(name);
}

/* pghost_PgHost_Host_delta_owned_append appends a record to a page this process owns. */
static bool
pghost_PgHost_Host_delta_owned_append(void *self, HostDeltaPagePage name, const uint8 *record, size_t record_len,
									  uint32 *result, HostError *error)
{
	(void) self;
	return pghost_delta_owned_append(name, record, record_len, result, error);
}

/* pghost_PgHost_Host_delta_owned_store stores a word of a page this process owns. */
static bool
pghost_PgHost_Host_delta_owned_store(void *self, HostDeltaPagePage name, uint64 index, uint64 value,
									 HostError *error)
{
	(void) self;
	return pghost_delta_owned_store(name, index, value, error);
}

/* pghost_PgHost_Host_delta_link links a page to its owner. */
static void
pghost_PgHost_Host_delta_link(void *self, HostDeltaPagePage name, HostDeltaDirectoryOwner owner)
{
	(void) self;
	pghost_delta_link(name, owner);
}

/* pghost_PgHost_Host_delta_append appends a record to an opened page for its owner. */
static bool
pghost_PgHost_Host_delta_append(void *self, const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
								const uint8 *record, size_t record_len, uint32 *result, HostError *error)
{
	(void) self;
	return pghost_delta_append(opened, owner, record, record_len, result, error);
}

/* pghost_PgHost_Host_delta_seal seals an opened page for its owner. */
static bool
pghost_PgHost_Host_delta_seal(void *self, const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
							  HostError *error)
{
	(void) self;
	return pghost_delta_seal(opened, owner, error);
}

/* pghost_PgHost_Host_delta_retire retires records and pages the delta no longer holds. */
static void
pghost_PgHost_Host_delta_retire(void *self, const HostDeltaLeaving *records, size_t records_len,
								const HostDeltaPagePage *names, size_t names_len)
{
	(void) self;
	pghost_delta_retire(records, records_len, names, names_len);
}

/* pghost_PgHost_Host_delta_packing orders the delta's partly used blocks and returns the live pages a packing moves. */
static bool
pghost_PgHost_Host_delta_packing(void *self, const uint64 *shapes, size_t shapes_len, StdVec *result)
{
	(void) self;
	return pghost_delta_packing(shapes, shapes_len, result);
}

/* pghost_PgHost_Host_delta_make makes an aged record of a kind by a name, or answers the one of that name. */
static bool
pghost_PgHost_Host_delta_make(void *self, const HostMayRaise *raise, const HostSharedKind *kind, const char *name,
							  size_t name_len, bool *result_ok, uint64 *result_0, uint64 *result_1,
							  uint64 *result_err, HostError *error)
{
	(void) self;
	(void) raise;
	return pghost_delta_make(kind, name, name_len, result_ok, result_0, result_1, result_err, error);
}

/* pghost_PgHost_Host_delta_record returns the place of a kind's aged record at a location under an epoch. */
static bool
pghost_PgHost_Host_delta_record(void *self, const HostSharedKind *kind, uint64 location, uint64 epoch,
								uint64 *result, HostError *error)
{
	(void) self;
	return pghost_delta_record(kind, location, epoch, result, error);
}

/* pghost_PgHost_Host_delta_publish publishes an aged record this process made. */
static void
pghost_PgHost_Host_delta_publish(void *self, uint64 place)
{
	(void) self;
	pghost_delta_publish(place);
}

/* pghost_PgHost_Host_delta_unmake unmakes an aged record this process made. */
static void
pghost_PgHost_Host_delta_unmake(void *self, uint64 place)
{
	(void) self;
	pghost_delta_unmake(place);
}

/* pghost_PgHost_Host_delta_pass passes the delta's epoch on. */
static void
pghost_PgHost_Host_delta_pass(void *self)
{
	(void) self;
	pghost_delta_pass();
}

/* pghost_PgHost_Host_delta_advanced returns the epoch retiring and the epoch passing, advancing the epoch under the lock the caller took. */
static void
pghost_PgHost_Host_delta_advanced(void *self, uint64 *result_0, uint64 *result_1)
{
	uint64		passed = shmem_records_epochs_retiring();

	(void) self;
	(void) shmem_records_epochs_advance();
	shmem_records_epochs_unlock();
	*result_0 = passed;
	*result_1 = shmem_records_epochs_passing();
}

/* pghost_PgHost_Host_delta_used returns the delta's bytes in use. */
static uint64
pghost_PgHost_Host_delta_used(void *self)
{
	(void) self;
	return pghost_delta_used();
}

/* pghost_PgHost_Host_delta_budget returns the delta's budget of bytes. */
static uint64
pghost_PgHost_Host_delta_budget(void *self)
{
	(void) self;
	return (uint64) pghost_delta_bytes();
}

/* pghost_PgHost_Host_delta_seed returns the region's seed. */
static void
pghost_PgHost_Host_delta_seed(void *self, uint8 *result)
{
	(void) self;
	shmem_seed(result);
}

/* pghost_PgHost_Host_delta_wait waits at the delta's budget for frees past a count. */
static bool
pghost_PgHost_Host_delta_wait(void *self, const HostMayRaise *raise, uint64 frees, HostError *error)
{
	(void) self;
	return pghost_delta_wait(raise, frees, error);
}

/* pghost_PgHost_Host_backup_begin counts a running backup, which holds collections off. */
static bool
pghost_PgHost_Host_backup_begin(void *self, const HostMayRaise *raise, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_collection_backup_begin(pghost_pg_raise(raise), &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_backup_end uncounts a running backup. */
static void
pghost_PgHost_Host_backup_end(void *self)
{
	(void) self;
	shmem_collection_backup_end();
}

/* pghost_PgHost_Host_backups_running reports whether a backup runs. */
static bool
pghost_PgHost_Host_backups_running(void *self)
{
	(void) self;
	return shmem_collection_backups_running();
}

/* pghost_PgHost_Host_collection_begin begins a collection, unless a backup runs. */
static bool
pghost_PgHost_Host_collection_begin(void *self)
{
	(void) self;
	return shmem_collection_collection_begin();
}

/* pghost_PgHost_Host_collection_end ends a collection. */
static void
pghost_PgHost_Host_collection_end(void *self)
{
	(void) self;
	shmem_collection_collection_end();
}

/* pghost_PgHost_Host_collection_ask asks the collector to collect a database, emptying a tablespace, and waits for its counts. */
static bool
pghost_PgHost_Host_collection_ask(void *self, const HostMayRaise *raise, uint32 database, uint32 emptied,
								  uint64 *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_collection_ask(pghost_pg_raise(raise), database, emptied, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_collection_asked returns the unanswered ask's word, the database it asks for, and the tablespace it empties. */
static bool
pghost_PgHost_Host_collection_asked(void *self, uint64 *result_0, uint32 *result_1, uint32 *result_2)
{
	std_TupleU64U32_t asked;

	(void) self;
	if (!shmem_collection_asked_for(&asked, result_2))
		return false;
	*result_0 = asked.f0;
	*result_1 = asked.f1;
	return true;
}

/* pghost_PgHost_Host_collection_answer writes the collector's answer to an ask. */
static void
pghost_PgHost_Host_collection_answer(void *self, uint64 word, const uint64 *counts)
{
	(void) self;
	shmem_collection_answer(word, counts);
}

/* pghost_PgHost_Host_collector_started records that the collector runs. */
static void
pghost_PgHost_Host_collector_started(void *self)
{
	(void) self;
	shmem_collection_worker_started();
}

/* pghost_PgHost_Host_collector_running reports whether a collector worker has started. */
static bool
pghost_PgHost_Host_collector_running(void *self)
{
	(void) self;
	return shmem_collection_worker_running();
}

/* pghost_PgHost_Host_database_locked takes a database's lock without waiting, reporting whether it did. */
static bool
pghost_PgHost_Host_database_locked(void *self, const HostMayRaise *raise, uint32 database, bool *result,
								   HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_database_locked(pghost_pg_raise(raise), database, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_database_unlocked releases a database's lock. */
static void
pghost_PgHost_Host_database_unlocked(void *self, uint32 database)
{
	(void) self;
	pg_calls_database_unlocked(database);
}

/* pghost_PgHost_Host_database_lock_waited waits for a database's lock, reporting whether it took it. */
static bool
pghost_PgHost_Host_database_lock_waited(void *self, const HostMayRaise *raise, uint32 database, bool *result,
										HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!pg_calls_database_lock_waited(pghost_pg_raise(raise), database, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_cache_get returns a node the placement's cache holds, pinned, or nothing. */
static bool
pghost_PgHost_Host_cache_get(void *self, const HostMayRaise *raise, HostCachePlacement placement,
							 const HostCacheKey *key, bool *has_result, HostCacheFound *result, HostError *error)
{
	ShmemCacheCache cache;
	bool		has_found;
	std_TupleU32RefSliceU8_t found;
	PgPgError	error_;

	(void) self;
	if (!pghost_cache_of(placement, &cache))
	{
		*has_result = false;
		return true;
	}
	if (!shmem_cache_Cache_get(&cache, pghost_pg_raise(raise), host_cache_Key_bytes(key), &has_found, &found,
							   &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*has_result = has_found;
	if (has_found)
	{
		result->frame = found.f0;
		result->bytes.tag = HOST_CACHE_BYTES_MAPPED;
		result->bytes.u.mapped.f0 = found.f1;
		result->bytes.u.mapped.f0_len = found.f1_len;
	}
	return true;
}

/* pghost_PgHost_Host_cache_install installs a node in the placement's cache, filled by the caller, pinned, or nothing. */
static bool
pghost_PgHost_Host_cache_install(void *self, const HostMayRaise *raise, HostCachePlacement placement,
								 const HostCacheKey *key, size_t len, void (*fill) (void *, uint8 *, size_t),
								 void *fill_captures, bool *has_result, HostCacheFound *result, HostError *error)
{
	ShmemCacheCache cache;
	bool		has_found;
	std_TupleU32RefSliceU8_t found;
	PgPgError	error_;

	(void) self;
	if (!pghost_cache_of(placement, &cache))
	{
		*has_result = false;
		return true;
	}
	if (!shmem_cache_Cache_install(&cache, pghost_pg_raise(raise), host_cache_Key_bytes(key), len, fill,
								   fill_captures, &has_found, &found, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	*has_result = has_found;
	if (has_found)
	{
		result->frame = found.f0;
		result->bytes.tag = HOST_CACHE_BYTES_MAPPED;
		result->bytes.u.mapped.f0 = found.f1;
		result->bytes.u.mapped.f0_len = found.f1_len;
	}
	return true;
}

/* pghost_PgHost_Host_cache_contains reports whether the placement's cache holds a node. */
static bool
pghost_PgHost_Host_cache_contains(void *self, const HostMayRaise *raise, HostCachePlacement placement,
								  const HostCacheKey *key, bool *result, HostError *error)
{
	ShmemCacheCache cache;
	PgPgError	error_;

	(void) self;
	if (!pghost_cache_of(placement, &cache))
	{
		*result = false;
		return true;
	}
	if (!shmem_cache_Cache_contains(&cache, pghost_pg_raise(raise), host_cache_Key_bytes(key), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_cache_unpin gives up a pin of a frame of the placement's cache. */
static void
pghost_PgHost_Host_cache_unpin(void *self, HostCachePlacement placement, uint32 frame)
{
	ShmemCacheCache cache;

	(void) self;
	if (pghost_cache_of(placement, &cache))
		shmem_cache_Cache_unpin(&cache, frame);
}

/* pghost_PgHost_Host_cache_make_local makes the backend's own cache of a size. */
static bool
pghost_PgHost_Host_cache_make_local(void *self, const HostMayRaise *raise, size_t bytes, HostError *error)
{
	const PgMayRaise *pg_raise = pghost_pg_raise(raise);
	ShmemCacheLayout layout = pghost_cache_layout(bytes);
	PgPgError	error_;

	(void) self;
	if (!shmem_cache_make_local(pg_raise, &layout, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_owner_enlarge makes room for one more pin in the current resource owner. */
static bool
pghost_PgHost_Host_owner_enlarge(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_cache_owner_enlarge(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_owner_top_enlarge makes room for one more pin in the top resource owner. */
static bool
pghost_PgHost_Host_owner_top_enlarge(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	PgPgError	error_;

	(void) self;
	if (!shmem_cache_owner_top_enlarge(pghost_pg_raise(raise), result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_owner_remember remembers a pin in a resource owner. */
static void
pghost_PgHost_Host_owner_remember(void *self, uint64 owner, uint64 pin)
{
	(void) self;
	shmem_cache_owner_remember(owner, pin);
}

/* pghost_PgHost_Host_owner_forget forgets a pin in a resource owner. */
static void
pghost_PgHost_Host_owner_forget(void *self, uint64 owner, uint64 pin)
{
	(void) self;
	shmem_cache_owner_forget(owner, pin);
}

/* pghost_PgHost_Host_owned_remember remembers a record in a resource owner, whose release the shared cache's own does. */
static void
pghost_PgHost_Host_owned_remember(void *self, uint64 owner, uint64 record, void (*release) (uint32))
{
	(void) self;
	(void) release;
	shmem_cache_owned_remember(owner, record);
}

/* pghost_PgHost_Host_owned_forget forgets a record in a resource owner. */
static void
pghost_PgHost_Host_owned_forget(void *self, uint64 owner, uint64 record)
{
	(void) self;
	shmem_cache_owned_forget(owner, record);
}

/*
 * pghost_PgHost_Host_btree_compare calls a class's comparison function with
 * the catalog capability io_catalog put in host's, and refuses an Io made
 * without one, the one check at run time of a door that reaches the
 * catalog, which every entry that compares through a class passes.
 */
static bool
pghost_PgHost_Host_btree_compare(void *self, const HostMayRaise *raise, const HostCompared *compared,
								 const uint8 *a, size_t a_len, const uint8 *b, size_t b_len, int32 *result,
								 HostError *error)
{
	DgCompared	pg_compared = {compared->function, compared->collation, compared->len, compared->by_value};
	const PgCatalogCaps *caps = std_any_downcast(host_MayRaise_inner(raise), &PG_CATALOG_CAPS_TYPE_ID);
	PgPgError	error_;

	(void) self;
	if (caps != NULL)
	{
		if (!pg_calls_btree_compare(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), pg_compared, a,
									a_len, b, b_len, result, &error_))
		{
			*error = pghost_held(&error_);
			return false;
		}
		return true;
	}
	{
		StdString	message;

		std_string_from_Checked(&message, "a class's comparison function was called with no catalog access",
								sizeof("a class's comparison function was called with no catalog access") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/*
 * pghost_PgHost_Host_sort_key asks Postgres for a text's sort key with the
 * catalog capability io_catalog put in host's, and refuses an Io made
 * without one, as btree_compare does.
 */
static bool
pghost_PgHost_Host_sort_key(void *self, const HostMayRaise *raise, uint32 collation, const uint8 *text_,
							size_t text_len, StdVec *result, HostError *error)
{
	const PgCatalogCaps *caps = std_any_downcast(host_MayRaise_inner(raise), &PG_CATALOG_CAPS_TYPE_ID);
	PgPgError	error_;

	(void) self;
	if (caps != NULL)
	{
		if (!pg_sort_key_of(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), collation, text_, text_len,
							result, &error_))
		{
			*error = pghost_held(&error_);
			return false;
		}
		return true;
	}
	{
		StdString	message;

		std_string_from_Checked(&message, "a collation's sort key was asked for with no catalog access",
								sizeof("a collation's sort key was asked for with no catalog access") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/* PGHOST_REFORM is the OnceLock of the re-form the server's entries installed for the process. */
static StdOnceLock PGHOST_REFORM;

/* PGHOST_REFORM_VALUE is PGHOST_REFORM's value. */
static PghostReform PGHOST_REFORM_VALUE;

/*
 * pghost_PgHost_Host_reformed re-forms a fold's row through the re-form the
 * server's entries installed, with the catalog capability io_catalog put in
 * host's, and refuses an Io made without one, as btree_compare does.
 */
static bool
pghost_PgHost_Host_reformed(void *self, const HostMayRaise *raise, const HostReformRow *row,
							HostReformedRow *result, HostError *error)
{
	const PgCatalogCaps *caps = std_any_downcast(host_MayRaise_inner(raise), &PG_CATALOG_CAPS_TYPE_ID);
	PgPgError	error_;

	(void) self;
	if (caps == NULL)
	{
		StdString	message;

		std_string_from_Checked(&message, "a fold's row was re-formed with no catalog access",
								sizeof("a fold's row was re-formed with no catalog access") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (!std_once_lock_get(&PGHOST_REFORM))
	{
		StdString	message;

		std_string_from_Checked(&message, "a fold's row was re-formed in a process with no re-form",
								sizeof("a fold's row was re-formed in a process with no re-form") - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (!PGHOST_REFORM_VALUE(caps, row, result, &error_))
	{
		*error = pghost_held(&error_);
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_prepare_compressing_frame_0_t is prepare_compressing's state across its pad: the dictionary before its box holds it. */
typedef struct pghost_PgHost_Host_prepare_compressing_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	int32		level;
	HostCompressDictionary *result;
	HostError  *error;
	bool		done;
	PgZstdCompressDictionary prepared;
	bool		prepared_live;
} pghost_PgHost_Host_prepare_compressing_frame_0_t;

/* pghost_PgHost_Host_prepare_compressing_body_0 is prepare_compressing's body. */
static void
pghost_PgHost_Host_prepare_compressing_body_0(void *frame_)
{
	pghost_PgHost_Host_prepare_compressing_frame_0_t *frame = frame_;
	PgZstdCompressDictionary *boxed;
	StdAny		any;

	if (!pg_zstd_CompressDictionary_new(frame->bytes, frame->bytes_len, frame->level, &frame->prepared))
	{
		*frame->error = host_Error_out_of_memory();
		frame->done = false;
		return;
	}
	frame->prepared_live = true;
	boxed = std_box_new_uninit_Checked(sizeof(PgZstdCompressDictionary), _Alignof(PgZstdCompressDictionary));
	frame->prepared_live = false;
	*boxed = frame->prepared;
	any = (StdAny)
	{
		&PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID, boxed
	};
	*frame->result = host_CompressDictionary_new(&any);
	frame->done = true;
}

/* pghost_PgHost_Host_prepare_compressing_pad_0 is prepare_compressing's pad: the dictionary while it is live. */
static void
pghost_PgHost_Host_prepare_compressing_pad_0(void *frame_)
{
	pghost_PgHost_Host_prepare_compressing_frame_0_t *frame = frame_;

	if (frame->prepared_live)
		pg_zstd_CompressDictionary_drop(&frame->prepared);
}

/* pghost_PgHost_Host_prepare_compressing prepares a dictionary for compressing at a level, in pg's form. */
static bool
pghost_PgHost_Host_prepare_compressing(void *self, const uint8 *bytes, size_t bytes_len, int32 level,
									   HostCompressDictionary *result, HostError *error)
{
	pghost_PgHost_Host_prepare_compressing_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len,
	.level = level, .result = result, .error = error};

	(void) self;
	std_landing(&frame, pghost_PgHost_Host_prepare_compressing_body_0, pghost_PgHost_Host_prepare_compressing_pad_0);
	return frame.done;
}

/* pghost_PgHost_Host_compress returns bytes compressed as one frame, or an error when libzstd can't allocate. */
static bool
pghost_PgHost_Host_compress(void *self, const uint8 *bytes, size_t bytes_len, int32 level,
							const HostCompressDictionary *dictionary, StdVec *result, HostError *error)
{
	const PgZstdCompressDictionary *prepared = NULL;

	(void) self;
	if (dictionary != NULL)
		prepared = pghost_prepared_CompressDictionary(host_CompressDictionary_prepared(dictionary));
	if (!pg_zstd_compress(bytes, bytes_len, level, prepared, result))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	return true;
}

/* pghost_PgHost_Host_train returns a dictionary trained on samples, or nothing. */
static bool
pghost_PgHost_Host_train(void *self, const StdBytes *samples, size_t samples_len, size_t capacity, StdVec *result)
{
	(void) self;
	return pg_zstd_train(samples, samples_len, capacity, result);
}

/* pghost_prepared_DecompressDictionary is prepared::<DecompressDictionary>: a dictionary as pg prepared it, how this host prepares every one. */
static const PgZstdDecompressDictionary *
pghost_prepared_DecompressDictionary(StdAny dictionary)
{
	const PgZstdDecompressDictionary *prepared = std_any_downcast(dictionary, &PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID);

	if (prepared == NULL)
		STD_OPTION_EXPECT_FAILED(983, 31, "every dictionary is one this host prepared");
	return prepared;
}

/* pghost_prepared_CompressDictionary is prepared::<CompressDictionary>: a dictionary as pg prepared it, how this host prepares every one. */
static const PgZstdCompressDictionary *
pghost_prepared_CompressDictionary(StdAny dictionary)
{
	const PgZstdCompressDictionary *prepared = std_any_downcast(dictionary, &PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID);

	if (prepared == NULL)
		STD_OPTION_EXPECT_FAILED(983, 31, "every dictionary is one this host prepared");
	return prepared;
}

/* pghost_waits makes an entry's capability to wait on another backend, which only this crate makes, as it makes the Io. */
HostIoWaits
pghost_waits(void)
{
	return host_io_Waits_new_for_pghost();
}

/* pghost_io hands an entry's capability down to the storage crates as host's Io, which borrows it. */
HostIoIo
pghost_io(const PgMayRaise *raise)
{
	StdAny		any = {&PG_MAY_RAISE_TYPE_ID, (void *) raise};

	return host_io_Io_new_for_pghost(any);
}

/*
 * pghost_io_catalog hands a catalog entry's capabilities down to the storage
 * crates as host's Io, which borrows them, so that a door that reaches the
 * catalog, a comparison through a class's function, can open them again.
 */
HostIoIo
pghost_io_catalog(const PgCatalogCaps *caps)
{
	StdAny		any = {&PG_CATALOG_CAPS_TYPE_ID, (void *) caps};

	return host_io_Io_new_for_pghost(any);
}

/*
 * pghost_any returns seam 2's value of Postgres' SnapshotAny, which sees
 * every row version, as the executor reads the row an update replaces.
 */
HostSnapshot
pghost_any(void)
{
	HostXid		xmin = {0};
	HostXid		xmax = {0};
	HostCommand command = {0};
	StdAny		of = {&STD_UNIT_TYPE_ID, STD_DANGLING};

	return host_Snapshot_made(HOST_SNAPSHOT_KIND_ANY, xmin, xmax, command, false, false, STD_DANGLING, 0,
							  STD_DANGLING, 0, of);
}

/* PGHOST_SNAPSHOT_KINDS are Postgres' snapshot types and the kinds seam 2 gives them. */
static const struct
{
	int32		f0;
	HostSnapshotKind f1;
}			PGHOST_SNAPSHOT_KINDS[7] = {
	{SNAPSHOT_MVCC, HOST_SNAPSHOT_KIND_MVCC},
	{SNAPSHOT_SELF, HOST_SNAPSHOT_KIND_CURRENT},
	{SNAPSHOT_ANY, HOST_SNAPSHOT_KIND_ANY},
	{SNAPSHOT_TOAST, HOST_SNAPSHOT_KIND_TOAST},
	{SNAPSHOT_DIRTY, HOST_SNAPSHOT_KIND_DIRTY},
	{SNAPSHOT_HISTORIC_MVCC, HOST_SNAPSHOT_KIND_HISTORIC_MVCC},
	{SNAPSHOT_NON_VACUUMABLE, HOST_SNAPSHOT_KIND_NON_VACUUMABLE},
};

/*
 * pghost_snapshot reads a snapshot an entry got into seam 2's value, its
 * arrays lent for as long as Postgres' snapshot is, which the value keeps
 * for its kind's doors.
 */
bool
pghost_snapshot(const PgMayRaise *raise, const PgSnapshot *snapshot, HostSnapshot *result, PgPgError *error)
{
	PgSnapshotView view;
	bool		found = false;
	HostSnapshotKind kind = HOST_SNAPSHOT_KIND_MVCC;

	if (!pg_snapshot_view(raise, snapshot, &view, error))
		return false;
	for (size_t i = 0, i_len = lengthof(PGHOST_SNAPSHOT_KINDS); i < i_len; i++)
	{
		if ((int64) PGHOST_SNAPSHOT_KINDS[i].f0 == (int64) view.kind)
		{
			kind = PGHOST_SNAPSHOT_KINDS[i].f1;
			found = true;
			break;
		}
	}
	if (!found)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("a snapshot of type "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is no type Postgres has")};
		StdFmtArg	args[] = {{&view.kind, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return false;
	}
	{
		HostXid		xmin = {view.xmin};
		HostXid		xmax = {view.xmax};
		HostCommand command = {view.command};
		StdAny		of = {&PG_SNAPSHOT_TYPE_ID, (void *) snapshot};

		*result = host_Snapshot_made(kind, xmin, xmax, command, view.recovery, view.overflowed, view.xip,
									 view.xip_len, view.subxip, view.subxip_len, of);
	}
	return true;
}

/* pghost_pg_raise returns the capability of pg's that io or io_catalog put in host's. */
const PgMayRaise *
pghost_pg_raise(const HostMayRaise *raise)
{
	const PgCatalogCaps *caps = std_any_downcast(host_MayRaise_inner(raise), &PG_CATALOG_CAPS_TYPE_ID);
	const PgMayRaise *may_raise;

	if (caps != NULL)
		return pg_CatalogCaps_may_raise(caps);
	may_raise = std_any_downcast(host_MayRaise_inner(raise), &PG_MAY_RAISE_TYPE_ID);
	if (may_raise == NULL)
		STD_OPTION_EXPECT_FAILED(1041, 46, "pghost makes host's capability only around pg's");
	return may_raise;
}

/* pghost_cache_layout_frame_0_t is cache_layout's map's state across its pad, as core's [T; N]::map runs it: its Drain over the classes and its Guard over the sizes it wrote. */
typedef struct pghost_cache_layout_frame_0_t
{
	size_t	   *result;
	size_t		drained;
	size_t		initialized;
} pghost_cache_layout_frame_0_t;

/* pghost_cache_layout_body_0 is cache_layout's map: each class's size, with get_unchecked_mut's and unchecked_add's preconditions. */
static void
pghost_cache_layout_body_0(void *frame_)
{
	pghost_cache_layout_frame_0_t *frame = frame_;

	while (frame->initialized < HOST_CACHE_CLASSES_LEN)
	{
		size_t		size = HOST_CACHE_CLASSES[frame->drained].f0;

		frame->drained = std_wrapping_add_usize(frame->drained, 1);
		std_slice_get_unchecked_mut_check(frame->initialized, HOST_CACHE_CLASSES_LEN, STD_ARRAY_MOD_FILE, 954, 28);
		frame->result[frame->initialized] = size;
		std_usize_unchecked_add_check(frame->initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
		frame->initialized = std_wrapping_add_usize(frame->initialized, 1);
	}
	std_array_Drain_drop(&HOST_CACHE_CLASSES[frame->drained], &HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN],
						 sizeof(HOST_CACHE_CLASSES[0]));
}

/* pghost_cache_layout_pad_0 is cache_layout's map's pad: the Guard's drop, then the Drain's. */
static void
pghost_cache_layout_pad_0(void *frame_)
{
	pghost_cache_layout_frame_0_t *frame = frame_;

	std_array_Guard_drop(frame->initialized, HOST_CACHE_CLASSES_LEN);
	std_array_Drain_drop(&HOST_CACHE_CLASSES[frame->drained], &HOST_CACHE_CLASSES[HOST_CACHE_CLASSES_LEN],
						 sizeof(HOST_CACHE_CLASSES[0]));
}

/* pghost_cache_layout returns the frames of a node cache of a size, by host's classes. */
ShmemCacheLayout
pghost_cache_layout(size_t bytes)
{
	ShmemCacheLayout layout;
	pghost_cache_layout_frame_0_t frame = {.result = layout.sizes};

	host_cache_frames(bytes, layout.counts);
	std_landing(&frame, pghost_cache_layout_body_0, pghost_cache_layout_pad_0);
	return layout;
}

/* pghost_cache_of returns a placement's cache in this process, or nothing for the backend's own before it is made. */
static bool
pghost_cache_of(HostCachePlacement placement, ShmemCacheCache *result)
{
	switch (placement)
	{
		case HOST_CACHE_PLACEMENT_SERVER:
			return shmem_cache_shared(result);
		case HOST_CACHE_PLACEMENT_BACKEND:
			return shmem_cache_local(result);
	}
	pg_unreachable();
}

/* pghost_raised returns the error an entry raises for an error a seam returned; it takes the error. */
PgPgError
pghost_raised(HostError *error)
{
	StdAny		cause;
	HostForked	forked;
	PgPgError	result;

	if (host_Error_into_cause(error, &cause, &forked))
	{
		PgPgError  *raised = std_any_downcast(cause, &PG_PG_ERROR_TYPE_ID);

		if (raised == NULL)
			STD_RESULT_EXPECT_FAILED(1061, 55, "pghost holds only pg's errors", &cause, std_any_Debug_fmt);
		result = *raised;
		std_free(cause.value, sizeof(PgPgError), _Alignof(PgPgError));
		return result;
	}
	return pg_PgError_hinted(forked.f0, &forked.f1, forked.f2);
}

/* pghost_held_closure_0 is from_fn's closure: the SQLSTATE's character at an index, decoded from Postgres' encoding. */
static uint8
pghost_held_closure_0(int32 code, size_t i)
{
	size_t		tmp_1 = STD_MUL(usize, 6, i, 1069, 62);
	int32		tmp_2 = STD_SHR(i32, code, (uint64) tmp_1, 1069, 53);

	return STD_ADD(u8, '0', (uint8) (tmp_2 & 0x3F), 1069, 44);
}

/* pghost_held_frame_0_t is held's state across its pad: the error until its box holds it, and from_fn's Guard. */
typedef struct pghost_held_frame_0_t
{
	PgPgError  *error;
	HostError  *result;
	bool		error_live;
	int32		code;
	uint8		sqlstate[5];
	size_t		initialized;
} pghost_held_frame_0_t;

/* pghost_held_body_0 is held's body. */
static void
pghost_held_body_0(void *frame_)
{
	pghost_held_frame_0_t *frame = frame_;
	PgPgError  *boxed;
	StdAny		any;

	frame->code = pg_PgError_sqlerrcode(frame->error);
	while (frame->initialized < 5)
	{
		uint8		item = pghost_held_closure_0(frame->code, frame->initialized);

		std_slice_get_unchecked_mut_check(frame->initialized, 5, STD_ARRAY_MOD_FILE, 954, 28);
		frame->sqlstate[frame->initialized] = item;
		std_usize_unchecked_add_check(frame->initialized, 1, STD_ARRAY_MOD_FILE, 955, 49);
		frame->initialized = std_wrapping_add_usize(frame->initialized, 1);
	}
	boxed = std_box_new_uninit_Checked(sizeof(PgPgError), _Alignof(PgPgError));
	frame->error_live = false;
	*boxed = *frame->error;
	any = (StdAny)
	{
		&PG_PG_ERROR_TYPE_ID, boxed
	};
	*frame->result = host_Error_postgres(frame->sqlstate, &any);
}

/* pghost_held_pad_0 is held's pad: from_fn's Guard's drop, then the error while it is live. */
static void
pghost_held_pad_0(void *frame_)
{
	pghost_held_frame_0_t *frame = frame_;

	if (frame->initialized < 5)
		std_array_Guard_drop(frame->initialized, 5);
	if (frame->error_live)
		pg_PgError_drop(frame->error);
}

/* pghost_held holds an error Postgres raised for the storage crates, with its SQLSTATE decoded from Postgres' encoding; it takes the error. */
HostError
pghost_held(PgPgError *error)
{
	HostError	result;
	pghost_held_frame_0_t frame = {.error = error, .result = &result, .error_live = true};

	std_landing(&frame, pghost_held_body_0, pghost_held_pad_0);
	return result;
}

/* The stable sort's shared parts at String, which its instance below calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS String
#define STD_SORT_T StdString
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"


/* pghost_String_PartialOrd_lt_FnMut_call_mut is the fn item String's PartialOrd::lt as the stable sort's is_less. */
static bool
pghost_String_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_String_PartialOrd_lt(a, b);
}

/* sort at String, which names sorts its names with. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS String
#define STD_STABLE_SORT_T StdString
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT pghost_String_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/* pghost_names_closure_0 is split's predicate: whether a byte is the NUL that ends a name. */
static bool
pghost_names_closure_0(void *captures, const uint8 *b)
{
	(void) captures;
	return *b == 0;
}

/* pghost_names_frame_0_t is names' state across its pad: the names collected so far. */
typedef struct pghost_names_frame_0_t
{
	const uint8 *listing;
	size_t		listing_len;
	StdVec	   *result;
	bool		result_live;
} pghost_names_frame_0_t;

/* pghost_names_body_0 is names' body: the listing split at its NULs, the empty pieces left out, each piece as lossy text. */
static void
pghost_names_body_0(void *frame_)
{
	pghost_names_frame_0_t *frame = frame_;
	StdSliceSplit split = std_slice_split_u8(frame->listing, frame->listing_len, pghost_names_closure_0, NULL);
	const uint8 *piece;
	size_t		piece_len;

	*frame->result = (StdVec) STD_VEC_NEW;
	frame->result_live = true;
	while (std_SliceSplit_next(&split, &piece, &piece_len))
	{
		StdCowStr	lossy;
		StdString	name;

		if (piece_len == 0)
			continue;
		std_string_from_utf8_lossy(piece, piece_len, &lossy);
		std_CowStr_into_owned(&lossy, &name);
		std_vec_push_Checked(frame->result, &name, sizeof(StdString), _Alignof(StdString));
	}
	std_slice_sort_String(frame->result->ptr, frame->result->len);
	frame->result_live = false;
}

/* pghost_names_pad_0 is names' pad: the names collected so far, while they are live. */
static void
pghost_names_pad_0(void *frame_)
{
	pghost_names_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_drop_Checked(frame->result, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* pghost_names returns a directory listing's names, which a NUL ends each of, in byte order. */
static void
pghost_names(const uint8 *listing, size_t listing_len, StdVec *result)
{
	pghost_names_frame_0_t frame = {.listing = listing, .listing_len = listing_len, .result = result};

	std_landing(&frame, pghost_names_body_0, pghost_names_pad_0);
}

/* pghost_c_path_closure_0 is ok_or_else's closure: the internal error of a path that holds a NUL byte. */
static HostError
pghost_c_path_closure_0(const char *path, size_t path_len)
{
	StdStr		shown = {path, path_len};
	StdFmtPiece pieces[] = {STD_FMT_LIT("path \""), STD_FMT_ARG(0), STD_FMT_LIT("\" holds a NUL byte")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* pghost_c_path returns a path as Postgres takes it, refusing one holding a NUL byte. */
static bool
pghost_c_path(const char *path, size_t path_len, StdCString *result, HostError *error)
{
	if (pghost_canonical(path, path_len, result))
		return true;
	*error = pghost_c_path_closure_0(path, path_len);
	return false;
}

/* pghost_canonical_frame_0_t is canonical's state across its pad: its arguments, its result, and the path's bytes. */
typedef struct pghost_canonical_frame_0_t
{
	const char *path;
	size_t		path_len;
	StdCString *result;
	bool		done;
	StdVec		bytes;
	bool		bytes_live;
} pghost_canonical_frame_0_t;

/* pghost_canonical_body_0 is canonical's body. */
static void
pghost_canonical_body_0(void *frame_)
{
	pghost_canonical_frame_0_t *frame = frame_;
	StdCString	made;
	StdNulError nul;
	const char *until;
	StdFromBytesUntilNulError until_error;

	if (!std_CString_new_bytes((const uint8 *) frame->path, frame->path_len, &made, &nul))
	{
		std_NulError_drop(&nul);
		frame->done = false;
		return;
	}
	std_CString_into_bytes_with_nul(&made, &frame->bytes);
	frame->bytes_live = true;
	pg_calls_canonicalize_path(frame->bytes.ptr, frame->bytes.len);
	if (!std_CStr_from_bytes_until_nul(frame->bytes.ptr, frame->bytes.len, &until, &until_error))
		STD_RESULT_EXPECT_FAILED(1091, 45, "canonicalize_path keeps the path's NUL", &until_error,
								 std_FromBytesUntilNulError_Debug_fmt);
	std_CStr_to_owned(until, frame->result);
	frame->done = true;
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* pghost_canonical_pad_0 is canonical's pad: the path's bytes while they are live. */
static void
pghost_canonical_pad_0(void *frame_)
{
	pghost_canonical_frame_0_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * pghost_canonical returns a path as Postgres takes it, canonical as
 * Postgres' canonicalize_path makes it, so its folders are separated by / on
 * every platform, or nothing for one holding a NUL byte, which names no
 * file.
 */
static bool
pghost_canonical(const char *path, size_t path_len, StdCString *result)
{
	pghost_canonical_frame_0_t frame = {.path = path, .path_len = path_len, .result = result};

	std_landing(&frame, pghost_canonical_body_0, pghost_canonical_pad_0);
	return frame.done;
}

/* pghost_signed returns a file offset or length as Postgres' signed one, refusing one past its range. */
static bool
pghost_signed(uint64 value, int64 *result, HostError *error)
{
	StdTryFromIntError tried;

	if (std_i64_try_from_u64(value, result, &tried))
		return true;
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("file offset "), STD_FMT_ARG(0), STD_FMT_LIT(" is too large")};
		StdFmtArg	args[] = {{&value, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = host_Error_fork((const uint8 *) "54000", &message);
		return false;
	}
}

/* pghost_since_postgres_epoch returns the time of day a Postgres timestamp names, in microseconds since Postgres' epoch. */
static StdSystemTime
pghost_since_postgres_epoch(int64 micros)
{
	StdSystemTime epoch = std_SystemTime_add(STD_UNIX_EPOCH, std_Duration_from_secs(PGHOST_POSTGRES_EPOCH), RUST_FILE,
											 1101, 17);
	StdDuration offset = std_Duration_from_micros(std_i64_unsigned_abs(micros));

	if (micros < 0)
		return std_SystemTime_sub(epoch, offset, RUST_FILE, 1103, 21);
	return std_SystemTime_add(epoch, offset, RUST_FILE, 1103, 45);
}

/* pghost_install_reform_closure_0 is get_or_init's closure: the re-form given. */
static void
pghost_install_reform_closure_0(void *captures, void *result)
{
	*(PghostReform *) result = *(const PghostReform *) captures;
}

/* pghost_install_reform installs the server's re-form of a fold's row in this process, if it has none yet. */
void
pghost_install_reform(PghostReform reform)
{
	std_once_lock_get_or_init(&PGHOST_REFORM, &PGHOST_REFORM_VALUE, pghost_install_reform_closure_0, &reform);
}

/* PGHOST_PG_HOST_HOST_OPS is PgHost's Host table, its ReadHost table first. */
static const HostHostOps PGHOST_PG_HOST_HOST_OPS = {
	.read_host = {
		.open_to_read = pghost_PgHost_ReadHost_open_to_read,
		.open_if_there = pghost_PgHost_ReadHost_open_if_there,
		.read_at = pghost_PgHost_ReadHost_read_at,
		.size = pghost_PgHost_ReadHost_size,
		.prefetch = pghost_PgHost_ReadHost_prefetch,
		.close = pghost_PgHost_ReadHost_close,
		.tablespace_root = pghost_PgHost_ReadHost_tablespace_root,
		.list_dir = pghost_PgHost_ReadHost_list_dir,
		.list_dir_if_there = pghost_PgHost_ReadHost_list_dir_if_there,
		.prepare_decompressing = pghost_PgHost_ReadHost_prepare_decompressing,
		.frame_size = pghost_PgHost_ReadHost_frame_size,
		.decompress = pghost_PgHost_ReadHost_decompress,
	},
	.now = pghost_PgHost_Host_now,
	.instant = pghost_PgHost_Host_instant,
	.process_id = pghost_PgHost_Host_process_id,
	.work_mem = pghost_PgHost_Host_work_mem,
	.session_database = pghost_PgHost_Host_session_database,
	.exiting = pghost_PgHost_Host_exiting,
	.unique = pghost_PgHost_Host_unique,
	.check_interrupts = pghost_PgHost_Host_check_interrupts,
	.hold_interrupts = pghost_PgHost_Host_hold_interrupts,
	.resume_interrupts = pghost_PgHost_Host_resume_interrupts,
	.open_ = pghost_PgHost_Host_open,
	.create_if_absent = pghost_PgHost_Host_create_if_absent,
	.open_scratch = pghost_PgHost_Host_open_scratch,
	.write_at = pghost_PgHost_Host_write_at,
	.truncate = pghost_PgHost_Host_truncate,
	.sync = pghost_PgHost_Host_sync,
	.rename_ = pghost_PgHost_Host_rename,
	.rename_durably = pghost_PgHost_Host_rename_durably,
	.remove_durably = pghost_PgHost_Host_remove_durably,
	.remove_durably_if_there = pghost_PgHost_Host_remove_durably_if_there,
	.remove_leftover = pghost_PgHost_Host_remove_leftover,
	.rename_back = pghost_PgHost_Host_rename_back,
	.remove_tree = pghost_PgHost_Host_remove_tree,
	.sync_path = pghost_PgHost_Host_sync_path,
	.make_dir = pghost_PgHost_Host_make_dir,
	.disk_space = pghost_PgHost_Host_disk_space,
	.tree_bytes = pghost_PgHost_Host_tree_bytes,
	.nap = pghost_PgHost_Host_nap,
	.wal_insert = pghost_PgHost_Host_wal_insert,
	.wal_new_cid = pghost_PgHost_Host_wal_new_cid,
	.wal_flush = pghost_PgHost_Host_wal_flush,
	.fold_point = pghost_PgHost_Host_fold_point,
	.checkpoint_now = pghost_PgHost_Host_checkpoint_now,
	.quiet_database = pghost_PgHost_Host_quiet_database,
	.unquiet_database = pghost_PgHost_Host_unquiet_database,
	.last_redo = pghost_PgHost_Host_last_redo,
	.wal_stand = pghost_PgHost_Host_wal_stand,
	.warn_kept_wal = pghost_PgHost_Host_warn_kept_wal,
	.next_fork_record = pghost_PgHost_Host_next_fork_record,
	.warn = pghost_PgHost_Host_warn,
	.log = pghost_PgHost_Host_log,
	.refuse_recovery = pghost_PgHost_Host_refuse_recovery,
	.delay_checkpoints = pghost_PgHost_Host_delay_checkpoints,
	.load_pauses = pghost_PgHost_Host_load_pauses,
	.resume_checkpoints = pghost_PgHost_Host_resume_checkpoints,
	.fail_checkpoints = pghost_PgHost_Host_fail_checkpoints,
	.xid_status = pghost_PgHost_Host_xid_status,
	.xid_in_progress = pghost_PgHost_Host_xid_in_progress,
	.xids_answered = pghost_PgHost_Host_xids_answered,
	.removable_in = pghost_PgHost_Host_removable_in,
	.current_writer = pghost_PgHost_Host_current_writer,
	.current_command = pghost_PgHost_Host_current_command,
	.xid_is_current = pghost_PgHost_Host_xid_is_current,
	.combo_command = pghost_PgHost_Host_combo_command,
	.key_lock = pghost_PgHost_Host_key_lock,
	.key_unlock = pghost_PgHost_Host_key_unlock,
	.key_count = pghost_PgHost_Host_key_count,
	.key_hold = pghost_PgHost_Host_key_hold,
	.key_release = pghost_PgHost_Host_key_release,
	.key_held_wait = pghost_PgHost_Host_key_held_wait,
	.xact_wait = pghost_PgHost_Host_xact_wait,
	.speculative_wait = pghost_PgHost_Host_speculative_wait,
	.lock_tuple = pghost_PgHost_Host_lock_tuple,
	.unlock_tuple = pghost_PgHost_Host_unlock_tuple,
	.actor = pghost_PgHost_Host_actor,
	.yield_point = pghost_PgHost_Host_yield_point,
	.pause = pghost_PgHost_Host_pause,
	.injected = pghost_PgHost_Host_injected,
	.at_exit = pghost_PgHost_Host_at_exit,
	.prepare_compressing = pghost_PgHost_Host_prepare_compressing,
	.compress = pghost_PgHost_Host_compress,
	.train = pghost_PgHost_Host_train,
	.shared_make = pghost_PgHost_Host_shared_make,
	.shared_publish = pghost_PgHost_Host_shared_publish,
	.shared_claim = pghost_PgHost_Host_shared_claim,
	.shared_free = pghost_PgHost_Host_shared_free,
	.shared_give_up = pghost_PgHost_Host_shared_give_up,
	.shared_open = pghost_PgHost_Host_shared_open,
	.shared_close = pghost_PgHost_Host_shared_close,
	.shared_reclaim = pghost_PgHost_Host_shared_reclaim,
	.shared_reclaimed = pghost_PgHost_Host_shared_reclaimed,
	.shared_unreclaim = pghost_PgHost_Host_shared_unreclaim,
	.shared_sweep = pghost_PgHost_Host_shared_sweep,
	.shared_index_find = pghost_PgHost_Host_shared_index_find,
	.shared_index_put = pghost_PgHost_Host_shared_index_put,
	.shared_index_remove = pghost_PgHost_Host_shared_index_remove,
	.shared_single = pghost_PgHost_Host_shared_single,
	.shared_ready = pghost_PgHost_Host_shared_ready,
	.shared_lock = pghost_PgHost_Host_shared_lock,
	.shared_unlock = pghost_PgHost_Host_shared_unlock,
	.flag_take = pghost_PgHost_Host_flag_take,
	.flag_try = pghost_PgHost_Host_flag_try,
	.flag_clear = pghost_PgHost_Host_flag_clear,
	.atomic = pghost_PgHost_Host_atomic,
	.shared_atomics = pghost_PgHost_Host_shared_atomics,
	.blob_read = pghost_PgHost_Host_blob_read,
	.blob_write = pghost_PgHost_Host_blob_write,
	.entry_read = pghost_PgHost_Host_entry_read,
	.entry_write = pghost_PgHost_Host_entry_write,
	.shared_location = pghost_PgHost_Host_shared_location,
	.shared_live = pghost_PgHost_Host_shared_live,
	.shared_gone = pghost_PgHost_Host_shared_gone,
	.slots_name = pghost_PgHost_Host_slots_name,
	.slots_rename = pghost_PgHost_Host_slots_rename,
	.slots_empty = pghost_PgHost_Host_slots_empty,
	.slots_claim = pghost_PgHost_Host_slots_claim,
	.slots_read = pghost_PgHost_Host_slots_read,
	.words_processes = pghost_PgHost_Host_words_processes,
	.words_mine = pghost_PgHost_Host_words_mine,
	.word_store = pghost_PgHost_Host_word_store,
	.word_store_own = pghost_PgHost_Host_word_store_own,
	.word_load = pghost_PgHost_Host_word_load,
	.words_place = pghost_PgHost_Host_words_place,
	.epochs_lock = pghost_PgHost_Host_epochs_lock,
	.epochs_unlock = pghost_PgHost_Host_epochs_unlock,
	.epochs_advance = pghost_PgHost_Host_epochs_advance,
	.epochs_oldest = pghost_PgHost_Host_epochs_oldest,
	.epochs_read = pghost_PgHost_Host_epochs_read,
	.epochs_end = pghost_PgHost_Host_epochs_end,
	.proc_number = pghost_PgHost_Host_proc_number,
	.delta_hold = pghost_PgHost_Host_delta_hold,
	.delta_release = pghost_PgHost_Host_delta_release,
	.delta_open = pghost_PgHost_Host_delta_open,
	.delta_bytes = pghost_PgHost_Host_delta_bytes,
	.delta_room = pghost_PgHost_Host_delta_room,
	.delta_filled = pghost_PgHost_Host_delta_filled,
	.delta_word = pghost_PgHost_Host_delta_word,
	.delta_alloc = pghost_PgHost_Host_delta_alloc,
	.delta_alloc_replacing = pghost_PgHost_Host_delta_alloc_replacing,
	.delta_give_back = pghost_PgHost_Host_delta_give_back,
	.delta_owned_append = pghost_PgHost_Host_delta_owned_append,
	.delta_owned_store = pghost_PgHost_Host_delta_owned_store,
	.delta_link = pghost_PgHost_Host_delta_link,
	.delta_append = pghost_PgHost_Host_delta_append,
	.delta_seal = pghost_PgHost_Host_delta_seal,
	.delta_retire = pghost_PgHost_Host_delta_retire,
	.delta_packing = pghost_PgHost_Host_delta_packing,
	.delta_make = pghost_PgHost_Host_delta_make,
	.delta_record = pghost_PgHost_Host_delta_record,
	.delta_publish = pghost_PgHost_Host_delta_publish,
	.delta_unmake = pghost_PgHost_Host_delta_unmake,
	.delta_pass = pghost_PgHost_Host_delta_pass,
	.delta_advanced = pghost_PgHost_Host_delta_advanced,
	.delta_used = pghost_PgHost_Host_delta_used,
	.delta_budget = pghost_PgHost_Host_delta_budget,
	.delta_seed = pghost_PgHost_Host_delta_seed,
	.delta_wait = pghost_PgHost_Host_delta_wait,
	.backup_begin = pghost_PgHost_Host_backup_begin,
	.backup_end = pghost_PgHost_Host_backup_end,
	.backups_running = pghost_PgHost_Host_backups_running,
	.collection_begin = pghost_PgHost_Host_collection_begin,
	.collection_end = pghost_PgHost_Host_collection_end,
	.collection_ask = pghost_PgHost_Host_collection_ask,
	.collection_asked = pghost_PgHost_Host_collection_asked,
	.collection_answer = pghost_PgHost_Host_collection_answer,
	.collector_started = pghost_PgHost_Host_collector_started,
	.collector_running = pghost_PgHost_Host_collector_running,
	.database_locked = pghost_PgHost_Host_database_locked,
	.database_unlocked = pghost_PgHost_Host_database_unlocked,
	.database_lock_waited = pghost_PgHost_Host_database_lock_waited,
	.cache_get = pghost_PgHost_Host_cache_get,
	.cache_install = pghost_PgHost_Host_cache_install,
	.cache_contains = pghost_PgHost_Host_cache_contains,
	.cache_unpin = pghost_PgHost_Host_cache_unpin,
	.cache_make_local = pghost_PgHost_Host_cache_make_local,
	.owner_enlarge = pghost_PgHost_Host_owner_enlarge,
	.owner_top_enlarge = pghost_PgHost_Host_owner_top_enlarge,
	.owner_remember = pghost_PgHost_Host_owner_remember,
	.owner_forget = pghost_PgHost_Host_owner_forget,
	.owned_remember = pghost_PgHost_Host_owned_remember,
	.owned_forget = pghost_PgHost_Host_owned_forget,
	.btree_compare = pghost_PgHost_Host_btree_compare,
	.sort_key = pghost_PgHost_Host_sort_key,
	.reformed = pghost_PgHost_Host_reformed,
};

/* pghost_install installs the server's host in this process, if it has none yet. */
void
pghost_install(void)
{
	if (!host_installed())
	{
		HostHost	host = {&PGHOST_PG_HOST_HOST_OPS, &PGHOST_HOST};
		HostRefused refused;

		if (!host_install(host, &refused))
			STD_RESULT_EXPECT_FAILED(1121, 30, "no host is installed yet", &refused, host_Refused_Debug_fmt);
	}
}

#ifdef DOLTGRES_TEST_PGHOST

/* test_pghost_tests_timestamps_convert_from_postgres_epoch: a Postgres timestamp converts to the time of day it names, before and after Postgres' epoch. */
void
test_pghost_tests_timestamps_convert_from_postgres_epoch(void)
{
	StdSystemTime epoch = std_SystemTime_add(STD_UNIX_EPOCH, std_Duration_from_secs(PGHOST_POSTGRES_EPOCH), RUST_FILE,
											 1132, 21);

	{
		StdSystemTime left = pghost_since_postgres_epoch(0);

		STD_ASSERT_EQ(1133, 9, std_SystemTime_eq(&left, &epoch), &left, std_SystemTime_Debug_fmt, &epoch,
					  std_SystemTime_Debug_fmt);
	}
	{
		StdSystemTime left = pghost_since_postgres_epoch(1500000);
		StdSystemTime right = std_SystemTime_add(epoch, std_Duration_from_micros(1500000), RUST_FILE, 1134, 53);

		STD_ASSERT_EQ(1134, 9, std_SystemTime_eq(&left, &right), &left, std_SystemTime_Debug_fmt, &right,
					  std_SystemTime_Debug_fmt);
	}
	{
		StdSystemTime left = pghost_since_postgres_epoch((int64) (-(int64) PGHOST_POSTGRES_EPOCH * 1000000));

		STD_ASSERT_EQ(1135, 9, std_SystemTime_eq(&left, &STD_UNIX_EPOCH), &left, std_SystemTime_Debug_fmt,
					  &STD_UNIX_EPOCH, std_SystemTime_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_PGHOST */

#endif							/* FRONTEND */
