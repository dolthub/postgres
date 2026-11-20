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
 * pg_checkversioned's check: the frontend entry that checks a stopped
 * server's data directory through fehost's reads and store's checks, and
 * hands each line and WAL range to the program's main.
 */
#include "postgres_fe.h"

#include "doltgres/chunks/file.h"
#include "doltgres/fehost/lib.h"
#include "doltgres/fsck/lib.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/fsck.h"

/* fsck_entries_reported hands the main a check's lines of a kind, each with its NULs taken out and one after it. */
static void
fsck_entries_reported(const PgFrontendLine *line, int32 kind, const StdVec *lines)
{
	for (size_t k = 0, k_len = lines->len; k < k_len; k++)
	{
		const StdString *said = &STD_VEC_AT(lines, StdString, k);
		char	   *bytes = palloc(said->len + 1);
		size_t		len = 0;

		for (size_t at = 0; at < said->len; at++)
			if (((const char *) said->ptr)[at] != '\0')
				bytes[len++] = ((const char *) said->ptr)[at];
		bytes[len] = '\0';
		pg_frontend_Line_report(line, kind, bytes);
		pfree(bytes);
	}
}

/*
 * fsck_entries_doltgres_fsck checks the data directory, reporting each line
 * and range, or fails with a message: a data directory's path that isn't
 * UTF-8, or a host already installed.
 */
bool
fsck_entries_doltgres_fsck(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
						   StdString *error)
{
	static const char not_utf8[] = "the data directory's path is not UTF-8";
	size_t		datadir_len = strlen(datadir);
	StdUtf8Error invalid;
	HostIoReadIo io;
	ChunksFileInterruptible interruptible;
	StoreFsckFsck fsck;

	if (!std_str_from_utf8((const uint8 *) datadir, datadir_len, &invalid))
	{
		std_string_from_Checked(error, not_utf8, strlen(not_utf8));
		return false;
	}
	if (!fehost_install(datadir, datadir_len, &io, error))
		return false;
	interruptible.ops = &CHUNKS_FILE_READ_IO_INTERRUPTIBLE_OPS;
	interruptible.self = &io;
	store_fsck_fsck(interruptible, &fsck);
	fsck_entries_reported(line, FSCK_ENTRIES_FOUND, &fsck.found);
	fsck_entries_reported(line, FSCK_ENTRIES_SETTLED, &fsck.settled);
	fsck_entries_reported(line, FSCK_ENTRIES_NOTE, &fsck.notes);
	for (size_t k = 0, k_len = fsck.kept.len; k < k_len; k++)
	{
		const StoreFsckKeptWal *range = &STD_VEC_AT(&fsck.kept, StoreFsckKeptWal, k);

		pg_frontend_Kept_hand(kept, range->repository, range->from, range->oldest, range->found);
	}
	store_fsck_Fsck_drop(&fsck);
	return true;
}
