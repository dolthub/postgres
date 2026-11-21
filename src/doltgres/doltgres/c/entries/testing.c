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
 * The bodies of the test module's entries (doltgres' entries/testing.rs),
 * built only with injection points: each the body of the entry of its name
 * in pg's test-entries.list, with the shared kinds and exit callbacks the
 * tests keep.
 */
#ifndef FRONTEND
#include "postgres.h"

#ifdef USE_INJECTION_POINTS
#include "catalog/pg_class_d.h"
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/key.h"
#include "doltgres/access/slot.h"
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/entries_tail.h"
#include "doltgres/doltgres/entries/testing.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/doltgres/kinds.h"
#include "doltgres/doltgres/layout.h"
#include "doltgres/encoding/key.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/owned.h"
#include "doltgres/host/shared.h"
#include "doltgres/host/shared/word.h"
#include "doltgres/pg/word.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/pghost/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/shmem/cache.h"
#include "doltgres/shmem/delta.h"
#include "doltgres/shmem/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/btree.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/lib.h"
#include "doltgres/store/session.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/published.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/vacuum.h"
#include "pg_calls.h"
#include "testing_corpus.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "entries" STD_SEP "testing.rs"

/*
 * doltgres_entries_testing_text_datum returns a text as a text datum, as
 * pg::text_datum of the text's CString, whose expect at a line and column of
 * testing.rs names a text that holds a NUL; it takes the text.
 */
static bool
doltgres_entries_testing_text_datum(const PgMayRaise *raise, StdString *owned, const char *expect, uint32 line,
									uint32 col, PgDatum *result, PgPgError *error)
{
	StdCString	c_text;
	StdNulError nul;
	bool		ok;

	if (!std_CString_new_String(owned, &c_text, &nul))
		std_result_unwrap_failed(expect, strlen(expect), &nul, std_NulError_Debug_fmt, RUST_FILE, line, col);
	ok = pg_calls_text_datum(raise, std_CString_as_ptr(&c_text), result, error);
	std_CString_drop(&c_text);
	return ok;
}

/*
 * doltgres_entries_testing_doltgres_test_shmem_store is the test module's
 * store: it writes the tests' record under the fork's lock held exclusively,
 * and waits at its injection point before releasing the lock, so a test can
 * hold the lock there.
 */
bool
doltgres_entries_testing_doltgres_test_shmem_store(const PgCaps *caps, int64 value, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	ShmemRegion region = shmem_Region_mapped();
	ShmemExclusive lock;
	bool		ok;

	if (!shmem_Region_exclusive(&region, raise, &lock, error))
		return false;
	shmem_Exclusive_store_test(&lock, (uint64) value);
	ok = pg_calls_injection_point(raise, "doltgres-shmem-store", error);
	shmem_Exclusive_drop(&lock);
	return ok;
}

/*
 * doltgres_entries_testing_doltgres_test_shmem_load is the test module's
 * load: it reads the tests' record under the fork's lock held shared.
 */
bool
doltgres_entries_testing_doltgres_test_shmem_load(const PgCaps *caps, int64 *result, PgPgError *error)
{
	ShmemRegion region = shmem_Region_mapped();
	ShmemShared lock;

	if (!shmem_Region_shared(&region, pg_Caps_may_raise(caps), &lock, error))
		return false;
	*result = (int64) shmem_Shared_load_test(&lock);
	shmem_Shared_drop(&lock);
	return true;
}

/* DoltgresEntriesTestingPairOps is Pair: takes and releases a lock, the indirect call host's seams make. */
typedef struct DoltgresEntriesTestingPairOps
{
	/* pair takes the lock exclusively and releases it. */
	bool		(*pair) (const void *self, const PgMayRaise *raise, PgPgError *error);
} DoltgresEntriesTestingPairOps;

/* doltgres_entries_testing_Region_Pair_pair is Region's Pair: its lock taken exclusively and released. */
static bool
doltgres_entries_testing_Region_Pair_pair(const void *self, const PgMayRaise *raise, PgPgError *error)
{
	ShmemExclusive lock;

	if (!shmem_Region_exclusive(self, raise, &lock, error))
		return false;
	shmem_Exclusive_drop(&lock);
	return true;
}

/* DOLTGRES_ENTRIES_TESTING_REGION_PAIR_OPS is Region's Pair table. */
static const DoltgresEntriesTestingPairOps DOLTGRES_ENTRIES_TESTING_REGION_PAIR_OPS = {
	doltgres_entries_testing_Region_Pair_pair
};

/*
 * doltgres_entries_testing_doltgres_test_lwlock_pairs is the test module's
 * measurement of the fork's lock (design items 14 and 17): it takes and
 * releases the lock pairs times through the guarded call (way 0) or through
 * the Pair table, read through a volatile as the Rust's black_box hides its
 * trait object (way 1); the test module times it, and makes the unguarded
 * calls of way 2 itself.
 */
bool
doltgres_entries_testing_doltgres_test_lwlock_pairs(const PgCaps *caps, int64 pairs, int32 way, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	ShmemRegion region = shmem_Region_mapped();
	const DoltgresEntriesTestingPairOps *volatile lock = &DOLTGRES_ENTRIES_TESTING_REGION_PAIR_OPS;

	for (int64 at = 0; at < pairs; at++)
	{
		if (way == 0)
		{
			ShmemExclusive held;

			if (!shmem_Region_exclusive(&region, raise, &held, error))
				return false;
			shmem_Exclusive_drop(&held);
		}
		else if (!lock->pair(&region, raise, error))
			return false;
	}
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_escalate is the test module's
 * escalation: it calls the test-only escalating wrapper, whose call raises,
 * so the backend PANICs rather than unwinding into the fork.
 */
bool
doltgres_entries_testing_doltgres_test_escalate(const PgCaps *caps, PgPgError *error)
{
	pg_calls_escalate();
	return true;
}

/* DOLTGRES_ENTRIES_TESTING_FIRST is FIRST: the test entry's first reading of the host's monotonic clock in this process. */
static struct
{
	StdOnceLock lock;
	StdInstant	value;
}			DOLTGRES_ENTRIES_TESTING_FIRST = {STD_ONCE_LOCK_NEW};

/* doltgres_entries_testing_instant_call is host::instant as FIRST's get_or_init takes it. */
static void
doltgres_entries_testing_instant_call(void *captures, void *result)
{
	*(StdInstant *) result = host_instant();
}

/* doltgres_entries_testing_i64_of returns a u128 as an i64, or i64::MAX for one too large, as try_from's unwrap_or. */
static int64
doltgres_entries_testing_i64_of(StdU128 value)
{
	int64		converted;
	StdTryFromIntError wide;

	return std_i64_try_from_u128(value, &converted, &wide) ? converted : PG_INT64_MAX;
}

/*
 * doltgres_entries_testing_reported reports the fork's own error (which 5)
 * or one Postgres raised opening a missing file (any other) at WARNING, and
 * answers zero.
 */
static bool
doltgres_entries_testing_reported(const PgCaps *caps, int32 which, int64 *result, PgPgError *error)
{
	static const char damaged[] = "a manifest the test damaged";
	static const char exists[] = "the test's missing file exists";
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	reported;
	HostError	failed;

	if (which == 5)
	{
		StdString	message;

		std_string_from_Checked(&message, damaged, sizeof(damaged) - 1);
		reported = host_Error_fork((const uint8 *) "XX001", &message);
	}
	else
	{
		HostFile	opened;

		if (host_open_to_read(HOST_IO_IO_READ_DOORS(&io), "doltgres_test_missing", strlen("doltgres_test_missing"),
							  &opened, &reported))
		{
			StdString	message;

			host_File_drop(&opened);
			std_string_from_Checked(&message, exists, sizeof(exists) - 1);
			*error = pg_PgError_fork((const uint8 *) "XX000", &message);
			return false;
		}
	}
	if (!host_warn(HOST_IO_IO_IO_DOORS(&io), &reported, "the test's report", &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = 0;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_host is the test module's view of
 * the server's host (design item 14): the process id (0), the time of day in
 * microseconds since the Unix epoch (1), a unique number (2), the redo point
 * of the last checkpoint completed (4), zero after a report at WARNING of
 * the fork's own error (5) or of one Postgres raised opening a missing file
 * (6), the session's database's id (7) and its tablespace's (8), or the
 * nanoseconds since this process's first reading of the monotonic clock (any
 * other).
 */
bool
doltgres_entries_testing_doltgres_test_host(const PgCaps *caps, int32 which, int64 *result, PgPgError *error)
{
	HostError	failed;

	switch (which)
	{
		case 0:
			*result = (int64) host_process_id();
			return true;
		case 1:
			{
				StdSystemTime now = host_now();
				StdDuration since;
				StdDuration ahead;

				if (!std_SystemTime_duration_since(&now, STD_UNIX_EPOCH, &since, &ahead))
					since = std_Duration_Default_default();
				*result = doltgres_entries_testing_i64_of(std_Duration_as_micros(&since));
				return true;
			}
		case 2:
			*result = (int64) host_unique();
			return true;
		case 4:
			{
				HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
				HostLsn		redo;

				if (!host_last_redo(HOST_IO_IO_IO_DOORS(&io), &redo, &failed))
				{
					*error = pghost_raised(&failed);
					return false;
				}
				*result = (int64) redo;
				return true;
			}
		case 7:
		case 8:
			{
				uint32		database;
				uint32		tablespace;

				if (!host_session_database(&database, &tablespace, &failed))
				{
					*error = pghost_raised(&failed);
					return false;
				}
				*result = (int64) (which == 7 ? database : tablespace);
				return true;
			}
		case 5:
		case 6:
			return doltgres_entries_testing_reported(caps, which, result, error);
		default:
			{
				StdDuration elapsed;

				std_once_lock_get_or_init(&DOLTGRES_ENTRIES_TESTING_FIRST.lock, &DOLTGRES_ENTRIES_TESTING_FIRST.value,
										  doltgres_entries_testing_instant_call, NULL);
				elapsed = host_elapsed(DOLTGRES_ENTRIES_TESTING_FIRST.value);
				*result = doltgres_entries_testing_i64_of(std_Duration_as_nanos(&elapsed));
				return true;
			}
	}
}

/* DOLTGRES_ENTRIES_TESTING_FILES is FILES: the folder, in the data directory, that the test module's file steps work in. */
static const char DOLTGRES_ENTRIES_TESTING_FILES[] = "doltgres_test_files";

/* DOLTGRES_ENTRIES_TESTING_SCRATCH is SCRATCH: what the scratch steps write. */
static const char DOLTGRES_ENTRIES_TESTING_SCRATCH[] = "versioned scratch";

/* doltgres_entries_testing_in returns a name joined to FILES, as folder.join(name). */
static void
doltgres_entries_testing_in(const char *name, StdPathBuf *result)
{
	std_path_join(DOLTGRES_ENTRIES_TESTING_FILES, sizeof(DOLTGRES_ENTRIES_TESTING_FILES) - 1, name, strlen(name),
				  result);
}

/* doltgres_entries_testing_empty returns String::new(). */
static void
doltgres_entries_testing_empty(StdString *result)
{
	std_vec_new(result, 1, 1);
}

/*
 * doltgres_entries_testing_joined_frame_0_t is joined's state across its
 * pad: the names it lists and the text it joins them into.
 */
typedef struct doltgres_entries_testing_joined_frame_0_t
{
	StdVec		names;
	StdString  *result;
} doltgres_entries_testing_joined_frame_0_t;

/* doltgres_entries_testing_joined_body_0 joins the listed names with commas. */
static void
doltgres_entries_testing_joined_body_0(void *frame_)
{
	doltgres_entries_testing_joined_frame_0_t *frame = frame_;

	std_str_join(frame->names.ptr, frame->names.len, sizeof(StdString), ",", 1, frame->result);
}

/* doltgres_entries_testing_joined_pad_0 drops the names of an unwound join. */
static void
doltgres_entries_testing_joined_pad_0(void *frame_)
{
	doltgres_entries_testing_joined_frame_0_t *frame = frame_;

	std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* doltgres_entries_testing_joined returns a folder's names joined with commas, as host::list_dir(..)?.join(","). */
static bool
doltgres_entries_testing_joined(HostIoIo *io, const char *path, size_t path_len, StdString *result,
								HostError *error)
{
	doltgres_entries_testing_joined_frame_0_t frame = {.result = result};

	if (!host_list_dir(HOST_IO_IO_READ_DOORS(io), path, path_len, &frame.names, error))
		return false;
	std_landing(&frame, doltgres_entries_testing_joined_body_0, doltgres_entries_testing_joined_pad_0);
	doltgres_entries_testing_joined_pad_0(&frame);
	return true;
}

/*
 * doltgres_entries_testing_read_all_frame_0_t is read_all's state across its
 * pad: the bytes it reads and their text.
 */
typedef struct doltgres_entries_testing_read_all_frame_0_t
{
	StdVec		bytes;
	StdCowStr	lossy;
	bool		lossy_live;
	uint64		size;
	StdString  *result;
} doltgres_entries_testing_read_all_frame_0_t;

/* doltgres_entries_testing_read_all_body_0 formats the bytes' text and the file's size. */
static void
doltgres_entries_testing_read_all_body_0(void *frame_)
{
	doltgres_entries_testing_read_all_frame_0_t *frame = frame_;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
	StdStr		shown;
	StdFmtArg	args[] = {{&shown, std_str_Display_fmt}, {&frame->size, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_string_from_utf8_lossy((const uint8 *) frame->bytes.ptr, frame->bytes.len, &frame->lossy);
	frame->lossy_live = true;
	shown = std_cow_str_deref(&frame->lossy);
	std_fmt_format_Checked(frame->result, &arguments);
}

/* doltgres_entries_testing_read_all_pad_0 drops the bytes and their text of an unwound read. */
static void
doltgres_entries_testing_read_all_pad_0(void *frame_)
{
	doltgres_entries_testing_read_all_frame_0_t *frame = frame_;

	if (frame->lossy_live)
		std_cow_str_drop(&frame->lossy);
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* doltgres_entries_testing_read_all returns a file's text and size. */
static bool
doltgres_entries_testing_read_all(HostIoIo *io, const HostFile *file, StdString *result, HostError *error)
{
	doltgres_entries_testing_read_all_frame_0_t frame = {.result = result};

	if (!host_size(HOST_IO_IO_READ_DOORS(io), file, &frame.size, error))
		return false;
	if ((uint64) (size_t) frame.size != frame.size)
	{
		StdTryFromIntError wide = {0};

		STD_RESULT_EXPECT_FAILED(331, 50, "the test files are small", &wide, std_TryFromIntError_Debug_fmt);
	}
	std_vec_u8_from_elem(&frame.bytes, 0, (size_t) frame.size);
	if (!host_read_at(HOST_IO_IO_READ_DOORS(io), file, 0, (uint8 *) frame.bytes.ptr, frame.bytes.len, error))
	{
		std_vec_free_Checked(&frame.bytes, 1, 1);
		return false;
	}
	std_landing(&frame, doltgres_entries_testing_read_all_body_0, doltgres_entries_testing_read_all_pad_0);
	doltgres_entries_testing_read_all_pad_0(&frame);
	return true;
}

/*
 * doltgres_entries_testing_read_far opens a file and reads it at an offset
 * past Postgres' range, which fails while the file is open.
 */
static bool
doltgres_entries_testing_read_far(HostIoIo *io, const StdPathBuf *path, HostError *error)
{
	HostFile	file;
	uint8		byte[1] = {0};
	bool		ok;

	if (!host_open_to_read(HOST_IO_IO_READ_DOORS(io), path->bytes.ptr, path->bytes.len, &file, error))
		return false;
	ok = host_read_at(HOST_IO_IO_READ_DOORS(io), &file, PG_UINT64_MAX, byte, sizeof(byte), error);
	host_File_drop(&file);
	return ok;
}

/* DOLTGRES_ENTRIES_TESTING_PATHS is how many paths besides a, b, and c a file step holds at once. */
#define DOLTGRES_ENTRIES_TESTING_PATHS 3

/* DOLTGRES_ENTRIES_TESTING_TEXTS is how many texts a file step holds at once. */
#define DOLTGRES_ENTRIES_TESTING_TEXTS 3

/*
 * doltgres_entries_testing_file_step_frame_0_t is file_step's state across
 * its pad: its arguments, the paths a, b, and c, the other paths, the file,
 * the texts, and the names a step holds, each with whether it is live, and
 * where the text and the outcome go.
 */
typedef struct doltgres_entries_testing_file_step_frame_0_t
{
	HostIoIo   *io;
	int32		step;
	StdPathBuf	a;
	bool		a_live;
	StdPathBuf	b;
	bool		b_live;
	StdPathBuf	c;
	bool		c_live;
	StdPathBuf	paths[DOLTGRES_ENTRIES_TESTING_PATHS];
	bool		paths_live[DOLTGRES_ENTRIES_TESTING_PATHS];
	HostFile	file;
	bool		file_live;
	StdString	texts[DOLTGRES_ENTRIES_TESTING_TEXTS];
	bool		texts_live[DOLTGRES_ENTRIES_TESTING_TEXTS];
	StdVec		names;
	bool		names_live;
	StdString  *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_file_step_frame_0_t;

/* doltgres_entries_testing_path_in makes a file step's other path at a place, a name joined to FILES. */
static StdPathBuf *
doltgres_entries_testing_path_in(doltgres_entries_testing_file_step_frame_0_t *frame, int at, const char *name)
{
	doltgres_entries_testing_in(name, &frame->paths[at]);
	frame->paths_live[at] = true;
	return &frame->paths[at];
}

/* doltgres_entries_testing_path_drop drops a file step's other path at a place. */
static void
doltgres_entries_testing_path_drop(doltgres_entries_testing_file_step_frame_0_t *frame, int at)
{
	frame->paths_live[at] = false;
	std_vec_free_Checked(&frame->paths[at].bytes, 1, 1);
}

/* doltgres_entries_testing_file_drop closes a file step's file. */
static void
doltgres_entries_testing_file_drop(doltgres_entries_testing_file_step_frame_0_t *frame)
{
	frame->file_live = false;
	host_File_drop(&frame->file);
}

/* doltgres_entries_testing_file_step_format formats a file step's text into its result. */
static void
doltgres_entries_testing_file_step_format(doltgres_entries_testing_file_step_frame_0_t *frame,
										  const StdFmtPiece *pieces, size_t pieces_len, const StdFmtArg *args)
{
	StdFmtArguments arguments = {pieces, pieces_len, args};

	std_fmt_format_Checked(frame->result, &arguments);
}

/*
 * doltgres_entries_testing_listed_text formats a file step's two flags and
 * the folder's names joined with commas, "{first} {second} {names}".
 */
static bool
doltgres_entries_testing_listed_text(doltgres_entries_testing_file_step_frame_0_t *frame, bool first, bool second)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&first, std_bool_Display_fmt}, {&second, std_bool_Display_fmt},
	{&frame->texts[0], std_String_Display_fmt}};

	if (!doltgres_entries_testing_joined(frame->io, DOLTGRES_ENTRIES_TESTING_FILES,
										 sizeof(DOLTGRES_ENTRIES_TESTING_FILES) - 1, &frame->texts[0], frame->error))
		return false;
	frame->texts_live[0] = true;
	doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
	return true;
}

/* doltgres_entries_testing_made creates a file if it is absent and closes it, as drop(host::create_if_absent(..)?). */
static bool
doltgres_entries_testing_made(HostIoIo *io, const StdPathBuf *path, HostError *error)
{
	bool		made;
	HostFile	file;

	if (!host_create_if_absent(HOST_IO_IO_IO_DOORS(io), path->bytes.ptr, path->bytes.len, &made, &file, error))
		return false;
	if (made)
		host_File_drop(&file);
	return true;
}

/* doltgres_entries_testing_file_step_body_0 runs the frame's step. */
static void
doltgres_entries_testing_file_step_body_0(void *frame_)
{
	doltgres_entries_testing_file_step_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(io);
	HostIoReadDoors reads = HOST_IO_IO_READ_DOORS(io);
	const char *folder = DOLTGRES_ENTRIES_TESTING_FILES;
	size_t		folder_len = sizeof(DOLTGRES_ENTRIES_TESTING_FILES) - 1;
	HostError  *error = frame->error;

	doltgres_entries_testing_in("a", &frame->a);
	frame->a_live = true;
	doltgres_entries_testing_in("b", &frame->b);
	frame->b_live = true;
	doltgres_entries_testing_in("c", &frame->c);
	frame->c_live = true;
	switch (frame->step)
	{
		case 0:
			if (!host_make_dir(doors, folder, folder_len, error) || !host_make_dir(doors, folder, folder_len, error) ||
				!host_open(doors, frame->a.bytes.ptr, frame->a.bytes.len, HOST_OPEN_CREATE_NEW, &frame->file, error))
				return;
			frame->file_live = true;
			if (!host_write_at(doors, &frame->file, 0, (const uint8 *) "versioned", 9, error) ||
				!host_write_at(doors, &frame->file, 9, (const uint8 *) " file", 5, error) ||
				!host_sync(doors, &frame->file, error) ||
				!host_sync_path(doors, frame->a.bytes.ptr, frame->a.bytes.len, false, error) ||
				!host_sync_path(doors, folder, folder_len, true, error))
				return;
			frame->ok = doltgres_entries_testing_read_all(io, &frame->file, frame->result, error);
			return;
		case 1:
			{
				uint8		buffer[20] = {0};

				if (!host_open_to_read(reads, frame->a.bytes.ptr, frame->a.bytes.len, &frame->file, error))
					return;
				frame->file_live = true;
				if (!host_read_at(reads, &frame->file, 0, buffer, sizeof(buffer), error))
					return;
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 2:
			if (!host_open(doors, frame->a.bytes.ptr, frame->a.bytes.len, HOST_OPEN_READ_WRITE, &frame->file, error))
				return;
			frame->file_live = true;
			if (!host_truncate(doors, &frame->file, 9, error))
				return;
			frame->ok = doltgres_entries_testing_read_all(io, &frame->file, frame->result, error);
			return;
		case 3:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(";"), STD_FMT_ARG(1), STD_FMT_LIT(";"),
				STD_FMT_ARG(2)};
				StdFmtArg	args[] = {{&frame->texts[0], std_String_Display_fmt},
				{&frame->texts[1], std_String_Display_fmt}, {&frame->texts[2], std_String_Display_fmt}};

				if (!host_open(doors, frame->c.bytes.ptr, frame->c.bytes.len, HOST_OPEN_CREATE_NEW, &frame->file,
							   error))
					return;
				doltgres_entries_testing_file_drop(frame);
				if (!doltgres_entries_testing_joined(io, folder, folder_len, &frame->texts[0], error))
					return;
				frame->texts_live[0] = true;
				if (!host_rename_durably(doors, frame->a.bytes.ptr, frame->a.bytes.len, frame->b.bytes.ptr,
										 frame->b.bytes.len, error) ||
					!doltgres_entries_testing_joined(io, folder, folder_len, &frame->texts[1], error))
					return;
				frame->texts_live[1] = true;
				if (!host_remove_durably(doors, frame->c.bytes.ptr, frame->c.bytes.len, error) ||
					!doltgres_entries_testing_joined(io, folder, folder_len, &frame->texts[2], error))
					return;
				frame->texts_live[2] = true;
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
		case 4:
			{
				StdPathBuf *missing = doltgres_entries_testing_path_in(frame, 0, "missing");

				if (!host_open_to_read(reads, missing->bytes.ptr, missing->bytes.len, &frame->file, error))
					return;
				frame->file_live = true;
				doltgres_entries_testing_file_drop(frame);
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 5:
			if (!doltgres_entries_testing_read_far(io, &frame->b, error))
				return;
			doltgres_entries_testing_empty(frame->result);
			frame->ok = true;
			return;
		case 6:
			if (!host_open_to_read(reads, frame->b.bytes.ptr, frame->b.bytes.len, &frame->file, error))
				return;
			frame->file_live = true;
			host_prefetch(&frame->file, 0, UINT64CONST(1) << 40);
			host_prefetch(&frame->file, PG_UINT64_MAX, 1);
			std_string_from_Checked(frame->result, "prefetched", 10);
			frame->ok = true;
			return;
		case 7:
			if (!host_open(doors, frame->b.bytes.ptr, frame->b.bytes.len, HOST_OPEN_READ_WRITE, &frame->file, error))
				return;
			frame->file_live = true;
			if (!host_write_at(doors, &frame->file, 9, (const uint8 *) " again", 6, error))
				return;
			frame->ok = doltgres_entries_testing_read_all(io, &frame->file, frame->result, error);
			return;
		case 8:
			if (!host_open(doors, frame->b.bytes.ptr, frame->b.bytes.len, HOST_OPEN_CREATE_NEW, &frame->file, error))
				return;
			frame->file_live = true;
			doltgres_entries_testing_file_drop(frame);
			doltgres_entries_testing_empty(frame->result);
			frame->ok = true;
			return;
		case 14:
			{
				StdPathBuf *missing = doltgres_entries_testing_path_in(frame, 0, "missing");
				StdPathBuf *d;
				bool		has;
				bool		absent;

				if (!host_open_if_there(reads, missing->bytes.ptr, missing->bytes.len, &has, &frame->file, error))
					return;
				if (has)
					host_File_drop(&frame->file);
				absent = !has;
				doltgres_entries_testing_path_drop(frame, 0);
				d = doltgres_entries_testing_path_in(frame, 0, "d");
				if (!host_open_if_there(reads, d->bytes.ptr, d->bytes.len, &has, &frame->file, error))
					return;
				if (has)
				{
					StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
					StdFmtArg	args[] = {{&absent, std_bool_Display_fmt}, {&frame->texts[0], std_String_Display_fmt}};

					frame->file_live = true;
					if (!doltgres_entries_testing_read_all(io, &frame->file, &frame->texts[0], error))
						return;
					frame->texts_live[0] = true;
					doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				}
				else
				{
					StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" absent")};
					StdFmtArg	args[] = {{&absent, std_bool_Display_fmt}};

					doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				}
				frame->ok = true;
				return;
			}
		case 15:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
				StdPathBuf *e = doltgres_entries_testing_path_in(frame, 0, "e");
				bool		made;
				bool		again;
				StdFmtArg	args[] = {{&made, std_bool_Display_fmt}, {&again, std_bool_Display_fmt}};

				if (!host_create_if_absent(doors, e->bytes.ptr, e->bytes.len, &made, &frame->file, error))
					return;
				if (made)
					host_File_drop(&frame->file);
				doltgres_entries_testing_path_drop(frame, 0);
				e = doltgres_entries_testing_path_in(frame, 0, "e");
				if (!host_create_if_absent(doors, e->bytes.ptr, e->bytes.len, &again, &frame->file, error))
					return;
				if (again)
					host_File_drop(&frame->file);
				again = !again;
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
		case 16:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
				StdPathBuf *nowhere_path = doltgres_entries_testing_path_in(frame, 0, "nowhere");
				bool		has;
				bool		nowhere;
				StdFmtArg	args[] = {{&nowhere, std_bool_Display_fmt}, {&frame->texts[0], std_String_Display_fmt}};

				if (!host_list_dir_if_there(reads, nowhere_path->bytes.ptr, nowhere_path->bytes.len, &has,
											&frame->names, error))
					return;
				if (has)
					std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
				nowhere = !has;
				doltgres_entries_testing_path_drop(frame, 0);
				if (!host_list_dir_if_there(reads, folder, folder_len, &has, &frame->names, error))
					return;
				if (!has)
					std_vec_new(&frame->names, sizeof(StdString), _Alignof(StdString));
				frame->names_live = true;
				std_str_join(frame->names.ptr, frame->names.len, sizeof(StdString), ",", 1, &frame->texts[0]);
				frame->texts_live[0] = true;
				frame->names_live = false;
				std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
		case 17:
			{
				StdPathBuf *e = doltgres_entries_testing_path_in(frame, 0, "e");
				bool		removed;
				bool		again;

				if (!host_remove_durably_if_there(doors, e->bytes.ptr, e->bytes.len, &removed, error))
					return;
				doltgres_entries_testing_path_drop(frame, 0);
				e = doltgres_entries_testing_path_in(frame, 0, "e");
				if (!host_remove_durably_if_there(doors, e->bytes.ptr, e->bytes.len, &again, error))
					return;
				doltgres_entries_testing_path_drop(frame, 0);
				frame->ok = doltgres_entries_testing_listed_text(frame, removed, again);
				return;
			}
		case 18:
			{
				StdPathBuf *nowhere = doltgres_entries_testing_path_in(frame, 0, "nowhere");
				StdPathBuf *e;
				bool		made;

				std_path_join(nowhere->bytes.ptr, nowhere->bytes.len, "e", 1, &frame->paths[1]);
				frame->paths_live[1] = true;
				e = &frame->paths[1];
				if (!host_create_if_absent(doors, e->bytes.ptr, e->bytes.len, &made, &frame->file, error))
					return;
				if (made)
					host_File_drop(&frame->file);
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 19:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				int32		number;
				StdFmtArg	args[] = {{&number, std_i32_Display_fmt}};

				if (!host_open_to_read(reads, frame->b.bytes.ptr, frame->b.bytes.len, &frame->file, error))
					return;
				frame->file_live = true;
				number = host_File_number(&frame->file);
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				doltgres_entries_testing_file_drop(frame);
				frame->ok = true;
				return;
			}
		case 20:
			{
				StdPathBuf *left = doltgres_entries_testing_path_in(frame, 0, "left");

				if (!doltgres_entries_testing_made(io, left, error))
					return;
				host_remove_leftover(doors, left->bytes.ptr, left->bytes.len);
				host_remove_leftover(doors, left->bytes.ptr, left->bytes.len);
				frame->ok = doltgres_entries_testing_joined(io, folder, folder_len, frame->result, error);
				return;
			}
		case 21:
			host_remove_leftover(doors, folder, folder_len);
			frame->ok = doltgres_entries_testing_joined(io, folder, folder_len, frame->result, error);
			return;
		case 22:
			{
				StdPathBuf *d = doltgres_entries_testing_path_in(frame, 0, "d");
				StdPathBuf *g = doltgres_entries_testing_path_in(frame, 1, "g");
				bool		took = host_rename_back(doors, d->bytes.ptr, d->bytes.len, g->bytes.ptr, g->bytes.len);
				bool		again = host_rename_back(doors, d->bytes.ptr, d->bytes.len, g->bytes.ptr, g->bytes.len);

				frame->ok = doltgres_entries_testing_listed_text(frame, took, again);
				return;
			}
		case 23:
			host_remove_leftover(doors, folder, folder_len);
			if (!doltgres_entries_testing_made(io, doltgres_entries_testing_path_in(frame, 0, "after"), error))
				return;
			doltgres_entries_testing_empty(frame->result);
			frame->ok = true;
			return;
		case 24:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("after-"), STD_FMT_ARG(0)};
				StdPathBuf *d = doltgres_entries_testing_path_in(frame, 0, "d");
				StdPathBuf *g = doltgres_entries_testing_path_in(frame, 1, "g");
				bool		took = host_rename_back(doors, d->bytes.ptr, d->bytes.len, g->bytes.ptr, g->bytes.len);
				StdFmtArg	args[] = {{&took, std_bool_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};

				doltgres_entries_testing_path_drop(frame, 1);
				doltgres_entries_testing_path_drop(frame, 0);
				std_fmt_format_Checked(&frame->texts[0], &arguments);
				frame->texts_live[0] = true;
				std_path_join(DOLTGRES_ENTRIES_TESTING_FILES, sizeof(DOLTGRES_ENTRIES_TESTING_FILES) - 1, frame->texts[0].ptr,
							  frame->texts[0].len, &frame->paths[0]);
				frame->paths_live[0] = true;
				if (!doltgres_entries_testing_made(io, &frame->paths[0], error))
					return;
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 25:
			{
				StdPathBuf *g = doltgres_entries_testing_path_in(frame, 0, "g");

				if (!host_open_to_read(reads, g->bytes.ptr, g->bytes.len, &frame->file, error))
					return;
				frame->file_live = true;
				doltgres_entries_testing_file_drop(frame);
				doltgres_entries_testing_path_drop(frame, 0);
				if (!doltgres_entries_testing_made(io, doltgres_entries_testing_path_in(frame, 0, "closed"), error))
					return;
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 26:
			pg_calls_report_at_log();
			if (!doltgres_entries_testing_made(io, doltgres_entries_testing_path_in(frame, 0, "reported"), error))
				return;
			doltgres_entries_testing_empty(frame->result);
			frame->ok = true;
			return;
		case 27:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "),
				STD_FMT_ARG(2)};
				StdPathBuf *tree = doltgres_entries_testing_path_in(frame, 0, "tree");
				bool		removed;
				bool		again;
				bool		contains = false;
				StdFmtArg	args[] = {{&removed, std_bool_Display_fmt}, {&again, std_bool_Display_fmt},
				{&contains, std_bool_Display_fmt}};

				if (!host_make_dir(doors, tree->bytes.ptr, tree->bytes.len, error))
					return;
				std_path_join(tree->bytes.ptr, tree->bytes.len, "inner", 5, &frame->paths[1]);
				frame->paths_live[1] = true;
				if (!host_make_dir(doors, frame->paths[1].bytes.ptr, frame->paths[1].bytes.len, error))
					return;
				doltgres_entries_testing_path_drop(frame, 1);
				std_path_join(tree->bytes.ptr, tree->bytes.len, "file", 4, &frame->paths[1]);
				frame->paths_live[1] = true;
				if (!doltgres_entries_testing_made(io, &frame->paths[1], error))
					return;
				doltgres_entries_testing_path_drop(frame, 1);
				std_path_join(tree->bytes.ptr, tree->bytes.len, "inner", 5, &frame->paths[1]);
				frame->paths_live[1] = true;
				std_path_join(frame->paths[1].bytes.ptr, frame->paths[1].bytes.len, "file", 4, &frame->paths[2]);
				frame->paths_live[2] = true;
				if (!doltgres_entries_testing_made(io, &frame->paths[2], error))
					return;
				doltgres_entries_testing_path_drop(frame, 2);
				doltgres_entries_testing_path_drop(frame, 1);
				if (!host_remove_tree(doors, tree->bytes.ptr, tree->bytes.len, &removed, error) ||
					!host_remove_tree(doors, tree->bytes.ptr, tree->bytes.len, &again, error) ||
					!host_list_dir(reads, folder, folder_len, &frame->names, error))
					return;
				frame->names_live = true;
				std_string_from_Checked(&frame->texts[0], "tree", 4);
				frame->texts_live[0] = true;
				for (size_t at = 0; at < frame->names.len && !contains; at++)
				{
					const StdString *name = &((const StdString *) frame->names.ptr)[at];

					contains = name->len == frame->texts[0].len && memcmp(name->ptr, frame->texts[0].ptr, name->len) == 0;
				}
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
		case 28:
			{
				StdPathBuf *kept = doltgres_entries_testing_path_in(frame, 0, "kept");

				if (!host_make_dir(doors, kept->bytes.ptr, kept->bytes.len, error))
					return;
				std_path_join(kept->bytes.ptr, kept->bytes.len, "held", 4, &frame->paths[1]);
				frame->paths_live[1] = true;
				if (!doltgres_entries_testing_made(io, &frame->paths[1], error))
					return;
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		case 29:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
				StdPathBuf *kept = doltgres_entries_testing_path_in(frame, 0, "kept");
				bool		removed;
				StdFmtArg	args[] = {{&removed, std_bool_Display_fmt}};

				if (!host_remove_tree(doors, kept->bytes.ptr, kept->bytes.len, &removed, error))
					return;
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
		default:
			{
				StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(";"), STD_FMT_ARG(1)};
				StdPathBuf *d = doltgres_entries_testing_path_in(frame, 0, "d");
				StdFmtArg	args[] = {{&frame->texts[0], std_String_Display_fmt},
				{&frame->texts[1], std_String_Display_fmt}};

				if (!host_rename(doors, frame->b.bytes.ptr, frame->b.bytes.len, d->bytes.ptr, d->bytes.len, error) ||
					!host_sync_path(doors, folder, folder_len, true, error) ||
					!doltgres_entries_testing_joined(io, folder, folder_len, &frame->texts[0], error))
					return;
				frame->texts_live[0] = true;
				if (!host_open_to_read(reads, d->bytes.ptr, d->bytes.len, &frame->file, error))
					return;
				frame->file_live = true;
				if (!doltgres_entries_testing_read_all(io, &frame->file, &frame->texts[1], error))
					return;
				frame->texts_live[1] = true;
				doltgres_entries_testing_file_step_format(frame, pieces, lengthof(pieces), args);
				frame->ok = true;
				return;
			}
	}
}

/* doltgres_entries_testing_file_step_pad_0 drops what a file step holds, closing its file first. */
static void
doltgres_entries_testing_file_step_pad_0(void *frame_)
{
	doltgres_entries_testing_file_step_frame_0_t *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->names_live)
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	for (int at = DOLTGRES_ENTRIES_TESTING_TEXTS - 1; at >= 0; at--)
		if (frame->texts_live[at])
			std_String_drop(&frame->texts[at]);
	for (int at = DOLTGRES_ENTRIES_TESTING_PATHS - 1; at >= 0; at--)
		if (frame->paths_live[at])
			std_vec_free_Checked(&frame->paths[at].bytes, 1, 1);
	if (frame->c_live)
		std_vec_free_Checked(&frame->c.bytes, 1, 1);
	if (frame->b_live)
		std_vec_free_Checked(&frame->b.bytes, 1, 1);
	if (frame->a_live)
		std_vec_free_Checked(&frame->a.bytes, 1, 1);
}

/*
 * doltgres_entries_testing_file_step runs a step against the files a, b, and
 * c in FILES: make the folder and write, sync, and read back a (0); read
 * past its end (1); truncate it (2); list the folder around a durable rename
 * and removal (3); open a missing file (4); fail between opening and closing
 * b (5); prefetch, in range and past it (6); write to b (7); create b, which
 * exists (8); rename b to d, sync the folder, and list it and read d (9);
 * through the doors that answer whether something is there, open a missing
 * file and d (14), create e twice (15), list a missing folder and the folder
 * (16), remove e twice (17), and create a file in a missing folder, which
 * raises (18); open b and return the host's number for it (19); leave and
 * remove leftovers and rename back (20 to 26); make a tree of two folders
 * with a file in each, remove it, and remove it again (27); make the folder
 * kept with the file held (28); and remove kept as a tree (29).
 */
static bool
doltgres_entries_testing_file_step(HostIoIo *io, int32 step, StdString *result, HostError *error)
{
	doltgres_entries_testing_file_step_frame_0_t frame = {.io = io, .step = step, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_file_step_body_0, doltgres_entries_testing_file_step_pad_0);
	doltgres_entries_testing_file_step_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_scratch_step_frame_0_t is scratch_step's state
 * across its pad: its arguments, its door, the scratch file, and where the
 * text and the outcome go.
 */
typedef struct doltgres_entries_testing_scratch_step_frame_0_t
{
	const PgCaps *caps;
	int32		step;
	HostIoIo	io;
	HostScratchFile scratch;
	bool		scratch_live;
	StdString  *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_scratch_step_frame_0_t;

/* doltgres_entries_testing_scratch_step_body_0 runs the frame's step against the scratch file. */
static void
doltgres_entries_testing_scratch_step_body_0(void *frame_)
{
	static const char failed_open[] = "the test failed with its scratch file open";
	doltgres_entries_testing_scratch_step_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostIoIoDoors doors;
	const HostFile *file;
	HostError	failed;

	frame->io = pghost_io(raise);
	doors = HOST_IO_IO_IO_DOORS(&frame->io);
	if (frame->step == 12)
	{
		HostScratchFile refused;
		bool		opened;

		pg_calls_no_owner(true);
		opened = host_open_scratch(&frame->io, &refused, &failed);
		if (opened)
			host_ScratchFile_drop(&refused);
		pg_calls_no_owner(false);
		if (!opened)
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		doltgres_entries_testing_empty(frame->result);
		frame->ok = true;
		return;
	}
	if (!host_open_scratch(&frame->io, &frame->scratch, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->scratch_live = true;
	file = host_ScratchFile_Deref_deref(&frame->scratch);
	switch (frame->step)
	{
		case 10:
			if (!host_write_at(doors, file, 0, (const uint8 *) DOLTGRES_ENTRIES_TESTING_SCRATCH,
							   sizeof(DOLTGRES_ENTRIES_TESTING_SCRATCH) - 1, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			if (!pg_calls_injection_point(raise, "doltgres-scratch-open", frame->error))
				return;
			if (!doltgres_entries_testing_read_all(&frame->io, file, frame->result, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			frame->ok = true;
			return;
		case 11:
			{
				uint8		block[64 << 10];

				memset(block, 1, sizeof(block));
				for (uint64 at = 0; at < 32; at++)
				{
					if (!host_write_at(doors, file, at * sizeof(block), block, sizeof(block), &failed))
					{
						*frame->error = pghost_raised(&failed);
						return;
					}
				}
				doltgres_entries_testing_empty(frame->result);
				frame->ok = true;
				return;
			}
		default:
			*frame->error = doltgres_entries_internal(failed_open, sizeof(failed_open) - 1);
			return;
	}
}

/* doltgres_entries_testing_scratch_step_pad_0 closes the scratch file of an unwound step. */
static void
doltgres_entries_testing_scratch_step_pad_0(void *frame_)
{
	doltgres_entries_testing_scratch_step_frame_0_t *frame = frame_;

	if (frame->scratch_live)
		host_ScratchFile_drop(&frame->scratch);
}

/*
 * doltgres_entries_testing_scratch_step runs a step against a scratch file:
 * write to it, hold it open at an injection point, and read it back (10);
 * write two megabytes to it (11); open one with no resource owner (12); and
 * fail with it open (13).
 */
static bool
doltgres_entries_testing_scratch_step(const PgCaps *caps, int32 step, StdString *result, PgPgError *error)
{
	doltgres_entries_testing_scratch_step_frame_0_t frame = {.caps = caps, .step = step, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_scratch_step_body_0, doltgres_entries_testing_scratch_step_pad_0);
	doltgres_entries_testing_scratch_step_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_dirs_frame_0_t is the directories step's state
 * across its pad: the two directories and where their text goes.
 */
typedef struct doltgres_entries_testing_dirs_frame_0_t
{
	StdPathBuf	dirs[2];
	bool		dirs_live[2];
	StdString  *result;
} doltgres_entries_testing_dirs_frame_0_t;

/* doltgres_entries_testing_dirs_body_0 names a database's directories in pg_default and in another tablespace. */
static void
doltgres_entries_testing_dirs_body_0(void *frame_)
{
	doltgres_entries_testing_dirs_frame_0_t *frame = frame_;
	uint32		pg_default = HOST_PG_DEFAULT;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(";"), STD_FMT_ARG(1), STD_FMT_LIT(";"), STD_FMT_ARG(2)};
	StdStr		shown[2];
	StdFmtArg	args[] = {{&pg_default, std_u32_Display_fmt}, {&shown[0], std_path_Display_fmt},
	{&shown[1], std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	frame->dirs[0] = host_tablespace_dir(5, HOST_PG_DEFAULT);
	frame->dirs_live[0] = true;
	frame->dirs[1] = host_tablespace_dir(5, 16400);
	frame->dirs_live[1] = true;
	for (int at = 0; at < 2; at++)
	{
		shown[at].ptr = frame->dirs[at].bytes.ptr;
		shown[at].len = frame->dirs[at].bytes.len;
	}
	std_fmt_format_Checked(frame->result, &arguments);
}

/* doltgres_entries_testing_dirs_pad_0 drops the directories of an unwound step. */
static void
doltgres_entries_testing_dirs_pad_0(void *frame_)
{
	doltgres_entries_testing_dirs_frame_0_t *frame = frame_;

	for (int at = 1; at >= 0; at--)
		if (frame->dirs_live[at])
			std_vec_free_Checked(&frame->dirs[at].bytes, 1, 1);
}

/*
 * doltgres_entries_testing_doltgres_test_file is the test module's view of
 * the server's host's file door (design item 14): it runs one of file_step's
 * steps, or of scratch_step's from 10 to 13, or names a database's
 * directories in pg_default and in another tablespace after pg_default's id
 * (30), and returns what the step saw as text, or raises the error a seam
 * returned.
 */
bool
doltgres_entries_testing_doltgres_test_file(const PgCaps *caps, int32 step, PgDatum *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	StdString	seen;

	if (step >= 10 && step <= 13)
	{
		if (!doltgres_entries_testing_scratch_step(caps, step, &seen, error))
			return false;
	}
	else if (step == 30)
	{
		doltgres_entries_testing_dirs_frame_0_t frame = {.result = &seen};

		std_landing(&frame, doltgres_entries_testing_dirs_body_0, doltgres_entries_testing_dirs_pad_0);
		doltgres_entries_testing_dirs_pad_0(&frame);
	}
	else
	{
		HostIoIo	io = pghost_io(raise);
		HostError	failed;

		if (!doltgres_entries_testing_file_step(&io, step, &seen, &failed))
		{
			*error = pghost_raised(&failed);
			return false;
		}
	}
	return doltgres_entries_testing_text_datum(raise, &seen, "the steps' text holds no NUL", 137, 47, result, error);
}

/*
 * doltgres_entries_testing_doltgres_test_wal is the test module's view of the
 * server's host's WAL seams (design items 8 and 14): it inserts a test record
 * holding a value, flushes the WAL through it, and returns the position just
 * past it.
 */
bool
doltgres_entries_testing_doltgres_test_wal(const PgCaps *caps, int64 value, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	uint8		bytes[8];
	HostLsn		lsn;
	HostError	failed;

	for (int at = 0; at < 8; at++)
		bytes[at] = (uint8) ((uint64) value >> (8 * at));
	if (!host_wal_insert(HOST_IO_IO_IO_DOORS(&io), HOST_WAL_KIND_TEST, bytes, sizeof(bytes), &lsn, &failed) ||
		!host_wal_flush(HOST_IO_IO_IO_DOORS(&io), lsn, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = (int64) lsn;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_wal_unknown inserts and flushes a
 * record of a kind no crate owns, of len zero bytes, whose redo stops
 * recovery.
 */
bool
doltgres_entries_testing_doltgres_test_wal_unknown(const PgCaps *caps, int32 len, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	StdVec		body;
	uint64		lsn;
	bool		ok;

	std_vec_u8_from_elem(&body, 0, len > 0 ? (size_t) len : 0);
	ok = pg_calls_wal_insert(raise, 14, (const uint8 *) body.ptr, body.len, &lsn, error) &&
		pg_calls_wal_flush(raise, lsn, error);
	std_vec_free_Checked(&body, 1, 1);
	return ok;
}

/*
 * doltgres_entries_testing_doltgres_test_delay opens a checkpoint delay and
 * then waits at its injection point, which the delay's open loads before its
 * critical section, and finishes it (step 0), so a test can show a
 * checkpoint waiting for it; leaves it unfinished through an error (1); has
 * its flag cleared inside it, as a Postgres call ending its own delay would,
 * and finishes it (2); or opens another inside it (any other step). The
 * unfinished delay is dropped before its error is made, which an unwind from
 * it would drop as well.
 */
bool
doltgres_entries_testing_doltgres_test_delay(const PgCaps *caps, int32 step, PgPgError *error)
{
	static const char inside[] = "error inside a checkpoint delay";
	static const char *const pauses[] = {"doltgres-delay-checkpoint"};
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostDelayCheckpoint delay;
	HostError	failed;

	if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(&io), pauses, lengthof(pauses), &delay, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	switch (step)
	{
		case 0:
			if (!host_pause(HOST_SHARED_IO_PAUSES(&io), "doltgres-delay-checkpoint", &failed))
			{
				host_DelayCheckpoint_drop(&delay);
				*error = pghost_raised(&failed);
				return false;
			}
			host_DelayCheckpoint_finish(&delay);
			return true;
		case 1:
			host_DelayCheckpoint_drop(&delay);
			*error = doltgres_entries_internal(inside, sizeof(inside) - 1);
			return false;
		case 2:
			pg_calls_clear_delay_flag();
			host_DelayCheckpoint_finish(&delay);
			return true;
		default:
			{
				HostDelayCheckpoint inner;

				if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(&io), NULL, 0, &inner, &failed))
				{
					host_DelayCheckpoint_drop(&delay);
					*error = pghost_raised(&failed);
					return false;
				}
				host_DelayCheckpoint_finish(&inner);
				host_DelayCheckpoint_finish(&delay);
				return true;
			}
	}
}

/* doltgres_entries_testing_test_value returns a test record's value, or false for a body of another length. */
static bool
doltgres_entries_testing_test_value(const uint8 *body, size_t body_len, int64 *result)
{
	uint64		value = 0;

	if (body_len != 8)
		return false;
	for (int at = 7; at >= 0; at--)
		value = (value << 8) | body[at];
	*result = (int64) value;
	return true;
}

/*
 * doltgres_entries_testing_redo_test replays a test record: it stores the
 * record's value in the tests' record in shared memory, after opening a
 * checkpoint delay for a negative value, which redo, outside any
 * transaction, must be refused.
 */
bool
doltgres_entries_testing_redo_test(const PgCaps *caps, const uint8 *body, size_t body_len, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	int64		value;
	ShmemRegion region;
	ShmemExclusive lock;

	if (!doltgres_entries_testing_test_value(body, body_len, &value))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("test record of "), STD_FMT_ARG(0), STD_FMT_LIT(" bytes holds no value")};
		StdFmtArg	args[] = {{&body_len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return false;
	}
	if (value < 0)
	{
		HostIoIo	io = pghost_io(raise);
		HostDelayCheckpoint delay;
		HostError	failed;

		if (!host_delay_checkpoint(HOST_IO_IO_IO_DOORS(&io), NULL, 0, &delay, &failed))
		{
			*error = pghost_raised(&failed);
			return false;
		}
		host_DelayCheckpoint_finish(&delay);
	}
	region = shmem_Region_mapped();
	if (!shmem_Region_exclusive(&region, raise, &lock, error))
		return false;
	shmem_Exclusive_store_test(&lock, (uint64) value);
	shmem_Exclusive_drop(&lock);
	return true;
}

/* doltgres_entries_testing_describe_test describes a test record by its value. */
void
doltgres_entries_testing_describe_test(const uint8 *body, size_t body_len, StdString *result)
{
	int64		value;

	if (doltgres_entries_testing_test_value(body, body_len, &value))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("value "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&value, std_i64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(result, &arguments);
	}
	else
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" bytes")};
		StdFmtArg	args[] = {{&body_len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(result, &arguments);
	}
}

/*
 * doltgres_entries_testing_doltgres_test_check checks for a pending
 * interrupt until one is, so a test can show a cancel stopping a long loop at
 * its next check (L2).
 */
bool
doltgres_entries_testing_doltgres_test_check(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	while (host_io_Io_check(&io, &failed))
		;
	*error = pghost_raised(&failed);
	return false;
}

/* taken at i64 and IoMutex<i64>, the counter's lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS I64
#define HOST_LOCK_TAKEN_T int64
#define HOST_LOCK_TAKEN_DEFAULT(place) (*(place) = 0)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_I64_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS I64
#define HOST_LOCK_T int64
#define HOST_LOCK_DEFAULT(place) (*(place) = 0)
#include "doltgres/host/lock_template.h"

/* DOLTGRES_ENTRIES_TESTING_COUNTER_TYPE is IoMutex<i64>'s size, Default, and drop, which frees nothing. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_COUNTER_TYPE = {sizeof(HostLockIoMutexI64),
_Alignof(HostLockIoMutexI64), host_lock_IoMutexI64_Default_default, NULL};

/*
 * DOLTGRES_ENTRIES_TESTING_COUNTER is COUNTER: backend-local state behind a
 * storage lock in a static, as a storage crate keeps its state; its
 * BackendLocal header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexI64 process;
}			DOLTGRES_ENTRIES_TESTING_COUNTER = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * doltgres_entries_testing_lock_frame_0_t is doltgres_test_lock's state
 * across its pad: its step, its door, the backend's counter and its guard,
 * each while live, and where the count goes.
 */
typedef struct doltgres_entries_testing_lock_frame_0_t
{
	const PgCaps *caps;
	int32		step;
	HostIoIo	io;
	HostLocalLocal counter;
	bool		counter_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	int64	   *result;
} doltgres_entries_testing_lock_frame_0_t;

/* doltgres_entries_testing_lock_body_0 adds one to the counter under its lock, or panics holding it. */
static void
doltgres_entries_testing_lock_body_0(void *frame_)
{
	doltgres_entries_testing_lock_frame_0_t *frame = frame_;
	int64	   *count;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	frame->counter = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_TESTING_COUNTER.local,
												 &DOLTGRES_ENTRIES_TESTING_COUNTER_TYPE);
	frame->counter_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->counter,
																						   &DOLTGRES_ENTRIES_TESTING_COUNTER_TYPE),
										  &DOLTGRES_ENTRIES_TESTING_I64_LOCK_TYPE, &frame->io.held);
	frame->guard_live = true;
	count = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	*count = STD_ADD(i64, *count, 1, 424, 5);
	if (frame->step != 0)
		STD_PANIC(426, 9, "a step's bug while the counter's lock is held");
	*frame->result = *count;
}

/* doltgres_entries_testing_lock_pad_0 releases the guard, then the backend's counter. */
static void
doltgres_entries_testing_lock_pad_0(void *frame_)
{
	doltgres_entries_testing_lock_frame_0_t *frame = frame_;

	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->counter_live)
		host_local_Local_drop(&frame->counter, &DOLTGRES_ENTRIES_TESTING_COUNTER_TYPE);
	frame->guard_live = frame->counter_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_lock adds one to the backend's
 * counter under its lock and returns it (step 0), or panics while holding the
 * lock (any other step), so a test can show the next entry rebuilding the
 * poisoned state rather than failing.
 */
bool
doltgres_entries_testing_doltgres_test_lock(const PgCaps *caps, int32 step, int64 *result, PgPgError *error)
{
	doltgres_entries_testing_lock_frame_0_t frame = {.caps = caps, .step = step, .result = result};

	std_landing(&frame, doltgres_entries_testing_lock_body_0, doltgres_entries_testing_lock_pad_0);
	doltgres_entries_testing_lock_pad_0(&frame);
	return true;
}

/* The indices of Tested's parts: its lock, its barrier, its later long lock, its flag, its atomics, its blob, and its note. */
#define DOLTGRES_ENTRIES_TESTING_TESTED_LOCK 0
#define DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER 1
#define DOLTGRES_ENTRIES_TESTING_TESTED_LISTS 2
#define DOLTGRES_ENTRIES_TESTING_TESTED_FLAG 0
#define DOLTGRES_ENTRIES_TESTING_TESTED_COUNT 0
#define DOLTGRES_ENTRIES_TESTING_TESTED_KEY 1
#define DOLTGRES_ENTRIES_TESTING_TESTED_BLOB 0
#define DOLTGRES_ENTRIES_TESTING_TESTED_NOTE 0

/* DOLTGRES_ENTRIES_TESTING_OTHER_KEY is OtherKey: the second pool's records' key. */
#define DOLTGRES_ENTRIES_TESTING_OTHER_KEY 0

/* DOLTGRES_ENTRIES_TESTING_LISTED is LISTED: the most locations the tests' table lists. */
#define DOLTGRES_ENTRIES_TESTING_LISTED 256

/* The indices of Table's parts: its lock, its gate, its reclaims' switch, its counter, its locations, its notes, and its slots. */
#define DOLTGRES_ENTRIES_TESTING_TABLE_LOCK 0
#define DOLTGRES_ENTRIES_TESTING_GATE 0
#define DOLTGRES_ENTRIES_TESTING_RECLAIMS 0
#define DOLTGRES_ENTRIES_TESTING_COUNTER_ATOMIC 1
#define DOLTGRES_ENTRIES_TESTING_LOCATIONS 0
#define DOLTGRES_ENTRIES_TESTING_NOTES 0
#define DOLTGRES_ENTRIES_TESTING_HITS 1

/* The indices of the process slots' words: Words' and Stays' first and second. */
#define DOLTGRES_ENTRIES_TESTING_WORD_FIRST 0
#define DOLTGRES_ENTRIES_TESTING_WORD_SECOND 1
#define DOLTGRES_ENTRIES_TESTING_STAY_FIRST 0
#define DOLTGRES_ENTRIES_TESTING_STAY_SECOND 1

/* The indices of Spare's lock, Outer's lock and its atomic that names its Inner record, and Inner's lock and page. */
#define DOLTGRES_ENTRIES_TESTING_SPARE_LOCK 0
#define DOLTGRES_ENTRIES_TESTING_OUTER_LOCK 0
#define DOLTGRES_ENTRIES_TESTING_OUTER_NAMES 0
#define DOLTGRES_ENTRIES_TESTING_INNER_LOCK 0
#define DOLTGRES_ENTRIES_TESTING_INNER_PAGE 0

/* doltgres_entries_testing_Tested_Pool_count is Tested's count: an eighth of the processes. */
static size_t
doltgres_entries_testing_Tested_Pool_count(const HostSharedLimits *limits)
{
	return limits->processes / 8;
}

static bool doltgres_entries_testing_Tested_Pool_reclaim(HostIoIo *io, bool *result, HostError *error);

/* DOLTGRES_ENTRIES_TESTING_TESTED_BLOBS is Tested's blob of at most 16 bytes. */
static const size_t DOLTGRES_ENTRIES_TESTING_TESTED_BLOBS[] = {16};

/* DOLTGRES_ENTRIES_TESTING_TESTED_ARRAYS is Tested's note: one entry of 8 bytes. */
static const std_TupleUsizeUsize_t DOLTGRES_ENTRIES_TESTING_TESTED_ARRAYS[] = {{1, 8}};

/*
 * DOLTGRES_ENTRIES_TESTED_POOL_OPS is Tested's Shape and Pool: the pooled
 * record doltgres_test_shared reaches, a lock, a long lock that guards the
 * table's slots, a later long lock, a flag, an atomic and a key, and a blob
 * of at most 16 bytes and a note of 8 under the lock.
 */
const HostSharedPoolOps DOLTGRES_ENTRIES_TESTED_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x7e570001), .locks = 3, .flags = 1, .atomics = 2,
		.blobs = DOLTGRES_ENTRIES_TESTING_TESTED_BLOBS, .blobs_len = lengthof(DOLTGRES_ENTRIES_TESTING_TESTED_BLOBS),
		.arrays = DOLTGRES_ENTRIES_TESTING_TESTED_ARRAYS,
	.arrays_len = lengthof(DOLTGRES_ENTRIES_TESTING_TESTED_ARRAYS)},
	.name = {"test", 4},
	.count = doltgres_entries_testing_Tested_Pool_count,
	.reclaim = doltgres_entries_testing_Tested_Pool_reclaim,
};

/* doltgres_entries_testing_Other_Pool_count is Other's count: two records. */
static size_t
doltgres_entries_testing_Other_Pool_count(const HostSharedLimits *limits)
{
	return 2;
}

/* doltgres_entries_testing_Other_Pool_reclaim is Other's reclaim, which frees nothing. */
static bool
doltgres_entries_testing_Other_Pool_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	*result = false;
	return true;
}

/* DOLTGRES_ENTRIES_OTHER_POOL_OPS is Other's Shape and Pool: a second pool, of two records with a key, whose doors refuse Tested's locations. */
const HostSharedPoolOps DOLTGRES_ENTRIES_OTHER_POOL_OPS = {
	.shape = {.id = UINT64CONST(0x7e570002), .atomics = 1},
	.name = {"other test", 10},
	.count = doltgres_entries_testing_Other_Pool_count,
	.reclaim = doltgres_entries_testing_Other_Pool_reclaim,
};

/* DOLTGRES_ENTRIES_TESTING_TABLE_BLOBS is Table's locations, LISTED of 8 bytes. */
static const size_t DOLTGRES_ENTRIES_TESTING_TABLE_BLOBS[] = {DOLTGRES_ENTRIES_TESTING_LISTED * sizeof(uint64)};

/* DOLTGRES_ENTRIES_TESTING_TABLE_ARRAYS is Table's four notes and eight slots, of 8 bytes each. */
static const std_TupleUsizeUsize_t DOLTGRES_ENTRIES_TESTING_TABLE_ARRAYS[] = {{4, 8}, {8, 8}};

/*
 * DOLTGRES_ENTRIES_TABLE_SINGLE_OPS is Table's Shape and Single: the test
 * module's single record, the table of the locations the tests published, as
 * a blob under its lock, the gate Tested's reclaim passes, the switch of that
 * reclaim, a counter, four notes under its lock, and eight slots under a
 * Tested record's long lock.
 */
const HostSharedSingleOps DOLTGRES_ENTRIES_TABLE_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x7e570003), .locks = 1, .flags = 1, .atomics = 2,
		.blobs = DOLTGRES_ENTRIES_TESTING_TABLE_BLOBS, .blobs_len = lengthof(DOLTGRES_ENTRIES_TESTING_TABLE_BLOBS),
		.arrays = DOLTGRES_ENTRIES_TESTING_TABLE_ARRAYS,
	.arrays_len = lengthof(DOLTGRES_ENTRIES_TESTING_TABLE_ARRAYS)},
	.words = host_shared_Single_words,
};

/* DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS is Words: the test module's slot for each process, two words. */
const HostSharedProcSlotsOps DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS = {.id = UINT64CONST(0x7e570004), .words = 2};

/* DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS is Stays: the test module's kept slot for each process, two words an exit leaves. */
const HostSharedProcSlotsOps DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS = {.id = UINT64CONST(0x7e570005), .words = 2,
.kept = true};

/* doltgres_entries_testing_Spare_Single_words is Spare's run: a word for each slot of the delta's page directory. */
static size_t
doltgres_entries_testing_Spare_Single_words(const HostSharedLimits *limits)
{
	return limits->delta_pages;
}

/*
 * DOLTGRES_ENTRIES_SPARE_SINGLE_OPS is Spare's Shape and Single: the test
 * module's second single record, a lock, which owns pages as the table's
 * does, and a run of a word for each slot of the delta's page directory.
 */
const HostSharedSingleOps DOLTGRES_ENTRIES_SPARE_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x7e570006), .locks = 1},
	.words = doltgres_entries_testing_Spare_Single_words,
};

/* doltgres_entries_testing_Outer_Aged_count is Outer's count: a record for each slot of the delta's page directory. */
static size_t
doltgres_entries_testing_Outer_Aged_count(const HostSharedLimits *limits)
{
	return limits->delta_pages;
}

/* DOLTGRES_ENTRIES_TESTING_OUTER_NAMES_IDS is Outer's NAMES: the shape of the Inner records it names. */
static const uint64 DOLTGRES_ENTRIES_TESTING_OUTER_NAMES_IDS[] = {UINT64CONST(0x7e570008)};

/*
 * DOLTGRES_ENTRIES_OUTER_AGED_OPS is Outer's Shape and Aged: the test
 * module's records that the delta's epochs free and that name others, a
 * memtable's stand-in, a lock, an atomic that names an Inner record, and a
 * list under the lock that takes entries in and out.
 */
const HostSharedAgedOps DOLTGRES_ENTRIES_OUTER_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x7e570007), .locks = 1, .atomics = 1, .lists = 1},
	.name = {"outer test", 10},
	.names = DOLTGRES_ENTRIES_TESTING_OUTER_NAMES_IDS,
	.names_len = lengthof(DOLTGRES_ENTRIES_TESTING_OUTER_NAMES_IDS),
	.count = doltgres_entries_testing_Outer_Aged_count,
};

/* doltgres_entries_testing_Inner_Aged_count is Inner's count: a record for each slot of the delta's page directory. */
static size_t
doltgres_entries_testing_Inner_Aged_count(const HostSharedLimits *limits)
{
	return limits->delta_pages;
}

/*
 * DOLTGRES_ENTRIES_INNER_AGED_OPS is Inner's Shape and Aged: the test
 * module's records that the delta's epochs free and that name pages, a
 * range's stand-in, a lock that owns its pages, and an atomic that names its
 * page.
 */
const HostSharedAgedOps DOLTGRES_ENTRIES_INNER_AGED_OPS = {
	.shape = {.id = UINT64CONST(0x7e570008), .locks = 1, .atomics = 1},
	.name = {"inner test", 10},
	.count = doltgres_entries_testing_Inner_Aged_count,
};

/* doltgres_entries_testing_load is Op::Load. */
static const HostSharedOp DOLTGRES_ENTRIES_TESTING_LOAD = {.tag = HOST_SHARED_OP_LOAD};

/* doltgres_entries_testing_add returns Op::Add of a value. */
static HostSharedOp
doltgres_entries_testing_add(uint64 value)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_ADD};

	op.u.add = value;
	return op;
}

/* doltgres_entries_testing_decoded returns the locations a table's bytes hold, as eight-byte little-endian words. */
static void
doltgres_entries_testing_decoded(const uint8 *bytes, size_t bytes_len, StdVec *result)
{
	size_t		count = bytes_len / 8;

	std_vec_with_capacity_Checked(result, count, sizeof(uint64), _Alignof(uint64));
	for (size_t at = 0; at < count; at++)
	{
		uint64		location = 0;

		for (int b = 7; b >= 0; b--)
			location = (location << 8) | bytes[at * 8 + b];
		((uint64 *) result->ptr)[at] = location;
	}
	result->len = count;
}

/*
 * doltgres_entries_testing_table_frame_0_t is listed's and relist's state
 * across their pad: their door, the change and its captures relist makes,
 * the table's guard, its blob's bytes, the locations they hold, and the
 * bytes written back, each while live, and where the locations and the
 * outcome go.
 */
typedef struct doltgres_entries_testing_table_frame_0_t
{
	HostIoIo   *io;
	void		(*change) (StdVec *locations, void *captures);
	void	   *captures;
	HostSharedGuard guard;
	bool		guard_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		locations;
	bool		locations_live;
	StdVec		written;
	bool		written_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_table_frame_0_t;

/* doltgres_entries_testing_table_pad_0 drops what a read or change of the table holds, its guard last. */
static void
doltgres_entries_testing_table_pad_0(void *frame_)
{
	doltgres_entries_testing_table_frame_0_t *frame = frame_;

	if (frame->written_live)
		std_vec_free_Checked(&frame->written, 1, 1);
	if (frame->locations_live)
		std_vec_free_Checked(&frame->locations, sizeof(uint64), _Alignof(uint64));
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->guard_live)
		host_shared_Guard_drop(&frame->guard);
	frame->written_live = frame->locations_live = frame->bytes_live = frame->guard_live = false;
}

/* doltgres_entries_testing_listed_body_0 reads the table's locations under its lock held shared. */
static void
doltgres_entries_testing_listed_body_0(void *frame_)
{
	doltgres_entries_testing_table_frame_0_t *frame = frame_;
	HostSharedRecord table;

	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, frame->error) ||
		!host_shared_Record_lock_shared(table, HOST_IO_IO_TAKES_SHORT(frame->io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK,
										&frame->guard, frame->error))
		return;
	frame->guard_live = true;
	frame->bytes = host_shared_Guard_read_blob(&frame->guard, DOLTGRES_ENTRIES_TESTING_LOCATIONS);
	frame->bytes_live = true;
	doltgres_entries_testing_decoded((const uint8 *) frame->bytes.ptr, frame->bytes.len, frame->result);
	frame->ok = true;
}

/* doltgres_entries_testing_listed returns the locations the tests' table lists. */
static bool
doltgres_entries_testing_listed(HostIoIo *io, StdVec *result, HostError *error)
{
	doltgres_entries_testing_table_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_listed_body_0, doltgres_entries_testing_table_pad_0);
	doltgres_entries_testing_table_pad_0(&frame);
	return frame.ok;
}

/* DoltgresEntriesTestingChange is the change relist makes to the table's locations, with its captures. */
typedef void (*DoltgresEntriesTestingChange) (StdVec *locations, void *captures);

/* doltgres_entries_testing_relist_body_0 changes the table's locations under its lock held exclusively. */
static void
doltgres_entries_testing_relist_body_0(void *frame_)
{
	doltgres_entries_testing_table_frame_0_t *frame = frame_;
	HostSharedRecord table;

	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, frame->error) ||
		!host_shared_Record_lock(table, HOST_IO_IO_TAKES_SHORT(frame->io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK,
								 &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	frame->bytes = host_shared_Guard_read_blob(&frame->guard, DOLTGRES_ENTRIES_TESTING_LOCATIONS);
	frame->bytes_live = true;
	doltgres_entries_testing_decoded((const uint8 *) frame->bytes.ptr, frame->bytes.len, &frame->locations);
	frame->locations_live = true;
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	frame->change(&frame->locations, frame->captures);
	std_vec_with_capacity_Checked(&frame->written, frame->locations.len * 8, 1, 1);
	frame->written_live = true;
	for (size_t at = 0; at < frame->locations.len; at++)
		for (int b = 0; b < 8; b++)
			((uint8 *) frame->written.ptr)[at * 8 + b] = (uint8) (((uint64 *) frame->locations.ptr)[at] >> (8 * b));
	frame->written.len = frame->locations.len * 8;
	frame->ok = host_shared_GuardExclusive_write_blob(&frame->guard, DOLTGRES_ENTRIES_TESTING_LOCATIONS,
													  (const uint8 *) frame->written.ptr, frame->written.len,
													  frame->error);
}

/*
 * doltgres_entries_testing_relist changes the locations the tests' table
 * lists, under its lock held exclusively.
 */
static bool
doltgres_entries_testing_relist(HostIoIo *io, DoltgresEntriesTestingChange change, void *captures,
								HostError *error)
{
	doltgres_entries_testing_table_frame_0_t frame = {.io = io, .change = change, .captures = captures,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_relist_body_0, doltgres_entries_testing_table_pad_0);
	doltgres_entries_testing_table_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_unlisted is reclaim's change of the table's locations: the freed location taken out. */
static void
doltgres_entries_testing_unlisted(StdVec *locations, void *captures)
{
	uint64		location = *(const uint64 *) captures;
	size_t		kept = 0;

	for (size_t at = 0; at < locations->len; at++)
		if (((uint64 *) locations->ptr)[at] != location)
			((uint64 *) locations->ptr)[kept++] = ((uint64 *) locations->ptr)[at];
	locations->len = kept;
}

/*
 * doltgres_entries_testing_reclaim_frame_0_t is Tested's reclaim's state
 * across its pad: its door, the table's locations, and the look at the
 * record it took, each while live, and where its answer and the outcome go.
 */
typedef struct doltgres_entries_testing_reclaim_frame_0_t
{
	HostIoIo   *io;
	StdVec		locations;
	bool		locations_live;
	HostSharedReclaim look;
	bool		look_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_reclaim_frame_0_t;

/* doltgres_entries_testing_reclaim_body_0 looks at each listed record in turn, freeing the first it may. */
static void
doltgres_entries_testing_reclaim_body_0(void *frame_)
{
	doltgres_entries_testing_reclaim_frame_0_t *frame = frame_;
	HostSharedRecord table;
	bool		undecided = false;

	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, frame->error))
		return;
	if (host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_RECLAIMS, DOLTGRES_ENTRIES_TESTING_LOAD) == 0)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	if (!doltgres_entries_testing_listed(frame->io, &frame->locations, frame->error))
		return;
	frame->locations_live = true;
	for (size_t at = 0; at < frame->locations.len; at++)
	{
		uint64		location = ((uint64 *) frame->locations.ptr)[at];
		HostSharedLook look;
		HostSharedFlagGuard gate;

		if (!host_shared_reclaim(HOST_SHARED_IO_WITHIN(frame->io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &look,
								 frame->error))
			return;
		if (look.tag == HOST_SHARED_LOOK_THEIRS)
			undecided = true;
		if (look.tag != HOST_SHARED_LOOK_MINE)
			continue;
		frame->look = look.u.mine;
		frame->look_live = true;
		if (!host_shared_Record_flag(table, HOST_IO_IO_TAKES_SHORT(frame->io), DOLTGRES_ENTRIES_TESTING_GATE, &gate,
									 frame->error))
			return;
		host_shared_FlagGuard_drop(&gate);
		if (host_shared_Record_atomic(host_shared_Reclaim_record(&frame->look), DOLTGRES_ENTRIES_TESTING_TESTED_COUNT,
									  DOLTGRES_ENTRIES_TESTING_LOAD) != 0)
		{
			frame->look_live = false;
			host_shared_Reclaim_drop(&frame->look);
			continue;
		}
		frame->look_live = false;
		if (host_shared_Reclaim_free(&frame->look))
		{
			if (!doltgres_entries_testing_relist(frame->io, doltgres_entries_testing_unlisted, &location,
												 frame->error))
				return;
			*frame->result = true;
			frame->ok = true;
			return;
		}
		undecided = true;
	}
	*frame->result = undecided;
	frame->ok = true;
}

/* doltgres_entries_testing_reclaim_pad_0 gives up the look and drops the locations of an unwound reclaim. */
static void
doltgres_entries_testing_reclaim_pad_0(void *frame_)
{
	doltgres_entries_testing_reclaim_frame_0_t *frame = frame_;

	if (frame->look_live)
		host_shared_Reclaim_drop(&frame->look);
	if (frame->locations_live)
		std_vec_free_Checked(&frame->locations, sizeof(uint64), _Alignof(uint64));
	frame->look_live = frame->locations_live = false;
}

/*
 * doltgres_entries_testing_Tested_Pool_reclaim frees the first record the
 * tests' table lists that no process holds and whose atomic is zero, once the
 * tests switched it on. After it takes a record to look at, it takes and
 * drops the table's gate, so a test that holds the gate holds a make inside
 * its reclaim.
 */
static bool
doltgres_entries_testing_Tested_Pool_reclaim(HostIoIo *io, bool *result, HostError *error)
{
	doltgres_entries_testing_reclaim_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_reclaim_body_0, doltgres_entries_testing_reclaim_pad_0);
	doltgres_entries_testing_reclaim_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_Owned_drop is Owned<Tested>'s drop, as a Vec's element drop takes it. */
static void
doltgres_entries_testing_Owned_drop(void *owned)
{
	host_shared_Owned_drop(owned);
}

/* doltgres_entries_testing_Held_drop is Held<Tested>'s drop, as a Vec's element drop takes it. */
static void
doltgres_entries_testing_Held_drop(void *held)
{
	host_shared_Held_drop(held);
}

/* doltgres_entries_testing_Reclaim_drop is Reclaim<Tested>'s drop, as a Vec's element drop takes it. */
static void
doltgres_entries_testing_Reclaim_drop(void *look)
{
	host_shared_Reclaim_drop(look);
}

/* doltgres_entries_testing_VecOwned_drop is Vec<Owned<Tested>>' drop. */
static void
doltgres_entries_testing_VecOwned_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(HostSharedOwned), _Alignof(HostSharedOwned),
						 doltgres_entries_testing_Owned_drop);
}

/* doltgres_entries_testing_VecHeld_drop is Vec<Held<Tested>>' drop. */
static void
doltgres_entries_testing_VecHeld_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(HostSharedHeld), _Alignof(HostSharedHeld), doltgres_entries_testing_Held_drop);
}

/* doltgres_entries_testing_VecReclaim_drop is Vec<Reclaim<Tested>>' drop. */
static void
doltgres_entries_testing_VecReclaim_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(HostSharedReclaim), _Alignof(HostSharedReclaim),
						 doltgres_entries_testing_Reclaim_drop);
}

/* taken at Vec<Owned<Tested>> and IoMutex<Vec<Owned<Tested>>>, OWNED's lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecOwned
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(HostSharedOwned), _Alignof(HostSharedOwned))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_testing_VecOwned_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_OWNED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecOwned
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(HostSharedOwned), _Alignof(HostSharedOwned))
#include "doltgres/host/lock_template.h"

/* taken at Vec<Held<Tested>> and IoMutex<Vec<Held<Tested>>>, HELD's lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecHeld
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(HostSharedHeld), _Alignof(HostSharedHeld))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_testing_VecHeld_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_HELD_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecHeld
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(HostSharedHeld), _Alignof(HostSharedHeld))
#include "doltgres/host/lock_template.h"

/* taken at Vec<Reclaim<Tested>> and IoMutex<Vec<Reclaim<Tested>>>, LOOKS' lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecReclaim
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) std_vec_new((place), sizeof(HostSharedReclaim), _Alignof(HostSharedReclaim))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_testing_VecReclaim_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_LOOKS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecReclaim
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) std_vec_new((place), sizeof(HostSharedReclaim), _Alignof(HostSharedReclaim))
#include "doltgres/host/lock_template.h"

/* doltgres_entries_testing_IoMutexVecOwned_drop is IoMutex<Vec<Owned<Tested>>>' drop glue: its value's. */
static void
doltgres_entries_testing_IoMutexVecOwned_drop(void *value)
{
	doltgres_entries_testing_VecOwned_drop(&((HostLockIoMutexVecOwned *) value)->value);
}

/* doltgres_entries_testing_IoMutexVecHeld_drop is IoMutex<Vec<Held<Tested>>>' drop glue: its value's. */
static void
doltgres_entries_testing_IoMutexVecHeld_drop(void *value)
{
	doltgres_entries_testing_VecHeld_drop(&((HostLockIoMutexVecHeld *) value)->value);
}

/* doltgres_entries_testing_IoMutexVecReclaim_drop is IoMutex<Vec<Reclaim<Tested>>>' drop glue: its value's. */
static void
doltgres_entries_testing_IoMutexVecReclaim_drop(void *value)
{
	doltgres_entries_testing_VecReclaim_drop(&((HostLockIoMutexVecReclaim *) value)->value);
}

/* DOLTGRES_ENTRIES_TESTING_OWNED_TYPE is IoMutex<Vec<Owned<Tested>>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_OWNED_TYPE = {sizeof(HostLockIoMutexVecOwned),
	_Alignof(HostLockIoMutexVecOwned), host_lock_IoMutexVecOwned_Default_default,
doltgres_entries_testing_IoMutexVecOwned_drop};

/* DOLTGRES_ENTRIES_TESTING_HELD_TYPE is IoMutex<Vec<Held<Tested>>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_HELD_TYPE = {sizeof(HostLockIoMutexVecHeld),
	_Alignof(HostLockIoMutexVecHeld), host_lock_IoMutexVecHeld_Default_default,
doltgres_entries_testing_IoMutexVecHeld_drop};

/* DOLTGRES_ENTRIES_TESTING_LOOKS_TYPE is IoMutex<Vec<Reclaim<Tested>>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_LOOKS_TYPE = {sizeof(HostLockIoMutexVecReclaim),
	_Alignof(HostLockIoMutexVecReclaim), host_lock_IoMutexVecReclaim_Default_default,
doltgres_entries_testing_IoMutexVecReclaim_drop};

/*
 * DOLTGRES_ENTRIES_TESTING_OWNED is OWNED: the records this backend made or
 * claimed through doltgres_test_shared and still owns; its BackendLocal
 * header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecOwned process;
}			DOLTGRES_ENTRIES_TESTING_OWNED = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * DOLTGRES_ENTRIES_TESTING_HELD is HELD: the records this backend opened
 * through doltgres_test_shared and keeps open; its BackendLocal header and
 * the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecHeld process;
}			DOLTGRES_ENTRIES_TESTING_HELD = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * DOLTGRES_ENTRIES_TESTING_LOOKS is LOOKS: the records this backend's
 * reclaims took through doltgres_test_shared and still look at; its
 * BackendLocal header and the process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecReclaim process;
}			DOLTGRES_ENTRIES_TESTING_LOOKS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * DOLTGRES_ENTRIES_TESTING_EXIT_OPENS is EXIT_OPENS: the location of the
 * record the test module's exit callback opens, or u64::MAX for none.
 */
static StdAtomicU64 DOLTGRES_ENTRIES_TESTING_EXIT_OPENS = STD_ATOMIC_U64_INIT(PG_UINT64_MAX);

/*
 * doltgres_entries_testing_exit_frame_0_t is the exit callback's state across
 * its pad: the handle it opened, the backend's list and its guard, each while
 * live.
 */
typedef struct doltgres_entries_testing_exit_frame_0_t
{
	const PgCaps *caps;
	uint64		location;
	HostSharedHeld held;
	bool		held_live;
	HostLocalLocal list;
	bool		list_live;
	StdRwLockWriteGuard kept;
	bool		kept_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_exit_frame_0_t;

/* doltgres_entries_testing_exit_body_0 opens the named record, adds one to its atomic, and keeps the handle. */
static void
doltgres_entries_testing_exit_body_0(void *frame_)
{
	doltgres_entries_testing_exit_frame_0_t *frame = frame_;
	HostIoIo	io = pghost_io(pg_Caps_may_raise(frame->caps));
	HostIoInDrop in_drop;
	HostError	failed;
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(&io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, frame->location, &has,
						  &frame->held, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
	if (!has)
		return;
	frame->held_live = true;
	(void) host_shared_Record_atomic(host_shared_Held_record(&frame->held), DOLTGRES_ENTRIES_TESTING_TESTED_COUNT,
									 doltgres_entries_testing_add(1));
	frame->list = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_TESTING_HELD.local,
											  &DOLTGRES_ENTRIES_TESTING_HELD_TYPE);
	frame->list_live = true;
	in_drop = host_io_InDrop_new();
	if (!host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->list,
																						 &DOLTGRES_ENTRIES_TESTING_HELD_TYPE),
										&in_drop, &frame->kept))
		return;
	frame->kept_live = true;
	std_vec_push_Checked(frame->kept.data, &frame->held, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	frame->held_live = false;
}

/* doltgres_entries_testing_exit_pad_0 releases the guard and the list, then closes a handle it didn't keep. */
static void
doltgres_entries_testing_exit_pad_0(void *frame_)
{
	doltgres_entries_testing_exit_frame_0_t *frame = frame_;

	if (frame->kept_live)
		std_RwLockWriteGuard_drop(&frame->kept);
	if (frame->list_live)
		host_local_Local_drop(&frame->list, &DOLTGRES_ENTRIES_TESTING_HELD_TYPE);
	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	frame->kept_live = frame->list_live = frame->held_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_exit_callback is the test module's
 * before_shmem_exit callback, which a test registers after the fork's own,
 * so it runs before them: it opens the record its backend named for it, adds
 * one to the record's atomic, and keeps the handle, which only the fork's
 * release then closes. With no record named it does nothing. It takes the
 * backend's list of handles as an exit takes a backend's state, by a try.
 */
bool
doltgres_entries_testing_doltgres_test_exit_callback(const PgCaps *caps, int32 code, PgDatum arg, PgPgError *error)
{
	doltgres_entries_testing_exit_frame_0_t frame = {.caps = caps, .error = error};

	frame.location = std_AtomicU64_load_relaxed(&DOLTGRES_ENTRIES_TESTING_EXIT_OPENS);
	if (frame.location == PG_UINT64_MAX)
		return true;
	std_landing(&frame, doltgres_entries_testing_exit_body_0, doltgres_entries_testing_exit_pad_0);
	doltgres_entries_testing_exit_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_counter_add adds a value to the single record's counter. */
static bool
doltgres_entries_testing_counter_add(uint64 value, HostError *error)
{
	HostSharedRecord table;

	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, error))
		return false;
	(void) host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_COUNTER_ATOMIC,
									 doltgres_entries_testing_add(value));
	return true;
}

/* doltgres_entries_testing_Counting_Exit_run is Counting's exit work: it adds one to the single record's counter. */
static bool
doltgres_entries_testing_Counting_Exit_run(HostIoIo *io, HostError *error)
{
	return doltgres_entries_testing_counter_add(1, error);
}

/* DOLTGRES_ENTRIES_TESTING_COUNTING_EXIT_OPS is Counting's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_COUNTING_EXIT_OPS = {doltgres_entries_testing_Counting_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_COUNTING_TYPE_ID is Counting's TypeId, which at_exit lists it by. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_COUNTING_TYPE_ID = {"doltgres::entries::testing::Counting", 0, 1,
NULL};

/* doltgres_entries_testing_held_count returns the atomic of the record the backend opened last, by a try of its list. */
static bool
doltgres_entries_testing_held_count(uint64 *result)
{
	HostLocalLocal list = host_local_BackendLocal_get(&DOLTGRES_ENTRIES_TESTING_HELD.local,
													  &DOLTGRES_ENTRIES_TESTING_HELD_TYPE);
	HostIoInDrop in_drop = host_io_InDrop_new();
	StdRwLockWriteGuard kept;
	bool		has = false;

	if (host_lock_IoMutex_lock_in_drop((HostLockIoMutex *) host_local_Local_Deref_deref(&list,
																						&DOLTGRES_ENTRIES_TESTING_HELD_TYPE),
									   &in_drop, &kept))
	{
		const StdVec *held = kept.data;

		has = held->len > 0;
		if (has)
			*result = host_shared_Record_atomic(host_shared_Held_record(&((const HostSharedHeld *) held->ptr)[held->len - 1]),
												DOLTGRES_ENTRIES_TESTING_TESTED_COUNT, DOLTGRES_ENTRIES_TESTING_LOAD);
		std_RwLockWriteGuard_drop(&kept);
	}
	host_local_Local_drop(&list, &DOLTGRES_ENTRIES_TESTING_HELD_TYPE);
	return has;
}

/*
 * doltgres_entries_testing_Noting_Exit_run is Noting's exit work: it notes
 * what its process still has, in the table's first three notes: its second
 * word, the atomic of the record it opened last, or -1 with none open, and
 * one more than whether it is in a transaction. It takes the backend's list
 * of handles as an exit takes a backend's state, by a try.
 */
static bool
doltgres_entries_testing_Noting_Exit_run(HostIoIo *io, HostError *error)
{
	HostSharedMine mine;
	uint64		notes[3];
	HostSharedRecord table;
	HostSharedGuard guard;

	if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS, &mine, error))
		return false;
	notes[0] = host_shared_Mine_load(&mine, DOLTGRES_ENTRIES_TESTING_WORD_SECOND);
	if (!doltgres_entries_testing_held_count(&notes[1]))
		notes[1] = PG_UINT64_MAX;
	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, error) ||
		!host_shared_Record_lock(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK, &guard,
								 error))
		return false;
	notes[2] = 1 + (uint64) pg_calls_in_transaction();
	for (size_t index = 0; index < lengthof(notes); index++)
	{
		uint8		bytes[8];

		for (int b = 0; b < 8; b++)
			bytes[b] = (uint8) (notes[index] >> (8 * b));
		if (!host_shared_GuardExclusive_set_entry(&guard, DOLTGRES_ENTRIES_TESTING_NOTES, index, bytes, sizeof(bytes),
												  error))
		{
			host_shared_Guard_drop(&guard);
			return false;
		}
	}
	host_shared_Guard_drop(&guard);
	return true;
}

/* DOLTGRES_ENTRIES_TESTING_NOTING_EXIT_OPS is Noting's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_NOTING_EXIT_OPS = {doltgres_entries_testing_Noting_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_NOTING_TYPE_ID is Noting's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_NOTING_TYPE_ID = {"doltgres::entries::testing::Noting", 0, 1, NULL};

/* doltgres_entries_testing_Failing_Exit_run is Failing's exit work: it returns an error. */
static bool
doltgres_entries_testing_Failing_Exit_run(HostIoIo *io, HostError *error)
{
	static const char failed[] = "the test's exit work failed";
	StdString	message;

	std_string_from_Checked(&message, failed, sizeof(failed) - 1);
	*error = host_Error_fork((const uint8 *) "22000", &message);
	return false;
}

/* DOLTGRES_ENTRIES_TESTING_FAILING_EXIT_OPS is Failing's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_FAILING_EXIT_OPS = {doltgres_entries_testing_Failing_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_FAILING_TYPE_ID is Failing's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_FAILING_TYPE_ID = {"doltgres::entries::testing::Failing", 0, 1,
NULL};

/*
 * doltgres_entries_testing_Pausing_Exit_run is Pausing's exit work: it
 * pauses at doltgres-test-exit-paused and then adds a hundred to the single
 * record's counter.
 */
static bool
doltgres_entries_testing_Pausing_Exit_run(HostIoIo *io, HostError *error)
{
	return host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-exit-paused", error) &&
		doltgres_entries_testing_counter_add(100, error);
}

/* DOLTGRES_ENTRIES_TESTING_PAUSING_EXIT_OPS is Pausing's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_PAUSING_EXIT_OPS = {doltgres_entries_testing_Pausing_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_PAUSING_TYPE_ID is Pausing's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_PAUSING_TYPE_ID = {"doltgres::entries::testing::Pausing", 0, 1,
NULL};

/*
 * doltgres_entries_testing_Relisting_Exit_run is Relisting's exit work: it
 * registers Counting, which its backend listed, and returns the refusal.
 */
static bool
doltgres_entries_testing_Relisting_Exit_run(HostIoIo *io, HostError *error)
{
	return host_at_exit(&DOLTGRES_ENTRIES_TESTING_COUNTING_TYPE_ID, &DOLTGRES_ENTRIES_TESTING_COUNTING_EXIT_OPS,
						error);
}

/* DOLTGRES_ENTRIES_TESTING_RELISTING_EXIT_OPS is Relisting's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_RELISTING_EXIT_OPS = {doltgres_entries_testing_Relisting_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_RELISTING_TYPE_ID is Relisting's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_RELISTING_TYPE_ID = {"doltgres::entries::testing::Relisting", 0, 1,
NULL};

/*
 * doltgres_entries_testing_Listing_Exit_run is Listing's exit work: it
 * registers Failing, which its backend didn't list, and returns the refusal.
 */
static bool
doltgres_entries_testing_Listing_Exit_run(HostIoIo *io, HostError *error)
{
	return host_at_exit(&DOLTGRES_ENTRIES_TESTING_FAILING_TYPE_ID, &DOLTGRES_ENTRIES_TESTING_FAILING_EXIT_OPS, error);
}

/* DOLTGRES_ENTRIES_TESTING_LISTING_EXIT_OPS is Listing's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_LISTING_EXIT_OPS = {doltgres_entries_testing_Listing_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_LISTING_TYPE_ID is Listing's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_LISTING_TYPE_ID = {"doltgres::entries::testing::Listing", 0, 1,
NULL};

/*
 * doltgres_entries_testing_Flagging_Exit_run is Flagging's exit work: it
 * takes the table's gate and then adds ten thousand to the single record's
 * counter.
 */
static bool
doltgres_entries_testing_Flagging_Exit_run(HostIoIo *io, HostError *error)
{
	HostSharedRecord table;
	HostSharedFlagGuard gate;

	if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, error) ||
		!host_shared_Record_flag(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_GATE, &gate, error))
		return false;
	host_shared_FlagGuard_drop(&gate);
	(void) host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_COUNTER_ATOMIC,
									 doltgres_entries_testing_add(10000));
	return true;
}

/* DOLTGRES_ENTRIES_TESTING_FLAGGING_EXIT_OPS is Flagging's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_FLAGGING_EXIT_OPS = {doltgres_entries_testing_Flagging_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_FLAGGING_TYPE_ID is Flagging's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_FLAGGING_TYPE_ID = {"doltgres::entries::testing::Flagging", 0, 1,
NULL};

/* doltgres_entries_testing_free_slots returns the number of free slots of Tested's pool, or -1 when it has none. */
static int64
doltgres_entries_testing_free_slots(void)
{
	size_t		kind;
	std_TupleU32U32_t pool;
	int64		free = 0;
	uint32		end;

	if (!shmem_records_kind(DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape.id, &kind) || !shmem_records_pool(kind, &pool))
		return -1;
	end = STD_ADD(u32, pool.f0, pool.f1, 933, 6);
	for (uint32 slot = pool.f0; slot < end; slot++)
		if (host_shared_word_state(shmem_records_word(slot)) == HOST_SHARED_WORD_STATE_FREE)
			free++;
	return free;
}

/*
 * doltgres_entries_testing_last_generation makes a record in a slot at its
 * last generation, published, and returns its location: it publishes a
 * record, claims it back, stores the slot's last generation over the word,
 * and publishes it again.
 */
static bool
doltgres_entries_testing_last_generation(HostIoIo *io, int64 *result, HostError *error)
{
	HostSharedOwned made;
	HostSharedOwned owned;
	bool		has;
	uint64		first;
	uint32		slot;
	uint64		word;

	if (!host_shared_make(&DOLTGRES_ENTRIES_TESTED_POOL_OPS, io, &made, error))
		return false;
	first = host_shared_Owned_publish(&made);
	if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, first, &has, &owned, error))
		return false;
	slot = (uint32) (first >> 32);
	word = shmem_records_word(slot);
	(void) shmem_records_swap(slot, word, word | (UINT64CONST(0xFFFFFFFF) << 32));
	*result = has ? (int64) host_shared_Owned_publish(&owned) : 0;
	return true;
}

/*
 * doltgres_entries_testing_matching returns how many of a sweep's pairs have
 * a location and a value, a negative one standing for any.
 */
static int64
doltgres_entries_testing_matching(const std_TupleU64U64_t *pairs, size_t pairs_len, int64 location, int64 value)
{
	int64		count = 0;

	for (size_t at = 0; at < pairs_len; at++)
		if ((location < 0 || pairs[at].f0 == (uint64) location) && (value < 0 || pairs[at].f1 == (uint64) value))
			count++;
	return count;
}

/*
 * doltgres_entries_testing_find_frame_0_t is find's state across its pad: the
 * sweep's pairs and the handle it opened, each while live, and where the
 * location and the outcome go.
 */
typedef struct doltgres_entries_testing_find_frame_0_t
{
	HostSharedWithin within;
	uint64		key;
	StdVec		pairs;
	bool		pairs_live;
	HostSharedHeld held;
	bool		held_live;
	bool	   *has_result;
	uint64	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_find_frame_0_t;

/* doltgres_entries_testing_find_body_0 sweeps and opens each record the sweep lists with the key. */
static void
doltgres_entries_testing_find_body_0(void *frame_)
{
	doltgres_entries_testing_find_frame_0_t *frame = frame_;

	if (!host_shared_sweep(frame->within, &DOLTGRES_ENTRIES_TESTED_POOL_OPS, DOLTGRES_ENTRIES_TESTING_TESTED_KEY,
						   &frame->pairs, frame->error))
		return;
	frame->pairs_live = true;
	for (size_t at = 0; at < frame->pairs.len; at++)
	{
		const std_TupleU64U64_t *pair = &((const std_TupleU64U64_t *) frame->pairs.ptr)[at];
		bool		has;
		bool		found;

		if (pair->f1 != frame->key)
			continue;
		if (!host_shared_open(frame->within, &DOLTGRES_ENTRIES_TESTED_POOL_OPS, pair->f0, &has, &frame->held,
							  frame->error))
			return;
		if (!has)
			continue;
		frame->held_live = true;
		found = host_shared_Record_atomic(host_shared_Held_record(&frame->held), DOLTGRES_ENTRIES_TESTING_TESTED_KEY,
										  DOLTGRES_ENTRIES_TESTING_LOAD) == frame->key;
		frame->held_live = false;
		host_shared_Held_drop(&frame->held);
		if (found)
		{
			*frame->has_result = true;
			*frame->result = pair->f0;
			frame->ok = true;
			return;
		}
	}
	*frame->has_result = false;
	frame->ok = true;
}

/* doltgres_entries_testing_find_pad_0 closes the handle and drops the pairs of an unwound find. */
static void
doltgres_entries_testing_find_pad_0(void *frame_)
{
	doltgres_entries_testing_find_frame_0_t *frame = frame_;

	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	if (frame->pairs_live)
		std_vec_free_Checked(&frame->pairs, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->held_live = frame->pairs_live = false;
}

/*
 * doltgres_entries_testing_find returns the location of the published record
 * of Tested whose key is a value, as a crate that keys its records does: it
 * sweeps, opens each record the sweep lists with the key, and reads the key
 * again under its handle.
 */
static bool
doltgres_entries_testing_find(HostSharedWithin within, uint64 key, bool *has_result, uint64 *result,
							  HostError *error)
{
	doltgres_entries_testing_find_frame_0_t frame = {.within = within, .key = key, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_find_body_0, doltgres_entries_testing_find_pad_0);
	doltgres_entries_testing_find_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_none returns the error of a step whose location names no record or whose backend keeps no handle. */
static PgPgError
doltgres_entries_testing_none(const char *what)
{
	StdStr		named = {what, strlen(what)};
	StdFmtPiece pieces[] = {STD_FMT_LIT("the test has no "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&named, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return pg_PgError_fork((const uint8 *) "XX000", &message);
}

/* doltgres_TupleI64Kept_t is an (i64, cache::Kept): a kept pin with its key's number. */
typedef struct doltgres_TupleI64Kept_t
{
	int64		f0;
	HostCacheKept f1;
} doltgres_TupleI64Kept_t;

/*
 * DoltgresEntriesTestingList is one of OWNED, HELD, LOOKS, and KEPT: its
 * BackendLocal and its type, its lock's type, and its handles' size,
 * alignment, and drop.
 */
typedef struct DoltgresEntriesTestingList
{
	HostLocalBackendLocal *local;
	const StdDefaultType *type;
	const HostLockType *lock_type;
	size_t		size;
	size_t		align;
	void		(*drop) (void *value);
} DoltgresEntriesTestingList;

/* DOLTGRES_ENTRIES_TESTING_OWNED_LIST is OWNED as a list. */
static const DoltgresEntriesTestingList DOLTGRES_ENTRIES_TESTING_OWNED_LIST = {&DOLTGRES_ENTRIES_TESTING_OWNED.local,
	&DOLTGRES_ENTRIES_TESTING_OWNED_TYPE, &DOLTGRES_ENTRIES_TESTING_OWNED_LOCK_TYPE, sizeof(HostSharedOwned),
_Alignof(HostSharedOwned), doltgres_entries_testing_Owned_drop};

/* DOLTGRES_ENTRIES_TESTING_HELD_LIST is HELD as a list. */
static const DoltgresEntriesTestingList DOLTGRES_ENTRIES_TESTING_HELD_LIST = {&DOLTGRES_ENTRIES_TESTING_HELD.local,
	&DOLTGRES_ENTRIES_TESTING_HELD_TYPE, &DOLTGRES_ENTRIES_TESTING_HELD_LOCK_TYPE, sizeof(HostSharedHeld),
_Alignof(HostSharedHeld), doltgres_entries_testing_Held_drop};

/* DOLTGRES_ENTRIES_TESTING_LOOKS_LIST is LOOKS as a list. */
static const DoltgresEntriesTestingList DOLTGRES_ENTRIES_TESTING_LOOKS_LIST = {&DOLTGRES_ENTRIES_TESTING_LOOKS.local,
	&DOLTGRES_ENTRIES_TESTING_LOOKS_TYPE, &DOLTGRES_ENTRIES_TESTING_LOOKS_LOCK_TYPE, sizeof(HostSharedReclaim),
_Alignof(HostSharedReclaim), doltgres_entries_testing_Reclaim_drop};

/*
 * DoltgresEntriesTestingListOp is what a step does with a list under its
 * lock: push a handle, pop the last, read the last's atomic, take the kept
 * pin of a number, reserve room for 64 pins, or take the whole list.
 */
typedef enum DoltgresEntriesTestingListOp
{
	DOLTGRES_ENTRIES_TESTING_LIST_PUSH,
	DOLTGRES_ENTRIES_TESTING_LIST_POP,
	DOLTGRES_ENTRIES_TESTING_LIST_LAST,
	DOLTGRES_ENTRIES_TESTING_LIST_TAKE,
	DOLTGRES_ENTRIES_TESTING_LIST_RESERVE,
	DOLTGRES_ENTRIES_TESTING_LIST_TAKE_ALL,
} DoltgresEntriesTestingListOp;

/*
 * doltgres_entries_testing_list_frame_0_t is a list operation's state across
 * its pad: the list, the door, the operation and the value it moves, the
 * backend's list and its guard, each while live, and whether it found one.
 */
typedef struct doltgres_entries_testing_list_frame_0_t
{
	const DoltgresEntriesTestingList *list;
	HostIoIo   *io;
	DoltgresEntriesTestingListOp op;
	void	   *value;
	bool		value_live;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	bool		has;
} doltgres_entries_testing_list_frame_0_t;

/* doltgres_entries_testing_list_body_0 takes the backend's list under its lock and does the operation. */
static void
doltgres_entries_testing_list_body_0(void *frame_)
{
	doltgres_entries_testing_list_frame_0_t *frame = frame_;
	StdVec	   *values;

	frame->local = host_local_BackendLocal_get(frame->list->local, frame->list->type);
	frame->local_live = true;
	frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local,
																						   frame->list->type),
										  frame->list->lock_type, &frame->io->held);
	frame->guard_live = true;
	values = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
	switch (frame->op)
	{
		case DOLTGRES_ENTRIES_TESTING_LIST_PUSH:
			std_vec_push_Checked(values, frame->value, frame->list->size, frame->list->align);
			frame->value_live = false;
			break;
		case DOLTGRES_ENTRIES_TESTING_LIST_POP:
			frame->has = std_vec_pop(values, frame->value, frame->list->size);
			break;
		case DOLTGRES_ENTRIES_TESTING_LIST_LAST:
			frame->has = values->len > 0;
			if (frame->has)
				*(uint64 *) frame->value =
					host_shared_Record_atomic(host_shared_Held_record(&((const HostSharedHeld *) values->ptr)[values->len - 1]),
											  DOLTGRES_ENTRIES_TESTING_TESTED_COUNT, DOLTGRES_ENTRIES_TESTING_LOAD);
			break;
		case DOLTGRES_ENTRIES_TESTING_LIST_TAKE:
			for (size_t at = 0; at < values->len && !frame->has; at++)
			{
				if (((const doltgres_TupleI64Kept_t *) values->ptr)[at].f0 != ((const doltgres_TupleI64Kept_t *) frame->value)->f0)
					continue;
				std_vec_swap_remove(values, at, frame->value, frame->list->size, frame->list->align);
				frame->has = true;
			}
			break;
		case DOLTGRES_ENTRIES_TESTING_LIST_RESERVE:
			std_vec_reserve_Checked(values, 64, frame->list->size, frame->list->align);
			break;
		case DOLTGRES_ENTRIES_TESTING_LIST_TAKE_ALL:
			*(StdVec *) frame->value = *values;
			std_vec_new(values, frame->list->size, frame->list->align);
			break;
	}
}

/* doltgres_entries_testing_list_pad_0 drops an unmoved value, then the guard, then the backend's list. */
static void
doltgres_entries_testing_list_pad_0(void *frame_)
{
	doltgres_entries_testing_list_frame_0_t *frame = frame_;

	if (frame->value_live)
		frame->list->drop(frame->value);
	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->local_live)
		host_local_Local_drop(&frame->local, frame->list->type);
	frame->value_live = frame->guard_live = frame->local_live = false;
}

/*
 * doltgres_entries_testing_list does an operation on one of the backend's
 * lists under its lock, X.get().lock(io) followed by a push, a pop, or a read
 * of the last handle's atomic, and returns whether a pop or a read found a
 * handle; a push takes the value.
 */
static bool
doltgres_entries_testing_list(const DoltgresEntriesTestingList *list, HostIoIo *io, DoltgresEntriesTestingListOp op,
							  void *value)
{
	doltgres_entries_testing_list_frame_0_t frame = {.list = list, .io = io, .op = op, .value = value,
	.value_live = op == DOLTGRES_ENTRIES_TESTING_LIST_PUSH};

	std_landing(&frame, doltgres_entries_testing_list_body_0, doltgres_entries_testing_list_pad_0);
	doltgres_entries_testing_list_pad_0(&frame);
	return frame.has;
}

/*
 * doltgres_entries_testing_shared_frame_0_t is doltgres_test_shared's state
 * across its pad: its arguments, its door, and each handle, guard, reader,
 * and list a step holds, with whether it is live, and where the answer and
 * the outcome go.
 */
typedef struct doltgres_entries_testing_shared_frame_0_t
{
	const PgCaps *caps;
	int32		step;
	uint64		location;
	int64		value;
	HostIoIo	io;
	HostSharedHeld held;
	bool		held_live;
	HostSharedOwned owned;
	bool		owned_live;
	StdVec		vec;
	bool		vec_live;
	size_t		vec_size;
	size_t		vec_align;

	/*
	 * vec_of_bytes is whether the list's elements are byte vectors, each
	 * freed with it.
	 */
	bool		vec_of_bytes;
	HostSharedEpochs epochs;
	bool		epochs_live;
	HostSharedReader reader;
	bool		reader_live;
	HostSharedGuard guard;
	bool		guard_live;
	HostSharedFlagGuard flag;
	bool		flag_live;
	HostSharedLongGuard barrier;
	bool		barrier_live;
	HostSharedLongGuard lists;
	bool		lists_live;
	int64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_shared_frame_0_t;

/* doltgres_entries_testing_shared_raised fails a step with a host error raised, as map_err(pghost::raised). */
static void
doltgres_entries_testing_shared_raised(doltgres_entries_testing_shared_frame_0_t *frame, HostError *failed)
{
	*frame->error = pghost_raised(failed);
}

/* doltgres_entries_testing_shared_answer ends a step with its answer. */
static void
doltgres_entries_testing_shared_answer(doltgres_entries_testing_shared_frame_0_t *frame, int64 answer)
{
	*frame->result = answer;
	frame->ok = true;
}

/* doltgres_entries_testing_pushed is relist's change of a step that publishes: the location pushed onto the table's. */
static void
doltgres_entries_testing_pushed(StdVec *locations, void *captures)
{
	std_vec_push_Checked(locations, captures, sizeof(uint64), _Alignof(uint64));
}

/* doltgres_entries_testing_le_word returns the little-endian word of 8 bytes, or 0 for bytes of another length. */
static int64
doltgres_entries_testing_le_word(const StdVec *bytes)
{
	int64		word = 0;

	if (bytes->len == 8)
		(void) doltgres_entries_testing_test_value((const uint8 *) bytes->ptr, bytes->len, &word);
	return word;
}

/* doltgres_entries_testing_le_bytes writes a value's 8 little-endian bytes. */
static void
doltgres_entries_testing_le_bytes(uint64 value, uint8 bytes[8])
{
	for (int b = 0; b < 8; b++)
		bytes[b] = (uint8) (value >> (8 * b));
}

/* doltgres_entries_testing_shared_vec keeps a list a step holds, with its elements' size and alignment. */
static void
doltgres_entries_testing_shared_vec(doltgres_entries_testing_shared_frame_0_t *frame, size_t size, size_t align)
{
	frame->vec_live = true;
	frame->vec_size = size;
	frame->vec_align = align;
}

/* doltgres_entries_testing_shared_listing_step runs one of the steps that take no record, answering whether it was one. */
static bool
doltgres_entries_testing_shared_listing_step(doltgres_entries_testing_shared_frame_0_t *frame)
{
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostIoIo   *io = &frame->io;
	uint64		location = frame->location;
	int64		value = frame->value;
	HostError	failed;
	HostSharedRecord table;
	bool		has;

	switch (frame->step)
	{
		case 0:
			if (!host_shared_make(&DOLTGRES_ENTRIES_TESTED_POOL_OPS, io, &frame->owned, &failed))
				break;
			location = host_shared_Owned_publish(&frame->owned);
			if (!doltgres_entries_testing_relist(io, doltgres_entries_testing_pushed, &location, &failed))
				break;
			doltgres_entries_testing_shared_answer(frame, (int64) location);
			return true;
		case 9:
			if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &has,
								  &frame->held, &failed))
				break;
			if (has)
				(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_HELD_LIST, io,
													 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &frame->held);
			doltgres_entries_testing_shared_answer(frame, has ? 1 : 0);
			return true;
		case 10:
			if (!doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_HELD_LIST, io, DOLTGRES_ENTRIES_TESTING_LIST_POP,
											   &frame->held))
			{
				*frame->error = doltgres_entries_testing_none("open record");
				return true;
			}
			host_shared_Held_drop(&frame->held);
			doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 11:
			if (!host_shared_read(io, &frame->reader, &failed))
				break;
			frame->reader_live = true;
			if (pg_calls_injection_point(raise, "doltgres-shared-reading", frame->error))
				doltgres_entries_testing_shared_answer(frame, (int64) host_shared_Reader_epoch(&frame->reader));
			return true;
		case 12:
		case 13:
			if (!host_shared_epochs(io, &frame->epochs, &failed))
				break;
			frame->epochs_live = true;
			doltgres_entries_testing_shared_answer(frame,
												   (int64) (frame->step == 12 ? host_shared_Epochs_oldest(&frame->epochs) :
															host_shared_Epochs_advance(&frame->epochs)));
			return true;
		case 14:
			if (!doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_OWNED_LIST, io,
											   DOLTGRES_ENTRIES_TESTING_LIST_POP, &frame->owned))
			{
				*frame->error = doltgres_entries_testing_none("record of its own");
				return true;
			}
			host_shared_free(&frame->owned);
			doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 16:
			doltgres_entries_testing_shared_answer(frame, doltgres_entries_testing_free_slots());
			return true;
		case 17:
			if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &has,
								   &frame->owned, &failed))
				break;
			if (has)
				(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_OWNED_LIST, io,
													 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &frame->owned);
			doltgres_entries_testing_shared_answer(frame, has ? 1 : 0);
			return true;
		case 18:
			{
				static const char failed_claim[] = "the test failed after its claim";

				if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &has,
									   &frame->owned, &failed))
					break;
				frame->owned_live = has;
				*frame->error = doltgres_entries_internal(failed_claim, sizeof(failed_claim) - 1);
				return true;
			}
		case 19:
			if (!host_shared_make(&DOLTGRES_ENTRIES_TESTED_POOL_OPS, io, &frame->owned, &failed))
				break;
			(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_OWNED_LIST, io,
												 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &frame->owned);
			doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 20:
			if (!doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_OWNED_LIST, io,
											   DOLTGRES_ENTRIES_TESTING_LIST_POP, &frame->owned))
			{
				*frame->error = doltgres_entries_testing_none("record of its own");
				return true;
			}
			location = host_shared_Owned_publish(&frame->owned);
			if (!doltgres_entries_testing_relist(io, doltgres_entries_testing_pushed, &location, &failed))
				break;
			doltgres_entries_testing_shared_answer(frame, (int64) location);
			return true;
		case 21:
			{
				HostSharedLook look;

				if (!host_shared_reclaim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &look,
										 &failed))
					break;
				if (look.tag == HOST_SHARED_LOOK_MINE)
					(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_LOOKS_LIST, io,
														 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &look.u.mine);
				doltgres_entries_testing_shared_answer(frame, look.tag == HOST_SHARED_LOOK_MINE ? 1 :
													   look.tag == HOST_SHARED_LOOK_THEIRS ? 2 : 0);
				return true;
			}
		case 22:
		case 23:
			{
				HostSharedReclaim look;

				if (!doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_LOOKS_LIST, io,
												   DOLTGRES_ENTRIES_TESTING_LIST_POP, &look))
				{
					*frame->error = doltgres_entries_testing_none("reclaim");
					return true;
				}
				if (frame->step == 22)
				{
					doltgres_entries_testing_shared_answer(frame, host_shared_Reclaim_free(&look) ? 1 : 0);
					return true;
				}
				host_shared_Reclaim_drop(&look);
				doltgres_entries_testing_shared_answer(frame, 0);
				return true;
			}
		case 24:
			{
				HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed))
					break;
				exchange.u.exchange.f0 = host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_RECLAIMS,
																   DOLTGRES_ENTRIES_TESTING_LOAD);
				exchange.u.exchange.f1 = (uint64) value;
				(void) host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_RECLAIMS, exchange);
				doltgres_entries_testing_shared_answer(frame, 0);
				return true;
			}
		case 25:
		case 26:
			if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed))
				break;
			doltgres_entries_testing_shared_answer(frame,
												   (int64) host_shared_Record_atomic(table, DOLTGRES_ENTRIES_TESTING_COUNTER_ATOMIC,
																					 frame->step == 25 ? doltgres_entries_testing_add((uint64) value) :
																					 DOLTGRES_ENTRIES_TESTING_LOAD));
			return true;
		case 27:
			if (value == 0)
			{
				if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_OTHER_POOL_OPS, location, &has,
									  &frame->held, &failed))
					break;
				if (has)
					host_shared_Held_drop(&frame->held);
			}
			else if (value == 1)
			{
				if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_OTHER_POOL_OPS, location, &has,
									   &frame->owned, &failed))
					break;
				if (has)
					host_shared_Owned_drop(&frame->owned);
			}
			else
			{
				HostSharedLook look;

				if (!host_shared_reclaim(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_OTHER_POOL_OPS, location, &look,
										 &failed))
					break;
				has = look.tag == HOST_SHARED_LOOK_MINE;
				host_shared_Look_drop(&look);
			}
			doltgres_entries_testing_shared_answer(frame, has ? 1 : 0);
			return true;
		case 28:
			{
				int64		made;

				if (!doltgres_entries_testing_last_generation(io, &made, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, made);
				return true;
			}
		case 29:
			if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
				!host_shared_Record_flag(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_GATE, &frame->flag,
										 &failed))
				break;
			frame->flag_live = true;
			if (pg_calls_injection_point(raise, "doltgres-shared-gated", frame->error))
				doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 33:
			doltgres_entries_testing_shared_answer(frame,
												   (int64) host_shared_word_holders(shmem_records_word((uint32) (location >> 32))));
			return true;
		case 34:
		case 58:
			{
				HostSharedMine mine;

				if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS, &mine, &failed))
					break;
				host_shared_Mine_store(&mine, DOLTGRES_ENTRIES_TESTING_WORD_FIRST, (uint64) value);
				if (frame->step == 58)
				{
					if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-paused", &failed))
						break;
					host_shared_Mine_store(&mine, DOLTGRES_ENTRIES_TESTING_WORD_SECOND, (uint64) value);
				}
				else
					host_shared_Mine_store(&mine, DOLTGRES_ENTRIES_TESTING_WORD_SECOND,
										   STD_ADD(u64, (uint64) value, 1, 1125, 38));
				doltgres_entries_testing_shared_answer(frame, (int64) host_shared_proc_number());
				return true;
			}
		case 35:
		case 69:
			{
				const HostSharedProcSlotsOps *slots = frame->step == 35 ? &DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS :
					&DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS;
				uint64		word;

				if (!host_shared_load(slots, value == 0 ? 0 : 1, (uint32) location, &word, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, (int64) word);
				return true;
			}
		case 36:
			{
				HostSharedMine mine;

				if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS, &mine, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame,
													   (int64) host_shared_Mine_load(&mine, DOLTGRES_ENTRIES_TESTING_WORD_SECOND));
				return true;
			}
		case 37:
			doltgres_entries_testing_shared_answer(frame, (int64) host_shared_proc_number());
			return true;
		case 38:
			{
				uint8		bytes[8];

				doltgres_entries_testing_le_bytes((uint64) value, bytes);
				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
					!host_shared_Record_lock(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK,
											 &frame->guard, &failed))
					break;
				frame->guard_live = true;
				if (!host_shared_GuardExclusive_set_entry(&frame->guard, DOLTGRES_ENTRIES_TESTING_NOTES, (size_t) location,
														  bytes, sizeof(bytes), &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, 0);
				return true;
			}
		case 39:
			{
				bool		listed = false;

				if (!host_shared_processes(&DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS, &frame->vec, &failed))
					break;
				doltgres_entries_testing_shared_vec(frame, sizeof(uint32), _Alignof(uint32));
				for (size_t at = 0; at < frame->vec.len; at++)
				{
					uint64		word;

					if (!host_shared_load(&DOLTGRES_ENTRIES_WORDS_PROC_SLOTS_OPS, DOLTGRES_ENTRIES_TESTING_WORD_FIRST,
										  ((const uint32 *) frame->vec.ptr)[at], &word, &failed))
					{
						doltgres_entries_testing_shared_raised(frame, &failed);
						return true;
					}
				}
				for (size_t at = 0; at < frame->vec.len && !listed; at++)
					listed = ((const uint32 *) frame->vec.ptr)[at] == (uint32) location;
				doltgres_entries_testing_shared_answer(frame, ((int64) frame->vec.len << 32) | (listed ? 1 : 0));
				return true;
			}
		case 40:
			if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
				!host_shared_Record_lock_shared(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK,
												&frame->guard, &failed))
				break;
			frame->guard_live = true;
			has = host_shared_Guard_entry(&frame->guard, DOLTGRES_ENTRIES_TESTING_NOTES, (size_t) location, &frame->vec,
										  &failed);
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			if (!has)
				break;
			doltgres_entries_testing_shared_vec(frame, 1, 1);
			doltgres_entries_testing_shared_answer(frame, doltgres_entries_testing_le_word(&frame->vec));
			return true;
		case 47:
			{
				uint64		named;

				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame,
													   host_shared_Record_named(table, DOLTGRES_ENTRIES_TESTING_HITS, &named) ? (int64) named : -1);
				return true;
			}
		case 50:
		case 55:
			if (!host_shared_sweep(HOST_SHARED_IO_WITHIN(io),
								   frame->step == 50 ? &DOLTGRES_ENTRIES_TESTED_POOL_OPS : &DOLTGRES_ENTRIES_OTHER_POOL_OPS,
								   frame->step == 50 ? DOLTGRES_ENTRIES_TESTING_TESTED_KEY : DOLTGRES_ENTRIES_TESTING_OTHER_KEY,
								   &frame->vec, &failed))
				break;
			doltgres_entries_testing_shared_vec(frame, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
			doltgres_entries_testing_shared_answer(frame,
												   doltgres_entries_testing_matching(frame->vec.ptr, frame->vec.len,
																					 (int64) location, value));
			return true;
		case 53:
			if (!host_shared_make(&DOLTGRES_ENTRIES_TESTED_POOL_OPS, io, &frame->owned, &failed))
				break;
			(void) host_shared_Record_atomic(host_shared_Owned_record(&frame->owned), DOLTGRES_ENTRIES_TESTING_TESTED_KEY,
											 doltgres_entries_testing_add((uint64) value));
			(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_OWNED_LIST, io,
												 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &frame->owned);
			doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 54:
			if (!host_shared_make(&DOLTGRES_ENTRIES_OTHER_POOL_OPS, io, &frame->owned, &failed))
				break;
			(void) host_shared_Record_atomic(host_shared_Owned_record(&frame->owned), DOLTGRES_ENTRIES_TESTING_OTHER_KEY,
											 doltgres_entries_testing_add((uint64) value));
			doltgres_entries_testing_shared_answer(frame, (int64) host_shared_Owned_publish(&frame->owned));
			return true;
		case 56:
			{
				uint64		found;

				if (!doltgres_entries_testing_find(HOST_SHARED_IO_WITHIN(io), (uint64) value, &has, &found, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, has ? (int64) found : -1);
				return true;
			}
		case 57:
			{
				uint64		found;

				if (!doltgres_entries_testing_find(HOST_SHARED_IO_WITHIN(io), (uint64) value, &has, &found, &failed))
					break;
				if (has)
				{
					doltgres_entries_testing_shared_answer(frame, (int64) found);
					return true;
				}
				if (!pg_calls_injection_point(raise, "doltgres-shared-finding", frame->error))
					return true;
				if (!host_shared_make(&DOLTGRES_ENTRIES_TESTED_POOL_OPS, io, &frame->owned, &failed))
					break;
				frame->owned_live = true;
				(void) host_shared_Record_atomic(host_shared_Owned_record(&frame->owned),
												 DOLTGRES_ENTRIES_TESTING_TESTED_KEY,
												 doltgres_entries_testing_add((uint64) value));
				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
					!host_shared_Record_lock(table, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_TABLE_LOCK,
											 &frame->guard, &failed))
					break;
				frame->guard_live = true;
				if (!doltgres_entries_testing_find(HOST_SHARED_GUARD_WITHIN(&frame->guard), (uint64) value, &has, &found,
												   &failed))
					break;
				if (has)
				{
					doltgres_entries_testing_shared_answer(frame, (int64) found);
					return true;
				}
				frame->owned_live = false;
				doltgres_entries_testing_shared_answer(frame, (int64) host_shared_Owned_publish(&frame->owned));
				return true;
			}
		case 63:
			{
				bool		gone;

				if (!host_shared_gone(value == 0 ? &DOLTGRES_ENTRIES_TESTED_POOL_OPS : &DOLTGRES_ENTRIES_OTHER_POOL_OPS,
									  location, &gone, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, gone ? 1 : 0);
				return true;
			}
		case 64:
			{
				uint64		count;

				if (!doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_HELD_LIST, io,
												   DOLTGRES_ENTRIES_TESTING_LIST_LAST, &count))
				{
					*frame->error = doltgres_entries_testing_none("open record");
					return true;
				}
				doltgres_entries_testing_shared_answer(frame, (int64) count);
				return true;
			}
		case 65:
			std_AtomicU64_store_relaxed(&DOLTGRES_ENTRIES_TESTING_EXIT_OPENS, location);
			if (!pg_calls_exit_callback(raise, frame->error))
				return true;
			if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &has,
								  &frame->held, &failed))
				break;
			if (has)
				host_shared_Held_drop(&frame->held);
			if (pg_calls_cancel_exit_callback(raise, frame->error))
				doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 66:
			std_AtomicU64_store_relaxed(&DOLTGRES_ENTRIES_TESTING_EXIT_OPENS, location);
			for (int64 at = 0; at < value; at++)
				if (!pg_calls_exit_callback(raise, frame->error))
					return true;
			doltgres_entries_testing_shared_answer(frame, 0);
			return true;
		case 67:
			{
				static const StdTypeId *const types[] = {&DOLTGRES_ENTRIES_TESTING_COUNTING_TYPE_ID,
					&DOLTGRES_ENTRIES_TESTING_NOTING_TYPE_ID, &DOLTGRES_ENTRIES_TESTING_FAILING_TYPE_ID,
					&DOLTGRES_ENTRIES_TESTING_PAUSING_TYPE_ID, &DOLTGRES_ENTRIES_TESTING_RELISTING_TYPE_ID,
				&DOLTGRES_ENTRIES_TESTING_FLAGGING_TYPE_ID, &DOLTGRES_ENTRIES_TESTING_LISTING_TYPE_ID};
				static const HostExitOps *const works[] = {&DOLTGRES_ENTRIES_TESTING_COUNTING_EXIT_OPS,
					&DOLTGRES_ENTRIES_TESTING_NOTING_EXIT_OPS, &DOLTGRES_ENTRIES_TESTING_FAILING_EXIT_OPS,
					&DOLTGRES_ENTRIES_TESTING_PAUSING_EXIT_OPS, &DOLTGRES_ENTRIES_TESTING_RELISTING_EXIT_OPS,
				&DOLTGRES_ENTRIES_TESTING_FLAGGING_EXIT_OPS, &DOLTGRES_ENTRIES_TESTING_LISTING_EXIT_OPS};

				for (size_t bit = 0; bit < lengthof(works); bit++)
					if ((value >> bit & 1) != 0 && !host_at_exit(types[bit], works[bit], &failed))
					{
						doltgres_entries_testing_shared_raised(frame, &failed);
						return true;
					}
				doltgres_entries_testing_shared_answer(frame, 0);
				return true;
			}
		case 68:
			{
				HostSharedMine mine;
				uint64		found;

				if (!host_shared_mine(HOST_IO_IO_TAKES_SHORT(io), &DOLTGRES_ENTRIES_STAYS_PROC_SLOTS_OPS, &mine, &failed))
					break;
				found = host_shared_Mine_load(&mine, DOLTGRES_ENTRIES_TESTING_STAY_FIRST);
				host_shared_Mine_store(&mine, DOLTGRES_ENTRIES_TESTING_STAY_FIRST, (uint64) value);
				host_shared_Mine_store(&mine, DOLTGRES_ENTRIES_TESTING_STAY_SECOND,
									   STD_ADD(u64, (uint64) value, 1, 1249, 38));
				doltgres_entries_testing_shared_answer(frame, (int64) ((uint64) found << 32) | (int64) host_shared_proc_number());
				return true;
			}
		case 32:
			{
				size_t		table_len;
				const HostSharedKind *kinds = doltgres_kinds_KINDS(&table_len);

				if (!pghost_shared_kinds(raise, kinds, table_len, &frame->vec, frame->error))
					return true;
				doltgres_entries_testing_shared_vec(frame, sizeof(ShmemRecordsKind), _Alignof(ShmemRecordsKind));
				doltgres_entries_testing_shared_answer(frame,
													   (int64) shmem_records_size(frame->vec.ptr, frame->vec.len,
																				  (size_t) pg_calls_max_procs()));
				return true;
			}
		case 31:
			{
				size_t		kind;
				std_TupleU32U32_t pool;

				doltgres_entries_testing_shared_answer(frame,
													   shmem_records_kind(DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape.id, &kind) &&
													   shmem_records_pool(kind, &pool) ? (int64) pool.f1 : -1);
				return true;
			}
		default:
			return false;
	}
	doltgres_entries_testing_shared_raised(frame, &failed);
	return true;
}

/* doltgres_entries_testing_shared_record_step runs a step against the record the frame holds open. */
static void
doltgres_entries_testing_shared_record_step(doltgres_entries_testing_shared_frame_0_t *frame)
{
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostIoIo   *io = &frame->io;
	HostIoTakesShort short_io = HOST_IO_IO_TAKES_SHORT(io);
	HostSharedRecord record = host_shared_Held_record(&frame->held);
	int64		value = frame->value;
	HostError	failed;
	HostSharedRecord table;

	switch (frame->step)
	{
		case 1:
		case 15:
			if (frame->step == 1)
			{
				std_vec_with_capacity_Checked(&frame->vec, 8, 1, 1);
				doltgres_entries_testing_le_bytes((uint64) value, (uint8 *) frame->vec.ptr);
				frame->vec.len = 8;
			}
			else
				std_vec_u8_from_elem(&frame->vec, 1, value > 0 ? (size_t) value : 0);
			doltgres_entries_testing_shared_vec(frame, 1, 1);
			if (!host_shared_Record_lock(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, &failed))
				break;
			frame->guard_live = true;
			if (!host_shared_GuardExclusive_write_blob(&frame->guard, DOLTGRES_ENTRIES_TESTING_TESTED_BLOB,
													   (const uint8 *) frame->vec.ptr, frame->vec.len, &failed))
				break;
			doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 2:
			if (!host_shared_Record_lock_shared(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard,
												&failed))
				break;
			frame->guard_live = true;
			frame->vec = host_shared_Guard_read_blob(&frame->guard, DOLTGRES_ENTRIES_TESTING_TESTED_BLOB);
			doltgres_entries_testing_shared_vec(frame, 1, 1);
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			doltgres_entries_testing_shared_answer(frame, doltgres_entries_testing_le_word(&frame->vec));
			return;
		case 3:
		case 30:
			if (!host_shared_Record_lock(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, &failed))
				break;
			frame->guard_live = true;
			if (pg_calls_injection_point(raise, "doltgres-shared-locked", frame->error) &&
				(frame->step == 3 || pg_calls_fatal(raise, frame->error)))
				doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 4:
			{
				HostIoIo	again = pghost_io(raise);
				HostSharedGuard second;

				if (!host_shared_Record_lock(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard,
											 &failed))
					break;
				frame->guard_live = true;
				if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(&again), DOLTGRES_ENTRIES_TESTING_TESTED_LOCK,
											 &second, &failed))
					break;
				host_shared_Guard_drop(&second);
				doltgres_entries_testing_shared_answer(frame, 0);
				return;
			}
		case 5:
			doltgres_entries_testing_shared_answer(frame,
												   (int64) host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_TESTED_COUNT,
																					 doltgres_entries_testing_add((uint64) value)));
			return;
		case 6:
			doltgres_entries_testing_shared_answer(frame,
												   (int64) host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_TESTED_COUNT,
																					 DOLTGRES_ENTRIES_TESTING_LOAD));
			return;
		case 7:
		case 8:
			if (!host_shared_Record_flag(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_FLAG, &frame->flag, &failed))
				break;
			frame->flag_live = true;
			if (frame->step == 8)
			{
				frame->flag_live = false;
				host_shared_FlagGuard_drop(&frame->flag);
				doltgres_entries_testing_shared_answer(frame, 0);
				return;
			}
			if (pg_calls_injection_point(raise, "doltgres-shared-flagged", frame->error))
				doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 41:
			if (!host_shared_Record_lock_long_shared(record, io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER,
													 &frame->barrier, &failed))
				break;
			frame->barrier_live = true;
			if (pg_calls_injection_point(raise, "doltgres-shared-barred", frame->error))
				doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 42:
			if (!host_shared_Record_lock_long(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER, &frame->barrier,
											  &failed))
				break;
			host_shared_LongGuard_drop(&frame->barrier);
			doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 43:
		case 46:
			{
				bool		marked = false;

				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
					!host_shared_Record_lock_long(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER,
												  &frame->barrier, &failed))
					break;
				frame->barrier_live = true;
				if (frame->step == 43 ?
					!host_shared_Record_mark(table, DOLTGRES_ENTRIES_TESTING_HITS, &frame->barrier, &marked, &failed) :
					!host_shared_Record_unmark(table, DOLTGRES_ENTRIES_TESTING_HITS, &frame->barrier, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, marked ? 1 : 0);
				return;
			}
		case 44:
			{
				uint8		bytes[8];
				bool		claimed;

				doltgres_entries_testing_le_bytes((uint64) value, bytes);
				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
					!host_shared_Record_lock_long_shared(record, io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER,
														 &frame->barrier, &failed))
					break;
				frame->barrier_live = true;
				if (!host_shared_Record_claim(table, DOLTGRES_ENTRIES_TESTING_HITS, &frame->barrier, bytes, sizeof(bytes),
											  &claimed, &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, claimed ? 1 : 0);
				return;
			}
		case 45:
			{
				int64		sum = 0;

				if (!host_shared_single(&DOLTGRES_ENTRIES_TABLE_SINGLE_OPS, &table, &failed) ||
					!host_shared_Record_lock_long(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER,
												  &frame->barrier, &failed))
					break;
				frame->barrier_live = true;
				if (!host_shared_Record_read(table, DOLTGRES_ENTRIES_TESTING_HITS, &frame->barrier, &frame->vec, &failed))
					break;
				doltgres_entries_testing_shared_vec(frame, sizeof(StdVec), _Alignof(StdVec));
				frame->vec_of_bytes = true;
				for (size_t at = 0; at < frame->vec.len; at++)
				{
					const StdVec *entry = &STD_VEC_AT(&frame->vec, StdVec, at);
					int64		hit = 0;

					(void) doltgres_entries_testing_test_value(entry->ptr, entry->len, &hit);
					sum = std_add_i64(sum, hit, STD_ACCUM_FILE, 206, 1);
				}
				doltgres_entries_testing_shared_answer(frame, ((int64) frame->vec.len << 32) | sum);
				return;
			}
		case 60:
		case 61:
		case 62:
			{
				HostSharedLongIo under;
				bool		shared = frame->step == 62 || (frame->step == 60 && value != 0);

				if (!host_shared_Record_lock_long_shared(record, io, DOLTGRES_ENTRIES_TESTING_TESTED_BARRIER,
														 &frame->barrier, &failed))
					break;
				frame->barrier_live = true;
				under = host_shared_LongGuard_io(&frame->barrier);
				if (shared ?
					!host_shared_LongIo_lock_long_shared(&under, DOLTGRES_ENTRIES_TESTING_TESTED_LISTS, &frame->lists,
														 &failed) :
					!host_shared_LongIo_lock_long(&under, DOLTGRES_ENTRIES_TESTING_TESTED_LISTS, &frame->lists, &failed))
					break;
				frame->lists_live = true;
				if (frame->step == 60 && !pg_calls_injection_point(raise, "doltgres-shared-listed", frame->error))
					return;
				doltgres_entries_testing_shared_answer(frame, 0);
				return;
			}
		case 59:
			if (!host_shared_Record_lock(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, &failed))
				break;
			frame->guard_live = true;
			if (!host_pause(HOST_SHARED_GUARD_PAUSES(&frame->guard), "doltgres-test-paused-locked", &failed))
				break;
			doltgres_entries_testing_shared_answer(frame, 0);
			return;
		case 48:
			{
				uint8		bytes[8];

				doltgres_entries_testing_le_bytes((uint64) value, bytes);
				if (!host_shared_Record_lock(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard,
											 &failed))
					break;
				frame->guard_live = true;
				if (!host_shared_GuardExclusive_set_entry(&frame->guard, DOLTGRES_ENTRIES_TESTING_TESTED_NOTE, 0, bytes,
														  sizeof(bytes), &failed))
					break;
				doltgres_entries_testing_shared_answer(frame, 0);
				return;
			}
		case 49:
			if (!host_shared_Record_lock_shared(record, short_io, DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard,
												&failed))
				break;
			frame->guard_live = true;
			if (!host_shared_Guard_entry(&frame->guard, DOLTGRES_ENTRIES_TESTING_TESTED_NOTE, 0, &frame->vec, &failed))
				break;
			doltgres_entries_testing_shared_vec(frame, 1, 1);
			frame->guard_live = false;
			host_shared_Guard_drop(&frame->guard);
			doltgres_entries_testing_shared_answer(frame, doltgres_entries_testing_le_word(&frame->vec));
			return;
		default:
			doltgres_entries_testing_shared_answer(frame, 0);
			return;
	}
	doltgres_entries_testing_shared_raised(frame, &failed);
}

/* doltgres_entries_testing_shared_body_0 runs the frame's step, opening the record at the location for one that takes it. */
static void
doltgres_entries_testing_shared_body_0(void *frame_)
{
	doltgres_entries_testing_shared_frame_0_t *frame = frame_;
	HostError	failed;
	bool		has;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (doltgres_entries_testing_shared_listing_step(frame))
		return;
	if (!host_shared_open(HOST_SHARED_IO_WITHIN(&frame->io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, frame->location, &has,
						  &frame->held, &failed))
	{
		doltgres_entries_testing_shared_raised(frame, &failed);
		return;
	}
	if (!has)
	{
		*frame->error = doltgres_entries_testing_none("record at the location");
		return;
	}
	frame->held_live = true;
	doltgres_entries_testing_shared_record_step(frame);
}

/* doltgres_entries_testing_shared_pad_0 releases what a step holds, its locks first and the record's handle last. */
static void
doltgres_entries_testing_shared_pad_0(void *frame_)
{
	doltgres_entries_testing_shared_frame_0_t *frame = frame_;

	if (frame->lists_live)
		host_shared_LongGuard_drop(&frame->lists);
	if (frame->barrier_live)
		host_shared_LongGuard_drop(&frame->barrier);
	if (frame->flag_live)
		host_shared_FlagGuard_drop(&frame->flag);
	if (frame->guard_live)
		host_shared_Guard_drop(&frame->guard);
	if (frame->reader_live)
		host_shared_Reader_drop(&frame->reader);
	if (frame->epochs_live)
		host_shared_Epochs_drop(&frame->epochs);
	if (frame->vec_live && frame->vec_of_bytes)
		for (size_t at = 0; at < frame->vec.len; at++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->vec, StdVec, at), 1, 1);
	if (frame->vec_live)
		std_vec_free_Checked(&frame->vec, frame->vec_size, frame->vec_align);
	if (frame->owned_live)
		host_shared_Owned_drop(&frame->owned);
	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	frame->lists_live = frame->barrier_live = frame->flag_live = frame->guard_live = false;
	frame->reader_live = frame->epochs_live = frame->vec_live = frame->owned_live = frame->held_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_shared runs one step against the
 * pooled records of Tested, so a test can show each shared part across
 * backends; testing.rs lists each step. Steps that take a location open its
 * record for the step.
 */
bool
doltgres_entries_testing_doltgres_test_shared(const PgCaps *caps, int32 step, int64 location, int64 value,
											  int64 *result, PgPgError *error)
{
	doltgres_entries_testing_shared_frame_0_t frame = {.caps = caps, .step = step, .location = (uint64) location,
	.value = value, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_shared_body_0, doltgres_entries_testing_shared_pad_0);
	doltgres_entries_testing_shared_pad_0(&frame);
	return frame.ok;
}

/* DOLTGRES_ENTRIES_TESTING_CORPUS is CORPUS: the compression corpus, in the order expected.tsv numbers its inputs. */
static const StdBytes DOLTGRES_ENTRIES_TESTING_CORPUS[] = {
	{DOLTGRES_ENTRIES_TESTING_TEXT, sizeof(DOLTGRES_ENTRIES_TESTING_TEXT)},
	{DOLTGRES_ENTRIES_TESTING_LEAVES, sizeof(DOLTGRES_ENTRIES_TESTING_LEAVES)},
	{DOLTGRES_ENTRIES_TESTING_REPEATED, sizeof(DOLTGRES_ENTRIES_TESTING_REPEATED)},
	{DOLTGRES_ENTRIES_TESTING_RANDOM, sizeof(DOLTGRES_ENTRIES_TESTING_RANDOM)},
};

/* DOLTGRES_ENTRIES_TESTING_LEAF is LEAF: the size of the leaf input's chunks, which train the corpus's dictionary, and of that dictionary. */
#define DOLTGRES_ENTRIES_TESTING_LEAF 4096

/*
 * doltgres_entries_testing_zstd_frame_0_t is doltgres_test_zstd's state
 * across its pad: its arguments, the samples, the trained dictionary, the
 * prepared one, and the output, each while live, and where the datum and the
 * outcome go.
 */
typedef struct doltgres_entries_testing_zstd_frame_0_t
{
	const PgCaps *caps;
	int32		input;
	int32		dictionary;
	StdVec		samples;
	bool		samples_live;
	StdVec		trained;
	bool		trained_live;
	HostCompressDictionary prepared;
	bool		prepared_live;
	StdVec		out;
	bool		out_live;
	PgDatum    *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_zstd_frame_0_t;

/* doltgres_entries_testing_zstd_body_0 compresses the input, or gives the trained dictionary. */
static void
doltgres_entries_testing_zstd_body_0(void *frame_)
{
	doltgres_entries_testing_zstd_frame_0_t *frame = frame_;
	StdBytes	bytes = {NULL, 0};
	const StdBytes *leaves = &DOLTGRES_ENTRIES_TESTING_CORPUS[1];
	size_t		chunks = (leaves->len + DOLTGRES_ENTRIES_TESTING_LEAF - 1) / DOLTGRES_ENTRIES_TESTING_LEAF;
	HostError	failed;

	if (frame->input >= 0 && (size_t) frame->input < lengthof(DOLTGRES_ENTRIES_TESTING_CORPUS))
		bytes = DOLTGRES_ENTRIES_TESTING_CORPUS[frame->input];
	std_vec_with_capacity_Checked(&frame->samples, chunks, sizeof(StdBytes), _Alignof(StdBytes));
	frame->samples_live = true;
	for (size_t at = 0; at < chunks; at++)
	{
		size_t		from = at * DOLTGRES_ENTRIES_TESTING_LEAF;

		((StdBytes *) frame->samples.ptr)[at].ptr = (const uint8 *) leaves->ptr + from;
		((StdBytes *) frame->samples.ptr)[at].len = Min(DOLTGRES_ENTRIES_TESTING_LEAF, leaves->len - from);
	}
	frame->samples.len = chunks;
	if (!host_train(frame->samples.ptr, frame->samples.len, DOLTGRES_ENTRIES_TESTING_LEAF, &frame->trained))
		std_vec_new(&frame->trained, 1, 1);
	frame->trained_live = true;
	switch (frame->dictionary)
	{
		case 0:
			if (!host_compress((const uint8 *) bytes.ptr, bytes.len, -1, NULL, &frame->out, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			break;
		case 1:
			if (!host_prepare_compressing((const uint8 *) frame->trained.ptr, frame->trained.len, -1, &frame->prepared,
										  &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			frame->prepared_live = true;
			if (!host_compress((const uint8 *) bytes.ptr, bytes.len, -1, &frame->prepared, &frame->out, &failed))
			{
				*frame->error = pghost_raised(&failed);
				return;
			}
			frame->prepared_live = false;
			host_CompressDictionary_drop(&frame->prepared);
			break;
		default:
			frame->out = frame->trained;
			frame->trained_live = false;
			break;
	}
	frame->out_live = true;
	frame->ok = pg_calls_bytea_datum(pg_Caps_may_raise(frame->caps), (const uint8 *) frame->out.ptr, frame->out.len,
									 frame->result, frame->error);
}

/* doltgres_entries_testing_zstd_pad_0 drops what the entry holds, the output first. */
static void
doltgres_entries_testing_zstd_pad_0(void *frame_)
{
	doltgres_entries_testing_zstd_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
	if (frame->prepared_live)
		host_CompressDictionary_drop(&frame->prepared);
	if (frame->trained_live)
		std_vec_free_Checked(&frame->trained, 1, 1);
	if (frame->samples_live)
		std_vec_free_Checked(&frame->samples, sizeof(StdBytes), _Alignof(StdBytes));
	frame->out_live = frame->prepared_live = frame->trained_live = frame->samples_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_zstd is the corpus check's zstd, at
 * level -1 as the storage design compresses: it returns a corpus input
 * compressed without a dictionary (0) or with the one trained on the leaf
 * input's chunks (1), or that dictionary itself (2).
 */
bool
doltgres_entries_testing_doltgres_test_zstd(const PgCaps *caps, int32 input, int32 dictionary, PgDatum *result,
											PgPgError *error)
{
	doltgres_entries_testing_zstd_frame_0_t frame = {.caps = caps, .input = input, .dictionary = dictionary,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_zstd_body_0, doltgres_entries_testing_zstd_pad_0);
	doltgres_entries_testing_zstd_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_cache_key returns the node cache key a test names
 * by a number: one location's and one address's, with the number at the end
 * of its locator, highest byte first, so that two numbers next to each other
 * differ only in the key's last byte.
 */
static HostCacheKey
doltgres_entries_testing_cache_key(int64 key)
{
	uint8		locator[HOST_CACHE_LOCATOR] = {0};
	uint8		address[HOST_CACHE_ADDRESS] = {0};

	for (int b = 0; b < 8; b++)
		locator[HOST_CACHE_LOCATOR - 1 - b] = (uint8) ((uint64) key >> (8 * b));
	return host_cache_key(1, address, locator);
}

/*
 * doltgres_entries_testing_cache_counts returns the pin count and the usage
 * count of the shared frame that holds a key, or false when none does.
 */
static bool
doltgres_entries_testing_cache_counts(const PgMayRaise *raise, const HostCacheKey *key, bool *has_result,
									  uint64 *pins, uint64 *usage, PgPgError *error)
{
	ShmemCacheCache shared;

	if (!shmem_cache_shared(&shared))
	{
		*has_result = false;
		return true;
	}
	return shmem_cache_Cache_counts(&shared, raise, host_cache_Key_bytes(key), has_result, pins, usage, error);
}

/* doltgres_entries_testing_TupleI64Kept_drop is (i64, Kept)'s drop: its pin's. */
static void
doltgres_entries_testing_TupleI64Kept_drop(void *value)
{
	host_cache_Kept_drop(&((doltgres_TupleI64Kept_t *) value)->f1);
}

/* doltgres_entries_testing_VecTupleI64Kept_drop is Vec<(i64, Kept)>'s drop. */
static void
doltgres_entries_testing_VecTupleI64Kept_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(doltgres_TupleI64Kept_t), _Alignof(doltgres_TupleI64Kept_t),
						 doltgres_entries_testing_TupleI64Kept_drop);
}

/* taken at Vec<(i64, Kept)> and IoMutex<Vec<(i64, Kept)>>, KEPT's lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecTupleI64Kept
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_TupleI64Kept_t), _Alignof(doltgres_TupleI64Kept_t))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_testing_VecTupleI64Kept_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_KEPT_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecTupleI64Kept
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_TupleI64Kept_t), _Alignof(doltgres_TupleI64Kept_t))
#include "doltgres/host/lock_template.h"

/* doltgres_entries_testing_IoMutexVecTupleI64Kept_drop is IoMutex<Vec<(i64, Kept)>>' drop glue: its value's. */
static void
doltgres_entries_testing_IoMutexVecTupleI64Kept_drop(void *value)
{
	doltgres_entries_testing_VecTupleI64Kept_drop(&((HostLockIoMutexVecTupleI64Kept *) value)->value);
}

/* DOLTGRES_ENTRIES_TESTING_KEPT_TYPE is IoMutex<Vec<(i64, Kept)>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_KEPT_TYPE = {sizeof(HostLockIoMutexVecTupleI64Kept),
	_Alignof(HostLockIoMutexVecTupleI64Kept), host_lock_IoMutexVecTupleI64Kept_Default_default,
doltgres_entries_testing_IoMutexVecTupleI64Kept_drop};

/*
 * DOLTGRES_ENTRIES_TESTING_KEPT is KEPT: the pins the node cache's tests keep
 * across calls, each with its key's number, as a cursor's state keeps its
 * node's pin between fetches; its BackendLocal header and the process's
 * value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecTupleI64Kept process;
}			DOLTGRES_ENTRIES_TESTING_KEPT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DOLTGRES_ENTRIES_TESTING_KEPT_LIST is KEPT as a list. */
static const DoltgresEntriesTestingList DOLTGRES_ENTRIES_TESTING_KEPT_LIST = {&DOLTGRES_ENTRIES_TESTING_KEPT.local,
	&DOLTGRES_ENTRIES_TESTING_KEPT_TYPE, &DOLTGRES_ENTRIES_TESTING_KEPT_LOCK_TYPE, sizeof(doltgres_TupleI64Kept_t),
_Alignof(doltgres_TupleI64Kept_t), doltgres_entries_testing_TupleI64Kept_drop};

/* doltgres_entries_testing_doltgres_test_cache_closure_0 is the fill install takes: every byte of the node the captured byte. */
static void
doltgres_entries_testing_doltgres_test_cache_closure_0(void *captures, uint8 *bytes, size_t len)
{
	memset(bytes, *(const uint8 *) captures, len);
}

/*
 * doltgres_entries_testing_cache_frame_0_t is doltgres_test_cache's state
 * across its pad: its arguments, its door, the read and the pin a step holds,
 * each while live, and where the answer and the outcome go.
 */
typedef struct doltgres_entries_testing_cache_frame_0_t
{
	const PgCaps *caps;
	int32		step;
	int64		number;
	int64		value;
	HostIoIo	io;
	HostCacheRead read;
	bool		read_live;
	HostCachePinned pinned;
	bool		pinned_live;
	int64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_cache_frame_0_t;

/* doltgres_entries_testing_cache_first returns the first byte of the pinned node, with the index's check at a line and column. */
static int64
doltgres_entries_testing_cache_first(const HostCachePinned *pinned, uint32 line, uint32 col)
{
	StdBytes	bytes = host_cache_Pinned_Deref_deref(pinned);

	std_index_check(0, bytes.len, RUST_FILE, line, col);
	return (int64) bytes.ptr[0];
}

/* doltgres_entries_testing_cache_keep keeps the frame's pin with its number in KEPT. */
static void
doltgres_entries_testing_cache_keep(doltgres_entries_testing_cache_frame_0_t *frame)
{
	doltgres_TupleI64Kept_t kept = {.f0 = frame->number};

	frame->pinned_live = false;
	kept.f1 = host_cache_Pinned_keep(&frame->pinned);
	(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_KEPT_LIST, &frame->io,
										 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &kept);
}

/* doltgres_entries_testing_cache_body_0 runs the frame's step on the node cache. */
static void
doltgres_entries_testing_cache_body_0(void *frame_)
{
	doltgres_entries_testing_cache_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_Caps_may_raise(frame->caps);
	HostIoIo   *io = &frame->io;
	HostCacheKey key = doltgres_entries_testing_cache_key(frame->number);
	uint8		byte = (uint8) frame->value;
	int32		step = frame->step;
	HostError	failed;
	bool		has;
	bool		found;
	uint64		pins;
	uint64		usage;

	frame->io = pghost_io(raise);
	switch (step)
	{
		case 0:
		case 3:
		case 5:
		case 9:
			{
				HostCachePlacement placement = step == 5 ? HOST_CACHE_PLACEMENT_BACKEND : HOST_CACHE_PLACEMENT_SERVER;

				if (step == 0)
					pg_calls_transaction_owner(true);
				found = host_cache_install(io, placement, &key, step == 9 ? 40000 : 100, doltgres_entries_testing_doltgres_test_cache_closure_0,
										   &byte, NULL, &frame->read, &failed);
				if (step == 0)
					pg_calls_transaction_owner(false);
				if (!found)
					break;
				if (frame->read.tag == HOST_CACHE_READ_CACHED && step == 0)
				{
					frame->pinned = frame->read.u.cached;
					frame->pinned_live = true;
					doltgres_entries_testing_cache_keep(frame);
				}
				else
					frame->read_live = true;
				*frame->result = frame->read.tag == HOST_CACHE_READ_CACHED ? 1 : 0;
				frame->ok = true;
				return;
			}
		case 1:
		case 8:
			if (step == 1)
			{
				pg_calls_transaction_owner(true);
				found = host_cache_get(io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &frame->pinned, &failed);
				pg_calls_transaction_owner(false);
				if (!found)
					break;
			}
			else
			{
				doltgres_TupleI64Kept_t taken = {.f0 = frame->number};

				has = doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_KEPT_LIST, io,
													DOLTGRES_ENTRIES_TESTING_LIST_TAKE, &taken) &&
					host_cache_Kept_resume(&taken.f1, io, &frame->pinned);
			}
			if (!has)
			{
				*frame->result = -1;
				frame->ok = true;
				return;
			}
			frame->pinned_live = true;
			*frame->result = doltgres_entries_testing_cache_first(&frame->pinned, step == 1 ? 1477 : 1501, 44);
			doltgres_entries_testing_cache_keep(frame);
			frame->ok = true;
			return;
		case 2:
		case 10:
			if (!doltgres_entries_testing_cache_counts(raise, &key, &has, &pins, &usage, frame->error))
				return;
			*frame->result = has ? (int64) (step == 2 ? pins : usage) : -1;
			frame->ok = true;
			return;
		case 4:
			if (!host_cache_make_local(HOST_IO_IO_IO_DOORS(io), STD_MUL(usize, (size_t) frame->value, 1024, 1483, 35),
									   &failed))
				break;
			*frame->result = 0;
			frame->ok = true;
			return;
		case 6:
		case 7:
			if (step == 7)
				pg_calls_no_owner(true);
			found = host_cache_get(io, step == 6 ? HOST_CACHE_PLACEMENT_BACKEND : HOST_CACHE_PLACEMENT_SERVER, &key, &has,
								   &frame->pinned, &failed);
			if (step == 7)
				pg_calls_no_owner(false);
			if (!found)
				break;
			frame->pinned_live = has;
			*frame->result = has ? doltgres_entries_testing_cache_first(&frame->pinned, step == 6 ? 1486 : 1491,
																		step == 6 ? 105 : 69) : -1;
			frame->ok = true;
			return;
		case 11:
			{
				bool		refused = false;

				(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_KEPT_LIST, io,
													 DOLTGRES_ENTRIES_TESTING_LIST_RESERVE, NULL);
				pg_calls_transaction_owner(true);
				for (int gets = 0; gets < 64; gets++)
				{
					pg_fail_allocations(true);
					found = host_cache_get(io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &frame->pinned, &failed);
					pg_fail_allocations(false);
					if (!found)
					{
						refused = true;
						break;
					}
					if (!has)
						break;
					frame->pinned_live = true;
					doltgres_entries_testing_cache_keep(frame);
				}
				pg_calls_transaction_owner(false);
				if (refused)
					break;
				*frame->result = 0;
				frame->ok = true;
				return;
			}
		case 12:
			host_cache_unpin_number(HOST_CACHE_PLACEMENT_SERVER, (uint32) frame->number);
			*frame->result = 0;
			frame->ok = true;
			return;
		case 13:
			if (!host_cache_contains(io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &failed))
				break;
			*frame->result = has ? 1 : 0;
			frame->ok = true;
			return;
		default:
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("the node cache's tests have no step "), STD_FMT_ARG(0)};
				StdFmtArg	args[] = {{&step, std_i32_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*frame->error = pg_PgError_fork((const uint8 *) "XX000", &message);
				return;
			}
	}
	*frame->error = pghost_raised(&failed);
}

/* doltgres_entries_testing_cache_pad_0 drops the pin and the read a step holds. */
static void
doltgres_entries_testing_cache_pad_0(void *frame_)
{
	doltgres_entries_testing_cache_frame_0_t *frame = frame_;

	if (frame->pinned_live)
		host_cache_Pinned_drop(&frame->pinned);
	if (frame->read_live)
		host_cache_Read_drop(&frame->read);
	frame->pinned_live = frame->read_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_cache runs a step of the node
 * cache's tests on a key numbered number, with nodes of 100 bytes of value;
 * testing.rs lists each step. A kept pin is taken under the current
 * transaction's resource owner, as a cursor's is under its portal's, and
 * lasts until that owner releases it.
 */
bool
doltgres_entries_testing_doltgres_test_cache(const PgCaps *caps, int32 step, int64 number, int64 value,
											 int64 *result, PgPgError *error)
{
	doltgres_entries_testing_cache_frame_0_t frame = {.caps = caps, .step = step, .number = number, .value = value,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_cache_body_0, doltgres_entries_testing_cache_pad_0);
	doltgres_entries_testing_cache_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_sized returns the size of a test's page from a word: its bytes in the lower half and its words in the upper. */
static HostDeltaPageSize
doltgres_entries_testing_sized(uint64 word)
{
	HostDeltaPageSize size = {(uint32) word, (uint32) (word >> 32)};

	return size;
}

/* DOLTGRES_ENTRIES_TESTING_NO_PAGE is NO_PAGE: the word that ends a test's chain of pages. */
#define DOLTGRES_ENTRIES_TESTING_NO_PAGE PG_UINT64_MAX

/* doltgres_entries_testing_fork returns the fork's internal error with a message. */
static HostError
doltgres_entries_testing_fork(const char *what)
{
	StdString	message;

	std_string_from_Checked(&message, what, strlen(what));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/*
 * doltgres_entries_testing_tested opens the Tested record at a location,
 * whose lock owns a test's pages and whose atomic names its newest page.
 */
static bool
doltgres_entries_testing_tested(HostIoIo *io, uint64 location, HostSharedHeld *result, HostError *error)
{
	bool		has;

	if (!host_shared_open(HOST_SHARED_IO_WITHIN(io), &DOLTGRES_ENTRIES_TESTED_POOL_OPS, location, &has, result,
						  error))
		return false;
	if (has)
		return true;
	*error = doltgres_entries_testing_fork("the test has no record");
	return false;
}

/* doltgres_entries_testing_paged returns the name a record's atomic holds. */
static HostDeltaPagePage
doltgres_entries_testing_paged(const HostSharedHeld *held)
{
	return host_delta_page_Page_named(host_shared_Record_atomic(host_shared_Held_record(held),
																DOLTGRES_ENTRIES_TESTING_TESTED_COUNT,
																DOLTGRES_ENTRIES_TESTING_LOAD));
}

/* doltgres_entries_testing_word_of returns eight bytes of a page at an offset as a number. */
static bool
doltgres_entries_testing_word_of(const HostDeltaPageRef *page, uint64 offset, int64 *result, HostError *error)
{
	HostDeltaLent lent;
	StdBytes	bytes;

	if (!host_delta_PageRef_bytes(page, offset, 8, &lent, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&lent);
	*result = 0;
	if (bytes.len == 8)
		(void) doltgres_entries_testing_test_value(bytes.ptr, bytes.len, result);
	host_delta_Lent_drop(&lent);
	return true;
}

/*
 * doltgres_entries_testing_linked_frame_0_t is linked's state across its
 * pad: the pages it links, how many it took, and the guard it holds, each
 * while live.
 */
typedef struct doltgres_entries_testing_linked_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	StdVec		pages;
	size_t		taken;
	HostDeltaOwned page;
	bool		page_live;
	HostSharedGuard guard;
	bool		guard_live;
	uint64		before;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_linked_frame_0_t;

/* doltgres_entries_testing_linked_body_0 links each page in turn as the record's newest. */
static void
doltgres_entries_testing_linked_body_0(void *frame_)
{
	doltgres_entries_testing_linked_frame_0_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);

	while (frame->taken < frame->pages.len)
	{
		HostSharedNamed named;
		HostDeltaNames names;

		frame->page = ((HostDeltaOwned *) frame->pages.ptr)[frame->taken++];
		frame->page_live = true;
		if (!host_delta_Owned_store(&frame->page, 0, frame->before, frame->error) ||
			!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(frame->io), DOLTGRES_ENTRIES_TESTING_TESTED_LOCK,
									 &frame->guard, frame->error))
			return;
		frame->guard_live = true;
		named = host_shared_Record_names(record, DOLTGRES_ENTRIES_TESTING_TESTED_COUNT);
		names.ops = &HOST_DELTA_NAMED_NAMES_OPS;
		names.self = &named;
		frame->page_live = false;
		frame->before = host_delta_page_Page_value(host_delta_Owned_link(&frame->page,
																		 &DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape,
																		 DOLTGRES_ENTRIES_TESTING_TESTED_LOCK,
																		 &frame->guard, names));
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
	frame->ok = true;
}

/* doltgres_entries_testing_linked_pad_0 releases the guard and drops the page and the pages not yet taken. */
static void
doltgres_entries_testing_linked_pad_0(void *frame_)
{
	doltgres_entries_testing_linked_frame_0_t *frame = frame_;

	if (frame->guard_live)
		host_shared_Guard_drop(&frame->guard);
	if (frame->page_live)
		host_delta_Owned_drop(&frame->page);
	while (frame->taken < frame->pages.len)
		host_delta_Owned_drop(&((HostDeltaOwned *) frame->pages.ptr)[frame->taken++]);
	std_vec_free_Checked(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	frame->guard_live = frame->page_live = false;
}

/*
 * doltgres_entries_testing_linked links pages under a record's lock, each as
 * the record's newest page with the page before it named in its first word,
 * and returns the name of the last; it takes the pages.
 */
static bool
doltgres_entries_testing_linked(HostIoIo *io, const HostSharedHeld *held, StdVec *pages, uint64 before,
								uint64 *result, HostError *error)
{
	doltgres_entries_testing_linked_frame_0_t frame = {.io = io, .held = held, .pages = *pages, .before = before,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_linked_body_0, doltgres_entries_testing_linked_pad_0);
	doltgres_entries_testing_linked_pad_0(&frame);
	*result = frame.before;
	return frame.ok;
}

/*
 * doltgres_entries_testing_chained allocates pages of a size, one a turn,
 * and links each as linked does, until a number of them are linked or the
 * delta refuses one, and returns how many it linked.
 */
static bool
doltgres_entries_testing_chained(HostIoIo *io, const HostSharedHeld *held, HostDeltaPageSize size, uint64 most,
								 int64 *result, HostError *error)
{
	uint64		count = 0;
	uint64		before = DOLTGRES_ENTRIES_TESTING_NO_PAGE;

	while (count < most)
	{
		bool		ok;
		StdVec		pages;
		HostDeltaFull full;

		if (!host_delta_alloc(io, &size, 1, &ok, &pages, &full, error))
			return false;
		if (!ok)
			break;
		if (!doltgres_entries_testing_linked(io, held, &pages, before, &before, error))
			return false;
		count++;
	}
	*result = (int64) count;
	return true;
}

/*
 * doltgres_entries_testing_chain_frame_0_t is chain's state across its pad:
 * the hold it reads under, while live, and the names it lists.
 */
typedef struct doltgres_entries_testing_chain_frame_0_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_chain_frame_0_t;

/* doltgres_entries_testing_chain_body_0 follows the record's chain from its newest page. */
static void
doltgres_entries_testing_chain_body_0(void *frame_)
{
	doltgres_entries_testing_chain_frame_0_t *frame = frame_;
	uint64		next;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	std_vec_new(frame->result, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	frame->result_live = true;
	next = host_delta_page_Page_value(doltgres_entries_testing_paged(frame->held));
	while (next != DOLTGRES_ENTRIES_TESTING_NO_PAGE)
	{
		HostDeltaPagePage name = host_delta_page_Page_named(next);
		HostDeltaPageRef page;
		HostDeltaWord word;

		std_vec_push_Checked(frame->result, &name, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		if (!host_delta_Hold_page(&frame->hold, name, &page, frame->error) ||
			!host_delta_PageRef_word(&page, 0, &word, frame->error))
			return;
		next = host_delta_Word_load(&word);
	}
	frame->result_live = false;
	frame->ok = true;
}

/* doltgres_entries_testing_chain_pad_0 drops the names of a failed or unwound chain, then the hold. */
static void
doltgres_entries_testing_chain_pad_0(void *frame_)
{
	doltgres_entries_testing_chain_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->result_live = frame->hold_live = false;
}

/* doltgres_entries_testing_chain returns the names of the pages a record's chain links, newest first. */
static bool
doltgres_entries_testing_chain(HostIoIo *io, const HostSharedHeld *held, StdVec *result, HostError *error)
{
	doltgres_entries_testing_chain_frame_0_t frame = {.io = io, .held = held, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_chain_body_0, doltgres_entries_testing_chain_pad_0);
	doltgres_entries_testing_chain_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_linked_by links a page under a record's lock of a
 * shape, named by the atomic of the Tested record that names a step's page,
 * and returns its name; it takes the page.
 */
static bool
doltgres_entries_testing_linked_by(HostIoIo *io, HostSharedRecord owner, const HostSharedShapeOps *shape, size_t lock,
								   HostDeltaOwned *page, const HostSharedHeld *held, int64 *result, HostError *error)
{
	HostSharedGuard guard;
	HostSharedNamed named;
	HostDeltaNames names;

	if (!host_shared_Record_lock(owner, HOST_IO_IO_TAKES_SHORT(io), lock, &guard, error))
	{
		host_delta_Owned_drop(page);
		return false;
	}
	named = host_shared_Record_names(host_shared_Held_record(held), DOLTGRES_ENTRIES_TESTING_TESTED_COUNT);
	names.ops = &HOST_DELTA_NAMED_NAMES_OPS;
	names.self = &named;
	*result = (int64) host_delta_page_Page_value(host_delta_Owned_link(page, shape, lock, &guard, names));
	host_shared_Guard_drop(&guard);
	return true;
}

/*
 * doltgres_entries_testing_under appends eight bytes to a page under a
 * record's lock of a shape and returns their offset, or seals the page and
 * returns zero.
 */
static bool
doltgres_entries_testing_under(HostIoIo *io, HostSharedRecord owner, const HostSharedShapeOps *shape, size_t lock,
							   const HostDeltaPageRef *page, bool seal, int64 *result, HostError *error)
{
	static const uint8 sevens[8] = {7, 7, 7, 7, 7, 7, 7, 7};
	HostSharedGuard guard;
	uint32		offset = 0;
	bool		ok;

	if (!host_shared_Record_lock(owner, HOST_IO_IO_TAKES_SHORT(io), lock, &guard, error))
		return false;
	ok = seal ? host_delta_PageRef_seal(page, shape, lock, &guard, error) :
		host_delta_PageRef_append(page, shape, lock, &guard, sevens, sizeof(sevens), &offset, error);
	host_shared_Guard_drop(&guard);
	*result = (int64) offset;
	return ok;
}

/* DOLTGRES_ENTRIES_TESTING_SMALL is SMALL: the size of a test record's page, 64 bytes and one word. */
static const HostDeltaPageSize DOLTGRES_ENTRIES_TESTING_SMALL = {64, 1};

/* doltgres_entries_testing_spare_word returns the word of the second single record's run at an index, which names a test's record. */
static bool
doltgres_entries_testing_spare_word(uint64 index, HostSharedRunWord *result, HostError *error)
{
	HostSharedRecord spare;

	return host_shared_single(&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, &spare, error) &&
		host_shared_Record_word(spare, &DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, (size_t) index, result, error);
}

/* doltgres_entries_testing_made_of makes a record of an aged shape, refusing one the delta has no room for. */
static bool
doltgres_entries_testing_made_of(HostIoIo *io, const HostSharedAgedOps *shape, HostDeltaMade *result,
								 HostError *error)
{
	bool		ok;
	HostDeltaFull full;

	if (!host_delta_make(io, shape, &ok, result, &full, error))
		return false;
	if (ok)
		return true;
	*error = doltgres_entries_testing_fork("the delta refused the test's record");
	return false;
}

/*
 * doltgres_entries_testing_inner_frame_0_t is inner's state across its pad:
 * the pages it allocated and how many it took, the page it fills, the record
 * it made, and the pages it links with it, each while live.
 */
typedef struct doltgres_entries_testing_inner_frame_0_t
{
	HostIoIo   *io;
	HostDeltaNames word;
	uint64		value;
	StdVec		pages;
	bool		pages_live;
	size_t		taken;
	HostDeltaOwned page;
	bool		page_live;
	HostDeltaMade made;
	bool		made_live;
	HostDeltaWith with;
	bool		with_live;
	bool	   *has_result;
	uint64	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_inner_frame_0_t;

/* doltgres_entries_testing_inner_body_0 fills a page with the value and links it and an Inner record that names it. */
static void
doltgres_entries_testing_inner_body_0(void *frame_)
{
	doltgres_entries_testing_inner_frame_0_t *frame = frame_;
	bool		ok;
	HostDeltaFull full;
	uint8		bytes[8];

	if (!host_delta_alloc(frame->io, &DOLTGRES_ENTRIES_TESTING_SMALL, 1, &ok, &frame->pages, &full, frame->error))
		return;
	if (!ok)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	frame->pages_live = true;
	if (!host_delta_make(frame->io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &ok, &frame->made, &full, frame->error))
		return;
	if (!ok)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	frame->made_live = true;
	frame->with = host_delta_With_Default_default();
	frame->with_live = true;
	doltgres_entries_testing_le_bytes(frame->value, bytes);
	while (frame->taken < frame->pages.len)
	{
		HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
		uint32		offset;

		frame->page = ((HostDeltaOwned *) frame->pages.ptr)[frame->taken++];
		frame->page_live = true;
		if (!host_delta_Owned_append(&frame->page, bytes, sizeof(bytes), &offset, frame->error))
			return;
		exchange.u.exchange.f1 = host_delta_page_Page_value(host_delta_Owned_name(&frame->page));
		(void) host_delta_Made_atomic(&frame->made, DOLTGRES_ENTRIES_TESTING_INNER_PAGE, exchange);
		frame->page_live = false;
		if (!host_delta_With_page(&frame->with, DOLTGRES_ENTRIES_TESTING_INNER_LOCK, &frame->page, frame->error))
			return;
	}
	frame->with_live = frame->made_live = false;
	*frame->result = host_delta_Made_link(&frame->made, &DOLTGRES_ENTRIES_INNER_AGED_OPS, frame->word, &frame->with);
	*frame->has_result = true;
	frame->ok = true;
}

/* doltgres_entries_testing_inner_pad_0 drops what inner holds: the page, the pages it links, the record, and the rest of the pages. */
static void
doltgres_entries_testing_inner_pad_0(void *frame_)
{
	doltgres_entries_testing_inner_frame_0_t *frame = frame_;

	if (frame->page_live)
		host_delta_Owned_drop(&frame->page);
	if (frame->with_live)
		host_delta_With_drop(&frame->with);
	if (frame->made_live)
		host_delta_Made_drop(&frame->made);
	if (frame->pages_live)
	{
		while (frame->taken < frame->pages.len)
			host_delta_Owned_drop(&((HostDeltaOwned *) frame->pages.ptr)[frame->taken++]);
		std_vec_free_Checked(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	}
	frame->page_live = frame->with_live = frame->made_live = frame->pages_live = false;
}

/*
 * doltgres_entries_testing_inner allocates a page holding a value, makes an
 * Inner record whose atomic names it, and links both into a word, answering
 * nothing when the delta refused the page or the record.
 */
static bool
doltgres_entries_testing_inner(HostIoIo *io, HostDeltaNames word, uint64 value, bool *has_result, uint64 *result,
							   HostError *error)
{
	doltgres_entries_testing_inner_frame_0_t frame = {.io = io, .word = word, .value = value,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_inner_body_0, doltgres_entries_testing_inner_pad_0);
	doltgres_entries_testing_inner_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_inner_page returns the name of the page the Inner record at a location names, under a hold. */
static bool
doltgres_entries_testing_inner_page(HostIoIo *io, uint64 location, HostDeltaPagePage *result, HostError *error)
{
	HostDeltaHold hold;
	HostSharedRecord record;
	bool		ok;

	if (!host_delta_hold(io, &hold, error))
		return false;
	ok = host_delta_Hold_record(&hold, &DOLTGRES_ENTRIES_INNER_AGED_OPS, location, &record, error);
	if (ok)
		*result = host_delta_page_Page_named(host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_INNER_PAGE,
																	   DOLTGRES_ENTRIES_TESTING_LOAD));
	host_delta_Hold_drop(&hold);
	return ok;
}

/*
 * DoltgresEntriesTestingRetired is the iterator a test hands retire:
 * names.into_iter().map(Ok), chained, when fails is set, with one item of the
 * test's failure. It owns the names, a Vec<Page>.
 */
typedef struct DoltgresEntriesTestingRetired
{
	StdVec		names;
	size_t		at;
	bool		fails;
} DoltgresEntriesTestingRetired;

/* doltgres_entries_testing_Retired_Iterator_next gives the next name, then the failure, then nothing. */
static bool
doltgres_entries_testing_Retired_Iterator_next(void *self_, void *result_)
{
	DoltgresEntriesTestingRetired *self = self_;
	HostDeltaRetireItem *result = result_;

	if (self->at < self->names.len)
	{
		result->is_ok = true;
		result->ok = ((const HostDeltaPagePage *) self->names.ptr)[self->at++];
		return true;
	}
	if (!self->fails)
		return false;
	self->fails = false;
	result->is_ok = false;
	result->err = doltgres_entries_testing_fork("the test's iterator failed");
	return true;
}

/* doltgres_entries_testing_Retired_Iterator_size_hint is the chain's exact hint: the names left, and the failure while it is. */
static StdSizeHint
doltgres_entries_testing_Retired_Iterator_size_hint(const void *self_)
{
	const DoltgresEntriesTestingRetired *self = self_;
	size_t		left = self->names.len - self->at + (self->fails ? 1 : 0);
	StdSizeHint hint = {left, true, left};

	return hint;
}

/* doltgres_entries_testing_Retired_drop is the iterator's drop: its names'. */
static void
doltgres_entries_testing_Retired_drop(void *self_)
{
	DoltgresEntriesTestingRetired *self = self_;

	std_vec_free_Checked(&self->names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
}

/* DOLTGRES_ENTRIES_TESTING_RETIRED_ITERATOR_OPS is the iterator's Iterator table. */
static const StdIteratorOps DOLTGRES_ENTRIES_TESTING_RETIRED_ITERATOR_OPS = {
	doltgres_entries_testing_Retired_Iterator_next,
	doltgres_entries_testing_Retired_Iterator_size_hint,
	doltgres_entries_testing_Retired_drop,
};

/*
 * doltgres_entries_testing_retired retires records and the pages a list
 * names, with the test's failure after the names when fails is set; it
 * takes the names.
 */
static bool
doltgres_entries_testing_retired(HostIoIo *io, const HostDeltaLeaving *records, size_t records_len, StdVec *names,
								 bool fails, HostError *error)
{
	DoltgresEntriesTestingRetired retired = {.names = *names, .fails = fails};
	StdIterator pages = {&DOLTGRES_ENTRIES_TESTING_RETIRED_ITERATOR_OPS, &retired};

	return host_delta_retire(io, records, records_len, pages, error);
}

/* doltgres_entries_testing_pages_drop drops a Vec<Owned>'s pages from an index on, then the vector. */
static void
doltgres_entries_testing_pages_drop(StdVec *pages, size_t from)
{
	while (from < pages->len)
		host_delta_Owned_drop(&((HostDeltaOwned *) pages->ptr)[from++]);
	std_vec_free_Checked(pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
}

/*
 * doltgres_entries_testing_delta_frame_0_t is delta_step's state across its
 * pad: its arguments, and every record, hold, vector, page, made record,
 * link, and guard a step holds, each while live.
 */
typedef struct doltgres_entries_testing_delta_frame_0_t
{
	HostIoIo   *io;
	int32		step;
	uint64		a;
	uint64		b;
	HostSharedHeld held;
	bool		held_live;
	HostSharedHeld owner;
	bool		owner_live;
	HostDeltaHold hold;
	bool		hold_live;
	HostDeltaLent lent;
	bool		lent_live;
	/* names is a Vec<Page> a step retires. */
	StdVec		names;
	bool		names_live;
	/* before and after are a list's entries read around a pause. */
	StdVec		before;
	bool		before_live;
	StdVec		after;
	bool		after_live;
	/* records is the Vec<Leaving> a step retires. */
	StdVec		records;
	bool		records_live;
	/* sizes is the Vec<Size> a step allocates at once. */
	StdVec		sizes;
	bool		sizes_live;
	/* pages is a Vec<Owned>, of which those from taken on are live. */
	StdVec		pages;
	bool		pages_live;
	size_t		taken;
	/* more is a second Vec<Owned>, all live. */
	StdVec		more;
	bool		more_live;
	HostDeltaOwned page;
	bool		page_live;
	HostDeltaMade made;
	bool		made_live;
	HostDeltaMade inner;
	bool		inner_live;
	HostDeltaWith with;
	bool		with_live;
	HostSharedGuard guard;
	bool		guard_live;
	int64	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_delta_frame_0_t;

/* doltgres_entries_testing_delta_done gives a step's answer. */
static void
doltgres_entries_testing_delta_done(doltgres_entries_testing_delta_frame_0_t *frame, int64 answer)
{
	*frame->result = answer;
	frame->ok = true;
}

/* doltgres_entries_testing_delta_failed fails a step with the fork's internal error. */
static void
doltgres_entries_testing_delta_failed(doltgres_entries_testing_delta_frame_0_t *frame, const char *what)
{
	*frame->error = doltgres_entries_testing_fork(what);
}

/* doltgres_entries_testing_repeated sets the frame's sizes to a size a number of times, as vec! does. */
static void
doltgres_entries_testing_repeated(doltgres_entries_testing_delta_frame_0_t *frame, HostDeltaPageSize size, size_t n)
{
	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	std_vec_resize(&frame->sizes, n, &size, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
}

/* doltgres_entries_testing_sizes_free frees the frame's sizes. */
static void
doltgres_entries_testing_sizes_free(doltgres_entries_testing_delta_frame_0_t *frame)
{
	frame->sizes_live = false;
	std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
}

/*
 * doltgres_entries_testing_delta_page allocates one page of a size as the
 * frame's page, failing when the delta refused it.
 */
static bool
doltgres_entries_testing_delta_page(doltgres_entries_testing_delta_frame_0_t *frame, HostDeltaPageSize size)
{
	bool		ok;
	HostDeltaFull full;

	if (!host_delta_alloc(frame->io, &size, 1, &ok, &frame->more, &full, frame->error))
		return false;
	if (ok)
	{
		frame->more_live = true;
		frame->page_live = std_vec_pop(&frame->more, &frame->page, sizeof(HostDeltaOwned));
		doltgres_entries_testing_pages_drop(&frame->more, 0);
		frame->more_live = false;
	}
	if (frame->page_live)
		return true;
	doltgres_entries_testing_delta_failed(frame, "the delta refused the test's page");
	return false;
}

/* doltgres_entries_testing_tested_guard locks the frame's held record's lock as the frame's guard. */
static bool
doltgres_entries_testing_tested_guard(doltgres_entries_testing_delta_frame_0_t *frame)
{
	if (!host_shared_Record_lock(host_shared_Held_record(&frame->held), HOST_IO_IO_TAKES_SHORT(frame->io),
								 DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	return true;
}

/*
 * doltgres_entries_testing_tested_link links the frame's page under its
 * guard as its held record's newest page, then releases the guard, and
 * returns the page's name.
 */
static HostDeltaPagePage
doltgres_entries_testing_tested_link(doltgres_entries_testing_delta_frame_0_t *frame)
{
	HostSharedNamed named = host_shared_Record_names(host_shared_Held_record(&frame->held),
													 DOLTGRES_ENTRIES_TESTING_TESTED_COUNT);
	HostDeltaNames names = {&HOST_DELTA_NAMED_NAMES_OPS, &named};
	HostDeltaPagePage linked;

	frame->page_live = false;
	linked = host_delta_Owned_link(&frame->page, &DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape,
								   DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, names);
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	return linked;
}

/* doltgres_entries_testing_held_page opens the frame's held record's page under the frame's hold, which it begins. */
static bool
doltgres_entries_testing_held_page(doltgres_entries_testing_delta_frame_0_t *frame, HostDeltaPageRef *result)
{
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	return host_delta_Hold_page(&frame->hold, doltgres_entries_testing_paged(&frame->held), result, frame->error);
}

/* doltgres_entries_testing_tested_at opens the Tested record at the frame's location a as its held record. */
static bool
doltgres_entries_testing_tested_at(doltgres_entries_testing_delta_frame_0_t *frame)
{
	if (!doltgres_entries_testing_tested(frame->io, frame->a, &frame->held, frame->error))
		return false;
	frame->held_live = true;
	return true;
}

/* doltgres_entries_testing_retire_names retires records and the frame's names, with the test's failure when fails is set. */
static bool
doltgres_entries_testing_retire_names(doltgres_entries_testing_delta_frame_0_t *frame, const HostDeltaLeaving *records,
									  size_t records_len, bool fails)
{
	frame->names_live = false;
	return doltgres_entries_testing_retired(frame->io, records, records_len, &frame->names, fails, frame->error);
}

/* doltgres_entries_testing_no_names sets the frame's names to an empty Vec<Page>. */
static void
doltgres_entries_testing_no_names(doltgres_entries_testing_delta_frame_0_t *frame)
{
	std_vec_new(&frame->names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	frame->names_live = true;
}

/* doltgres_entries_testing_aged_slots returns the first slot and the count of an aged kind's pool in the server's table. */
static bool
doltgres_entries_testing_aged_slots(doltgres_entries_testing_delta_frame_0_t *frame, uint64 id,
									std_TupleU32U32_t *result)
{
	size_t		kind;

	if (shmem_records_kind(id, &kind) && shmem_records_aged(kind, result))
		return true;
	doltgres_entries_testing_delta_failed(frame, "the test's aged kind isn't in the server's table");
	return false;
}

/* doltgres_entries_testing_outer_at opens the Outer record at the frame's location a under the frame's hold, which it begins. */
static bool
doltgres_entries_testing_outer_at(doltgres_entries_testing_delta_frame_0_t *frame, HostSharedRecord *result)
{
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return false;
	frame->hold_live = true;
	return host_delta_Hold_record(&frame->hold, &DOLTGRES_ENTRIES_OUTER_AGED_OPS, frame->a, result, frame->error);
}

/*
 * doltgres_entries_testing_entries reads every entry of an Outer record's
 * list under its shared lock, as a Vec<u64> it sets live with a flag.
 */
static bool
doltgres_entries_testing_entries(doltgres_entries_testing_delta_frame_0_t *frame, HostSharedRecord record,
								 StdVec *result, bool *live)
{
	HostDeltaListListView list;
	uint64		len;

	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(frame->io), DOLTGRES_ENTRIES_TESTING_OUTER_LOCK,
										&frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	list = host_delta_list_Guard_list(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape, 0);
	len = host_delta_list_ListView_len(&list);
	std_vec_new(result, sizeof(uint64), _Alignof(uint64));
	*live = true;
	for (uint64 index = 0; index < len; index++)
	{
		uint64		entry;

		if (!host_delta_list_ListView_get(&list, &frame->hold, index, &entry, frame->error))
			return false;
		std_vec_push_Checked(result, &entry, sizeof(uint64), _Alignof(uint64));
	}
	frame->guard_live = false;
	host_shared_Guard_drop(&frame->guard);
	return true;
}

/* doltgres_entries_testing_delta_first runs delta_step's steps 0 to 29. */
static void
doltgres_entries_testing_delta_first(doltgres_entries_testing_delta_frame_0_t *frame)
{
	HostIoIo   *io = frame->io;
	uint64		a = frame->a;
	uint64		b = frame->b;
	int32		step = frame->step;
	HostDeltaPageRef page;
	HostDeltaWord word;
	HostDeltaFull full;
	bool		ok;
	int64		answer;
	uint8		bytes[8];
	uint32		offset;

	switch (step)
	{
		case 0:
			doltgres_entries_testing_delta_done(frame, (int64) host_delta_used());
			return;
		case 1:
			if (!doltgres_entries_testing_tested_at(frame) ||
				!doltgres_entries_testing_delta_page(frame, doltgres_entries_testing_sized(b)) ||
				!doltgres_entries_testing_tested_guard(frame))
				return;
			doltgres_entries_testing_delta_done(frame,
												(int64) host_delta_page_Page_value(doltgres_entries_testing_tested_link(frame)));
			return;
		case 2:
		case 12:
			if (!doltgres_entries_testing_tested_at(frame) ||
				!doltgres_entries_testing_tested(io, step == 2 ? a : b, &frame->owner, frame->error))
				return;
			frame->owner_live = true;
			if (!doltgres_entries_testing_held_page(frame, &page) ||
				!host_shared_Record_lock(host_shared_Held_record(&frame->owner), HOST_IO_IO_TAKES_SHORT(io),
										 DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, frame->error))
				return;
			frame->guard_live = true;
			doltgres_entries_testing_le_bytes(b, bytes);
			if (!host_delta_PageRef_append(&page, &DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape,
										   DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, bytes, sizeof(bytes),
										   &offset, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, (int64) offset);
			return;
		case 3:
		case 5:
			if (!doltgres_entries_testing_tested_at(frame) || !host_delta_hold(io, &frame->hold, frame->error))
				return;
			frame->hold_live = true;
			if (step == 5 && !host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-held", frame->error))
				return;
			if (!host_delta_Hold_page(&frame->hold, doltgres_entries_testing_paged(&frame->held), &page, frame->error) ||
				!doltgres_entries_testing_word_of(&page, b, &answer, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, answer);
			return;
		case 4:
			{
				HostDeltaPagePage name;

				if (!doltgres_entries_testing_tested_at(frame))
					return;
				doltgres_entries_testing_no_names(frame);
				name = doltgres_entries_testing_paged(&frame->held);
				std_vec_push_Checked(&frame->names, &name, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
				if (!doltgres_entries_testing_retire_names(frame, NULL, 0, false))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 6:
		case 7:
		case 8:
		case 9:
			if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_held_page(frame, &page) ||
				!host_delta_PageRef_word(&page, step == 7 ? b : 0, &word, frame->error))
				return;
			switch (step)
			{
				case 6:
					host_delta_Word_store(&word, b);
					answer = 0;
					break;
				case 7:
					answer = (int64) host_delta_Word_load(&word);
					break;
				case 8:
					answer = (int64) host_delta_Word_fetch_add(&word, b);
					break;
				default:
					answer = (int64) host_delta_Word_compare_exchange(&word, b & UINT64CONST(0xFFFFFFFF), b >> 32);
					break;
			}
			doltgres_entries_testing_delta_done(frame, answer);
			return;
		case 10:
			doltgres_entries_testing_repeated(frame, doltgres_entries_testing_sized(a), (size_t) b);
			if (!host_delta_alloc(io, frame->sizes.ptr, frame->sizes.len, &ok, &frame->more, &full, frame->error))
				return;
			if (ok)
				doltgres_entries_testing_pages_drop(&frame->more, 0);
			doltgres_entries_testing_delta_done(frame, ok ? 1 : 0);
			return;
		case 11:
			{
				StdBytes	lent;
				int64		number[2] = {0};

				if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_held_page(frame, &page) ||
					!host_delta_PageRef_bytes(&page, b, 16, &frame->lent, frame->error))
					return;
				frame->lent_live = true;
				lent = host_delta_Lent_Deref_deref(&frame->lent);
				for (int at = 0; at < 2; at++)
				{
					std_range_check(at * 8, at * 8 + 8, lent.len, RUST_FILE, 1721, 57);
					(void) doltgres_entries_testing_test_value(lent.ptr + at * 8, 8, &number[at]);
				}
				doltgres_entries_testing_delta_done(frame, STD_ADD(i64, STD_MUL(i64, number[0], 1000, 1722, 16),
																   number[1], 1722, 16));
				return;
			}
		case 13:
			if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_held_page(frame, &page) ||
				!doltgres_entries_testing_tested_guard(frame) ||
				!host_delta_PageRef_seal(&page, &DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape,
										 DOLTGRES_ENTRIES_TESTING_TESTED_LOCK, &frame->guard, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 14:
			{
				HostDeltaPageSize size = {64, 2};
				HostDeltaPagePage named;
				HostDeltaPagePage linked;

				if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_delta_page(frame, size))
					return;
				doltgres_entries_testing_le_bytes(b, bytes);
				if (!host_delta_Owned_append(&frame->page, bytes, sizeof(bytes), &offset, frame->error) ||
					!host_delta_Owned_store(&frame->page, 1, STD_ADD(u64, b, 1, 1739, 27), frame->error))
					return;
				named = host_delta_Owned_name(&frame->page);
				if (!doltgres_entries_testing_tested_guard(frame))
					return;
				linked = doltgres_entries_testing_tested_link(frame);
				doltgres_entries_testing_delta_done(frame, linked.f0 == named.f0 && offset == 0 ?
													(int64) host_delta_page_Page_value(linked) : -1);
				return;
			}
		case 15:
			{
				HostDeltaPageSize size = doltgres_entries_testing_sized(a);

				if (!host_delta_alloc(io, &size, 1, &ok, &frame->pages, &full, frame->error))
					return;
				frame->pages_live = ok;
				if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-owned", frame->error))
					return;
				doltgres_entries_testing_delta_failed(frame, "the test failed after its allocation");
				return;
			}
		case 16:
			{
				HostDeltaPageSize size = {(uint32) a, 0};

				if (!doltgres_entries_testing_delta_page(frame, size))
					return;
				doltgres_entries_testing_le_bytes(b, bytes);
				if (!host_delta_Owned_append(&frame->page, bytes, sizeof(bytes), &offset, frame->error) ||
					!host_delta_Owned_append(&frame->page, bytes, sizeof(bytes), &offset, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 17:
			{
				int64		frees = 0;

				for (uint32 slot = 0; slot < shmem_delta_slots(); slot++)
					if (host_delta_page_state(shmem_delta_word(slot)) == HOST_DELTA_PAGE_STATE_FREE)
						frees++;
				doltgres_entries_testing_delta_done(frame, frees);
				return;
			}
		case 18:
			if (a == 0)
			{
				if (!host_delta_hold(io, &frame->hold, frame->error))
					return;
				frame->hold_live = true;
			}
			else
			{
				HostDeltaPageSize size = doltgres_entries_testing_sized(4096);

				if (!host_delta_alloc(io, &size, 1, &ok, &frame->pages, &full, frame->error))
					return;
				frame->pages_live = ok;
			}
			doltgres_entries_testing_repeated(frame, doltgres_entries_testing_sized(8192),
											  STD_ADD(usize, (size_t) shmem_delta_slots(), 1, 1767, 42));
			if (!host_delta_alloc(io, frame->sizes.ptr, frame->sizes.len, &ok, &frame->more, &full, frame->error))
				return;
			if (ok)
			{
				frame->more_live = true;
				doltgres_entries_testing_delta_failed(frame, "the delta gave more pages than it has slots");
				return;
			}
			if (!host_delta_wait(io, &full, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 20:
		case 29:
			{
				HostDeltaPageSize size = {4096 - 64, 8};

				if (!doltgres_entries_testing_tested_at(frame) ||
					!doltgres_entries_testing_chained(io, &frame->held,
													  step == 20 ? doltgres_entries_testing_sized(b) : size,
													  step == 20 ? PG_UINT64_MAX : b, &answer, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, answer);
				return;
			}
		case 21:
			if (!doltgres_entries_testing_tested_at(frame))
				return;
			if (!doltgres_entries_testing_chain(io, &frame->held, &frame->names, frame->error))
				return;
			frame->names_live = true;
			answer = (int64) frame->names.len;
			if (!doltgres_entries_testing_retire_names(frame, NULL, 0, false))
				return;
			doltgres_entries_testing_delta_done(frame, answer);
			return;
		case 22:
			for (uint32 slot = 0; slot < shmem_delta_slots(); slot++)
			{
				uint64		loaded = shmem_delta_word(slot);

				if (host_delta_page_state(loaded) == HOST_DELTA_PAGE_STATE_FREE)
					(void) shmem_delta_swap(slot, loaded, PG_UINT64_MAX >> 28 << 28);
			}
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 23:
			doltgres_entries_testing_no_names(frame);
			if (!doltgres_entries_testing_retire_names(frame, NULL, 0, false))
				return;
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 25:
			{
				HostDeltaPageSize sizes[2] = {doltgres_entries_testing_sized(b), doltgres_entries_testing_sized(b)};
				int64		waits = 0;
				uint64		last;

				if (!doltgres_entries_testing_tested_at(frame))
					return;
				for (;;)
				{
					if (!host_delta_alloc(io, sizes, lengthof(sizes), &ok, &frame->pages, &full, frame->error))
						return;
					if (ok)
						break;
					waits = STD_ADD(i64, waits, 1, 1804, 25);
					if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-refused", frame->error) ||
						!host_delta_wait(io, &full, frame->error))
						return;
				}
				if (!doltgres_entries_testing_linked(io, &frame->held, &frame->pages, DOLTGRES_ENTRIES_TESTING_NO_PAGE,
													 &last, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, waits);
				return;
			}
		case 26:
			{
				static const HostDeltaPageSize mixed[] = {{1 << 16}, {1 << 15}, {1 << 14}, {1 << 13}, {1 << 12},
				{1 << 12}, {1 << 13}, {1 << 14}, {1 << 15}, {1 << 16}};
				HostDeltaPageSize largest = doltgres_entries_testing_sized(HOST_DELTA_PAGE_LARGEST);

				if (!host_delta_alloc(io, mixed, lengthof(mixed), &ok, &frame->more, &full, frame->error))
					return;
				if (!ok)
				{
					doltgres_entries_testing_delta_failed(frame, "the delta refused the mixed sizes");
					return;
				}
				doltgres_entries_testing_pages_drop(&frame->more, 0);
				if (!host_delta_alloc(io, &largest, 1, &ok, &frame->more, &full, frame->error))
					return;
				if (ok)
					doltgres_entries_testing_pages_drop(&frame->more, 0);
				doltgres_entries_testing_delta_done(frame, ok ? 1 : 0);
				return;
			}
		case 27:
			if (!host_delta_hold(io, &frame->hold, frame->error))
				return;
			frame->hold_live = true;
			if (!host_delta_Hold_page(&frame->hold, host_delta_page_Page_named(b), &page, frame->error) ||
				!doltgres_entries_testing_word_of(&page, 0, &answer, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, answer);
			return;
		default:
			doltgres_entries_testing_delta_failed(frame, "the delta's tests have no such step");
			return;
	}
}

/* doltgres_entries_testing_delta_second runs delta_step's steps 30 to 44. */
static void
doltgres_entries_testing_delta_second(doltgres_entries_testing_delta_frame_0_t *frame)
{
	HostIoIo   *io = frame->io;
	uint64		a = frame->a;
	uint64		b = frame->b;
	int32		step = frame->step;
	HostDeltaPageRef page;
	HostSharedRecord record;
	HostSharedRunWord word;
	HostDeltaNames names;
	HostDeltaFull full;
	bool		ok;
	int64		answer;
	uint8		bytes[8];
	uint32		offset;

	switch (step)
	{
		case 30:
			if (!doltgres_entries_testing_tested_at(frame))
				return;
			if (!doltgres_entries_testing_chain(io, &frame->held, &frame->names, frame->error))
				return;
			frame->names_live = true;
			if (!doltgres_entries_testing_retire_names(frame, NULL, 0, true))
				return;
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 31:
			{
				uint32		ordinary = STD_SUB(u32, shmem_delta_slots(),
											   (uint32) (HOST_DELTA_PAGE_BLOCK / HOST_DELTA_PAGE_SLOT_BYTES), 1834, 28);
				uint64		spent = host_delta_page_released(PG_UINT64_MAX >> 31 << 31);

				for (uint32 slot = 0; slot < shmem_delta_slots(); slot++)
				{
					for (;;)
					{
						uint64		loaded = shmem_delta_word(slot);

						if (loaded == spent)
							break;
						if (shmem_delta_swap(slot, loaded, spent))
						{
							if (slot < ordinary)
								shmem_delta_hint_clear(slot);
							shmem_delta_spend(slot >= ordinary);
							break;
						}
					}
				}
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 32:
			{
				HostDeltaPageSize size = {64, 1};
				const HostSharedSingleOps *single = b == 0 ? &DOLTGRES_ENTRIES_TABLE_SINGLE_OPS :
					&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS;

				if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_delta_page(frame, size) ||
					!host_shared_single(single, &record, frame->error))
					return;
				frame->page_live = false;
				if (!doltgres_entries_testing_linked_by(io, record, &single->shape,
														b == 0 ? DOLTGRES_ENTRIES_TESTING_TABLE_LOCK :
														DOLTGRES_ENTRIES_TESTING_SPARE_LOCK, &frame->page,
														&frame->held, &answer, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, answer);
				return;
			}
		case 33:
		case 34:
			{
				const HostSharedShapeOps *shape = &DOLTGRES_ENTRIES_TESTED_POOL_OPS.shape;
				size_t		lock = DOLTGRES_ENTRIES_TESTING_TESTED_LOCK;

				if (!doltgres_entries_testing_tested_at(frame) || !doltgres_entries_testing_held_page(frame, &page))
					return;
				if (b == 0 || b == 1)
				{
					const HostSharedSingleOps *single = b == 0 ? &DOLTGRES_ENTRIES_TABLE_SINGLE_OPS :
						&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS;

					if (!host_shared_single(single, &record, frame->error))
						return;
					shape = &single->shape;
					lock = b == 0 ? DOLTGRES_ENTRIES_TESTING_TABLE_LOCK : DOLTGRES_ENTRIES_TESTING_SPARE_LOCK;
				}
				else
					record = host_shared_Held_record(&frame->held);
				if (!doltgres_entries_testing_under(io, record, shape, lock, &page, step == 34, &answer, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, answer);
				return;
			}
		case 35:
			{
				HostDeltaPageSize size = {64, 1};

				if (!doltgres_entries_testing_delta_page(frame, size))
					return;
				doltgres_entries_testing_le_bytes(b, bytes);
				if (!host_delta_Owned_append(&frame->page, bytes, sizeof(bytes), &offset, frame->error) ||
					!host_shared_single(&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, &record, frame->error) ||
					!host_shared_Record_word(record, &DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, (size_t) a, &word,
											 frame->error) ||
					!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_SPARE_LOCK,
											 &frame->guard, frame->error))
					return;
				frame->guard_live = true;
				names.ops = &HOST_DELTA_RUN_WORD_NAMES_OPS;
				names.self = &word;
				frame->page_live = false;
				answer = (int64) host_delta_page_Page_value(host_delta_Owned_link(&frame->page,
																				  &DOLTGRES_ENTRIES_SPARE_SINGLE_OPS.shape,
																				  DOLTGRES_ENTRIES_TESTING_SPARE_LOCK,
																				  &frame->guard, names));
				doltgres_entries_testing_delta_done(frame, answer);
				return;
			}
		case 36:
		case 37:
			if (!host_shared_single(&DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, &record, frame->error) ||
				!host_shared_Record_word(record, &DOLTGRES_ENTRIES_SPARE_SINGLE_OPS, (size_t) a, &word, frame->error))
				return;
			if (step == 36)
				host_shared_RunWord_store(&word, b);
			doltgres_entries_testing_delta_done(frame, step == 36 ? 0 : (int64) host_shared_RunWord_load(&word));
			return;
		case 38:
			{
				bool		has;
				uint64		location;

				if (!doltgres_entries_testing_spare_word(a, &word, frame->error))
					return;
				names.ops = &HOST_DELTA_RUN_WORD_NAMES_OPS;
				names.self = &word;
				if (!doltgres_entries_testing_inner(io, names, b, &has, &location, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, has ? (int64) location : -1);
				return;
			}
		case 39:
			if (!host_delta_hold(io, &frame->hold, frame->error))
				return;
			frame->hold_live = true;
			if ((b == 1 && !host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-held", frame->error)) ||
				!host_delta_Hold_record(&frame->hold, &DOLTGRES_ENTRIES_INNER_AGED_OPS, a, &record, frame->error) ||
				!host_delta_Hold_page(&frame->hold,
									  host_delta_page_Page_named(host_shared_Record_atomic(record,
																						   DOLTGRES_ENTRIES_TESTING_INNER_PAGE,
																						   DOLTGRES_ENTRIES_TESTING_LOAD)),
									  &page, frame->error) ||
				!doltgres_entries_testing_word_of(&page, 0, &answer, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, answer);
			return;
		case 40:
		case 48:
		case 49:
			{
				HostDeltaPagePage first;
				HostDeltaLeaving leaving;

				if (!doltgres_entries_testing_inner_page(io, a, &first, frame->error))
					return;
				doltgres_entries_testing_no_names(frame);
				std_vec_push_Checked(&frame->names, &first, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
				if (step == 48)
				{
					HostDeltaWord named;

					doltgres_entries_testing_repeated(frame, DOLTGRES_ENTRIES_TESTING_SMALL, (size_t) b);
					if (!host_delta_alloc(io, frame->sizes.ptr, frame->sizes.len, &ok, &frame->pages, &full,
										  frame->error))
						return;
					if (!ok)
					{
						doltgres_entries_testing_delta_failed(frame, "the delta refused the test's pages");
						return;
					}
					frame->pages_live = true;
					if (!host_delta_hold(io, &frame->hold, frame->error))
						return;
					frame->hold_live = true;
					if (!host_delta_Hold_record(&frame->hold, &DOLTGRES_ENTRIES_INNER_AGED_OPS, a, &record,
												frame->error) ||
						!host_delta_Hold_page(&frame->hold, first, &page, frame->error) ||
						!host_delta_PageRef_word(&page, 0, &named, frame->error))
						return;
					names.ops = &HOST_DELTA_WORD_NAMES_OPS;
					names.self = &named;
					while (frame->taken < frame->pages.len)
					{
						HostDeltaPagePage linked;

						frame->page = ((HostDeltaOwned *) frame->pages.ptr)[frame->taken++];
						frame->page_live = true;
						if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io),
													 DOLTGRES_ENTRIES_TESTING_INNER_LOCK, &frame->guard, frame->error))
							return;
						frame->guard_live = true;
						frame->page_live = false;
						linked = host_delta_Owned_link(&frame->page, &DOLTGRES_ENTRIES_INNER_AGED_OPS.shape,
													   DOLTGRES_ENTRIES_TESTING_INNER_LOCK, &frame->guard, names);
						std_vec_push_Checked(&frame->names, &linked, sizeof(HostDeltaPagePage),
											 _Alignof(HostDeltaPagePage));
						frame->guard_live = false;
						host_shared_Guard_drop(&frame->guard);
					}
					frame->pages_live = false;
					doltgres_entries_testing_pages_drop(&frame->pages, frame->taken);
					frame->hold_live = false;
					host_delta_Hold_drop(&frame->hold);
					doltgres_entries_testing_sizes_free(frame);
				}
				leaving = host_delta_Leaving_of(&DOLTGRES_ENTRIES_INNER_AGED_OPS, a);
				if (!doltgres_entries_testing_retire_names(frame, &leaving, 1, step == 49))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 41:
			{
				uint64		end = STD_ADD(u64, a, b, 1913, 31);

				std_vec_new(&frame->records, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving));
				frame->records_live = true;
				for (uint64 index = a; index < end; index++)
				{
					HostDeltaLeaving leaving;

					if (!doltgres_entries_testing_spare_word(index, &word, frame->error))
						return;
					leaving = host_delta_Leaving_of(&DOLTGRES_ENTRIES_INNER_AGED_OPS, host_shared_RunWord_load(&word));
					std_vec_push_Checked(&frame->records, &leaving, sizeof(HostDeltaLeaving),
										 _Alignof(HostDeltaLeaving));
				}
				doltgres_entries_testing_no_names(frame);
				if (!doltgres_entries_testing_retire_names(frame, frame->records.ptr, frame->records.len, false))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 42:
			{
				HostDeltaPageSize sizes[2] = {DOLTGRES_ENTRIES_TESTING_SMALL, DOLTGRES_ENTRIES_TESTING_SMALL};
				HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
				StdVecIntoIter pages;
				StdIterator iterator = {&STD_VEC_INTO_ITER_ITERATOR_OPS, &pages};

				if (!host_delta_alloc(io, sizes, lengthof(sizes), &ok, &frame->pages, &full, frame->error))
					return;
				if (!ok)
				{
					doltgres_entries_testing_delta_failed(frame, "the delta refused the test's pages");
					return;
				}
				frame->pages_live = true;
				if (!doltgres_entries_testing_made_of(io, &DOLTGRES_ENTRIES_OUTER_AGED_OPS, &frame->made, frame->error))
					return;
				frame->made_live = true;
				if (!doltgres_entries_testing_made_of(io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &frame->inner, frame->error))
					return;
				frame->inner_live = true;
				if (!std_vec_pop(&frame->pages, &frame->page, sizeof(HostDeltaOwned)))
				{
					doltgres_entries_testing_delta_failed(frame, "the delta gave fewer pages than asked");
					return;
				}
				frame->page_live = true;
				std_index_check(0, frame->pages.len, RUST_FILE, 1924, 13);
				doltgres_entries_testing_le_bytes(a, bytes);
				if (!host_delta_Owned_append(&((HostDeltaOwned *) frame->pages.ptr)[0], bytes, sizeof(bytes), &offset,
											 frame->error))
					return;
				std_index_check(0, frame->pages.len, RUST_FILE, 1925, 63);
				exchange.u.exchange.f1 = host_delta_page_Page_value(host_delta_Owned_name(&((HostDeltaOwned *) frame->pages.ptr)[0]));
				(void) host_delta_Made_atomic(&frame->inner, DOLTGRES_ENTRIES_TESTING_INNER_PAGE, exchange);
				exchange.u.exchange.f1 = host_delta_Made_location(&frame->inner);
				(void) host_delta_Made_atomic(&frame->made, DOLTGRES_ENTRIES_TESTING_OUTER_NAMES, exchange);
				frame->with = host_delta_With_Default_default();
				frame->with_live = true;
				frame->page_live = false;
				if (!host_delta_With_page(&frame->with, DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, &frame->page, frame->error))
					return;
				frame->inner_live = frame->pages_live = false;
				pages = std_Vec_IntoIterator_into_iter(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned),
													   host_delta_Owned_drop);
				if (!host_delta_With_record(&frame->with, &DOLTGRES_ENTRIES_INNER_AGED_OPS,
											DOLTGRES_ENTRIES_TESTING_INNER_LOCK, &frame->inner,
											iterator, frame->error))
					return;
				if (b == 0)
				{
					if (!doltgres_entries_testing_spare_word(a, &word, frame->error))
						return;
					names.ops = &HOST_DELTA_RUN_WORD_NAMES_OPS;
					names.self = &word;
					frame->made_live = frame->with_live = false;
					doltgres_entries_testing_delta_done(frame,
														(int64) host_delta_Made_link(&frame->made,
																					 &DOLTGRES_ENTRIES_OUTER_AGED_OPS,
																					 names, &frame->with));
					return;
				}
				if (b != 1 && !host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-made", frame->error))
					return;
				doltgres_entries_testing_delta_failed(frame, "the test failed before its link");
				return;
			}
		case 43:
			{
				HostSharedRecord inner;

				if (!doltgres_entries_testing_outer_at(frame, &record) ||
					!host_delta_Hold_record(&frame->hold, &DOLTGRES_ENTRIES_INNER_AGED_OPS,
											host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_OUTER_NAMES,
																	  DOLTGRES_ENTRIES_TESTING_LOAD),
											&inner, frame->error) ||
					!host_delta_Hold_page(&frame->hold,
										  host_delta_page_Page_named(host_shared_Record_atomic(inner,
																							   DOLTGRES_ENTRIES_TESTING_INNER_PAGE,
																							   DOLTGRES_ENTRIES_TESTING_LOAD)),
										  &page, frame->error))
					return;
				switch (b)
				{
					case 0:
						ok = doltgres_entries_testing_under(io, inner, &DOLTGRES_ENTRIES_INNER_AGED_OPS.shape,
															DOLTGRES_ENTRIES_TESTING_INNER_LOCK, &page, false, &answer,
															frame->error);
						break;
					case 1:
					case 2:
						ok = doltgres_entries_testing_under(io, record, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape,
															DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, &page, b == 2,
															&answer, frame->error);
						break;
					default:
						ok = true;
						answer = a == host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_OUTER_NAMES,
																DOLTGRES_ENTRIES_TESTING_LOAD) ? 1 : 0;
						break;
				}
				if (ok)
					doltgres_entries_testing_delta_done(frame, answer);
				return;
			}
		case 44:
			{
				std_TupleU32U32_t pool;
				uint32		end;
				int64		frees = 0;

				if (!doltgres_entries_testing_aged_slots(frame, a == 0 ? DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape.id :
														 DOLTGRES_ENTRIES_INNER_AGED_OPS.shape.id, &pool))
					return;
				end = STD_ADD(u32, pool.f0, pool.f1, 1958, 25);
				for (uint32 slot = pool.f0; slot < end; slot++)
					if (host_delta_page_state(shmem_records_word(slot)) == HOST_DELTA_PAGE_STATE_FREE)
						frees++;
				doltgres_entries_testing_delta_done(frame, frees);
				return;
			}
		default:
			doltgres_entries_testing_delta_first(frame);
			return;
	}
}

/* doltgres_entries_testing_delta_body_0 runs delta_step's steps 45 to 61, and the rest through its other parts. */
static void
doltgres_entries_testing_delta_body_0(void *frame_)
{
	doltgres_entries_testing_delta_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	uint64		a = frame->a;
	uint64		b = frame->b;
	int32		step = frame->step;
	HostSharedRecord record;
	HostSharedRunWord word;
	HostDeltaNames names;
	HostDeltaFull full;
	HostDeltaWith with;
	bool		ok;
	uint64		value;

	switch (step)
	{
		case 45:
			{
				uint64		count = 0;

				for (;;)
				{
					if (!host_delta_make(io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &ok, &frame->made, &full, frame->error))
						return;
					if (!ok)
						break;
					frame->made_live = true;
					if (!doltgres_entries_testing_spare_word(STD_ADD(u64, a, count, 1964, 39), &word, frame->error))
						return;
					names.ops = &HOST_DELTA_RUN_WORD_NAMES_OPS;
					names.self = &word;
					with = host_delta_With_Default_default();
					frame->made_live = false;
					(void) host_delta_Made_link(&frame->made, &DOLTGRES_ENTRIES_INNER_AGED_OPS, names, &with);
					count = STD_ADD(u64, count, 1, 1965, 17);
				}
				doltgres_entries_testing_delta_done(frame, (int64) count);
				return;
			}
		case 46:
			{
				int64		waits = 0;

				for (;;)
				{
					if (!host_delta_make(io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &ok, &frame->made, &full, frame->error))
						return;
					if (ok)
						break;
					waits = STD_ADD(i64, waits, 1, 1975, 25);
					if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-refused", frame->error) ||
						!host_delta_wait(io, &full, frame->error))
						return;
				}
				frame->made_live = true;
				if (!doltgres_entries_testing_spare_word(a, &word, frame->error))
					return;
				names.ops = &HOST_DELTA_RUN_WORD_NAMES_OPS;
				names.self = &word;
				with = host_delta_With_Default_default();
				frame->made_live = false;
				(void) host_delta_Made_link(&frame->made, &DOLTGRES_ENTRIES_INNER_AGED_OPS, names, &with);
				doltgres_entries_testing_delta_done(frame, waits);
				return;
			}
		case 47:
			{
				std_TupleU32U32_t pool;
				uint32		end;
				uint64		last = b == 0 ? PG_UINT64_MAX >> 28 << 28 : host_delta_page_released(PG_UINT64_MAX >> 28 << 28);

				if (!doltgres_entries_testing_aged_slots(frame, DOLTGRES_ENTRIES_INNER_AGED_OPS.shape.id, &pool))
					return;
				end = STD_ADD(u32, pool.f0, pool.f1, 1990, 32);
				for (uint32 slot = pool.f0; slot < end; slot++)
				{
					uint64		loaded = shmem_records_word(slot);

					if (host_delta_page_state(loaded) == HOST_DELTA_PAGE_STATE_FREE)
						(void) shmem_records_swap(slot, loaded, last);
				}
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 50:
			doltgres_entries_testing_repeated(frame, DOLTGRES_ENTRIES_TESTING_SMALL,
											  STD_ADD(usize, (size_t) shmem_delta_slots(), 1, 1999, 59));
			if (!host_delta_alloc(io, frame->sizes.ptr, frame->sizes.len, &ok, &frame->more, &full, frame->error))
				return;
			if (ok)
			{
				frame->more_live = true;
				doltgres_entries_testing_delta_failed(frame, "the delta gave more pages than it has slots");
				return;
			}
			doltgres_entries_testing_sizes_free(frame);
			if (!doltgres_entries_testing_made_of(io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &frame->made, frame->error))
				return;
			frame->made_live = true;
			if (!host_delta_wait(io, &full, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 51:
			if (!doltgres_entries_testing_made_of(io, &DOLTGRES_ENTRIES_INNER_AGED_OPS, &frame->made, frame->error))
				return;
			frame->made_live = true;
			(void) host_delta_Made_atomic(&frame->made, DOLTGRES_ENTRIES_TESTING_INNER_PAGE,
										  doltgres_entries_testing_add(77));
			doltgres_entries_testing_delta_done(frame, 0);
			return;
		case 52:
			{
				uint64		end = STD_ADD(u64, a, b, 2014, 29);
				uint64		sum = 0;

				if (!host_delta_hold(io, &frame->hold, frame->error))
					return;
				frame->hold_live = true;
				for (uint64 index = a; index < end; index++)
				{
					if (!doltgres_entries_testing_spare_word(index, &word, frame->error) ||
						!host_delta_Hold_record(&frame->hold, &DOLTGRES_ENTRIES_INNER_AGED_OPS,
												host_shared_RunWord_load(&word), &record, frame->error))
						return;
					sum = STD_ADD(u64, sum, host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_INNER_PAGE,
																	  DOLTGRES_ENTRIES_TESTING_LOAD), 2015, 17);
				}
				doltgres_entries_testing_delta_done(frame, (int64) sum);
				return;
			}
		case 53:
			for (;;)
			{
				HostDeltaListListView list;
				HostDeltaListListMut entries;
				uint64		length;
				size_t		wants;

				if (!doltgres_entries_testing_outer_at(frame, &record) ||
					!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io),
													DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, &frame->guard, frame->error))
					return;
				frame->guard_live = true;
				list = host_delta_list_Guard_list(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape, 0);
				length = host_delta_list_ListView_len(&list);
				wants = host_delta_list_ListView_wants(&list);
				frame->guard_live = false;
				host_shared_Guard_drop(&frame->guard);
				if (length >= b)
				{
					doltgres_entries_testing_delta_done(frame, (int64) length);
					return;
				}
				doltgres_entries_testing_repeated(frame, HOST_DELTA_LIST_PAGE, wants);
				if (!host_delta_alloc(io, frame->sizes.ptr, frame->sizes.len, &ok, &frame->more, &full, frame->error))
					return;
				if (!ok)
				{
					doltgres_entries_testing_delta_failed(frame, "the delta refused the list's pages");
					return;
				}
				frame->more_live = true;
				doltgres_entries_testing_sizes_free(frame);
				if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_OUTER_LOCK,
											 &frame->guard, frame->error))
					return;
				frame->guard_live = true;
				entries = host_delta_list_GuardExclusive_list_mut(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape,
																  DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, 0);
				if (!host_delta_list_ListMut_push(&entries, &frame->hold, STD_ADD(u64, 1000, length, 2033, 47),
												  &frame->more, frame->error))
					return;
				frame->guard_live = false;
				host_shared_Guard_drop(&frame->guard);
				frame->more_live = false;
				doltgres_entries_testing_pages_drop(&frame->more, 0);
				frame->hold_live = false;
				host_delta_Hold_drop(&frame->hold);
			}
		case 54:
			{
				HostDeltaListListView list;

				if (!doltgres_entries_testing_outer_at(frame, &record) ||
					!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io),
													DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, &frame->guard, frame->error))
					return;
				frame->guard_live = true;
				list = host_delta_list_Guard_list(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape, 0);
				if (!host_delta_list_ListView_get(&list, &frame->hold, b, &value, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, (int64) value);
				return;
			}
		case 55:
			{
				size_t		before;

				if (!doltgres_entries_testing_outer_at(frame, &record) ||
					!doltgres_entries_testing_entries(frame, record, &frame->before, &frame->before_live) ||
					!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-test-delta-held", frame->error) ||
					!doltgres_entries_testing_entries(frame, record, &frame->after, &frame->after_live))
					return;
				before = frame->before.len;
				doltgres_entries_testing_delta_done(frame, frame->after.len > before &&
													(before == 0 ||
													 memcmp(frame->after.ptr, frame->before.ptr,
															before * sizeof(uint64)) == 0) ? (int64) before : -1);
				return;
			}
		case 56:
			{
				HostDeltaListListMut entries;

				if (!doltgres_entries_testing_outer_at(frame, &record) ||
					!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), DOLTGRES_ENTRIES_TESTING_OUTER_LOCK,
											 &frame->guard, frame->error))
					return;
				frame->guard_live = true;
				entries = host_delta_list_GuardExclusive_list_mut(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape,
																  DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, 0);
				(void) host_delta_list_ListMut_close(&entries);
				entries = host_delta_list_GuardExclusive_list_mut(&frame->guard, &DOLTGRES_ENTRIES_OUTER_AGED_OPS.shape,
																  DOLTGRES_ENTRIES_TESTING_OUTER_LOCK, 0);
				if (!host_delta_list_ListMut_set(&entries, &frame->hold, 0, 1, frame->error))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 57:
			{
				HostDeltaLeaving leaving;

				if (!doltgres_entries_testing_outer_at(frame, &record))
					return;
				value = host_shared_Record_atomic(record, DOLTGRES_ENTRIES_TESTING_OUTER_NAMES,
												  DOLTGRES_ENTRIES_TESTING_LOAD);
				frame->hold_live = false;
				host_delta_Hold_drop(&frame->hold);
				leaving = host_delta_Leaving_of(&DOLTGRES_ENTRIES_INNER_AGED_OPS, value);
				doltgres_entries_testing_no_names(frame);
				if (!doltgres_entries_testing_retire_names(frame, &leaving, 1, false))
					return;
				doltgres_entries_testing_delta_done(frame, 0);
				return;
			}
		case 58:
			{
				uint8		seed[32];
				int64		first;

				host_delta_seed(seed);
				(void) doltgres_entries_testing_test_value(seed, 8, &first);
				doltgres_entries_testing_delta_done(frame, first);
				return;
			}
		case 59:
			{
				HostDeltaPageSize size = doltgres_entries_testing_sized(a);
				HostDeltaPageSize small = {size.bytes / 8, size.words};
				HostDeltaPagePage replaced;
				HostDeltaOwned replacing;
				bool		whole;

				std_vec_new(&frame->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
				frame->pages_live = true;
				while (frame->pages.len < (size_t) b)
				{
					if (!host_delta_alloc(io, &size, 1, &ok, &frame->more, &full, frame->error))
						return;
					if (!ok)
						break;
					frame->more_live = true;
					frame->page_live = std_vec_pop(&frame->more, &frame->page, sizeof(HostDeltaOwned));
					frame->more_live = false;
					doltgres_entries_testing_pages_drop(&frame->more, 0);
					if (frame->page_live)
					{
						std_vec_push_Checked(&frame->pages, &frame->page, sizeof(HostDeltaOwned),
											 _Alignof(HostDeltaOwned));
						frame->page_live = false;
					}
				}
				if (frame->pages.len == 0)
				{
					doltgres_entries_testing_delta_failed(frame, "the delta gave no page");
					return;
				}
				replaced = host_delta_Owned_name(&((HostDeltaOwned *) frame->pages.ptr)[0]);
				if (!host_delta_alloc_replacing(io, size, replaced, &whole, &replacing, &full, frame->error))
					return;
				if (whole)
					host_delta_Owned_drop(&replacing);
				if (!host_delta_alloc_replacing(io, small, replaced, &ok, &replacing, &full, frame->error))
					return;
				if (ok)
					host_delta_Owned_drop(&replacing);
				doltgres_entries_testing_delta_done(frame, (whole ? 10 : 0) + (ok ? 1 : 0));
				return;
			}
		case 60:
			{
				HostDeltaPagePage head;

				if (!doltgres_entries_testing_tested_at(frame))
					return;
				head = doltgres_entries_testing_paged(&frame->held);
				if (!host_delta_alloc_replacing(io, doltgres_entries_testing_sized(b), head, &ok, &frame->page, &full,
												frame->error))
					return;
				if (!ok)
				{
					doltgres_entries_testing_delta_failed(frame, "the delta refused the test's replacement");
					return;
				}
				frame->page_live = true;
				if (!host_delta_Owned_store(&frame->page, 0, host_delta_page_Page_value(head), frame->error) ||
					!doltgres_entries_testing_tested_guard(frame))
					return;
				doltgres_entries_testing_delta_done(frame,
													(int64) host_delta_page_Page_value(doltgres_entries_testing_tested_link(frame)));
				return;
			}
		case 61:
			if (!txn_spill_relieved(io, &ok, frame->error))
				return;
			doltgres_entries_testing_delta_done(frame, ok ? 1 : 0);
			return;
		default:
			doltgres_entries_testing_delta_second(frame);
			return;
	}
}

/* doltgres_entries_testing_delta_pad_0 drops what a step holds, in the reverse of the order a step takes it. */
static void
doltgres_entries_testing_delta_pad_0(void *frame_)
{
	doltgres_entries_testing_delta_frame_0_t *frame = frame_;

	if (frame->guard_live)
		host_shared_Guard_drop(&frame->guard);
	if (frame->with_live)
		host_delta_With_drop(&frame->with);
	if (frame->page_live)
		host_delta_Owned_drop(&frame->page);
	if (frame->inner_live)
		host_delta_Made_drop(&frame->inner);
	if (frame->made_live)
		host_delta_Made_drop(&frame->made);
	if (frame->more_live)
		doltgres_entries_testing_pages_drop(&frame->more, 0);
	if (frame->pages_live)
		doltgres_entries_testing_pages_drop(&frame->pages, frame->taken);
	if (frame->sizes_live)
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	if (frame->records_live)
		std_vec_free_Checked(&frame->records, sizeof(HostDeltaLeaving), _Alignof(HostDeltaLeaving));
	if (frame->after_live)
		std_vec_free_Checked(&frame->after, sizeof(uint64), _Alignof(uint64));
	if (frame->before_live)
		std_vec_free_Checked(&frame->before, sizeof(uint64), _Alignof(uint64));
	if (frame->names_live)
		std_vec_free_Checked(&frame->names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	if (frame->lent_live)
		host_delta_Lent_drop(&frame->lent);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->owner_live)
		host_shared_Held_drop(&frame->owner);
	if (frame->held_live)
		host_shared_Held_drop(&frame->held);
	frame->guard_live = frame->with_live = frame->page_live = frame->inner_live = frame->made_live = false;
	frame->more_live = frame->pages_live = frame->sizes_live = frame->records_live = false;
	frame->after_live = frame->before_live = frame->names_live = frame->lent_live = false;
	frame->hold_live = frame->owner_live = frame->held_live = false;
}

/* doltgres_entries_testing_delta_step runs one step of doltgres_test_delta. */
static bool
doltgres_entries_testing_delta_step(HostIoIo *io, int32 step, uint64 a, uint64 b, int64 *result, HostError *error)
{
	doltgres_entries_testing_delta_frame_0_t frame = {.io = io, .step = step, .a = a, .b = b, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_delta_body_0, doltgres_entries_testing_delta_pad_0);
	doltgres_entries_testing_delta_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_doltgres_test_delta runs one step against the
 * delta's pages, so a test can show each of their doors across backends;
 * testing.rs lists each step.
 */
bool
doltgres_entries_testing_doltgres_test_delta(const PgCaps *caps, int32 step, int64 a, int64 b, int64 *result,
											 PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io;
	HostError	failed;

	if (step == 19)
	{
		uint64		epoch;

		if (!shmem_records_epochs_read(raise, &epoch, error))
			return false;
		shmem_records_epochs_end(epoch);
		*result = (int64) epoch;
		return true;
	}
	io = pghost_io(raise);
	if (doltgres_entries_testing_delta_step(&io, step, (uint64) a, (uint64) b, result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_testing_pg_fork returns the fork's internal error with a message, as an entry raises it. */
static PgPgError
doltgres_entries_testing_pg_fork(const char *what)
{
	StdString	message;

	std_string_from_Checked(&message, what, strlen(what));
	return pg_PgError_fork((const uint8 *) "XX000", &message);
}

/*
 * doltgres_entries_testing_doltgres_test_status is the test module's view of
 * the status door: it returns 0 for an xid in progress, 1 for one committed,
 * and 2 for one aborted.
 */
bool
doltgres_entries_testing_doltgres_test_status(const PgCaps *caps, int64 xid, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostXid		of = {(uint64) xid};
	HostStatus	status;
	HostError	failed;

	if (!host_status(HOST_IO_IO_IO_DOORS(&io), of, &status, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	switch (status)
	{
		case HOST_STATUS_IN_PROGRESS:
			*result = 0;
			break;
		case HOST_STATUS_COMMITTED:
			*result = 1;
			break;
		case HOST_STATUS_ABORTED:
			*result = 2;
			break;
	}
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_in_progress is the test module's
 * view of the in_progress door: it returns 1 for an xid in progress and 0 for
 * one that isn't.
 */
bool
doltgres_entries_testing_doltgres_test_in_progress(const PgCaps *caps, int64 xid, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostXid		of = {(uint64) xid};
	bool		in_progress;
	HostError	failed;

	if (!host_in_progress(HOST_IO_IO_IO_DOORS(&io), of, &in_progress, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = in_progress ? 1 : 0;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_writer is the test module's view of
 * the current_writer door: it returns the current subtransaction's xid for 0
 * and its top-level transaction's for 1.
 */
bool
doltgres_entries_testing_doltgres_test_writer(const PgCaps *caps, int32 which, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostXid		writer;
	HostXid		top;
	HostError	failed;

	if (!host_current_writer(HOST_IO_IO_IO_DOORS(&io), &writer, &top, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = (int64) (which == 0 ? writer : top).f0;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_command is the test module's view
 * of the current_command door: it returns the current command's id, marking
 * it used where used isn't 0.
 */
bool
doltgres_entries_testing_doltgres_test_command(const PgCaps *caps, int32 used, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostCommand command;
	HostError	failed;

	if (!host_current_command(HOST_IO_IO_IO_DOORS(&io), used != 0, &command, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	*result = (int64) command.f0;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_is_current is the test module's view
 * of the is_current door: it returns 1 for an xid of the current transaction
 * and 0 for any other.
 */
bool
doltgres_entries_testing_doltgres_test_is_current(const PgCaps *caps, int64 xid, int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostXid		of = {(uint64) xid};

	(void) error;
	*result = host_is_current(HOST_IO_IO_IO_DOORS(&io), of) ? 1 : 0;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_status_table runs one step against
 * the commit status table, so a test can show its entries across backends;
 * testing.rs lists each step.
 */
bool
doltgres_entries_testing_doltgres_test_status_table(const PgCaps *caps, int32 step, int64 xid, int64 value,
													int64 *result, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostXid		of = {(uint64) xid};
	TxnStatusEntry entry;
	bool		has;
	bool		ok;
	uint64		count;
	HostError	failed;

	switch (step)
	{
		case 0:
			{
				TxnStatusOutcome pending = {.tag = TXN_STATUS_OUTCOME_PENDING_IN};
				TxnStatusRoomed roomed;

				pending.u.pending_in = value >= 0 && value <= PG_UINT32_MAX ? (uint32) value : PG_UINT32_MAX;
				ok = txn_status_take(&io, of, pending, &roomed, &has, &failed);
				if (ok)
					*result = has ? 1 : 0;
				break;
			}
		case 1:
		case 4:
			ok = txn_status_lookup(&io, of, &has, &entry, &failed);
			if (ok && !has)
				*result = -1;
			else if (ok && step == 1)
				*result = (int64) entry.count;
			else if (ok)
				*result = txn_status_Outcome_word(entry.outcome, &count) ? (int64) count : -1;
			break;
		case 2:
			ok = txn_status_remove(&io, of, &has, &failed);
			if (ok)
				*result = has ? 1 : 0;
			break;
		case 3:
			ok = txn_status_add(&io, of, (uint64) value, &has, &count, &failed);
			if (ok)
				*result = has ? (int64) count : -1;
			break;
		default:
			*error = doltgres_entries_testing_pg_fork("the status table's tests have no such step");
			return false;
	}
	if (ok)
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_testing_doltgres_test_kept_wal checks the WAL from one
 * position to another as a checkpoint checks the WAL it keeps for a
 * manifest's fallback, warning of a segment it can't read.
 */
bool
doltgres_entries_testing_doltgres_test_kept_wal(const PgCaps *caps, int64 from, int64 to, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (store_read_kept_wal(&io, (HostLsn) from, (HostLsn) to, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_testing_storage_raised returns a storage's error as an entry raises it, through chunks' raised. */
static PgPgError
doltgres_entries_testing_storage_raised(ChunksErrorError *error)
{
	HostError	raised = chunks_packs_shared_raised(error);

	return pghost_raised(&raised);
}

/* doltgres_entries_testing_doltgres_test_repository_closure_3 is doltgres_test_repository's gathering: the tests' one base, of the storage 1 in pg_default. */
static bool
doltgres_entries_testing_doltgres_test_repository_closure_3(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
															ChunksErrorError *error)
{
	RepoGathered gathered = {.tag = REPO_GATHERED_BASE};

	(void) captures;
	(void) io;
	(void) read;
	(void) error;
	gathered.u.base.storage.tablespace = HOST_PG_DEFAULT;
	gathered.u.base.storage.relation = 1;
	std_slice_to_vec_Checked(&gathered.u.base.record.bytes, (const uint8 *) "base", 4, 1, 1);
	std_vec_new(&gathered.u.base.record.addresses, sizeof(chunks_TupleU32Locator_t),
				_Alignof(chunks_TupleU32Locator_t));
	gathered.u.base.folded = 5;
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	std_vec_push_Checked(result, &gathered, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/* doltgres_entries_testing_doltgres_test_repository_closure_4 is doltgres_test_repository's after-write, which does nothing. */
static bool
doltgres_entries_testing_doltgres_test_repository_closure_4(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/* doltgres_entries_testing_Repo_drop is Repo's drop glue, as an Arc's value takes it. */
static void
doltgres_entries_testing_Repo_drop(void *repo)
{
	repo_Repo_drop(repo);
}

/* doltgres_entries_testing_doltgres_test_session_closure_2 is doltgres_test_session's gathering, doltgres_test_repository's base. */
static bool
doltgres_entries_testing_doltgres_test_session_closure_2(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
														 ChunksErrorError *error)
{
	return doltgres_entries_testing_doltgres_test_repository_closure_3(captures, io, read, result, error);
}

/* doltgres_entries_testing_doltgres_test_session_closure_3 is doltgres_test_session's after-write, which does nothing. */
static bool
doltgres_entries_testing_doltgres_test_session_closure_3(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/*
 * doltgres_entries_testing_repository_frame_0_t is doltgres_test_repository's
 * and doltgres_test_session's state across their pads: the repository, the
 * session's handle on it, the reader a save takes, and the session's
 * manifest, each while live.
 */
typedef struct doltgres_entries_testing_repository_frame_0_t
{
	const PgCaps *caps;
	int32		id;
	bool		save;
	HostIoIo	io;
	RepoRepo	repo;
	bool		repo_live;
	StdArcInner *session;
	bool		session_live;
	HostSharedReader reading;
	bool		reading_live;
	StdArcInner *manifest;
	bool		manifest_live;
	int64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_repository_frame_0_t;

/* doltgres_entries_testing_saved_once takes a reader and saves a repository once with the tests' base. */
static bool
doltgres_entries_testing_saved_once(doltgres_entries_testing_repository_frame_0_t *frame, const RepoRepo *repo)
{
	HostError	failed;
	ChunksErrorError storage;

	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->reading_live = true;
	if (repo_Repo_save(repo, &frame->io, &frame->reading, false, NULL,
					   frame->save ? doltgres_entries_testing_doltgres_test_session_closure_2 : doltgres_entries_testing_doltgres_test_repository_closure_3,
					   NULL, frame->save ? doltgres_entries_testing_doltgres_test_session_closure_3 :
					   doltgres_entries_testing_doltgres_test_repository_closure_4, NULL, &storage))
		return true;
	*frame->error = doltgres_entries_testing_storage_raised(&storage);
	return false;
}

/* doltgres_entries_testing_repository_body_0 makes the frame's repository, saves it, and closes it. */
static void
doltgres_entries_testing_repository_body_0(void *frame_)
{
	doltgres_entries_testing_repository_frame_0_t *frame = frame_;
	ChunksErrorError storage;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (frame->id <= 0)
	{
		static const char positive_message[] = "a database id is positive";
		HostError	positive;
		StdString	message;

		std_string_from_Checked(&message, positive_message, sizeof(positive_message) - 1);
		positive = host_Error_fork((const uint8 *) "22023", &message);
		*frame->error = pghost_raised(&positive);
		return;
	}
	if (!repo_Repo_create(&frame->io, (uint32) frame->id, HOST_PG_DEFAULT, &frame->repo, &storage))
	{
		*frame->error = doltgres_entries_testing_storage_raised(&storage);
		return;
	}
	frame->repo_live = true;
	if (!doltgres_entries_testing_saved_once(frame, &frame->repo))
		return;
	frame->repo_live = false;
	if (!repo_Repo_close(&frame->repo, &frame->io, &frame->reading, &storage))
	{
		*frame->error = doltgres_entries_testing_storage_raised(&storage);
		return;
	}
	frame->ok = true;
}

/* doltgres_entries_testing_repository_pad_0 drops the manifest, the reader, and the repository or the session's handle on it. */
static void
doltgres_entries_testing_repository_pad_0(void *frame_)
{
	doltgres_entries_testing_repository_frame_0_t *frame = frame_;

	if (frame->manifest_live)
		repo_ArcManifest_drop(frame->manifest);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->session_live)
		std_arc_drop(frame->session, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_testing_Repo_drop);
	if (frame->repo_live)
		repo_Repo_drop(&frame->repo);
	frame->manifest_live = frame->reading_live = frame->session_live = frame->repo_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_repository makes the repository of
 * a database id in pg_default, as CREATE DATABASE will, created and saved
 * once with a base, for the cases of pg_checkversioned over a repository the
 * server made.
 */
bool
doltgres_entries_testing_doltgres_test_repository(const PgCaps *caps, int32 id, PgPgError *error)
{
	doltgres_entries_testing_repository_frame_0_t frame = {.caps = caps, .id = id, .error = error};

	std_landing(&frame, doltgres_entries_testing_repository_body_0, doltgres_entries_testing_repository_pad_0);
	doltgres_entries_testing_repository_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_RegistersAtExit_Exit_run_closure_0 is a registration that does nothing. */
static bool
doltgres_entries_testing_RegistersAtExit_Exit_run_closure_0(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
															void *result, ChunksErrorError *error)
{
	(void) captures;
	(void) registering;
	(void) slots;
	(void) result;
	(void) error;
	return true;
}

/*
 * doltgres_entries_testing_RegistersAtExit_Exit_run is the registration
 * test's exit work: it registers, and says in the server's log that it did.
 */
static bool
doltgres_entries_testing_RegistersAtExit_Exit_run(HostIoIo *io, HostError *error)
{
	ChunksErrorError storage;
	uint8		unit;

	if (!chunks_packs_roots_RootSlots_register(io, doltgres_entries_testing_RegistersAtExit_Exit_run_closure_0, NULL, NULL, &unit, &storage))
	{
		*error = chunks_packs_shared_raised(&storage);
		return false;
	}
	return host_log(HOST_IO_IO_IO_DOORS(io), "a registration entered at the backend's exit took its root slots", error);
}

/* DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_EXIT_OPS is RegistersAtExit's Exit table. */
static const HostExitOps DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_EXIT_OPS = {
doltgres_entries_testing_RegistersAtExit_Exit_run};

/* DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_TYPE_ID is RegistersAtExit's TypeId. */
static const StdTypeId DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_TYPE_ID = {
"doltgres::entries::testing::RegistersAtExit", 0, 1, NULL};

/* doltgres_entries_testing_doltgres_test_registered_closure_0 is a registration that pauses at doltgres-test-registered. */
static bool
doltgres_entries_testing_doltgres_test_registered_closure_0(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
															void *result, ChunksErrorError *error)
{
	HostError	failed;

	(void) captures;
	(void) slots;
	(void) result;
	if (host_pause(HOST_SHARED_REGISTERING_PAUSES(registering), "doltgres-test-registered", &failed))
		return true;
	*error = chunks_error_Error_FromError_from(&failed);
	return false;
}

/*
 * doltgres_entries_testing_doltgres_test_registered enters a root
 * registration and pauses inside it, having listed an exit work that
 * registers, for the case of a backend terminated inside a registration,
 * whose exit runs above that frame.
 */
bool
doltgres_entries_testing_doltgres_test_registered(const PgCaps *caps, PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;
	ChunksErrorError storage;
	uint8		unit;

	if (!host_at_exit(&DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_TYPE_ID,
					  &DOLTGRES_ENTRIES_TESTING_REGISTERS_AT_EXIT_EXIT_OPS, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (chunks_packs_roots_RootSlots_register(&io, doltgres_entries_testing_doltgres_test_registered_closure_0, NULL, NULL, &unit, &storage))
		return true;
	*error = doltgres_entries_testing_storage_raised(&storage);
	return false;
}

/* doltgres_entries_testing_session_body_0 opens the session's repository, saves it when asked, and reads its manifest's fold point. */
static void
doltgres_entries_testing_session_body_0(void *frame_)
{
	doltgres_entries_testing_repository_frame_0_t *frame = frame_;
	HostError	failed;
	ChunksErrorError storage;
	const RepoRepo *repo;

	frame->io = pghost_io(pg_Caps_may_raise(frame->caps));
	if (!store_session_session_repo(&frame->io, &frame->session, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->session_live = true;
	repo = STD_ARC_VALUE(frame->session, _Alignof(RepoRepo));
	if (frame->save)
	{
		if (!doltgres_entries_testing_saved_once(frame, repo))
			return;
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (!repo_Repo_manifest(repo, &frame->io, &frame->manifest, &storage))
	{
		*frame->error = doltgres_entries_testing_storage_raised(&storage);
		return;
	}
	frame->manifest_live = true;
	*frame->result = (int64) repo_Repo_newest(frame->manifest)->folded;
	frame->ok = true;
}

/*
 * doltgres_entries_testing_doltgres_test_session opens the backend's session
 * repository, saves it once with a base first when asked, and returns the
 * fold point its newest manifest names, for the cases of the repository
 * CREATE DATABASE made.
 */
bool
doltgres_entries_testing_doltgres_test_session(const PgCaps *caps, bool save, int64 *result, PgPgError *error)
{
	doltgres_entries_testing_repository_frame_0_t frame = {.caps = caps, .save = save, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_session_body_0, doltgres_entries_testing_repository_pad_0);
	doltgres_entries_testing_repository_pad_0(&frame);
	return frame.ok;
}

/*
 * DOLTGRES_ENTRIES_TESTING_WORDS is WORDS: the words and values that tell a
 * wrong cast in a word's reader or writer.
 */
static const uint64 DOLTGRES_ENTRIES_TESTING_WORDS[18] = {
	0,
	1,
	PG_UINT64_MAX,
	0x7f,
	0x80,
	0xff,
	0x7fff,
	0x8000,
	0xffff,
	UINT64CONST(0x7fffffff),
	UINT64CONST(0x80000000),
	UINT64CONST(0xffffffff),
	UINT64CONST(0x7fffffffffffffff),
	UINT64CONST(0x8000000000000000),
	UINT64CONST(0xffffffff00000005),
	UINT64CONST(0x7fc00000),
	UINT64CONST(0xffffffff7fa00001),
	UINT64CONST(0x7ff8000000000001),
};

/*
 * doltgres_entries_testing_doltgres_test_words compares each of pg::word's
 * readers and writers with Postgres' own macro over WORDS, and returns how
 * many of them differ.
 */
bool
doltgres_entries_testing_doltgres_test_words(const PgCaps *caps, int64 *result, PgPgError *error)
{
	int64		differ = 0;

	(void) caps;
	(void) error;
	for (size_t at = 0; at < lengthof(DOLTGRES_ENTRIES_TESTING_WORDS); at++)
	{
		uint64		word = DOLTGRES_ENTRIES_TESTING_WORDS[at];
		float4		narrow = pg_word_to_float4(word);
		float8		wide = pg_word_to_float8(word);
		uint32		narrow_bits;
		uint64		wide_bits;
		uint64		read[8];
		uint64		written[6];

		memcpy(&narrow_bits, &narrow, sizeof(narrow_bits));
		memcpy(&wide_bits, &wide, sizeof(wide_bits));
		read[0] = pg_word_to_bool(word) ? 1 : 0;
		read[1] = pg_word_to_char(word);
		read[2] = (uint64) (int64) pg_word_to_int16(word);
		read[3] = (uint64) (int64) pg_word_to_int32(word);
		read[4] = (uint64) pg_word_to_int64(word);
		read[5] = pg_word_to_oid(word);
		read[6] = narrow_bits;
		read[7] = wide_bits;
		written[0] = pg_word_from_bool(word != 0);
		written[1] = pg_word_from_char((uint8) word);
		written[2] = pg_word_from_int16((int16) word);
		written[3] = pg_word_from_int32((int32) word);
		written[4] = pg_word_from_int64((int64) word);
		written[5] = pg_word_from_oid((uint32) word);
		for (int32 kind = 0; kind < (int32) lengthof(read); kind++)
			differ = STD_ADD(i64, differ, pg_calls_word_read(word, kind) != read[kind] ? 1 : 0, 2364, 13);
		for (int32 kind = 0; kind < (int32) lengthof(written); kind++)
			differ = STD_ADD(i64, differ, pg_calls_word_write(word, kind) != written[kind] ? 1 : 0, 2367, 13);
	}
	*result = differ;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_att returns one part of an
 * attribute of a slot's descriptor as the binding unpacks it: its type, its
 * length, whether it is passed by value, or whether it was dropped; -1 for an
 * attribute out of range.
 */
bool
doltgres_entries_testing_doltgres_test_att(const PgCaps *caps, PgSlot *slot, int32 att, int32 field, int64 *result,
										   PgPgError *error)
{
	PgAttOption of = pg_Att_of(pg_calls_slot_att(slot, att));

	(void) caps;
	(void) error;
	if (!of.has)
		*result = -1;
	else if (field == 0)
		*result = (int64) pg_Att_type_id(of.att);
	else if (field == 1)
		*result = (int64) pg_Att_length(of.att);
	else if (field == 2)
		*result = pg_Att_by_value(of.att) ? 1 : 0;
	else
		*result = pg_Att_dropped(of.att) ? 1 : 0;
	return true;
}

/* DoltgresEntriesTestingTestRecord is TestRecord: the owned records' test kind, a value, whose drop TEST_DROPS counts. */
typedef struct DoltgresEntriesTestingTestRecord
{
	int64		f0;
} DoltgresEntriesTestingTestRecord;

/* DOLTGRES_ENTRIES_TESTING_TEST_DROPS is TEST_DROPS: it counts the backend's test records dropped. */
static StdAtomicU64 DOLTGRES_ENTRIES_TESTING_TEST_DROPS;

/* DOLTGRES_ENTRIES_TESTING_TEST_SLOT is TEST_SLOT: the slot of the backend's last test record's handle. */
static StdAtomicU32 DOLTGRES_ENTRIES_TESTING_TEST_SLOT;

/* DOLTGRES_ENTRIES_TESTING_TEST_GENERATION is TEST_GENERATION: the generation of the backend's last test record's handle. */
static StdAtomicU64 DOLTGRES_ENTRIES_TESTING_TEST_GENERATION;

/* doltgres_entries_testing_TestRecord_drop is TestRecord's Drop: it counts the drop. */
static void
doltgres_entries_testing_TestRecord_drop(void *record)
{
	(void) record;
	(void) std_AtomicU64_fetch_add_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_DROPS, 1);
}

/* DOLTGRES_ENTRIES_TESTING_TEST_RECORDS is TEST_RECORDS, the backend's test records. */
HostOwnedTable DOLTGRES_ENTRIES_TESTING_TEST_RECORDS = HOST_OWNED_TABLE_NEW;

/* doltgres_entries_testing_TestRecord_Owned_table is TestRecord's Owned::table, TEST_RECORDS. */
static HostOwnedTable *
doltgres_entries_testing_TestRecord_Owned_table(void)
{
	return &DOLTGRES_ENTRIES_TESTING_TEST_RECORDS;
}

/* TestRecord's Table, host's owned Table<TestRecord>, with its kind's glue. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_testing_TestRecord
#define HOST_OWNED_RS DoltgresEntriesTestingTestRecord
#define HOST_OWNED_R DoltgresEntriesTestingTestRecord
#define HOST_OWNED_R_DROP doltgres_entries_testing_TestRecord_drop
#define HOST_OWNED_KIND 15
#define HOST_OWNED_TABLE_FN doltgres_entries_testing_TestRecord_Owned_table
#include "doltgres/host/owned_template.h"

/* TestRecord's Table's with at an i64. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_entries_testing_TestRecord
#define HOST_OWNED_RS DoltgresEntriesTestingTestRecord
#define HOST_OWNED_R DoltgresEntriesTestingTestRecord
#define HOST_OWNED_R_DROP doltgres_entries_testing_TestRecord_drop
#define HOST_OWNED_KIND 15
#define HOST_OWNED_TABLE_FN doltgres_entries_testing_TestRecord_Owned_table
#define HOST_OWNED_WITH_TS I64
#define HOST_OWNED_WITH_T int64
#include "doltgres/host/owned_template.h"

/* doltgres_entries_testing_doltgres_test_owned_closure_1 is step 1's lend: the last handle's record's value. */
static bool
doltgres_entries_testing_doltgres_test_owned_closure_1(void *captures, HostIoIo *io, DoltgresEntriesTestingTestRecord *record,
													   int64 *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	*result = record->f0;
	return true;
}

/* doltgres_entries_testing_doltgres_test_owned_closure_3 is step 4's lend: the forged handle's record's value. */
static bool
doltgres_entries_testing_doltgres_test_owned_closure_3(void *captures, HostIoIo *io, DoltgresEntriesTestingTestRecord *record,
													   int64 *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	*result = record->f0;
	return true;
}

/* doltgres_entries_testing_doltgres_test_owned_closure_5 is step 5's lend: the comparison of 1 and 2 by the captured Compared. */
static bool
doltgres_entries_testing_doltgres_test_owned_closure_5(void *captures, HostIoIo *io, DoltgresEntriesTestingTestRecord *record,
													   int64 *result, HostError *error)
{
	int32		one = 1;
	int32		two = 2;
	int32		compared;

	(void) record;
	if (!host_btree_compare(io, captures, (const uint8 *) &one, sizeof(one), (const uint8 *) &two, sizeof(two),
							&compared, error))
		return false;
	*result = compared;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_owned runs one step against the
 * owned records' test kind, so a test can show a record across calls;
 * testing.rs lists each step.
 */
bool
doltgres_entries_testing_doltgres_test_owned(const PgCatalogCaps *caps, int32 step, int64 value, int64 *result,
											 PgPgError *error)
{
	HostIoIo	io = pghost_io_catalog(caps);
	HostOwnedHandle last = {std_AtomicU32_load_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_SLOT),
	std_AtomicU64_load_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_GENERATION)};
	HostError	failed;
	bool		ok;

	switch (step)
	{
		case 0:
			{
				DoltgresEntriesTestingTestRecord record = {value};
				HostOwnedHandle handle;

				ok = host_owned_TableDoltgresEntriesTestingTestRecord_make(&DOLTGRES_ENTRIES_TESTING_TEST_RECORDS, &io,
																		   record, &handle, &failed);
				if (ok)
				{
					std_AtomicU32_store_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_SLOT, handle.slot);
					std_AtomicU64_store_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_GENERATION, handle.generation);
					*result = 0;
				}
				break;
			}
		case 1:
		case 4:
			if (step == 4)
				last.generation = STD_ADD(u64, last.generation, 1, 2608, 60);
			ok = host_owned_TableDoltgresEntriesTestingTestRecord_withI64(&DOLTGRES_ENTRIES_TESTING_TEST_RECORDS, &io,
																		  last,
																		  step == 1 ? doltgres_entries_testing_doltgres_test_owned_closure_1 :
																		  doltgres_entries_testing_doltgres_test_owned_closure_3,
																		  NULL, NULL, result, &failed);
			break;
		case 2:
			ok = host_owned_TableDoltgresEntriesTestingTestRecord_end(&DOLTGRES_ENTRIES_TESTING_TEST_RECORDS, &io, last,
																	  &failed);
			if (ok)
				*result = 0;
			break;
		case 3:
			*result = (int64) std_AtomicU64_load_relaxed(&DOLTGRES_ENTRIES_TESTING_TEST_DROPS);
			return true;
		case 5:
			{
				HostCompared compared = {.len = 4, .by_value = true};

				if (value < 0 || value > PG_UINT32_MAX)
				{
					static const char no_function[] = "no such function";
					StdString	message;

					std_string_from_Checked(&message, no_function, sizeof(no_function) - 1);
					*error = pg_PgError_fork((const uint8 *) "22023", &message);
					return false;
				}
				compared.function = (uint32) value;
				ok = host_owned_TableDoltgresEntriesTestingTestRecord_withI64(&DOLTGRES_ENTRIES_TESTING_TEST_RECORDS,
																			  &io, last,
																			  doltgres_entries_testing_doltgres_test_owned_closure_5,
																			  &compared, NULL, result, &failed);
				break;
			}
		default:
			*error = doltgres_entries_testing_pg_fork("the owned records' tests have no such step");
			return false;
	}
	if (ok)
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_testing_states_frame_0_t is states' state across its pad:
 * its arguments, the repository, its reader, and the states it holds, each
 * while live.
 */
typedef struct doltgres_entries_testing_states_frame_0_t
{
	HostIoIo   *io;
	int64		repository;
	int64		count;
	StdPathBuf	versioned;
	bool		versioned_live;
	RepoRepo	repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	/* holding is a Vec<Held<StorageState>>. */
	StdVec		holding;
	bool		holding_live;
	int64	   *result;
	HostError  *error;
	bool		ok;
} doltgres_entries_testing_states_frame_0_t;

/* doltgres_entries_testing_unnumbered returns the error of a number the storage states' tests can't take. */
static HostError
doltgres_entries_testing_unnumbered(void)
{
	static const char unnumbered[] = "the storage states' tests take 32-bit numbers";
	StdString	message;

	std_string_from_Checked(&message, unnumbered, sizeof(unnumbered) - 1);
	return host_Error_fork((const uint8 *) "22023", &message);
}

/* doltgres_entries_testing_states_body_0 makes the repository and holds a state for each of its storages. */
static void
doltgres_entries_testing_states_body_0(void *frame_)
{
	doltgres_entries_testing_states_frame_0_t *frame = frame_;
	ChunksErrorError storage;
	uint32		id;
	uint32		count;

	if (frame->repository <= 0 || frame->repository > PG_UINT32_MAX || frame->count < 0 ||
		frame->count > PG_UINT32_MAX)
	{
		*frame->error = doltgres_entries_testing_unnumbered();
		return;
	}
	id = (uint32) frame->repository;
	count = (uint32) frame->count;
	std_path_join(".", 1, HOST_VERSIONED, strlen(HOST_VERSIONED), &frame->versioned);
	frame->versioned_live = true;
	if (!chunks_file_create_dir(HOST_IO_IO_IO_DOORS(frame->io), (const char *) frame->versioned.bytes.ptr,
								frame->versioned.bytes.len, &storage))
	{
		*frame->error = txn_base_storage(&storage);
		return;
	}
	frame->versioned_live = false;
	std_vec_free_Checked(&frame->versioned.bytes, 1, 1);
	if (!repo_Repo_create(frame->io, id, HOST_PG_DEFAULT, &frame->repo, &storage))
	{
		*frame->error = txn_base_storage(&storage);
		return;
	}
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		return;
	frame->reading_live = true;
	std_vec_new(&frame->holding, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	frame->holding_live = true;
	for (uint64 relation = 1; relation <= count; relation++)
	{
		HostStorage held = {HOST_PG_DEFAULT, id, (uint32) relation};
		HostSharedHeld state;

		if (!txn_state_state(frame->io, &frame->repo, &frame->reading, &held, TXN_STATE_PERSISTENCE_PERMANENT, false,
							 NULL, &state, frame->error))
			return;
		std_vec_push_Checked(&frame->holding, &state, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	}
	*frame->result = (int64) count;
	frame->ok = true;
}

/* doltgres_entries_testing_states_pad_0 drops the states held, the reader, the repository, and the path. */
static void
doltgres_entries_testing_states_pad_0(void *frame_)
{
	doltgres_entries_testing_states_frame_0_t *frame = frame_;

	if (frame->holding_live)
	{
		for (size_t at = 0; at < frame->holding.len; at++)
			host_shared_Held_drop(&((HostSharedHeld *) frame->holding.ptr)[at]);
		std_vec_free_Checked(&frame->holding, sizeof(HostSharedHeld), _Alignof(HostSharedHeld));
	}
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		repo_Repo_drop(&frame->repo);
	if (frame->versioned_live)
		std_vec_free_Checked(&frame->versioned.bytes, 1, 1);
	frame->holding_live = frame->reading_live = frame->repo_live = frame->versioned_live = false;
}

/* doltgres_entries_testing_states makes and holds the states of doltgres_test_states. */
static bool
doltgres_entries_testing_states(HostIoIo *io, int64 repository, int64 count, int64 *result, HostError *error)
{
	doltgres_entries_testing_states_frame_0_t frame = {.io = io, .repository = repository, .count = count,
	.result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_states_body_0, doltgres_entries_testing_states_pad_0);
	doltgres_entries_testing_states_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_doltgres_test_states creates a repository with an
 * id, making pg_versioned when it is missing, and makes a storage state for
 * each of a count of its storages, holding them all until the call ends, so
 * a test can count the states' pool: it returns the count made, or the pool's
 * error.
 */
bool
doltgres_entries_testing_doltgres_test_states(const PgCaps *caps, int64 repository, int64 count, int64 *result,
											  PgPgError *error)
{
	HostIoIo	io = pghost_io(pg_Caps_may_raise(caps));
	HostError	failed;

	if (doltgres_entries_testing_states(&io, repository, count, result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/*
 * doltgres_entries_testing_doltgres_test_relation_locks returns how many
 * relation locks Postgres can grant at once as the backend's own lock
 * globals count them after its init, which the storage states' pool's count
 * must equal.
 */
bool
doltgres_entries_testing_doltgres_test_relation_locks(const PgCaps *caps, int64 *result, PgPgError *error)
{
	size_t		count;

	if (!pg_calls_granted_relation_locks(pg_Caps_may_raise(caps), &count, error))
		return false;
	*result = (int64) count;
	return true;
}

/*
 * doltgres_entries_testing_numbers_datum returns numbers apart by spaces as
 * a text datum, whose text's expect at a line and column of testing.rs names
 * a text that holds a NUL.
 */
static bool
doltgres_entries_testing_numbers_datum(const PgMayRaise *raise, const uint64 *numbers, size_t numbers_len,
									   const char *expect, uint32 line, uint32 col, PgDatum *result,
									   PgPgError *error)
{
	StdString	shown;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
	StdFmtArg	args[3];
	StdFmtArguments arguments = {pieces, 2 * numbers_len - 1, args};

	for (size_t at = 0; at < numbers_len; at++)
	{
		args[at].value = &numbers[at];
		args[at].fmt = std_u64_Display_fmt;
	}
	std_fmt_format_Checked(&shown, &arguments);
	return doltgres_entries_testing_text_datum(raise, &shown, expect, line, col, result, error);
}

/*
 * doltgres_entries_testing_doltgres_test_collect asks the collector worker
 * for one full collection of the session's database's repository and returns
 * what it did as the packs it retired, those it copied out, and its live
 * bytes, all 0 when a base backup kept it from finishing.
 */
bool
doltgres_entries_testing_doltgres_test_collect(const PgCatalogCaps *caps, PgDatum *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	HostIoIo	io = pghost_io(raise);
	uint32		database;
	uint32		tablespace;
	uint64		counts[3];
	HostError	failed;

	if (!host_session_database(&database, &tablespace, &failed) ||
		!host_collection_ask(&io, database, CHUNKS_PACKS_DEFAULT_SPACE, counts, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (counts[0] == PG_UINT64_MAX)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("the collector's collection of database "), STD_FMT_ARG(0),
		STD_FMT_LIT(" failed")};
		StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		failed = host_Error_fork((const uint8 *) "XX000", &message);
		*error = pghost_raised(&failed);
		return false;
	}
	return doltgres_entries_testing_numbers_datum(raise, counts, lengthof(counts),
												  "a collection's counts hold no NUL", 3141, 61, result, error);
}

/*
 * doltgres_entries_testing_doltgres_test_disk_space returns the free bytes
 * and the size of the data directory's volume, as the collector's schedule
 * reads them.
 */
bool
doltgres_entries_testing_doltgres_test_disk_space(const PgCaps *caps, PgDatum *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io = pghost_io(raise);
	StdPathBuf	dir = host_versioned_dir(HOST_PG_DEFAULT);
	uint64		space[2];
	HostError	failed;
	bool		ok;

	ok = host_disk_space(HOST_IO_IO_IO_DOORS(&io), (const char *) dir.bytes.ptr, dir.bytes.len, &space[0], &space[1],
						 &failed);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!ok)
	{
		*error = pghost_raised(&failed);
		return false;
	}
	return doltgres_entries_testing_numbers_datum(raise, space, lengthof(space), "two numbers hold no NUL", 3151, 68,
												  result, error);
}

/*
 * doltgres_entries_testing_doltgres_test_database_lock takes, without
 * waiting, or gives back the lock the collector worker takes on a database
 * for each check, as a session lock that outlives the call, answering
 * whether it took it.
 */
bool
doltgres_entries_testing_doltgres_test_database_lock(const PgCaps *caps, uint32 database, bool hold, bool *result,
													 PgPgError *error)
{
	HostIoIo	io;
	HostError	failed;

	if (!hold)
	{
		host_database_unlocked(database);
		*result = true;
		return true;
	}
	io = pghost_io(pg_Caps_may_raise(caps));
	if (host_database_locked(&io, database, result, &failed))
		return true;
	*error = pghost_raised(&failed);
	return false;
}

/* doltgres_entries_testing_doltgres_test_built_nodes_closure_0 is doltgres_test_built_nodes' pick: it counts each built base's nodes and picks none. */
static bool
doltgres_entries_testing_doltgres_test_built_nodes_closure_0(void *captures, HostIoIo *io, uint32 repository, uint32 tablespace,
															 uint32 relation, uint64 point, uint64 writer, bool *result,
															 HostError *error)
{
	uint64	   *count = captures;

	(void) io;
	(void) repository;
	(void) tablespace;
	(void) relation;
	(void) point;
	(void) writer;
	(void) error;
	*count = STD_ADD(u64, *count, 1, 3174, 9);
	*result = false;
	return true;
}

/*
 * doltgres_entries_testing_doltgres_test_built_nodes returns how many built
 * bases' nodes every repository's working state's delta holds, as a replay
 * that installs or drops each built base leaves none.
 */
bool
doltgres_entries_testing_doltgres_test_built_nodes(const PgCaps *caps, PgDatum *result, PgPgError *error)
{
	const PgMayRaise *raise = pg_Caps_may_raise(caps);
	HostIoIo	io = pghost_io(raise);
	uint64		count = 0;
	HostError	failed;
	StdString	shown;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&count, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!txn_memtable_branch_every_nodes_taken(&io, doltgres_entries_testing_doltgres_test_built_nodes_closure_0, &count, NULL,
											   &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	std_fmt_format_Checked(&shown, &arguments);
	return doltgres_entries_testing_text_datum(raise, &shown, "a count holds no NUL", 3178, 60, result, error);
}

/*
 * doltgres_entries_testing_doltgres_test_epochs returns the oldest epoch a
 * live reader started at and the current epoch, so a case shows which reader
 * keeps what the epochs keep.
 */
bool
doltgres_entries_testing_doltgres_test_epochs(const PgCaps *caps, PgDatum *result, PgPgError *error)
{
	uint64		epochs[2];

	epochs[0] = shmem_records_epochs_passing();
	epochs[1] = shmem_records_epochs_current();
	return doltgres_entries_testing_numbers_datum(pg_Caps_may_raise(caps), epochs, lengthof(epochs),
												  "two numbers hold no NUL", 3196, 58, result, error);
}

/*
 * doltgres_entries_testing_identity_frame_0_t is doltgres_test_identity's
 * state across its pad: its arguments, the session's repository, its reader,
 * and the key and value a lookup holds, each while live.
 */
typedef struct doltgres_entries_testing_identity_frame_0_t
{
	const PgCatalogCaps *caps;
	int32		kind;
	uint32		number;
	int32		attnum;
	HostIoIo	io;
	HostIoWaits waits;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	StdVec		key;
	bool		key_live;
	StdVec		value;
	bool		value_live;
	PgDatum    *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_identity_frame_0_t;

/*
 * doltgres_entries_testing_identified returns the id a key of the database's
 * identity tree pairs with a number, or none; it takes the frame's key.
 */
static bool
doltgres_entries_testing_identified(doltgres_entries_testing_identity_frame_0_t *frame, TxnIdentityDatabase tree,
									bool *has_result, uint64 *result)
{
	HostError	failed;
	bool		ok;

	ok = txn_identity_looked_up(&frame->io, &frame->waits, tree, frame->key.ptr, frame->key.len, has_result,
								&frame->value, &failed);
	frame->key_live = false;
	std_vec_free_Checked(&frame->key, 1, 1);
	if (ok && *has_result)
	{
		frame->value_live = true;
		ok = txn_identity_id(frame->value.ptr, frame->value.len, result, &failed);
		frame->value_live = false;
		std_vec_free_Checked(&frame->value, 1, 1);
	}
	if (!ok)
		*frame->error = pghost_raised(&failed);
	return ok;
}

/* doltgres_entries_testing_identity_body_0 looks the frame's object up in the database's identity tree. */
static void
doltgres_entries_testing_identity_body_0(void *frame_)
{
	doltgres_entries_testing_identity_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	TxnIdentityDatabase tree;
	uint32		database;
	bool		has;
	uint64		id;
	StdString	shown;

	frame->io = pghost_io(raise);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	database = pg_calls_my_database_id();
	if (database == 0)
	{
		*frame->error = doltgres_entries_testing_pg_fork("a backend has no database");
		return;
	}
	tree.f0 = STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo));
	tree.f1 = &frame->reading;
	tree.f2 = database;
	frame->waits = pghost_waits();
	txn_identity_by_oid(frame->kind == 1 ? TXN_IDENTITY_KIND_SCHEMA : TXN_IDENTITY_KIND_RELATION, frame->number,
						&frame->key);
	frame->key_live = true;
	if (!doltgres_entries_testing_identified(frame, tree, &has, &id))
		return;
	if (frame->kind != 1 && frame->kind != 2)
	{
		if (has && frame->attnum >= 0 && frame->attnum <= PG_UINT16_MAX)
		{
			txn_identity_column_by_number(id, (uint16) frame->attnum, &frame->key);
			frame->key_live = true;
			if (!doltgres_entries_testing_identified(frame, tree, &has, &id))
				return;
		}
		else
			has = false;
	}
	if (has)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&id, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&shown, &arguments);
	}
	else
		std_string_from_Checked(&shown, "none", 4);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "an id's text holds no NUL", 3047, 47,
													frame->result, frame->error);
}

/* doltgres_entries_testing_identity_pad_0 drops the value, the key, the reader, and the repository. */
static void
doltgres_entries_testing_identity_pad_0(void *frame_)
{
	doltgres_entries_testing_identity_frame_0_t *frame = frame_;

	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_testing_Repo_drop);
	frame->value_live = frame->key_live = frame->reading_live = frame->repo_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_identity returns the id the
 * database's identity tree pairs with an object's number, as text, or
 * "none": a schema's OID for kind 1, a relation's for kind 2, and for kind 3
 * a column's attribute number under the id of the table of the OID given.
 */
bool
doltgres_entries_testing_doltgres_test_identity(const PgCatalogCaps *caps, int32 kind, uint32 number, int32 attnum,
												PgDatum *result, PgPgError *error)
{
	doltgres_entries_testing_identity_frame_0_t frame = {.caps = caps, .kind = kind, .number = number,
	.attnum = attnum, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_identity_body_0, doltgres_entries_testing_identity_pad_0);
	doltgres_entries_testing_identity_pad_0(&frame);
	return frame.ok;
}

/* state.rs' held at Option<Base>, which entries.c defines. */
#define TXN_STATE_TS OptionBase
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseBase *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* state.rs' held at Option<Entry>. */
#define TXN_STATE_TS OptionEntry
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnMemtableMapEntry *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnMemtableMapEntry *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at bool, which entries.c defines. */
#define TXN_STATE_TS Bool
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *result, HostError *error
#define TXN_STATE_DECLARE
#include "doltgres/txn/state_template.h"

/* state.rs' held at (bool, Sizes). */
#define TXN_STATE_TS TupleBoolSizes
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *result_0, TxnBaseSizes *result_1, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *result_0; TxnBaseSizes *result_1;
#define TXN_STATE_RESULT_INIT , result_0, result_1
#define TXN_STATE_RESULT_ARGS(c) , (c)->result_0, (c)->result_1
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at Vec<String>. */
#define TXN_STATE_TS VecString
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , StdVec *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS StdVec *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* doltgres_entries_testing_strings_drop drops a Vec<String>: its strings, then the vector. */
static void
doltgres_entries_testing_strings_drop(StdVec *strings)
{
	for (size_t at = 0; at < strings->len; at++)
		std_String_drop(&((StdString *) strings->ptr)[at]);
	std_vec_free_Checked(strings, sizeof(StdString), _Alignof(StdString));
}

/* doltgres_entries_testing_ctid returns a tuple id as its ctid's text, its block above its offset. */
static void
doltgres_entries_testing_ctid(uint64 tid, StdString *result)
{
	uint64		block = tid >> 16;
	uint64		offset = tid & 0xffff;
	StdFmtPiece pieces[] = {STD_FMT_LIT("("), STD_FMT_ARG(0), STD_FMT_LIT(","), STD_FMT_ARG(1), STD_FMT_LIT(")")};
	StdFmtArg	args[] = {{&block, std_u64_Display_fmt}, {&offset, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(result, &arguments);
}

/*
 * doltgres_entries_testing_held_frame_0_t is the state across their pads of
 * the entries that read a relation's storage's state: their arguments, the
 * session's repository, its reader, and what a read holds, each while live.
 */
typedef struct doltgres_entries_testing_held_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgRel *target;
	int32		block;
	uint64		tid;
	HostIoIo	io;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	/* index is whether target is an index, whose state index_making finds. */
	bool		index;
	/* catalog_io is whether the entry's Io reads the catalog. */
	bool		catalog_io;
	const PgRel *heap;
	TxnStateFound found;
	TreesKeyOrder order;
	bool		order_live;
	bool		keyless;
	/* key_columns is the Vec<(usize, Order, KeyType)> of the table's key. */
	StdVec		key_columns;
	bool		key_columns_live;
	DoltgresIndexDescribed described;
	bool		described_live;
	uint64		folding;
	/* warned is the Vec<String> of a fold's warnings, live from warned_at on. */
	StdVec		warned;
	bool		warned_live;
	size_t		warned_at;
	HostDeltaHold hold;
	bool		hold_live;
	txn_TupleVecU8VecVersion_t next;
	bool		next_live;
	/* after is the Vec<u8> of the key a read of the delta's keys follows. */
	StdVec		after;
	bool		after_live;
	/* parts is the Vec<String> of a key's versions' texts. */
	StdVec		parts;
	bool		parts_live;
	TxnMemtableMapEntry entry;
	bool		entry_live;
	TxnBaseBase base;
	bool		base_live;
	/* strings is a Vec<String> a read lists. */
	StdVec		strings;
	bool		strings_live;
	/* ops is a Vec<&str> of a key's ops. */
	StdVec		ops;
	bool		ops_live;
	PgDatum    *datum;
	bool	   *answer;
	int32	   *levels;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_held_frame_0_t;

/* doltgres_entries_testing_held_repo returns the repository the frame's session's Arc holds. */
static const RepoRepo *
doltgres_entries_testing_held_repo(const doltgres_entries_testing_held_frame_0_t *frame)
{
	return STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo));
}

/* doltgres_entries_testing_read opens the frame's session repository, with the frame's Io, and its reader. */
static bool
doltgres_entries_testing_read(doltgres_entries_testing_held_frame_0_t *frame)
{
	HostError	failed;

	frame->io = frame->catalog_io ? pghost_io_catalog(frame->caps) : pghost_io(pg_CatalogCaps_may_raise(frame->caps));
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return false;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return false;
	}
	frame->reading_live = true;
	return true;
}

/*
 * doltgres_entries_testing_opened opens the frame's session repository and
 * its reader, and finds the frame's target's storage's state, making it when
 * the backend doesn't hold it.
 */
static bool
doltgres_entries_testing_opened(doltgres_entries_testing_held_frame_0_t *frame)
{
	if (!doltgres_entries_testing_read(frame))
		return false;
	if (frame->index)
		return doltgres_entries_index_making(&frame->io, frame->target, &frame->found, frame->error);
	return doltgres_entries_making(frame->caps, &frame->io, frame->target, &frame->found, frame->error);
}

/*
 * doltgres_entries_testing_shaped opens what opened does, finding the frame's
 * target's key order and whether it is keyless before its state.
 */
static bool
doltgres_entries_testing_shaped(doltgres_entries_testing_held_frame_0_t *frame)
{
	if (!doltgres_entries_testing_read(frame) ||
		!doltgres_entries_shape(frame->caps, &frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								frame->target, NULL, &frame->order, &frame->keyless, frame->error))
		return false;
	frame->order_live = true;
	return doltgres_entries_making(frame->caps, &frame->io, frame->target, &frame->found, frame->error);
}

/* doltgres_entries_testing_held_pad_0 drops what a read holds, the reader, and the repository's Arc. */
static void
doltgres_entries_testing_held_pad_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;

	if (frame->ops_live)
		std_vec_free_Checked(&frame->ops, sizeof(StdStr), _Alignof(StdStr));
	if (frame->parts_live)
		doltgres_entries_testing_strings_drop(&frame->parts);
	if (frame->after_live)
		std_vec_free_Checked(&frame->after, 1, 1);
	if (frame->next_live)
	{
		std_vec_free_Checked(&frame->next.f0, 1, 1);
		for (size_t at = 0; at < frame->next.f1.len; at++)
			txn_memtable_version_Version_drop(&((TxnMemtableVersionVersion *) frame->next.f1.ptr)[at]);
		std_vec_free_Checked(&frame->next.f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	}
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->warned_live)
	{
		while (frame->warned_at < frame->warned.len)
			std_String_drop(&((StdString *) frame->warned.ptr)[frame->warned_at++]);
		std_vec_free_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString));
	}
	if (frame->described_live)
		doltgres_index_Described_drop(&frame->described);
	if (frame->key_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->key_columns);
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->strings_live)
		doltgres_entries_testing_strings_drop(&frame->strings);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->entry_live && frame->entry.has_value)
		std_vec_free_Checked(&frame->entry.value, 1, 1);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_testing_Repo_drop);
	frame->ops_live = frame->parts_live = frame->after_live = frame->next_live = frame->hold_live = false;
	frame->warned_live = frame->described_live = frame->key_columns_live = frame->order_live = false;
	frame->strings_live = frame->base_live = frame->entry_live = false;
	frame->reading_live = frame->repo_live = false;
}

/* doltgres_entries_testing_held_run runs an entry's body on a frame, then its pad. */
static bool
doltgres_entries_testing_held_run(doltgres_entries_testing_held_frame_0_t *frame, void (*body) (void *frame))
{
	std_landing(frame, body, doltgres_entries_testing_held_pad_0);
	doltgres_entries_testing_held_pad_0(frame);
	return frame->ok;
}

/* doltgres_entries_testing_doltgres_test_free_space_closure_0 is doltgres_test_free_space's lent call: the block's newest entry in the free space map. */
static bool
doltgres_entries_testing_doltgres_test_free_space_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															const HostSharedHeld *held, bool *some, TxnMemtableMapEntry *result,
															HostError *error)
{
	(void) repo;
	return txn_memtable_map_newest(io, host_shared_Held_record(held), TXN_MEMTABLE_MAP_MAP_FREE,
								   *(const uint32 *) captures, some, result, error);
}

/* doltgres_entries_testing_free_space_body_0 reads the frame's block's newest free space entry as its text. */
static void
doltgres_entries_testing_free_space_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	uint32		block = (uint32) frame->block;
	uint16		most = pg_calls_max_heap_tuples_per_page();
	HostError	failed;
	bool		some;
	StdString	shown;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_OptionEntry(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
									frame->found, doltgres_entries_testing_doltgres_test_free_space_closure_0, &block, NULL, &some,
									&frame->entry, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->entry_live = some;
	if (some && frame->entry.has_value)
	{
		TxnFreeSpaceFree entry;
		StdString	joined;
		StdStr		full = {" full", 0};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" {"), STD_FMT_ARG(1), STD_FMT_LIT("}"), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&entry.bytes, std_u64_Display_fmt}, {&joined, std_String_Display_fmt},
		{&full, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (!txn_free_space_Free_decode(frame->entry.value.ptr, frame->entry.value.len, block, (uint64) block << 16,
										most, &entry, &failed))
		{
			*frame->error = pghost_raised(&failed);
			return;
		}
		std_vec_new(&frame->strings, sizeof(StdString), _Alignof(StdString));
		frame->strings_live = true;
		for (uint16 n = 1; n <= most && n != 0; n++)
		{
			StdString	number;
			StdFmtPiece number_pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	number_args[] = {{&n, std_u16_Display_fmt}};
			StdFmtArguments number_arguments = {number_pieces, lengthof(number_pieces), number_args};

			if (!txn_free_space_Free_has(&entry, n))
				continue;
			std_fmt_format_Checked(&number, &number_arguments);
			std_vec_push_Checked(&frame->strings, &number, sizeof(StdString), _Alignof(StdString));
		}
		std_str_join(frame->strings.ptr, frame->strings.len, sizeof(StdString), ",", 1, &joined);
		full.len = entry.full ? 5 : 0;
		std_fmt_format_Checked(&shown, &arguments);
		std_String_drop(&joined);
	}
	else
		std_string_from_Checked(&shown, "none", 4);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "an entry's text holds no NUL", 2856, 47,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_free_space returns a block's newest
 * free space entry in a table's delta as its free bytes, its free offsets,
 * and "full" when its full flag is set, or "none" for a block with no entry,
 * so a case compares a standby's map with its primary's.
 */
bool
doltgres_entries_testing_doltgres_test_free_space(const PgCatalogCaps *caps, const PgRel *target, int32 block,
												  PgDatum *result, PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .block = block,
	.datum = result, .error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_free_space_body_0);
}

/* doltgres_entries_testing_doltgres_test_base_sizes_closure_0 is doltgres_test_base_sizes' lent call: the state's newest base. */
static bool
doltgres_entries_testing_doltgres_test_base_sizes_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															const HostSharedHeld *held, bool *some, TxnBaseBase *result,
															HostError *error)
{
	(void) captures;
	(void) repo;
	return txn_state_newest(io, held, some, result, error);
}

/* doltgres_entries_testing_base_sizes_body_0 reads the newest base's sizes and tuple ids as their text. */
static void
doltgres_entries_testing_base_sizes_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	bool		some;
	StdString	shown;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_OptionBase(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								   frame->found, doltgres_entries_testing_doltgres_test_base_sizes_closure_0, NULL, NULL, &some,
								   &frame->base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->base_live = some;
	if (some)
	{
		TxnBaseSizes sizes = frame->base.sizes;
		StdString	fresh;
		StdString	counter;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2),
		STD_FMT_LIT(" "), STD_FMT_ARG(3), STD_FMT_LIT(" "), STD_FMT_ARG(4)};
		StdFmtArg	args[] = {{&sizes.heap_bytes, std_u64_Display_fmt}, {&sizes.rows, std_u64_Display_fmt},
		{&sizes.entries, std_u64_Display_fmt}, {&fresh, std_String_Display_fmt}, {&counter, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		doltgres_entries_testing_ctid(frame->base.fresh_end, &fresh);
		doltgres_entries_testing_ctid(frame->base.counter, &counter);
		std_fmt_format_Checked(&shown, &arguments);
		std_String_drop(&counter);
		std_String_drop(&fresh);
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	else
		std_string_from_Checked(&shown, "none", 4);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "a base's sizes' text holds no NUL", 2878, 47,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_base_sizes returns a table's
 * storage's newest base's sizes as its heap bytes, its rows, and its
 * entries, then its fresh end and counter as ctids, or "none" for a storage
 * with no base.
 */
bool
doltgres_entries_testing_doltgres_test_base_sizes(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
												  PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .datum = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_base_sizes_body_0);
}

static void doltgres_entries_testing_hex(StdBytes bytes, StdString *result);

/* doltgres_entries_testing_base_points_body_0 reads the newest base's root, fold point, and read point as their text. */
static void
doltgres_entries_testing_base_points_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	bool		some;
	StdString	shown;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_OptionBase(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								   frame->found, doltgres_entries_testing_doltgres_test_base_sizes_closure_0, NULL, NULL, &some,
								   &frame->base, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->base_live = some;
	if (some)
	{
		StdString	root;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&root, std_String_Display_fmt}, {&frame->base.folded, std_u64_Display_fmt},
		{&frame->base.read, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (frame->base.has_root)
		{
			StdBytes	address = {frame->base.root.address.f0, sizeof(frame->base.root.address.f0)};

			doltgres_entries_testing_hex(address, &root);
		}
		else
			std_vec_new(&root, 1, 1);
		std_fmt_format_Checked(&shown, &arguments);
		std_String_drop(&root);
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	else
		std_string_from_Checked(&shown, "none", 4);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "a base's points' text holds no NUL", 2899, 47,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_base_points returns a table's
 * storage's newest base's root as its address in hex, with its fold point and
 * its read point, or "none" for a storage with no base, so a standby's base
 * is compared with its primary's.
 */
bool
doltgres_entries_testing_doltgres_test_base_points(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
												   PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .datum = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_base_points_body_0);
}

/* doltgres_entries_testing_doltgres_test_held_closure_0 is doltgres_test_held's lent call: whether a version holds the tuple id. */
static bool
doltgres_entries_testing_doltgres_test_held_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
													  bool *result, HostError *error)
{
	(void) repo;
	return txn_vacuum_held(io, host_shared_Held_record(held), *(const uint64 *) captures, result, error);
}

/* doltgres_entries_testing_doltgres_test_mapped_closure_0 is doltgres_test_mapped's lent call: whether a map holds a version. */
static bool
doltgres_entries_testing_doltgres_test_mapped_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														const HostSharedHeld *held, bool *result, HostError *error)
{
	(void) captures;
	(void) io;
	(void) repo;
	(void) error;
	*result = txn_state_mapped(host_shared_Held_record(held));
	return true;
}

/* doltgres_entries_testing_answered answers a question of the frame's target's state by a lent call. */
static void
doltgres_entries_testing_answered(doltgres_entries_testing_held_frame_0_t *frame,
								  bool (*call) (void *captures, HostIoIo *io, const RepoRepo *repo,
												const HostSharedHeld *held, bool *result, HostError *error))
{
	HostError	failed;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_Bool(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading, frame->found,
							 call, &frame->tid, NULL, frame->answer, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/* doltgres_entries_testing_held_body_0 answers whether a version holds the frame's tuple id. */
static void
doltgres_entries_testing_held_body_0(void *frame_)
{
	doltgres_entries_testing_answered(frame_, doltgres_entries_testing_doltgres_test_held_closure_0);
}

/* doltgres_entries_testing_mapped_body_0 answers whether a map of the frame's target's state holds a version. */
static void
doltgres_entries_testing_mapped_body_0(void *frame_)
{
	doltgres_entries_testing_answered(frame_, doltgres_entries_testing_doltgres_test_mapped_closure_0);
}

/*
 * doltgres_entries_testing_doltgres_test_held reports whether a row version
 * in a table's delta holds a tuple id, its block above its offset, so a case
 * finds a standby's delta mapping a tuple id its free space map holds free.
 */
bool
doltgres_entries_testing_doltgres_test_held(const PgCatalogCaps *caps, const PgRel *target, uint64 tid, bool *result,
											PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .tid = tid, .answer = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_held_body_0);
}

/*
 * doltgres_entries_testing_doltgres_test_mapped says whether a table's
 * storage's delta holds a version in any of its maps, its tuple id table,
 * its free space map, or its value map, by the words that name their
 * memtables.
 */
bool
doltgres_entries_testing_doltgres_test_mapped(const PgCatalogCaps *caps, const PgRel *target, bool *result,
											  PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .answer = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_mapped_body_0);
}

/* doltgres_entries_testing_doltgres_test_totals_closure_0 is doltgres_test_totals' lent call: whether the state's totals are known, and them. */
static bool
doltgres_entries_testing_doltgres_test_totals_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														const HostSharedHeld *held, bool *result_0, TxnBaseSizes *result_1,
														HostError *error)
{
	(void) captures;
	(void) io;
	(void) repo;
	(void) error;
	*result_0 = txn_state_known(held);
	*result_1 = txn_state_totals(held);
	return true;
}

/* doltgres_entries_testing_totals_body_0 reads the state's totals as their text. */
static void
doltgres_entries_testing_totals_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	bool		known;
	TxnBaseSizes sizes;
	StdString	shown;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2),
	STD_FMT_LIT(" "), STD_FMT_ARG(3), STD_FMT_LIT(" "), STD_FMT_ARG(4), STD_FMT_LIT(" "), STD_FMT_ARG(5)};
	StdFmtArg	args[] = {{&known, std_bool_Display_fmt}, {&sizes.entries, std_u64_Display_fmt},
	{&sizes.leaf_bytes, std_u64_Display_fmt}, {&sizes.key_bytes, std_u64_Display_fmt},
	{&sizes.heap_bytes, std_u64_Display_fmt}, {&sizes.rows, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_TupleBoolSizes(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
									   frame->found, doltgres_entries_testing_doltgres_test_totals_closure_0, NULL, NULL, &known,
									   &sizes, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	std_fmt_format_Checked(&shown, &arguments);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "a state's totals' text holds no NUL", 3215, 47,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_totals returns a fork table's
 * storage's totals as the state holds them, with no count: whether they are
 * known, then its entries, leaf bytes, key bytes, heap bytes, and rows.
 */
bool
doltgres_entries_testing_doltgres_test_totals(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
											  PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .datum = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_totals_body_0);
}

/*
 * doltgres_entries_testing_doltgres_test_scan_shape returns a fork table's
 * shape as its scans' costs read it: its tree's leaves and levels above
 * them, then its tuple id tree's.
 */
bool
doltgres_entries_testing_doltgres_test_scan_shape(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
												  PgPgError *error)
{
	DoltgresEntriesScanned shape;
	uint64		heights[2];
	StdString	shown;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2),
	STD_FMT_LIT(" "), STD_FMT_ARG(3)};
	StdFmtArg	args[] = {{&shape.rows.leaves, std_u64_Display_fmt}, {&heights[0], std_u64_Display_fmt},
	{&shape.tuple_ids.leaves, std_u64_Display_fmt}, {&heights[1], std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!doltgres_entries_scanned(caps, 0, pg_calls_rel_id(target), &shape, error))
		return false;
	heights[0] = shape.rows.height;
	heights[1] = shape.tuple_ids.height;
	std_fmt_format_Checked(&shown, &arguments);
	return doltgres_entries_testing_text_datum(pg_CatalogCaps_may_raise(caps), &shown,
											   "a scan shape's text holds no NUL", __LINE__, 0, result, error);
}

/* doltgres_entries_testing_op_name returns the name doltgres_test_changes gives a version's op. */
static StdStr
doltgres_entries_testing_op_name(TxnMemtableVersionOpRefTag op)
{
	static const char *const names[] = {"put", "delete", "emptied", "named", "move", "add", "remove", "lock",
	"add_row", "update_row", "delete_row", "lock_row"};
	StdStr		name = {names[op], strlen(names[op])};

	StaticAssertStmt(lengthof(names) == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW + 1, "every op has a name");
	return name;
}

/*
 * doltgres_entries_testing_doltgres_test_changes_closure_0_0 is doltgres_test_changes' each:
 * a key's ops, oldest first and apart by spaces, as the next of the keys.
 */
static bool
doltgres_entries_testing_doltgres_test_changes_closure_0_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len,
														   bool has_place, size_t place, StdVec *versions, HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;
	StdString	ops;

	(void) io;
	(void) key;
	(void) key_len;
	(void) has_place;
	(void) place;
	(void) error;
	std_vec_with_capacity_Checked(&frame->ops, versions->len, sizeof(StdStr), _Alignof(StdStr));
	frame->ops_live = true;
	for (size_t at = versions->len; at > 0; at--)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&((TxnMemtableVersionVersion *)
																				versions->ptr)[at - 1]);
		StdStr		name = doltgres_entries_testing_op_name(txn_memtable_version_Record_op(&record).tag);

		std_vec_push_Checked(&frame->ops, &name, sizeof(StdStr), _Alignof(StdStr));
	}
	std_str_join(frame->ops.ptr, frame->ops.len, sizeof(StdStr), " ", 1, &ops);
	frame->ops_live = false;
	std_vec_free_Checked(&frame->ops, sizeof(StdStr), _Alignof(StdStr));
	std_vec_push_Checked(&frame->strings, &ops, sizeof(StdString), _Alignof(StdString));
	return true;
}

/* doltgres_entries_testing_doltgres_test_changes_closure_0 is doltgres_test_changes' lent call: each key's ops, in the keys' order. */
static bool
doltgres_entries_testing_doltgres_test_changes_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
														 const HostSharedHeld *held, StdVec *result, HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;

	(void) repo;
	std_vec_new(result, sizeof(StdString), _Alignof(StdString));
	frame->strings_live = true;
	return txn_rewrite_keys(io, held, NULL, 0, 0, doltgres_entries_testing_doltgres_test_changes_closure_0_0, frame, NULL, error);
}

/* doltgres_entries_testing_changes_body_0 reads the delta's keys' ops as their text. */
static void
doltgres_entries_testing_changes_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	StdString	shown;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_VecString(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								  frame->found, doltgres_entries_testing_doltgres_test_changes_closure_0, frame, NULL,
								  &frame->strings, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	std_str_join(frame->strings.ptr, frame->strings.len, sizeof(StdString), "; ", 2, &shown);
	frame->strings_live = false;
	doltgres_entries_testing_strings_drop(&frame->strings);
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "a delta's ops hold no NUL", 2958, 58,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_changes returns a table's delta's
 * row versions as each key's ops, oldest first and apart by spaces, the keys
 * in their bytes' order and apart by "; ", so a case reads which versions a
 * rewrite carried.
 */
bool
doltgres_entries_testing_doltgres_test_changes(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
											   PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .datum = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_changes_body_0);
}

/* state.rs' held at i32. */
#define TXN_STATE_TS I32
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , int32 *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS int32 *result;
#define TXN_STATE_RESULT_INIT , result
#define TXN_STATE_RESULT_ARGS(c) , (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* state.rs' held at Option<Sizes>. */
#define TXN_STATE_TS OptionSizes
#define TXN_STATE_RETURNS bool
#define TXN_STATE_RESULT_PARAMS , bool *some, TxnBaseSizes *result, HostError *error
#define TXN_STATE_DECLARE
#define TXN_STATE_DEFINE
#define TXN_STATE_RESULT_FIELDS bool *some; TxnBaseSizes *result;
#define TXN_STATE_RESULT_INIT , some, result
#define TXN_STATE_RESULT_ARGS(c) , (c)->some, (c)->result
#define TXN_STATE_RESULT_ERROR error
#include "doltgres/txn/state_template.h"

/* trees' pin at Result<Vec<String>, Error>, for a base's lines. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultVecString
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , StdVec *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS StdVec *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* trees' pin at Result<i32, Error>, for a base's levels. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultI32
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , int32 *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS int32 *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* doltgres_entries_testing_hex returns bytes as their hex digits, two to a byte. */
static void
doltgres_entries_testing_hex(StdBytes bytes, StdString *result)
{
	static const char digits[] = "0123456789abcdef";

	std_vec_new(result, 1, 1);
	for (size_t at = 0; at < bytes.len; at++)
	{
		std_vec_push_Checked(result, &digits[bytes.ptr[at] >> 4], 1, 1);
		std_vec_push_Checked(result, &digits[bytes.ptr[at] & 0xf], 1, 1);
	}
}

/* doltgres_entries_testing_lines_datum returns lines apart by newlines as a text datum, or "none" for no lines; it takes them. */
static bool
doltgres_entries_testing_lines_datum(const PgMayRaise *raise, StdVec *lines, bool *live, const char *expect,
									 uint32 line, uint32 col, PgDatum *result, PgPgError *error)
{
	StdString	shown;

	if (lines->len == 0)
		std_string_from_Checked(&shown, "none", 4);
	else
		std_str_join(lines->ptr, lines->len, sizeof(StdString), "\n", 1, &shown);
	*live = false;
	doltgres_entries_testing_strings_drop(lines);
	return doltgres_entries_testing_text_datum(raise, &shown, expect, line, col, result, error);
}

/*
 * DoltgresEntriesTestingLines is how a base's scope shows each entry it
 * reads: its stamps' tuple ids, its key's and value's bytes and its stamps,
 * or none, for a scope that finds the levels.
 */
typedef enum DoltgresEntriesTestingLines
{
	DOLTGRES_ENTRIES_TESTING_LINES_STAMPS,
	DOLTGRES_ENTRIES_TESTING_LINES_ENTRIES,
	DOLTGRES_ENTRIES_TESTING_LINES_LEVELS,
} DoltgresEntriesTestingLines;

/*
 * doltgres_entries_testing_base_frame_0_t is a base's scope's state across
 * its pad: the tree, how it shows it, where its lines or its levels go, its
 * cursor and an entry's stamps and parts, each while live.
 */
typedef struct doltgres_entries_testing_base_frame_0_t
{
	TreesStoreTree root;
	DoltgresEntriesTestingLines lines;
	HostIoIo   *io;
	TreesPinPin *pin;
	StdVec	   *result;
	int32	   *levels;
	ChunksErrorError *error;
	TreesCursorCursor cursor;
	bool		cursor_live;
	StdVec		stamps;
	bool		stamps_live;
	/* parts is the Vec<String> of an entry's stamps' texts. */
	StdVec		parts;
	bool		parts_live;
	bool		result_live;
	bool		ok;
} doltgres_entries_testing_base_frame_0_t;

/* doltgres_entries_testing_base_line returns the line of the cursor's entry the frame shows. */
static bool
doltgres_entries_testing_base_line(doltgres_entries_testing_base_frame_0_t *frame, StdString *result)
{
	if (!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &frame->stamps, frame->error))
		return false;
	frame->stamps_live = true;
	std_vec_with_capacity_Checked(&frame->parts, frame->stamps.len, sizeof(StdString), _Alignof(StdString));
	frame->parts_live = true;
	for (size_t at = 0; at < frame->stamps.len; at++)
	{
		const ChunksTrailerStamp *stamp = &((const ChunksTrailerStamp *) frame->stamps.ptr)[at];
		StdString	part;

		doltgres_entries_testing_ctid(stamp->tuple, &part);
		if (frame->lines == DOLTGRES_ENTRIES_TESTING_LINES_ENTRIES)
		{
			StdString	ctid = part;
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&stamp->txn, std_u64_Display_fmt}, {&ctid, std_String_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&part, &arguments);
			std_String_drop(&ctid);
		}
		std_vec_push_Checked(&frame->parts, &part, sizeof(StdString), _Alignof(StdString));
	}
	frame->stamps_live = false;
	std_vec_free_Checked(&frame->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	if (frame->lines == DOLTGRES_ENTRIES_TESTING_LINES_STAMPS)
	{
		if (frame->parts.len == 0)
			std_string_from_Checked(result, "-", 1);
		else
			std_str_join(frame->parts.ptr, frame->parts.len, sizeof(StdString), " ", 1, result);
	}
	else
	{
		StdString	key;
		StdString	value;
		StdString	stamps;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&key, std_String_Display_fmt}, {&value, std_String_Display_fmt},
		{&stamps, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		doltgres_entries_testing_hex(trees_cursor_Cursor_key(&frame->cursor), &key);
		doltgres_entries_testing_hex(trees_cursor_Cursor_value(&frame->cursor), &value);
		std_str_join(frame->parts.ptr, frame->parts.len, sizeof(StdString), " ", 1, &stamps);
		std_fmt_format_Checked(result, &arguments);
		std_String_drop(&stamps);
		std_String_drop(&value);
		std_String_drop(&key);
	}
	frame->parts_live = false;
	doltgres_entries_testing_strings_drop(&frame->parts);
	return true;
}

/* doltgres_entries_testing_base_body_0 reads the tree from its first entry: each entry's line, or the level whose node is the root. */
static void
doltgres_entries_testing_base_body_0(void *frame_)
{
	doltgres_entries_testing_base_frame_0_t *frame = frame_;

	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->root), &frame->cursor,
								   frame->error))
		return;
	frame->cursor_live = true;
	if (frame->lines == DOLTGRES_ENTRIES_TESTING_LINES_LEVELS)
	{
		*frame->levels = -1;
		for (int level = 0; level <= PG_UINT8_MAX; level++)
		{
			TreesStoreTree at;

			if (trees_cursor_Cursor_tree_at(&frame->cursor, (uint8) level, &at) &&
				trees_store_Tree_PartialEq_eq(&at, &frame->root))
			{
				*frame->levels = level;
				break;
			}
		}
		frame->ok = true;
		return;
	}
	std_vec_new(frame->result, sizeof(StdString), _Alignof(StdString));
	frame->result_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdString	line;

		if (!doltgres_entries_testing_base_line(frame, &line))
			return;
		std_vec_push_Checked(frame->result, &line, sizeof(StdString), _Alignof(StdString));
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->error))
			return;
	}
	frame->result_live = false;
	frame->ok = true;
}

/* doltgres_entries_testing_base_pad_0 drops an entry's parts and stamps, the cursor, and the lines of a failed read. */
static void
doltgres_entries_testing_base_pad_0(void *frame_)
{
	doltgres_entries_testing_base_frame_0_t *frame = frame_;

	if (frame->parts_live)
		doltgres_entries_testing_strings_drop(&frame->parts);
	if (frame->stamps_live)
		std_vec_free_Checked(&frame->stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	if (frame->result_live)
		doltgres_entries_testing_strings_drop(frame->result);
	frame->parts_live = frame->stamps_live = frame->cursor_live = frame->result_live = false;
}

/* doltgres_entries_testing_base_lines is a base's scope of lines, in its pin. */
static bool
doltgres_entries_testing_base_lines(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result,
									ChunksErrorError *error)
{
	doltgres_entries_testing_base_frame_0_t *frame = captures;

	frame->io = io;
	frame->pin = pin;
	frame->result = result;
	frame->error = error;
	std_landing(frame, doltgres_entries_testing_base_body_0, doltgres_entries_testing_base_pad_0);
	doltgres_entries_testing_base_pad_0(frame);
	return frame->ok;
}

/* doltgres_entries_testing_doltgres_test_base_levels_closure_0_1 is a base's scope of levels, in its pin. */
static bool
doltgres_entries_testing_doltgres_test_base_levels_closure_0_1(void *captures, HostIoIo *io, TreesPinPin *pin, int32 *result,
															   ChunksErrorError *error)
{
	doltgres_entries_testing_base_frame_0_t *frame = captures;

	frame->io = io;
	frame->pin = pin;
	frame->levels = result;
	frame->error = error;
	std_landing(frame, doltgres_entries_testing_base_body_0, doltgres_entries_testing_base_pad_0);
	doltgres_entries_testing_base_pad_0(frame);
	return frame->ok;
}

/* doltgres_entries_testing_doltgres_test_index_base_closure_0_1 is doltgres_test_index_base's pin scope: its base's lines. */
static bool
doltgres_entries_testing_doltgres_test_index_base_closure_0_1(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result,
															  ChunksErrorError *error)
{
	return doltgres_entries_testing_base_lines(captures, io, pin, result, error);
}

/* doltgres_entries_testing_doltgres_test_index_fold_closure_0_1 is doltgres_test_index_fold's pin scope: its base's lines. */
static bool
doltgres_entries_testing_doltgres_test_index_fold_closure_0_1(void *captures, HostIoIo *io, TreesPinPin *pin, StdVec *result,
															  ChunksErrorError *error)
{
	return doltgres_entries_testing_base_lines(captures, io, pin, result, error);
}

/*
 * doltgres_entries_testing_newest_root returns the root of the newest base
 * of a held state, or none for no base or an empty one.
 */
static bool
doltgres_entries_testing_newest_root(HostIoIo *io, const HostSharedHeld *held, bool *has_result,
									 TreesStoreTree *result, HostError *error)
{
	TxnBaseBase base;
	bool		some;

	if (!txn_state_newest(io, held, &some, &base, error))
		return false;
	*has_result = some && base.has_root;
	if (some)
	{
		*result = base.root;
		txn_base_Base_drop(&base);
	}
	return true;
}

/*
 * doltgres_entries_testing_held_lines reads a held state's newest base as
 * lines in its pin, none for no base or an empty one.
 */
static bool
doltgres_entries_testing_held_lines(doltgres_entries_testing_held_frame_0_t *frame, HostIoIo *io, const RepoRepo *repo,
									const HostSharedHeld *held, DoltgresEntriesTestingLines lines, StdVec *result,
									HostError *error)
{
	doltgres_entries_testing_base_frame_0_t scope = {.lines = lines};
	TreesStoreReader store;
	ChunksErrorError failed;
	bool		has;

	if (!doltgres_entries_testing_newest_root(io, held, &has, &scope.root, error))
		return false;
	if (!has)
	{
		std_vec_new(result, sizeof(StdString), _Alignof(StdString));
		return true;
	}
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	if (trees_pin_pin_ResultVecString(io, TREES_STORE_READER_NODE_STORE(&store),
									  lines == DOLTGRES_ENTRIES_TESTING_LINES_STAMPS ?
									  doltgres_entries_testing_doltgres_test_index_base_closure_0_1 :
									  doltgres_entries_testing_doltgres_test_index_fold_closure_0_1, &scope, NULL, result, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/* doltgres_entries_testing_doltgres_test_index_base_closure_0 is doltgres_test_index_base's lent call: its base's lines. */
static bool
doltgres_entries_testing_doltgres_test_index_base_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															const HostSharedHeld *held, StdVec *result, HostError *error)
{
	return doltgres_entries_testing_held_lines(captures, io, repo, held, DOLTGRES_ENTRIES_TESTING_LINES_STAMPS, result,
											   error);
}

/* doltgres_entries_testing_index_base_body_0 reads the index's base's entries as their lines. */
static void
doltgres_entries_testing_index_base_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	HostError	failed;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_VecString(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								  frame->found, doltgres_entries_testing_doltgres_test_index_base_closure_0, frame, NULL,
								  &frame->strings, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->strings_live = true;
	frame->ok = doltgres_entries_testing_lines_datum(pg_CatalogCaps_may_raise(frame->caps), &frame->strings,
													 &frame->strings_live, "an index's base's text holds no NUL",
													 3003, 47, frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_index_base returns the entries an
 * index's storage's base holds, a line for each in key order: its count of
 * copies and its stamps' tuple ids, or a dash for an entry with no stamp, or
 * "none" for no base or an empty one.
 */
bool
doltgres_entries_testing_doltgres_test_index_base(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
												  PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .index = true,
	.datum = result, .error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_index_base_body_0);
}

/* doltgres_entries_testing_doltgres_test_base_levels_closure_0 is doltgres_test_base_levels' lent call: its newest base's levels above its leaves. */
static bool
doltgres_entries_testing_doltgres_test_base_levels_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															 const HostSharedHeld *held, int32 *result, HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;
	doltgres_entries_testing_base_frame_0_t scope = {.lines = DOLTGRES_ENTRIES_TESTING_LINES_LEVELS};
	TreesStoreReader store;
	ChunksErrorError failed;
	bool		has;

	if (!doltgres_entries_testing_newest_root(io, held, &has, &scope.root, error))
		return false;
	if (!has)
	{
		*result = -1;
		return true;
	}
	store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);
	if (trees_pin_pin_ResultI32(io, TREES_STORE_READER_NODE_STORE(&store), doltgres_entries_testing_doltgres_test_base_levels_closure_0_1,
								&scope, NULL, result, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/* doltgres_entries_testing_base_levels_body_0 finds the levels of the target's storage's newest base tree. */
static void
doltgres_entries_testing_base_levels_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	HostError	failed;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_I32(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading, frame->found,
							doltgres_entries_testing_doltgres_test_base_levels_closure_0, frame, NULL, frame->levels, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_entries_testing_doltgres_test_base_levels returns the levels
 * above the leaves of the newest base tree of a relation's storage, an
 * index's or a table's, found as the level at which a cursor at the tree's
 * first entry has the root on its path, or -1 for no tree.
 */
bool
doltgres_entries_testing_doltgres_test_base_levels(const PgCatalogCaps *caps, const PgRel *target, int32 *result,
												   PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target,
	.index = pg_calls_rel_kind(target) == RELKIND_INDEX, .catalog_io = true, .levels = result, .error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_base_levels_body_0);
}

/* doltgres_entries_testing_version_shown returns how doltgres_test_index_entries shows a version. */
static void
doltgres_entries_testing_version_shown(const TxnMemtableVersionVersion *version, StdString *result)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	const char *named;
	StdString	ctid;
	StdStr		name;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&name, std_str_Display_fmt}, {&ctid, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	switch (op.tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			named = "put";
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
			named = "delete";
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			name.ptr = op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD ? "add" : "remove";
			name.len = strlen(name.ptr);
			doltgres_entries_testing_ctid(op.tag == TXN_MEMTABLE_VERSION_OP_REF_ADD ? op.u.add.tuple.f0 :
										  op.u.remove.f0.f0, &ctid);
			std_fmt_format_Checked(result, &arguments);
			std_String_drop(&ctid);
			return;
		default:
			named = "other";
			break;
	}
	std_string_from_Checked(result, named, strlen(named));
}

/*
 * doltgres_entries_testing_doltgres_test_index_entries_closure_0 is
 * doltgres_test_index_entries' lent call: a line for each key of the
 * delta, in key order, of its versions newest first.
 */
static bool
doltgres_entries_testing_doltgres_test_index_entries_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															   const HostSharedHeld *held, StdVec *result, HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;
	bool		some;

	(void) repo;
	if (!host_delta_hold(io, &frame->hold, error))
		return false;
	frame->hold_live = true;
	std_vec_new(result, sizeof(StdString), _Alignof(StdString));
	frame->strings_live = true;
	for (;;)
	{
		StdString	line;

		if (!txn_memtable_table_next(io, &frame->hold, host_shared_Held_record(held), frame->after_live,
									 frame->after.ptr, frame->after.len, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &some,
									 &frame->next, error))
			return false;
		if (!some)
			break;
		frame->next_live = true;
		std_vec_with_capacity_Checked(&frame->parts, frame->next.f1.len, sizeof(StdString), _Alignof(StdString));
		frame->parts_live = true;
		for (size_t at = 0; at < frame->next.f1.len; at++)
		{
			StdString	shown;

			doltgres_entries_testing_version_shown(&((const TxnMemtableVersionVersion *) frame->next.f1.ptr)[at],
												   &shown);
			std_vec_push_Checked(&frame->parts, &shown, sizeof(StdString), _Alignof(StdString));
		}
		std_str_join(frame->parts.ptr, frame->parts.len, sizeof(StdString), ", ", 2, &line);
		frame->parts_live = false;
		doltgres_entries_testing_strings_drop(&frame->parts);
		std_vec_push_Checked(result, &line, sizeof(StdString), _Alignof(StdString));
		frame->next_live = false;
		for (size_t at = 0; at < frame->next.f1.len; at++)
			txn_memtable_version_Version_drop(&((TxnMemtableVersionVersion *) frame->next.f1.ptr)[at]);
		std_vec_free_Checked(&frame->next.f1, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		if (frame->after_live)
			std_vec_free_Checked(&frame->after, 1, 1);
		frame->after = frame->next.f0;
		frame->after_live = true;
	}
	frame->hold_live = false;
	host_delta_Hold_drop(&frame->hold);
	return true;
}

/* doltgres_entries_testing_index_entries_body_0 reads the index's delta's versions as their lines. */
static void
doltgres_entries_testing_index_entries_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	HostError	failed;

	if (!doltgres_entries_testing_opened(frame))
		return;
	if (!txn_state_held_VecString(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								  frame->found, doltgres_entries_testing_doltgres_test_index_entries_closure_0, frame, NULL,
								  &frame->strings, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->strings_live = true;
	frame->ok = doltgres_entries_testing_lines_datum(pg_CatalogCaps_may_raise(frame->caps), &frame->strings,
													 &frame->strings_live, "an index's entries' text holds no NUL",
													 2919, 47, frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_index_entries returns the versions
 * an index's storage's delta holds, a line for each key in key order, each
 * version newest first as what it does and, for a counted entry's copy, the
 * copy's tuple id, or "none" for a storage with no versions.
 */
bool
doltgres_entries_testing_doltgres_test_index_entries(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
													 PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .index = true,
	.datum = result, .error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_index_entries_body_0);
}

/* doltgres_entries_testing_doltgres_test_count_closure_0 is doltgres_test_count's lent call: a recount of the state's totals. */
static bool
doltgres_entries_testing_doltgres_test_count_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
													   const HostSharedHeld *held, bool *some, TxnBaseSizes *result,
													   HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;
	TreesStoreReader store = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, &frame->reading);

	return txn_fold_recount(io, TREES_STORE_READER_NODE_STORE(&store), held, &frame->order, frame->keyless, some,
							result, error);
}

/* doltgres_entries_testing_count_body_0 recounts the target's storage's totals as their text. */
static void
doltgres_entries_testing_count_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	HostError	failed;
	bool		some;
	TxnBaseSizes sizes;
	StdString	shown;

	if (!doltgres_entries_testing_shaped(frame))
		return;
	if (!txn_state_held_OptionSizes(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
									frame->found, doltgres_entries_testing_doltgres_test_count_closure_0, frame, NULL, &some,
									&sizes, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	if (some)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("true "), STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1),
		STD_FMT_LIT(" "), STD_FMT_ARG(2), STD_FMT_LIT(" "), STD_FMT_ARG(3), STD_FMT_LIT(" "), STD_FMT_ARG(4)};
		StdFmtArg	args[] = {{&sizes.entries, std_u64_Display_fmt}, {&sizes.leaf_bytes, std_u64_Display_fmt},
		{&sizes.key_bytes, std_u64_Display_fmt}, {&sizes.heap_bytes, std_u64_Display_fmt},
		{&sizes.rows, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&shown, &arguments);
	}
	else
		std_string_from_Checked(&shown, "unresolved", strlen("unresolved"));
	frame->ok = doltgres_entries_testing_text_datum(raise, &shown, "a count's text holds no NUL", 3239, 47,
													frame->datum, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_count returns a fresh count of a
 * fork table's storage's totals, as a recount makes it, in
 * doltgres_test_totals' order after "true", or "unresolved" when the count
 * met a version whose writer hasn't ended.
 */
bool
doltgres_entries_testing_doltgres_test_count(const PgCatalogCaps *caps, const PgRel *target, PgDatum *result,
											 PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = target, .catalog_io = true,
	.datum = result, .error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_count_body_0);
}

/* doltgres_entries_testing_doltgres_test_index_fold_closure_0 is doltgres_test_index_fold's lent call: the fold now, then its base's lines. */
static bool
doltgres_entries_testing_doltgres_test_index_fold_closure_0(void *captures, HostIoIo *io, const RepoRepo *repo,
															const HostSharedHeld *held, StdVec *result, HostError *error)
{
	doltgres_entries_testing_held_frame_0_t *frame = captures;
	TxnInsertPage page = doltgres_entries_page();
	bool		has_point;
	HostLsn		point;
	bool		installed;

	if (!txn_fold_point(io, &has_point, &point, error) ||
		!txn_fold_fold_now(io, repo, &frame->reading, held, &frame->order, frame->keyless, has_point, point,
						   frame->folding, &frame->warned, &page, NULL, &installed, error))
		return false;
	return doltgres_entries_testing_held_lines(frame, io, repo, held, DOLTGRES_ENTRIES_TESTING_LINES_ENTRIES, result,
											   error);
}

/* doltgres_entries_testing_index_fold_body_0 folds the index's storage now and reads its base's entries as their lines. */
static void
doltgres_entries_testing_index_fold_body_0(void *frame_)
{
	doltgres_entries_testing_held_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	HostError	failed;

	frame->io = pghost_io_catalog(frame->caps);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_recorded(frame->caps, &frame->io, doltgres_entries_testing_held_repo(frame),
								   &frame->reading, frame->heap, &frame->base, frame->error))
		return;
	frame->base_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->base.key, &frame->key_columns, frame->error))
		return;
	frame->key_columns_live = true;
	frame->keyless = frame->key_columns.len == 0;
	if (!doltgres_index_describe(raise, catalog, frame->target, frame->key_columns.ptr, frame->key_columns.len,
								 &frame->base.key, &frame->described, frame->error))
		return;
	frame->described_live = true;
	doltgres_index_Described_order(&frame->described, NULL, &frame->order);
	frame->order_live = true;
	frame->described_live = false;
	doltgres_index_Described_drop(&frame->described);
	if (!doltgres_entries_index_making(&frame->io, frame->target, &frame->found, frame->error) ||
		!pg_calls_fold_horizon(raise, frame->heap, &frame->folding, frame->error))
		return;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	if (!txn_state_held_VecString(&frame->io, doltgres_entries_testing_held_repo(frame), &frame->reading,
								  frame->found, doltgres_entries_testing_doltgres_test_index_fold_closure_0, frame, NULL,
								  &frame->strings, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->strings_live = true;
	while (frame->warned_at < frame->warned.len)
	{
		StdString	detail = ((StdString *) frame->warned.ptr)[frame->warned_at++];
		StdCString	c_detail;
		StdNulError nul;
		bool		ok;

		if (std_CString_new_String(&detail, &c_detail, &nul))
		{
			ok = pg_calls_warn_fold_root(raise, frame->target, std_CString_as_ptr(&c_detail), frame->error);
			std_CString_drop(&c_detail);
		}
		else
		{
			std_NulError_drop(&nul);
			ok = pg_calls_warn_fold_root(raise, frame->target, "", frame->error);
		}
		if (!ok)
			return;
	}
	frame->warned_live = false;
	std_vec_free_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->ok = doltgres_entries_testing_lines_datum(raise, &frame->strings, &frame->strings_live,
													 "an index's base's text holds no NUL", 3124, 47, frame->datum,
													 frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_index_fold folds an index's storage
 * now in its tree's order, as doltgres_test_fold folds a table's, and
 * returns its base's entries, a line each of its key's and value's bytes and
 * its stamps, so a case compares a folded index's base with a fresh build's.
 */
bool
doltgres_entries_testing_doltgres_test_index_fold(const PgCatalogCaps *caps, const PgRel *heap, const PgRel *index,
												  PgDatum *result, PgPgError *error)
{
	doltgres_entries_testing_held_frame_0_t frame = {.caps = caps, .target = index, .heap = heap, .datum = result,
	.error = error};

	return doltgres_entries_testing_held_run(&frame, doltgres_entries_testing_index_fold_body_0);
}

/*
 * doltgres_entries_testing_key_frame_0_t is doltgres_test_key's state across
 * its pad: its arguments, the column's key type, and the key, each while
 * live.
 */
typedef struct doltgres_entries_testing_key_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgCall *call;
	AccessKeyKeyType key_type;
	bool		key_type_live;
	StdVec		key;
	bool		key_live;
	PgDatum    *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_key_frame_0_t;

/* doltgres_entries_testing_key_body_0 encodes the call's first argument as its type's key column. */
static void
doltgres_entries_testing_key_body_0(void *frame_)
{
	doltgres_entries_testing_key_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};
	EncodingKeyOrder order;
	AccessKeyValue value = {.tag = ACCESS_KEY_VALUE_WORD};
	bool		has_value = !pg_calls_arg_null(frame->call, 0);
	uint64		word;
	uint32		type;
	bool		by_value;

	if (!pg_calls_arg_word(raise, catalog, frame->call, 1, &word, frame->error))
		return;
	order.descending = pg_word_to_bool(word);
	if (!pg_calls_arg_word(raise, catalog, frame->call, 2, &word, frame->error))
		return;
	order.nulls_first = pg_word_to_bool(word);
	if (!pg_calls_arg_type(raise, catalog, frame->call, 0, &type, frame->error) ||
		!doltgres_layout_key_type(raise, catalog, type, pg_calls_call_collation(frame->call), &frame->key_type,
								  frame->error))
		return;
	frame->key_type_live = true;
	if (has_value)
	{
		if (!pg_calls_arg_byval(raise, catalog, frame->call, 0, &by_value, frame->error))
			return;
		if (by_value)
		{
			if (!pg_calls_arg_word(raise, catalog, frame->call, 0, &value.u.word, frame->error))
				return;
		}
		else
		{
			value.tag = ACCESS_KEY_VALUE_BYTES;
			if (!pg_calls_arg_bytes(raise, catalog, frame->call, 0, &value.u.bytes.ptr, &value.u.bytes.len,
									frame->error))
				return;
		}
	}
	std_vec_new(&frame->key, 1, 1);
	frame->key_live = true;
	if (!access_key_encode(&frame->key, sorter, &frame->key_type, has_value, value, order, frame->error))
		return;
	frame->ok = pg_calls_bytea_datum(raise, frame->key.ptr, frame->key.len, frame->result, frame->error);
}

/* doltgres_entries_testing_key_pad_0 frees the key and drops the key type. */
static void
doltgres_entries_testing_key_pad_0(void *frame_)
{
	doltgres_entries_testing_key_frame_0_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->key_type_live)
		access_key_KeyType_drop(&frame->key_type);
	frame->key_live = frame->key_type_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_key returns the key column access
 * makes of the function's first argument, a value of any type or NULL,
 * descending when its second argument is true and with NULLs first when its
 * third is.
 */
bool
doltgres_entries_testing_doltgres_test_key(const PgCatalogCaps *caps, const PgCall *fcinfo, PgDatum *result,
										   PgPgError *error)
{
	doltgres_entries_testing_key_frame_0_t frame = {.caps = caps, .call = fcinfo, .result = result,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_key_body_0, doltgres_entries_testing_key_pad_0);
	doltgres_entries_testing_key_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_placed_frame_0_t is placed's state across its pad:
 * the placing, while live.
 */
typedef struct doltgres_entries_testing_placed_frame_0_t
{
	const PgCatalogCaps *caps;
	const AccessHeaptupleRowLayout *layout;
	const AccessHeaptupleSet *values;
	size_t		values_len;
	bool		external;
	AccessHeaptuplePlacing placing;
	bool		placing_live;
	AccessHeaptupleFormed *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_placed_frame_0_t;

/* doltgres_entries_testing_placed_body_0 places the row, naming each blob it asks for by its bytes' hash. */
static void
doltgres_entries_testing_placed_body_0(void *frame_)
{
	doltgres_entries_testing_placed_frame_0_t *frame = frame_;
	AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};
	AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
	AccessHeaptupleBlob blob;

	if (!access_heaptuple_RowLayout_placing(frame->layout, sorter, names, frame->values, frame->values_len, NULL, 0,
											frame->external, ACCESS_HEAPTUPLE_TOAST_TUPLE_TARGET, &frame->placing,
											frame->error))
		return;
	frame->placing_live = true;
	while (access_heaptuple_Placing_blob(&frame->placing, &blob))
	{
		StdBytes	bytes = blob.tag == ACCESS_HEAPTUPLE_BLOB_VALUE ? blob.u.value : blob.u.chunk.node;

		if (!access_heaptuple_Placing_wrote(&frame->placing, chunks_address_Address_of(bytes.ptr, bytes.len),
											chunks_trailer_Locator_Default_default(), frame->error))
			return;
	}
	frame->placing_live = false;
	frame->ok = access_heaptuple_Placing_formed(&frame->placing, frame->result, frame->error);
}

/* doltgres_entries_testing_placed_pad_0 drops the placing. */
static void
doltgres_entries_testing_placed_pad_0(void *frame_)
{
	doltgres_entries_testing_placed_frame_0_t *frame = frame_;

	if (frame->placing_live)
		access_heaptuple_Placing_drop(&frame->placing);
	frame->placing_live = false;
}

/*
 * doltgres_entries_testing_placed returns a row of values placed through no
 * store, as one whose heap tuple holds a value out of line when external says
 * so, each blob it asks for named by its bytes' hash and no locator, as a
 * check that writes nothing needs.
 */
static bool
doltgres_entries_testing_placed(const PgCatalogCaps *caps, const AccessHeaptupleRowLayout *layout,
								const AccessHeaptupleSet *values, size_t values_len, bool external,
								AccessHeaptupleFormed *result, PgPgError *error)
{
	doltgres_entries_testing_placed_frame_0_t frame = {.caps = caps, .layout = layout, .values = values,
	.values_len = values_len, .external = external, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_placed_body_0, doltgres_entries_testing_placed_pad_0);
	doltgres_entries_testing_placed_pad_0(&frame);
	return frame.ok;
}

/* doltgres_TupleVecU8VecU8U64_t is a BaseRow, a (Vec<u8>, Vec<u8>, u64): a formed row's key, tuple, and heap bytes. */
typedef struct doltgres_TupleVecU8VecU8U64_t
{
	StdVec		f0;
	StdVec		f1;
	uint64		f2;
} doltgres_TupleVecU8VecU8U64_t;

/* doltgres_entries_testing_BaseRow_drop is a BaseRow's drop: its key's and its tuple's. */
static void
doltgres_entries_testing_BaseRow_drop(void *value)
{
	doltgres_TupleVecU8VecU8U64_t *row = value;

	std_vec_free_Checked(&row->f0, 1, 1);
	std_vec_free_Checked(&row->f1, 1, 1);
}

/* doltgres_entries_testing_VecBaseRow_drop is Vec<BaseRow>'s drop. */
static void
doltgres_entries_testing_VecBaseRow_drop(void *value)
{
	std_vec_drop_Checked(value, sizeof(doltgres_TupleVecU8VecU8U64_t), _Alignof(doltgres_TupleVecU8VecU8U64_t),
						 doltgres_entries_testing_BaseRow_drop);
}

/* taken at Vec<BaseRow> and IoMutex<Vec<BaseRow>>, BASE_ROWS' lock, which testing.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS VecBaseRow
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_TupleVecU8VecU8U64_t), _Alignof(doltgres_TupleVecU8VecU8U64_t))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_testing_VecBaseRow_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecBaseRow
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(doltgres_TupleVecU8VecU8U64_t), _Alignof(doltgres_TupleVecU8VecU8U64_t))
#include "doltgres/host/lock_template.h"

/* doltgres_entries_testing_IoMutexVecBaseRow_drop is IoMutex<Vec<BaseRow>>' drop glue: its value's. */
static void
doltgres_entries_testing_IoMutexVecBaseRow_drop(void *value)
{
	doltgres_entries_testing_VecBaseRow_drop(&((HostLockIoMutexVecBaseRow *) value)->value);
}

/* DOLTGRES_ENTRIES_TESTING_BASE_ROWS_TYPE is IoMutex<Vec<BaseRow>>' size, Default, and drop. */
static const StdDefaultType DOLTGRES_ENTRIES_TESTING_BASE_ROWS_TYPE = {sizeof(HostLockIoMutexVecBaseRow),
	_Alignof(HostLockIoMutexVecBaseRow), host_lock_IoMutexVecBaseRow_Default_default,
doltgres_entries_testing_IoMutexVecBaseRow_drop};

/*
 * DOLTGRES_ENTRIES_TESTING_BASE_ROWS is BASE_ROWS: the rows the test writer
 * formed for a base it hasn't saved yet; its BackendLocal header and the
 * process's value.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecBaseRow process;
}			DOLTGRES_ENTRIES_TESTING_BASE_ROWS = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LIST is BASE_ROWS as a list. */
static const DoltgresEntriesTestingList DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LIST = {
	&DOLTGRES_ENTRIES_TESTING_BASE_ROWS.local, &DOLTGRES_ENTRIES_TESTING_BASE_ROWS_TYPE,
	&DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LOCK_TYPE, sizeof(doltgres_TupleVecU8VecU8U64_t),
_Alignof(doltgres_TupleVecU8VecU8U64_t), doltgres_entries_testing_BaseRow_drop};

/*
 * doltgres_entries_testing_row_frame_0_t is the state across their pads of
 * the entries that form a slot's row by a target table's keyed layout: their
 * arguments, the table's key and key columns, the slot's columns and values,
 * the layout, and the formed row, each while live.
 */
typedef struct doltgres_entries_testing_row_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgRel *target;
	PgSlot	   *source;
	PgIndexFetch *fetch;
	uint64		copy;
	TxnBaseKey	key;
	bool		key_live;
	/* key_columns is the Vec<(usize, Order, KeyType)> of the table's key. */
	StdVec		key_columns;
	bool		key_columns_live;
	/* columns and held are the slot's Vec<Column> and Vec<Held>. */
	StdVec		columns;
	bool		columns_live;
	StdVec		held;
	bool		held_live;
	/* values is the Vec<Set> of the slot's values. */
	StdVec		values;
	bool		values_live;
	AccessHeaptupleRowLayout layout;
	bool		layout_live;
	AccessHeaptupleFormed formed;
	bool		formed_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_row_frame_0_t;

/* doltgres_entries_testing_slot_formed forms the frame's slot's row by its target table's keyed layout. */
static bool
doltgres_entries_testing_slot_formed(doltgres_entries_testing_row_frame_0_t *frame)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	bool		ok;

	if (!doltgres_layout_catalog_key(raise, catalog, frame->target, &frame->key, frame->error))
		return false;
	frame->key_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->key_columns, frame->error))
		return false;
	frame->key_columns_live = true;
	if (!doltgres_layout_slot_row(raise, catalog, frame->target, frame->source, &frame->columns, &frame->held,
								  frame->error))
		return false;
	frame->columns_live = frame->held_live = true;
	doltgres_layout_values(frame->columns.ptr, frame->columns.len, frame->held.ptr, frame->held.len,
						   &frame->values);
	frame->values_live = true;
	frame->columns_live = frame->key_columns_live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &frame->key_columns, &frame->layout);
	frame->layout_live = true;
	ok = doltgres_entries_testing_placed(frame->caps, &frame->layout, frame->values.ptr, frame->values.len, false,
										 &frame->formed, frame->error);
	frame->layout_live = false;
	access_heaptuple_RowLayout_drop(&frame->layout);
	if (ok)
		frame->formed_live = true;
	return ok;
}

/* doltgres_entries_testing_row_pad_0 drops the formed row, the layout, the values, the slot's row, and the key. */
static void
doltgres_entries_testing_row_pad_0(void *frame_)
{
	doltgres_entries_testing_row_frame_0_t *frame = frame_;

	if (frame->formed_live)
		access_heaptuple_Formed_drop(&frame->formed);
	if (frame->layout_live)
		access_heaptuple_RowLayout_drop(&frame->layout);
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->held_live)
		doltgres_layout_VecHeld_drop(&frame->held);
	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	if (frame->key_columns_live)
		doltgres_layout_VecKeyed_drop(&frame->key_columns);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->formed_live = frame->layout_live = frame->values_live = frame->held_live = frame->columns_live = false;
	frame->key_columns_live = frame->key_live = false;
}

/* doltgres_entries_testing_base_row_body_0 forms the slot's row and keeps it for the next base save. */
static void
doltgres_entries_testing_base_row_body_0(void *frame_)
{
	doltgres_entries_testing_row_frame_0_t *frame = frame_;
	HostIoIo	io;
	doltgres_TupleVecU8VecU8U64_t row;

	if (!doltgres_entries_testing_slot_formed(frame))
		return;
	io = pghost_io(pg_CatalogCaps_may_raise(frame->caps));
	row.f0 = frame->formed.key;
	row.f1 = frame->formed.tuple;
	row.f2 = frame->formed.heap_bytes;
	std_vec_new(&frame->formed.key, 1, 1);
	std_vec_new(&frame->formed.tuple, 1, 1);
	(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LIST, &io,
										 DOLTGRES_ENTRIES_TESTING_LIST_PUSH, &row);
	frame->ok = true;
}

/*
 * doltgres_entries_testing_doltgres_test_base_row forms the row a heap slot
 * holds as a table's tree stores it, by a target table's keyed layout, for
 * the base doltgres_test_base_save saves next.
 */
bool
doltgres_entries_testing_doltgres_test_base_row(const PgCatalogCaps *caps, const PgRel *target, PgSlot *source,
												PgPgError *error)
{
	doltgres_entries_testing_row_frame_0_t frame = {.caps = caps, .target = target, .source = source,
	.error = error};

	std_landing(&frame, doltgres_entries_testing_base_row_body_0, doltgres_entries_testing_row_pad_0);
	doltgres_entries_testing_row_pad_0(&frame);
	return frame.ok;
}

/* doltgres_entries_testing_index_fetch_key_body_0 forms the slot's row and names its key to the fetch. */
static void
doltgres_entries_testing_index_fetch_key_body_0(void *frame_)
{
	doltgres_entries_testing_row_frame_0_t *frame = frame_;

	if (!doltgres_entries_testing_slot_formed(frame))
		return;
	frame->ok = pg_calls_fetch_set_key(pg_CatalogCaps_may_raise(frame->caps), frame->fetch, frame->formed.key.ptr,
									   frame->formed.key.len, frame->copy, frame->error);
}

/*
 * doltgres_entries_testing_doltgres_test_index_fetch_key names to an index
 * fetch the row key a heap slot's row has by a target table's keyed layout,
 * as the fork's index method names an entry's key, with the tuple id of the
 * copy it names on a table without a primary key, zero for none.
 */
bool
doltgres_entries_testing_doltgres_test_index_fetch_key(const PgCatalogCaps *caps, PgIndexFetch *fetch,
													   const PgRel *target, PgSlot *source, uint64 copy,
													   PgPgError *error)
{
	doltgres_entries_testing_row_frame_0_t frame = {.caps = caps, .target = target, .source = source,
	.fetch = fetch, .copy = copy, .error = error};

	std_landing(&frame, doltgres_entries_testing_index_fetch_key_body_0, doltgres_entries_testing_row_pad_0);
	doltgres_entries_testing_row_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_doltgres_test_fold folds a table's storage now,
 * as the rewrite and the database moves do, and answers whether it installed
 * a base.
 */
bool
doltgres_entries_testing_doltgres_test_fold(const PgCatalogCaps *caps, const PgRel *target, bool *result,
											PgPgError *error)
{
	return doltgres_entries_folded_now(caps, target, target, result, error);
}

/* doltgres_TupleVecU8VecStamp_t is a (Vec<u8>, Vec<Stamp>): a keyed row's tuple and its copies' stamps. */
typedef struct doltgres_TupleVecU8VecStamp_t
{
	StdVec		f0;
	StdVec		f1;
} doltgres_TupleVecU8VecStamp_t;

/* doltgres_entries_testing_TupleVecU8VecStamp_drop is a (Vec<u8>, Vec<Stamp>)'s drop. */
static void
doltgres_entries_testing_TupleVecU8VecStamp_drop(doltgres_TupleVecU8VecStamp_t *value)
{
	std_vec_free_Checked(&value->f0, 1, 1);
	std_vec_free_Checked(&value->f1, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
}

/* BTreeMap<Vec<u8>, (Vec<u8>, Vec<Stamp>)>, base_save's rows by key. */
#define STD_BTREE_SCOPE extern
#define STD_BTREE_DECLARE
#define STD_BTREE_DEFINE
#define STD_BTREE_KS VecU8
#define STD_BTREE_K StdVec
#define STD_BTREE_VS TupleVecU8VecStamp
#define STD_BTREE_V doltgres_TupleVecU8VecStamp_t
#define STD_BTREE_ITERATE
#define STD_BTREE_DROP
#define STD_BTREE_INTO_ITERATE
#define STD_BTREE_LAYOUT VEC_U8_ROW
#define STD_BTREE_CMP(a, b) std_bytes_Ord_cmp((a)->ptr, (a)->len, (b)->ptr, (b)->len)
#define STD_BTREE_KEY_DROP(k) std_vec_free_Checked((k), 1, 1)
#define STD_BTREE_VAL_DROP(v) doltgres_entries_testing_TupleVecU8VecStamp_drop(v)
#define STD_BTREE_VAL_DEFAULT(v) \
	(std_vec_new(&(v)->f0, 1, 1), std_vec_new(&(v)->f1, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)))
#define STD_BTREE_TABLES
#define STD_BTREE_TABLE_ARGS
#include "doltgres/std/btree_template.h"

/* doltgres_entries_testing_edits_drop drops a Vec<Edit>: its edits, then the vector. */
static void
doltgres_entries_testing_edits_drop(StdVec *edits)
{
	std_vec_drop_Checked(edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit), trees_mutate_Edit_drop);
}

/* doltgres_entries_testing_Published_drop is Published's drop glue, as a Vec's element takes it. */
static void
doltgres_entries_testing_Published_drop(void *published)
{
	trees_published_Published_drop(published);
}

/*
 * doltgres_entries_testing_base_tree_frame_0_t is base_tree's state across
 * its pad: its arguments, the keys and the positions of the sorted edits, the
 * edits taken from their places, the writer, and the published roots and
 * packs, each while live.
 */
typedef struct doltgres_entries_testing_base_tree_frame_0_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const RepoStorage *storage;
	TxnBaseBase *base;
	/* edits is the Vec<Edit> to write, whose taken ones are moved out. */
	StdVec		edits;
	bool		edits_live;
	TreesShape	shape;
	bool		shape_live;

	/*
	 * place is the base's tree it writes: 0 its root, 1 its tuple ids, 2 its
	 * free space.
	 */
	int			place;
	StdVec		keys;
	bool		keys_live;
	StdVec		positions;
	bool		positions_live;
	/* taken marks the edits moved out of edits. */
	StdVec		taken;
	bool		taken_live;
	StdVec		sorted;
	bool		sorted_live;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	size_t		root;
	StdVec		published;
	bool		published_live;
	StdVec		finished;
	bool		finished_live;
	StdVec		bytes;
	StdVec		addresses;
	bool		encoded_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_base_tree_frame_0_t;

/* doltgres_entries_testing_base_tree_failed makes a storage's error the entry's. */
static void
doltgres_entries_testing_base_tree_failed(doltgres_entries_testing_base_tree_frame_0_t *frame,
										  ChunksErrorError *failed)
{
	HostError	storage = txn_base_storage(failed);

	*frame->error = pghost_raised(&storage);
}

/* doltgres_entries_testing_base_tree_closure_2 is base_tree's register run: the root held at a slot and published, as its base. */
static bool
doltgres_entries_testing_base_tree_closure_2(void *captures, HostIoRegistering *registering,
											 ChunksPacksRootsRootSlots *slots, void *result, ChunksErrorError *error)
{
	doltgres_entries_testing_base_tree_frame_0_t *frame = captures;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, registering};
	ChunksPacksRootsRootSlot *slot;

	chunks_packs_roots_RootSlots_one(slots, &slot);
	if (!trees_store_BrandedWriter_hold(&frame->writer, io, slot, frame->root, error))
		return false;
	frame->writer_live = false;
	if (!trees_store_BrandedWriter_publish(&frame->writer, io, &frame->published, &frame->finished, error))
		return false;
	frame->published_live = frame->finished_live = true;
	*(TreesStoreTree *) result = trees_published_Published_base(&((const TreesPublishedPublished *)
																  frame->published.ptr)[0]);
	frame->published_live = frame->finished_live = false;
	std_vec_drop_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished),
						 doltgres_entries_testing_Published_drop);
	std_vec_free_Checked(&frame->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	return true;
}

/* doltgres_entries_testing_base_tree_closure_4 is base_tree's gathering: the base's record of the storage, at fold point 0. */
static bool
doltgres_entries_testing_base_tree_closure_4(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
											 ChunksErrorError *error)
{
	doltgres_entries_testing_base_tree_frame_0_t *frame = captures;
	RepoGathered gathered = {.tag = REPO_GATHERED_BASE};

	(void) io;
	(void) read;
	(void) error;
	gathered.u.base.storage = *frame->storage;
	std_vec_clone(&gathered.u.base.record.bytes, &frame->bytes, 1, 1);
	std_vec_clone(&gathered.u.base.record.addresses, &frame->addresses, sizeof(chunks_TupleU32Locator_t),
				  _Alignof(chunks_TupleU32Locator_t));
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	std_vec_push_Checked(result, &gathered, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/* doltgres_entries_testing_base_tree_closure_5 is base_tree's after-write, which does nothing. */
static bool
doltgres_entries_testing_base_tree_closure_5(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/*
 * doltgres_entries_testing_base_tree_body_0 sorts the edits, writes their
 * tree, publishes its root under a registration, sets it in the base's
 * record, and saves the record.
 */
static void
doltgres_entries_testing_base_tree_body_0(void *frame_)
{
	doltgres_entries_testing_base_tree_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	TreesStoreReader reader;
	TreesStoreTree tree;
	TreesStoreTree published;
	size_t		empty;

	std_vec_with_capacity_Checked(&frame->keys, frame->edits.len, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	for (size_t at = 0; at < frame->edits.len; at++)
	{
		StdBytes	key = trees_mutate_Edit_key(&((const TreesMutateEdit *) frame->edits.ptr)[at]);

		std_vec_push_Checked(&frame->keys, &key, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (!trees_sorted_positions(frame->io, frame->keys.ptr, frame->keys.len, &frame->shape.order, &frame->positions,
								&failed))
	{
		doltgres_entries_testing_base_tree_failed(frame, &failed);
		return;
	}
	frame->positions_live = true;
	std_vec_from_elem_zeroed(&frame->taken, frame->edits.len, 1, 1);
	frame->taken_live = true;
	std_vec_new(&frame->sorted, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	frame->sorted_live = true;
	for (size_t at = 0; at < frame->positions.len; at++)
	{
		size_t		position = ((const size_t *) frame->positions.ptr)[at];

		std_index_check(position, frame->taken.len, RUST_FILE, 2785, 55);
		if (((uint8 *) frame->taken.ptr)[position])
			continue;
		((uint8 *) frame->taken.ptr)[position] = 1;
		std_vec_push_Checked(&frame->sorted, &((TreesMutateEdit *) frame->edits.ptr)[position],
							 sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	}
	reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	if (!trees_store_Writer_new(frame->io, reader, 0, &frame->plain, &failed))
	{
		doltgres_entries_testing_base_tree_failed(frame, &failed);
		return;
	}
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	if (!trees_store_BrandedWriter_empty(&frame->writer, frame->io, &empty, &failed))
	{
		doltgres_entries_testing_base_tree_failed(frame, &failed);
		return;
	}
	{
		TreesStoreTreeRef root = trees_store_BrandedWriter_root(&frame->writer, empty);

		tree = trees_store_Tree_at(root.address, root.locator);
	}
	frame->sorted_live = false;
	if (!trees_store_BrandedWriter_mutate(&frame->writer, frame->io, TREES_STORE_TREE_NAMED(&tree),
										  TREES_CHUNKER_DEFAULT_TARGET, &frame->sorted, &frame->shape, &frame->root,
										  &failed) ||
		!chunks_packs_roots_RootSlots_register(frame->io, doltgres_entries_testing_base_tree_closure_2, frame, NULL,
											   &published, &failed))
	{
		doltgres_entries_testing_base_tree_failed(frame, &failed);
		return;
	}
	switch (frame->place)
	{
		case 0:
			frame->base->has_root = true;
			frame->base->root = published;
			break;
		case 1:
			frame->base->has_tuple_ids = true;
			frame->base->tuple_ids = published;
			break;
		default:
			frame->base->has_free_space = true;
			frame->base->free_space = published;
			break;
	}
	txn_base_Base_encode(frame->base, &frame->bytes, &frame->addresses);
	frame->encoded_live = true;
	if (!repo_Repo_save(frame->repo, frame->io, frame->reading, false, NULL, doltgres_entries_testing_base_tree_closure_4,
						frame, doltgres_entries_testing_base_tree_closure_5, NULL, &failed))
	{
		doltgres_entries_testing_base_tree_failed(frame, &failed);
		return;
	}
	frame->ok = true;
}

/* doltgres_entries_testing_base_tree_pad_0 drops what base_tree holds, the edits not taken, and the shape. */
static void
doltgres_entries_testing_base_tree_pad_0(void *frame_)
{
	doltgres_entries_testing_base_tree_frame_0_t *frame = frame_;

	if (frame->encoded_live)
	{
		std_vec_free_Checked(&frame->bytes, 1, 1);
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
	if (frame->finished_live)
		std_vec_free_Checked(&frame->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	if (frame->published_live)
		std_vec_drop_Checked(&frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished),
							 doltgres_entries_testing_Published_drop);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->sorted_live)
		doltgres_entries_testing_edits_drop(&frame->sorted);
	if (frame->edits_live)
	{
		for (size_t at = 0; at < frame->edits.len; at++)
			if (!frame->taken_live || !((const uint8 *) frame->taken.ptr)[at])
				trees_mutate_Edit_drop(&((TreesMutateEdit *) frame->edits.ptr)[at]);
		std_vec_free_Checked(&frame->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	}
	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, 1, 1);
	if (frame->positions_live)
		std_vec_free_Checked(&frame->positions, sizeof(size_t), _Alignof(size_t));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	frame->encoded_live = frame->finished_live = frame->published_live = frame->writer_live = false;
	frame->sorted_live = frame->edits_live = frame->taken_live = frame->positions_live = false;
	frame->keys_live = frame->shape_live = false;
}

/*
 * doltgres_entries_testing_base_tree writes a tree of edits in a shape as a
 * part of a table's base, sets it in the base's record where place names,
 * and saves the record, so that the tree is named durably before the next
 * part is written; it takes the edits and the shape. It sorts the edits and
 * writes the tree before it registers, since a comparator's comparisons run
 * user code, which never runs while a registration lasts.
 */
static bool
doltgres_entries_testing_base_tree(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
								   const RepoStorage *storage, TxnBaseBase *base, StdVec *edits, TreesShape *shape,
								   int place, PgPgError *error)
{
	doltgres_entries_testing_base_tree_frame_0_t frame = {.io = io, .repo = repo, .reading = reading,
		.storage = storage, .base = base, .edits = *edits, .edits_live = true, .shape = *shape, .shape_live = true,
	.place = place, .error = error};

	std_landing(&frame, doltgres_entries_testing_base_tree_body_0, doltgres_entries_testing_base_tree_pad_0);
	doltgres_entries_testing_base_tree_pad_0(&frame);
	return frame.ok;
}

/*
 * doltgres_entries_testing_base_save_frame_0_t is doltgres_test_base_save's
 * state across its pad: its arguments, the table's key, its key columns and
 * order, the rows taken and the one at hand, the rows by key and their
 * iterator, the tuple ids' pairs, the edits, the free space map's room, the
 * base, the session's repository, and its reader, each while live.
 */
typedef struct doltgres_entries_testing_base_save_frame_0_t
{
	const PgCatalogCaps *caps;
	const PgRel *target;
	int32		extra;
	HostIoIo	io;
	TxnBaseKey	key;
	bool		key_live;
	StdVec		columns;
	TreesKeyOrder order;
	bool		order_live;
	/* rows is the Vec<BaseRow> taken from BASE_ROWS, live from rows_at on. */
	StdVec		rows;
	bool		rows_live;
	size_t		rows_at;
	doltgres_TupleVecU8VecU8U64_t row;
	bool		row_key_live;
	bool		row_tuple_live;
	StdBTreeMap entries;
	bool		entries_live;
	StdBTreeIntoIter iter;
	bool		iter_live;

	/*
	 * pairs and edits are the tuple id tree's and the table tree's
	 * Vec<Edit>s.
	 */
	StdVec		pairs;
	bool		pairs_live;
	StdVec		edits;
	bool		edits_live;
	/* room is the free space map's edit, a Vec<Edit> of one or none. */
	StdVec		room;
	bool		room_live;
	TxnBaseBase base;
	bool		base_live;
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	int64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_base_save_frame_0_t;

/* doltgres_entries_testing_entry_edit returns the edit that puts an entry of a key and a value; it takes both. */
static TreesMutateEdit
doltgres_entries_testing_entry_edit(StdVec *key, StdVec *value)
{
	TreesMutateEdit edit = {.tag = TREES_MUTATE_EDIT_PUT};

	trees_build_Entry_new_VecU8VecU8(key, value, &edit.u.put);
	return edit;
}

/*
 * doltgres_entries_testing_base_save_rows pairs each row taken with a tuple
 * id in heap's order, and gathers the rows by key with their copies' stamps.
 */
static void
doltgres_entries_testing_base_save_rows(doltgres_entries_testing_base_save_frame_0_t *frame, uint64 writer,
										uint16 most, uint64 usable, uint64 *block, uint64 *offset, uint64 *used)
{
	while (frame->rows_at < frame->rows.len)
	{
		StdBTreeEntryVecU8TupleVecU8VecStamp entry;
		doltgres_TupleVecU8VecStamp_t *rowed;
		ChunksTrailerStamp stamp;
		TreesMutateEdit edit;
		StdVec		paired;
		StdVec		tuple_key;
		uint64		heap_bytes;
		uint64		tuple_id;
		uint8		tid_key[6];

		frame->row = ((doltgres_TupleVecU8VecU8U64_t *) frame->rows.ptr)[frame->rows_at++];
		frame->row_key_live = frame->row_tuple_live = true;
		heap_bytes = frame->row.f2;
		frame->base.sizes.heap_bytes = STD_ADD(u64, frame->base.sizes.heap_bytes, heap_bytes, 2701, 9);
		if (*offset == (uint64) most || (*offset > 0 && STD_ADD(u64, *used, heap_bytes, 2702, 56) > usable))
		{
			*block = STD_ADD(u64, *block, 1, 2703, 38);
			*offset = 0;
			*used = 0;
		}
		*offset = STD_ADD(u64, *offset, 1, 2705, 27);
		*used = STD_ADD(u64, *used, heap_bytes, 2705, 39);
		tuple_id = *block << 16 | *offset;
		txn_base_pair_value(frame->row.f0.ptr, frame->row.f0.len, (uint32) heap_bytes, &paired);
		txn_base_tuple_id_key(tuple_id, tid_key);
		std_slice_to_vec_Checked(&tuple_key, tid_key, sizeof(tid_key), 1, 1);
		edit = doltgres_entries_testing_entry_edit(&tuple_key, &paired);
		std_vec_push_Checked(&frame->pairs, &edit, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
		stamp.txn = writer;
		stamp.tuple = tuple_id;
		frame->row_key_live = false;
		entry = std_btree_BTreeMapVecU8TupleVecU8VecStamp_entry(&frame->entries, &frame->row.f0);
		if (entry.tag == STD_BTREE_ENTRY_OCCUPIED)
			rowed = std_btree_OccupiedEntryVecU8TupleVecU8VecStamp_into_mut(&entry.u.occupied);
		else
		{
			doltgres_TupleVecU8VecStamp_t inserted = {.f0 = frame->row.f1};

			frame->row_tuple_live = false;
			std_vec_new(&inserted.f1, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
			rowed = std_btree_VacantEntryVecU8TupleVecU8VecStamp_insert(&entry.u.vacant, &inserted);
		}
		std_vec_push_Checked(&rowed->f1, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		if (frame->row_tuple_live)
		{
			frame->row_tuple_live = false;
			std_vec_free_Checked(&frame->row.f1, 1, 1);
		}
	}
	frame->rows_live = false;
	std_vec_free_Checked(&frame->rows, sizeof(doltgres_TupleVecU8VecU8U64_t), _Alignof(doltgres_TupleVecU8VecU8U64_t));
}

/*
 * doltgres_entries_testing_base_save_edits makes each row by key, in key
 * order, the edit that puts it with its stamps and the extra value ids.
 */
static void
doltgres_entries_testing_base_save_edits(doltgres_entries_testing_base_save_frame_0_t *frame, bool keyless)
{
	uint32		values = (uint32) (frame->extra < 0 ? -(int64) frame->extra : frame->extra);
	StdVec		key;
	doltgres_TupleVecU8VecStamp_t rowed;

	frame->iter = std_btree_BTreeMapVecU8TupleVecU8VecStamp_IntoIterator_into_iter(&frame->entries);
	frame->entries_live = false;
	frame->iter_live = true;
	std_vec_new(&frame->edits, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	frame->edits_live = true;
	while (std_btree_IntoIterVecU8TupleVecU8VecStamp_Iterator_next(&frame->iter, &key, &rowed))
	{
		TreesMutateEdit edit;
		StdVec		value;

		if (keyless)
		{
			txn_counted_keyless_value((uint64) rowed.f1.len, rowed.f0.ptr, rowed.f0.len, &value);
			std_vec_free_Checked(&rowed.f0, 1, 1);
		}
		else
			value = rowed.f0;
		edit = doltgres_entries_testing_entry_edit(&key, &value);
		std_vec_free_Checked(&edit.u.put.stamps, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		edit.u.put.stamps = rowed.f1;
		std_vec_free_Checked(&edit.u.put.values, sizeof(uint32), _Alignof(uint32));
		std_vec_new(&edit.u.put.values, sizeof(uint32), _Alignof(uint32));
		for (uint32 id = 1; id <= values && id != 0; id++)
			std_vec_push_Checked(&edit.u.put.values, &id, sizeof(uint32), _Alignof(uint32));
		std_vec_push_Checked(&frame->edits, &edit, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	}
	frame->iter_live = false;
	std_btree_IntoIterVecU8TupleVecU8VecStamp_Drop_drop(&frame->iter);
}

/* doltgres_entries_testing_base_save_sizes counts the edits' entries, leaf bytes, and key bytes into the base's sizes. */
static void
doltgres_entries_testing_base_save_sizes(doltgres_entries_testing_base_save_frame_0_t *frame)
{
	TxnBaseSizes *sizes = &frame->base.sizes;

	for (size_t at = 0; at < frame->edits.len; at++)
	{
		const TreesMutateEdit *edit = &((const TreesMutateEdit *) frame->edits.ptr)[at];
		const TreesBuildEntry *put = &edit->u.put;

		if (edit->tag != TREES_MUTATE_EDIT_PUT)
			continue;
		sizes->entries = STD_ADD(u64, sizes->entries, 1, 2725, 13);
		sizes->leaf_bytes = STD_ADD(u64, sizes->leaf_bytes,
									trees_chunker_leaf_entry_size(put->key.ptr, put->key.len, put->value.ptr,
																  put->value.len), 2726, 13);
		sizes->key_bytes = STD_ADD(u64, sizes->key_bytes, (uint64) put->key.len, 2727, 13);
	}
}

/* doltgres_entries_testing_base_save_room puts the free space map's entry of the last page's room, when it has room. */
static void
doltgres_entries_testing_base_save_room(doltgres_entries_testing_base_save_frame_0_t *frame, uint64 count,
										uint64 page, uint64 used, uint64 block, uint64 offset, uint16 most)
{
	std_vec_new(&frame->room, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	frame->room_live = true;
	if (count > 0 && page > used && offset < (uint64) most)
	{
		TxnFreeSpaceFree free = txn_free_space_Free_new(STD_SUB(u64, page, used, 2749, 48), NULL, 0, most);
		uint8		block_key[4];
		StdVec		key;
		StdVec		value;
		TreesMutateEdit edit;

		txn_free_space_key((uint32) block, block_key);
		std_slice_to_vec_Checked(&key, block_key, sizeof(block_key), 1, 1);
		txn_free_space_Free_encode(&free, &value);
		edit = doltgres_entries_testing_entry_edit(&key, &value);
		std_vec_push_Checked(&frame->room, &edit, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	}
}

/* doltgres_entries_testing_base_save_trees writes the base's trees in turn, the table's, the tuple ids', and the free space map's. */
static bool
doltgres_entries_testing_base_save_trees(doltgres_entries_testing_base_save_frame_0_t *frame, DgStorage located)
{
	const RepoRepo *repo = STD_ARC_VALUE(frame->repo, _Alignof(RepoRepo));
	RepoStorage storage = {located.tablespace, located.relation};
	TreesShape	shape = {.order = frame->order};

	frame->order_live = frame->edits_live = false;
	if (!doltgres_entries_testing_base_tree(&frame->io, repo, &frame->reading, &storage, &frame->base, &frame->edits,
											&shape, 0, frame->error))
		return false;
	shape = trees_Shape_Default_default();
	frame->pairs_live = false;
	if (!doltgres_entries_testing_base_tree(&frame->io, repo, &frame->reading, &storage, &frame->base, &frame->pairs,
											&shape, 1, frame->error))
		return false;
	if (frame->room.len == 0)
		return true;
	shape = txn_free_space_shape();
	frame->room_live = false;
	return doltgres_entries_testing_base_tree(&frame->io, repo, &frame->reading, &storage, &frame->base, &frame->room,
											  &shape, 2, frame->error);
}

/* doltgres_entries_testing_base_save_body_0 saves the rows taken as the target table's base, a tree at a time. */
static void
doltgres_entries_testing_base_save_body_0(void *frame_)
{
	doltgres_entries_testing_base_save_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	DgStorage	located;
	uint16		most;
	uint64		page;
	uint64		usable;
	HostXid		subtransaction;
	HostXid		writer;
	HostError	failed;
	bool		keyless;
	uint64		count;
	uint64		block = 0;
	uint64		offset = 0;
	uint64		used = 0;
	uint64		next;

	if (!doltgres_layout_catalog_key(raise, catalog, frame->target, &frame->key, frame->error))
		return;
	frame->key_live = true;
	if (!doltgres_layout_key_columns(raise, catalog, &frame->key, &frame->columns, frame->error))
		return;
	keyless = frame->columns.len == 0;
	if (!doltgres_layout_key_order(raise, catalog, &frame->key, &frame->columns, &frame->order, frame->error))
		return;
	frame->order_live = true;
	located = pg_calls_rel_storage(frame->target);
	most = pg_calls_max_heap_tuples_per_page();
	page = pg_calls_page_bytes();
	usable = pg_calls_rel_usable_bytes(frame->target);
	frame->io = pghost_io_catalog(frame->caps);
	(void) doltgres_entries_testing_list(&DOLTGRES_ENTRIES_TESTING_BASE_ROWS_LIST, &frame->io,
										 DOLTGRES_ENTRIES_TESTING_LIST_TAKE_ALL, &frame->rows);
	frame->rows_live = true;
	if (!host_current_writer(HOST_IO_IO_IO_DOORS(&frame->io), &subtransaction, &writer, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	count = (uint64) frame->rows.len;
	frame->entries_live = true;
	std_vec_new(&frame->pairs, sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit));
	frame->pairs_live = true;
	frame->base.sizes.rows = count;
	doltgres_entries_testing_base_save_rows(frame, writer.f0, most, usable, &block, &offset, &used);
	doltgres_entries_testing_base_save_edits(frame, keyless);
	doltgres_entries_testing_base_save_sizes(frame);
	next = block << 16 | STD_ADD(u64, offset, 1, 2730, 31);
	frame->base.counter = next;
	frame->base.fresh_end = next;
	frame->base.table = pg_calls_rel_id(frame->target);
	frame->base.key = frame->key;
	frame->key_live = false;
	frame->base_live = true;
	doltgres_entries_testing_base_save_room(frame, count, page, used, block, offset, most);
	if (!doltgres_entries_repository(&frame->io, &frame->repo, frame->error))
		return;
	frame->repo_live = true;
	if (!host_shared_read(&frame->io, &frame->reading, &failed))
	{
		*frame->error = pghost_raised(&failed);
		return;
	}
	frame->reading_live = true;
	if (!doltgres_entries_testing_base_save_trees(frame, located))
		return;
	*frame->result = (int64) count;
	frame->ok = true;
}

/* doltgres_entries_testing_base_save_pad_0 drops what base_save holds. */
static void
doltgres_entries_testing_base_save_pad_0(void *frame_)
{
	doltgres_entries_testing_base_save_frame_0_t *frame = frame_;

	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	if (frame->repo_live)
		std_arc_drop(frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), doltgres_entries_testing_Repo_drop);
	if (frame->room_live)
		doltgres_entries_testing_edits_drop(&frame->room);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->edits_live)
		doltgres_entries_testing_edits_drop(&frame->edits);
	if (frame->iter_live)
		std_btree_IntoIterVecU8TupleVecU8VecStamp_Drop_drop(&frame->iter);
	if (frame->pairs_live)
		doltgres_entries_testing_edits_drop(&frame->pairs);
	if (frame->entries_live)
		std_btree_BTreeMapVecU8TupleVecU8VecStamp_drop(&frame->entries);
	if (frame->row_tuple_live)
		std_vec_free_Checked(&frame->row.f1, 1, 1);
	if (frame->row_key_live)
		std_vec_free_Checked(&frame->row.f0, 1, 1);
	if (frame->rows_live)
	{
		while (frame->rows_at < frame->rows.len)
			doltgres_entries_testing_BaseRow_drop(&((doltgres_TupleVecU8VecU8U64_t *) frame->rows.ptr)[frame->rows_at++]);
		std_vec_free_Checked(&frame->rows, sizeof(doltgres_TupleVecU8VecU8U64_t),
							 _Alignof(doltgres_TupleVecU8VecU8U64_t));
	}
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	if (frame->key_live)
		txn_base_Key_drop(&frame->key);
	frame->reading_live = frame->repo_live = frame->base_live = frame->room_live = frame->edits_live = false;
	frame->iter_live = frame->pairs_live = frame->entries_live = frame->row_tuple_live = false;
	frame->row_key_live = frame->rows_live = frame->order_live = frame->key_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_base_save saves the rows
 * doltgres_test_base_row formed as a table's base, before any scan of the
 * table, and returns how many rows it saved; testing.rs says how it lays
 * them out.
 */
bool
doltgres_entries_testing_doltgres_test_base_save(const PgCatalogCaps *caps, const PgRel *target, int32 extra,
												 int64 *result, PgPgError *error)
{
	doltgres_entries_testing_base_save_frame_0_t frame = {.caps = caps, .target = target, .extra = extra,
	.entries = STD_BTREE_MAP_NEW, .result = result, .error = error};

	std_landing(&frame, doltgres_entries_testing_base_save_body_0, doltgres_entries_testing_base_save_pad_0);
	doltgres_entries_testing_base_save_pad_0(&frame);
	return frame.ok;
}

/* DOLTGRES_ENTRIES_TESTING_TOAST_COMPRESSED is TOAST_COMPRESSED: slot_value_toast's state of a value heap compressed inline. */
#define DOLTGRES_ENTRIES_TESTING_TOAST_COMPRESSED 1

/* DOLTGRES_ENTRIES_TESTING_TOAST_EXTERNAL is TOAST_EXTERNAL: slot_value_toast's state of a value heap moved out of line. */
#define DOLTGRES_ENTRIES_TESTING_TOAST_EXTERNAL 2

/* doltgres_entries_testing_four_numbers returns the error of a version not given as four numbers. */
static PgPgError
doltgres_entries_testing_four_numbers(void)
{
	return doltgres_entries_testing_pg_fork("a version is four numbers");
}

/*
 * doltgres_entries_testing_given_version reads a stored row's version given
 * as its writer, its command, its deleter, and the deleter's command, apart
 * by commas, with its table and its ctid.
 */
static bool
doltgres_entries_testing_given_version(const uint8 *given, size_t given_len, uint32 table, access_TupleU32U16_t ctid,
									   AccessSlotVersion *result, PgPgError *error)
{
	StdUtf8Error utf8;
	StdParseIntError parse;
	const char *chars = (const char *) given;
	size_t		starts[4];
	size_t		ends[4];
	size_t		numbers = 0;
	size_t		start = 0;

	if (!std_str_from_utf8(given, given_len, &utf8))
	{
		*error = doltgres_entries_testing_four_numbers();
		return false;
	}
	for (size_t at = 0; at <= given_len; at++)
	{
		if (at < given_len && chars[at] != ',')
			continue;
		if (numbers < lengthof(starts))
		{
			starts[numbers] = start;
			ends[numbers] = at;
		}
		numbers++;
		start = at + 1;
	}
	if (numbers != 4 ||
		!std_str_parse_u64(chars + starts[0], ends[0] - starts[0], &result->writer, &parse) ||
		!std_str_parse_u32(chars + starts[1], ends[1] - starts[1], &result->command, &parse) ||
		!std_str_parse_u64(chars + starts[2], ends[2] - starts[2], &result->deleter, &parse) ||
		!std_str_parse_u32(chars + starts[3], ends[3] - starts[3], &result->deleter_command, &parse))
	{
		*error = doltgres_entries_testing_four_numbers();
		return false;
	}
	result->table = table;
	result->ctid = ctid;
	return true;
}

/* doltgres_TupleOptionVecU8U64_t is an (Option<Vec<u8>>, u64): a slot's value's stored bytes, if any, and its word. */
typedef struct doltgres_TupleOptionVecU8U64_t
{
	bool		has_f0;
	StdVec		f0;
	uint64		f1;
} doltgres_TupleOptionVecU8U64_t;

/* doltgres_entries_testing_Field is an Option<Vec<u8>>: a field's bytes, or none. */
typedef struct doltgres_entries_testing_Field
{
	bool		has;
	StdVec		bytes;
} doltgres_entries_testing_Field;

/*
 * doltgres_entries_testing_slot_frame_0_t is doltgres_test_slot's state
 * across its pad: its arguments, the key and its columns, the fields, their
 * toast states, the columns and held values, the key types, the tuple, the
 * keyed layout, the values, the formed row, and the layout, each while live.
 */
typedef struct doltgres_entries_testing_slot_frame_0_t
{
	const PgCatalogCaps *caps;
	PgSlot	   *slot;
	PgSlot	   *source;
	int32		keys;
	bool		descending;
	const char *version;
	StdVec		key;
	/* key_columns is a Vec<(usize, Order, KeyType)>, moved into the layout. */
	StdVec		key_columns;
	bool		key_columns_live;
	/* fields is a Vec<Option<Vec<u8>>>, toast a Vec<i32>. */
	StdVec		fields;
	StdVec		toast;
	/* columns is a Vec<Column>, moved into the layout once it is made. */
	StdVec		columns;
	bool		columns_live;
	/* held is a Vec<(Option<Vec<u8>>, u64)>. */
	StdVec		held;
	/* key_types is a Vec<KeyType>. */
	StdVec		key_types;
	bool		vecs_live;
	AccessKeyKeyType key_type;
	bool		key_type_live;
	/* field is the field of the attribute at hand, while it isn't in fields. */
	doltgres_entries_testing_Field field;
	bool		field_live;
	/* refs is the Vec<Option<&[u8]>> of the fields a tuple's write takes. */
	StdVec		refs;
	bool		refs_live;
	StdVec		tuple;
	bool		tuple_live;
	StdVec		keyed;
	bool		keyed_live;
	/* values is a Vec<Set>. */
	StdVec		values;
	bool		values_live;
	AccessHeaptupleRowLayout layout;
	bool		layout_live;
	AccessHeaptupleFormed formed;
	bool		formed_live;
	PgPgError  *error;
	bool		ok;
} doltgres_entries_testing_slot_frame_0_t;

/* doltgres_entries_testing_slot_values reads each attribute of the source slot into the frame: its column, its value, and its key column or field. */
static bool
doltgres_entries_testing_slot_values(doltgres_entries_testing_slot_frame_0_t *frame, size_t bound, size_t *short_at,
									 bool *has_short)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	EncodingKeyOrder order = {frame->descending, frame->descending};
	int32		held_natts = pg_calls_slot_tuple_natts(frame->source);
	int32		natts = pg_calls_slot_natts(frame->source);

	for (int32 i = 0; i < natts; i++)
	{
		PgAttOption of;
		AccessHeaptupleColumn column = {0};
		doltgres_TupleOptionVecU8U64_t held = {0};
		doltgres_entries_testing_Field *field = &frame->field;
		int32		toast;

		if (i == held_natts)
		{
			*has_short = true;
			*short_at = frame->fields.len;
		}
		of = pg_Att_of(pg_calls_slot_att(frame->source, i));
		if (!of.has)
		{
			*frame->error = doltgres_entries_testing_pg_fork("a slot's descriptor lost an attribute");
			return false;
		}
		column.typid = pg_Att_type_id(of.att);
		column.len = pg_Att_length(of.att);
		column.by_value = pg_Att_by_value(of.att);
		column.align = pg_Att_align(of.att);
		if (!access_heaptuple_Storage_of(pg_calls_slot_att_storage(frame->source, i), &column.storage, frame->error))
			return false;
		column.dropped = pg_Att_dropped(of.att);
		std_vec_push_Checked(&frame->columns, &column, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
		if (column.dropped)
		{
			std_vec_push_Checked(&frame->held, &held, sizeof(held), _Alignof(doltgres_TupleOptionVecU8U64_t));
			continue;
		}
		held.f1 = pg_calls_slot_value_word(frame->source, i);
		if (!pg_calls_slot_value_null(frame->source, i))
		{
			uint8		word[8];
			const uint8 *bytes;
			size_t		len;

			if (column.by_value)
			{
				doltgres_entries_testing_le_bytes(held.f1, word);
				len = (size_t) abs(column.len);
				std_range_to_check(len, sizeof(word), RUST_FILE, 2427, 18);
				bytes = word;
			}
			else if (!pg_calls_slot_value_bytes(raise, catalog, frame->source, i, &bytes, &len, frame->error))
				return false;
			std_slice_to_vec_Checked(&field->bytes, bytes, len, 1, 1);
			held.has_f0 = field->has = frame->field_live = true;
			std_vec_clone(&held.f0, &field->bytes, 1, 1);
		}
		std_vec_push_Checked(&frame->held, &held, sizeof(held), _Alignof(doltgres_TupleOptionVecU8U64_t));
		if (i < frame->keys)
		{
			AccessKeyNames names = {&DOLTGRES_LAYOUT_BLOB_NAMES_NAMES_OPS, NULL};
			AccessKeySortKeys sorter = {&ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS, frame->caps};
			AccessKeyValue value = {.tag = column.by_value ? ACCESS_KEY_VALUE_WORD : ACCESS_KEY_VALUE_BYTES};
			access_TupleUsizeOrderKeyType_t keyed = {.f0 = (size_t) i, .f1 = order};
			bool		cut;
			bool		exact;
			bool		cuttable;

			if (!doltgres_layout_key_type(raise, catalog, column.typid, pg_calls_slot_att_collation(frame->source, i),
										  &frame->key_type, frame->error))
				return false;
			frame->key_type_live = true;
			if (column.by_value)
				value.u.word = held.f1;
			else if (field->has)
			{
				value.u.bytes.ptr = field->bytes.ptr;
				value.u.bytes.len = field->bytes.len;
			}
			if (!access_key_encode_within(&frame->key, sorter, names, &frame->key_type, field->has, value, order, bound,
										  false, &cut, frame->error))
				return false;
			exact = access_key_is_exact(&frame->key_type);
			cuttable = access_key_can_be_cut(&frame->key_type, bound);
			keyed.f2 = frame->key_type;
			frame->key_type_live = false;
			std_vec_push_Checked(&frame->key_columns, &keyed, sizeof(keyed), _Alignof(access_TupleUsizeOrderKeyType_t));
			if (exact && (!cuttable || !cut))
			{
				if (field->has)
					std_vec_free_Checked(&field->bytes, 1, 1);
				field->has = frame->field_live = false;
			}
			if (exact && !cuttable)
				continue;
			if (exact && !cut)
			{
				toast = -1;
				std_vec_push_Checked(&frame->fields, field, sizeof(*field), _Alignof(doltgres_entries_testing_Field));
				std_vec_push_Checked(&frame->toast, &toast, sizeof(int32), _Alignof(int32));
				continue;
			}
		}
		toast = pg_calls_slot_value_toast(frame->source, i);
		std_vec_push_Checked(&frame->fields, field, sizeof(*field), _Alignof(doltgres_entries_testing_Field));
		field->has = frame->field_live = false;
		std_vec_push_Checked(&frame->toast, &toast, sizeof(int32), _Alignof(int32));
	}
	return true;
}

/* doltgres_entries_testing_slot_checked forms the row through the layout's placing and checks it against the test's own bytes. */
static bool
doltgres_entries_testing_slot_checked(doltgres_entries_testing_slot_frame_0_t *frame)
{
	const int32 *toast = frame->toast.ptr;
	bool		external = false;
	bool		compressed = false;
	EncodingTupleTuple parsed;

	std_vec_with_capacity_Checked(&frame->values, frame->columns.len, sizeof(AccessHeaptupleSet),
								  _Alignof(AccessHeaptupleSet));
	frame->values_live = true;
	for (size_t at = 0; at < frame->columns.len && at < frame->held.len; at++)
	{
		const AccessHeaptupleColumn *column = &((const AccessHeaptupleColumn *) frame->columns.ptr)[at];
		const doltgres_TupleOptionVecU8U64_t *held = &((const doltgres_TupleOptionVecU8U64_t *) frame->held.ptr)[at];
		AccessHeaptupleSet value = {.tag = ACCESS_HEAPTUPLE_SET_NULL};
		StdBytes	bytes = {held->f0.ptr, held->f0.len};

		if (held->has_f0 && column->by_value)
		{
			value.tag = ACCESS_HEAPTUPLE_SET_WORD;
			value.u.word = held->f1;
		}
		else if (held->has_f0 && column->len == -1)
		{
			value.tag = ACCESS_HEAPTUPLE_SET_VARLENA;
			value.u.varlena = bytes;
		}
		else if (held->has_f0)
		{
			value.tag = ACCESS_HEAPTUPLE_SET_FIXED;
			value.u.fixed = bytes;
		}
		std_vec_push_Checked(&frame->values, &value, sizeof(value), _Alignof(AccessHeaptupleSet));
	}
	for (size_t at = 0; at < frame->toast.len; at++)
	{
		external |= toast[at] == DOLTGRES_ENTRIES_TESTING_TOAST_EXTERNAL;
		compressed |= toast[at] == DOLTGRES_ENTRIES_TESTING_TOAST_COMPRESSED;
	}
	frame->columns_live = frame->key_columns_live = false;
	access_heaptuple_RowLayout_new(&frame->columns, &frame->key_columns, &frame->layout);
	frame->layout_live = true;
	if (!doltgres_entries_testing_placed(frame->caps, &frame->layout, frame->values.ptr, frame->values.len, external,
										 &frame->formed, frame->error))
		return false;
	frame->formed_live = true;
	frame->layout_live = false;
	access_heaptuple_RowLayout_drop(&frame->layout);
	if (!external && !compressed &&
		(frame->formed.key.len != frame->key.len || memcmp(frame->formed.key.ptr, frame->key.ptr, frame->key.len) != 0 ||
		 frame->formed.tuple.len != frame->tuple.len ||
		 memcmp(frame->formed.tuple.ptr, frame->tuple.ptr, frame->tuple.len) != 0))
	{
		*frame->error = doltgres_entries_testing_pg_fork("forming a row gave other bytes than the test's own steps");
		return false;
	}
	if (!encoding_tuple_Tuple_parse(frame->formed.tuple.ptr, frame->formed.tuple.len, &parsed))
	{
		*frame->error = doltgres_entries_testing_pg_fork("a placed row doesn't parse");
		return false;
	}
	if (!compressed)
		for (size_t f = 0; f < frame->toast.len; f++)
			if ((encoding_tuple_Tuple_field(&parsed, f).tag == ENCODING_TUPLE_FIELD_EXTERNAL) !=
				(toast[f] == DOLTGRES_ENTRIES_TESTING_TOAST_EXTERNAL))
			{
				*frame->error = doltgres_entries_testing_pg_fork("a row's values out of line differ from heap's");
				return false;
			}
	return true;
}

/* doltgres_entries_testing_slot_body_0 stores the source slot's row in the slot as its table's tree would store it. */
static void
doltgres_entries_testing_slot_body_0(void *frame_)
{
	doltgres_entries_testing_slot_frame_0_t *frame = frame_;
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(frame->caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(frame->caps);
	int32		keyed_natts = Min(frame->keys, pg_calls_slot_natts(frame->source));
	AccessKeyKeyTypes key_types;
	AccessSlotVersion version;
	AccessSlotOutside outside = {0};
	access_TupleU32U16_t ctid;
	uint32		table;
	size_t		bound;
	size_t		short_at = 0;
	bool		has_short = false;

	std_vec_new(&frame->key, 1, 1);
	std_vec_new(&frame->key_columns, sizeof(access_TupleUsizeOrderKeyType_t), _Alignof(access_TupleUsizeOrderKeyType_t));
	std_vec_new(&frame->fields, sizeof(doltgres_entries_testing_Field), _Alignof(doltgres_entries_testing_Field));
	std_vec_new(&frame->toast, sizeof(int32), _Alignof(int32));
	std_vec_new(&frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
	std_vec_new(&frame->held, sizeof(doltgres_TupleOptionVecU8U64_t), _Alignof(doltgres_TupleOptionVecU8U64_t));
	std_vec_new(&frame->key_types, sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType));
	frame->vecs_live = frame->key_columns_live = frame->columns_live = true;
	for (int32 i = 0; i < keyed_natts; i++)
	{
		PgAttOption of = pg_Att_of(pg_calls_slot_att(frame->source, i));

		if (!of.has || pg_Att_dropped(of.att))
			continue;
		if (!doltgres_layout_key_type(raise, catalog, pg_Att_type_id(of.att),
									  pg_calls_slot_att_collation(frame->source, i), &frame->key_type, frame->error))
			return;
		std_vec_push_Checked(&frame->key_types, &frame->key_type, sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType));
	}
	key_types.items = frame->key_types.ptr;
	key_types.len = frame->key_types.len;
	key_types.stride = sizeof(AccessKeyKeyType);
	key_types.offset = 0;
	bound = access_key_table_bound(key_types);
	if (!doltgres_entries_testing_slot_values(frame, bound, &short_at, &has_short))
		return;
	if (has_short)
	{
		while (frame->fields.len > short_at)
		{
			doltgres_entries_testing_Field *field = &((doltgres_entries_testing_Field *) frame->fields.ptr)[--frame->fields.len];

			if (field->has)
				std_vec_free_Checked(&field->bytes, 1, 1);
		}
	}
	{
		encoding_tuple_write_overflow_t none = {0};

		std_vec_with_capacity_Checked(&frame->refs, frame->fields.len, sizeof(StdBytes), _Alignof(StdBytes));
		frame->refs_live = true;
		for (size_t at = 0; at < frame->fields.len; at++)
		{
			const doltgres_entries_testing_Field *field = &((const doltgres_entries_testing_Field *) frame->fields.ptr)[at];
			StdBytes	bytes = {field->has ? field->bytes.ptr : NULL, field->has ? field->bytes.len : 0};

			std_vec_push_Checked(&frame->refs, &bytes, sizeof(StdBytes), _Alignof(StdBytes));
		}
		encoding_tuple_write(frame->refs.ptr, frame->refs.len, false, none, &frame->tuple);
		frame->tuple_live = true;
		frame->refs_live = false;
		std_vec_free_Checked(&frame->refs, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->key_columns.len == 0)
	{
		ChunksAddressAddress address = chunks_address_Address_of(frame->tuple.ptr, frame->tuple.len);

		std_range_to_check(ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN, sizeof(address.f0), RUST_FILE, 2461, 32);
		for (size_t at = 0; at < ACCESS_HEAPTUPLE_KEYLESS_KEY_LEN; at++)
			std_vec_push_Checked(&frame->key, &address.f0[at], 1, 1);
	}
	access_slot_keyed(frame->key_columns.ptr, frame->key_columns.len, &frame->keyed);
	frame->keyed_live = true;
	if (!has_short && !doltgres_entries_testing_slot_checked(frame))
		return;
	access_slot_identity(frame->source, &table, &ctid);
	if (frame->version[0] == '\0')
	{
		version.writer = pg_calls_slot_tuple_xmin(frame->source);
		version.command = pg_calls_slot_tuple_cmin(frame->source);
		if (!pg_calls_slot_tuple_xmax(raise, frame->source, &version.deleter, frame->error) ||
			!pg_calls_slot_tuple_cmax(raise, frame->source, &version.deleter_command, frame->error))
			return;
		version.table = table;
		version.ctid = ctid;
	}
	else if (!doltgres_entries_testing_given_version((const uint8 *) frame->version, strlen(frame->version), table,
													 ctid, &version, frame->error))
		return;
	frame->ok = access_slot_store(raise, frame->slot, frame->keyed.ptr, frame->keyed.len, frame->key.ptr,
								  frame->key.len, frame->tuple.ptr, frame->tuple.len, outside, NULL, 0, &version,
								  frame->error);
}

/* doltgres_entries_testing_slot_pad_0 drops what doltgres_test_slot holds. */
static void
doltgres_entries_testing_slot_pad_0(void *frame_)
{
	doltgres_entries_testing_slot_frame_0_t *frame = frame_;

	if (frame->formed_live)
		access_heaptuple_Formed_drop(&frame->formed);
	if (frame->layout_live)
		access_heaptuple_RowLayout_drop(&frame->layout);
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, sizeof(AccessHeaptupleSet), _Alignof(AccessHeaptupleSet));
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, 1, 1);
	if (frame->tuple_live)
		std_vec_free_Checked(&frame->tuple, 1, 1);
	if (frame->refs_live)
		std_vec_free_Checked(&frame->refs, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->field_live)
		std_vec_free_Checked(&frame->field.bytes, 1, 1);
	if (frame->key_type_live)
		access_key_KeyType_drop(&frame->key_type);
	if (frame->vecs_live)
	{
		for (size_t at = 0; at < frame->key_types.len; at++)
			access_key_KeyType_drop(&((AccessKeyKeyType *) frame->key_types.ptr)[at]);
		std_vec_free_Checked(&frame->key_types, sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType));
		for (size_t at = 0; at < frame->held.len; at++)
			if (((doltgres_TupleOptionVecU8U64_t *) frame->held.ptr)[at].has_f0)
				std_vec_free_Checked(&((doltgres_TupleOptionVecU8U64_t *) frame->held.ptr)[at].f0, 1, 1);
		std_vec_free_Checked(&frame->held, sizeof(doltgres_TupleOptionVecU8U64_t),
							 _Alignof(doltgres_TupleOptionVecU8U64_t));
		if (frame->columns_live)
			std_vec_free_Checked(&frame->columns, sizeof(AccessHeaptupleColumn), _Alignof(AccessHeaptupleColumn));
		std_vec_free_Checked(&frame->toast, sizeof(int32), _Alignof(int32));
		for (size_t at = 0; at < frame->fields.len; at++)
			if (((doltgres_entries_testing_Field *) frame->fields.ptr)[at].has)
				std_vec_free_Checked(&((doltgres_entries_testing_Field *) frame->fields.ptr)[at].bytes, 1, 1);
		std_vec_free_Checked(&frame->fields, sizeof(doltgres_entries_testing_Field),
							 _Alignof(doltgres_entries_testing_Field));
		if (frame->key_columns_live)
			doltgres_layout_VecKeyed_drop(&frame->key_columns);
		std_vec_free_Checked(&frame->key, 1, 1);
	}
	frame->formed_live = frame->layout_live = frame->values_live = frame->keyed_live = frame->tuple_live = false;
	frame->refs_live = frame->field_live = frame->key_type_live = frame->vecs_live = false;
	frame->columns_live = frame->key_columns_live = false;
}

/*
 * doltgres_entries_testing_doltgres_test_slot stores in a slot of the fork's
 * kind the row another slot holds, as its table's tree would store it, by
 * the test's own steps; testing.rs says how it keys and checks the row. The
 * row's version is the heap tuple's own header, or the one given as its four
 * numbers apart by commas.
 */
bool
doltgres_entries_testing_doltgres_test_slot(const PgCatalogCaps *caps, PgSlot *slot, PgSlot *source, int32 keys,
											bool descending, const char *version, PgPgError *error)
{
	doltgres_entries_testing_slot_frame_0_t frame = {.caps = caps, .slot = slot, .source = source, .keys = keys,
	.descending = descending, .version = version, .error = error};

	std_landing(&frame, doltgres_entries_testing_slot_body_0, doltgres_entries_testing_slot_pad_0);
	doltgres_entries_testing_slot_pad_0(&frame);
	return frame.ok;
}

#endif							/* USE_INJECTION_POINTS */

#endif							/* FRONTEND */
