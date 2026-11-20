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
 * The free space of a directory's volume, the one operating system call the
 * fork makes outside Postgres' port layer, which has none for it (PLAN's
 * decisions, 2026-11-10): statvfs on Unix and GetDiskFreeSpaceExA on Windows,
 * each answering the bytes this process may still write.
 */

#include "postgres.h"

#ifndef WIN32
#include <sys/statvfs.h>
#endif

#include "doltgres/disk_space.h"

/*
 * doltgres_disk_space stores the bytes a directory's volume holds and those of
 * them still free to this process, raising an error when the system can't say.
 */
void
doltgres_disk_space(const char *path, uint64 *available, uint64 *size)
{
#ifdef WIN32
	ULARGE_INTEGER free_bytes;
	ULARGE_INTEGER total_bytes;

	if (!GetDiskFreeSpaceExA(path, &free_bytes, &total_bytes, NULL))
	{
		_dosmaperr(GetLastError());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get the free space of directory \"%s\": %m", path)));
	}
	*available = free_bytes.QuadPart;
	*size = total_bytes.QuadPart;
#else
	struct statvfs volume;

	if (statvfs(path, &volume) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get the free space of directory \"%s\": %m", path)));
	*available = (uint64) volume.f_bavail * volume.f_frsize;
	*size = (uint64) volume.f_blocks * volume.f_frsize;
#endif
}
