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
 * The frontend test binaries' std::fs, std::env::temp_dir, and
 * std::process::id (the port text's section 7), over Postgres' frontend port
 * layer, since simc, which gives the backend tests' std::fs, is the
 * backend's Rust: the forms the frontend configuration's tests call, each
 * with its case. An error is the port layer's errno, as an io::Error of that
 * raw code, where std's on Windows is GetLastError's.
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/io.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fe.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/std/vec.h"

#define RUST_FILE __FILE__

/* std_fe_fs_failed sets an error to the port layer's errno, as an io::Error of that raw code, and reports failure. */
static bool
std_fe_fs_failed(StdIoError *error)
{
	*error = std_io_Error_from_raw_os_error(errno);
	return false;
}

/* std_fe_fs_c_path returns a path's bytes with a NUL after them, as the port layer takes a path. */
static char *
std_fe_fs_c_path(const char *path, size_t path_len)
{
	return pnstrdup(path, path_len);
}

/* std_fs_write is fs::write: a file made or emptied, and the bytes written to it. */
bool
std_fs_write(const char *path, size_t path_len, const uint8 *contents, size_t contents_len, StdIoError *error)
{
	char	   *c_path = std_fe_fs_c_path(path, path_len);
	int			fd = open(c_path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, pg_file_create_mode);
	size_t		done = 0;
	bool		ok = fd >= 0;

	pfree(c_path);
	if (!ok)
		return std_fe_fs_failed(error);
	while (ok && done < contents_len)
	{
		ssize_t		written = write(fd, contents + done, contents_len - done);

		if (written < 0)
			ok = std_fe_fs_failed(error);
		else
			done += (size_t) written;
	}
	if (close(fd) != 0 && ok)
		ok = std_fe_fs_failed(error);
	return ok;
}

/* std_fs_create_dir_all is fs::create_dir_all: a directory and each parent it lacks made, one that stands made again without an error. */
bool
std_fs_create_dir_all(const char *path, size_t path_len, StdIoError *error)
{
	char	   *c_path = std_fe_fs_c_path(path, path_len);
	bool		ok;

	canonicalize_path(c_path);
	ok = pg_mkdir_p(c_path, pg_dir_create_mode) == 0;
	pfree(c_path);
	return ok || std_fe_fs_failed(error);
}

/* std_fe_fs_removed removes a directory with what it holds, reporting the first failure's errno. */
static bool
std_fe_fs_removed(const char *path)
{
	DIR		   *dir = opendir(path);
	struct dirent *entry;
	bool		ok = dir != NULL;

	while (ok && (errno = 0, entry = readdir(dir)) != NULL)
	{
		char	   *inside;
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		inside = psprintf("%s/%s", path, entry->d_name);
		if (lstat(inside, &st) != 0)
			ok = false;
		else if (S_ISDIR(st.st_mode))
			ok = std_fe_fs_removed(inside);
		else
			ok = unlink(inside) == 0;
		pfree(inside);
	}
	if (ok && errno != 0)
		ok = false;
	if (dir != NULL)
	{
		int			saved = errno;

		closedir(dir);
		errno = saved;
	}
	return ok && rmdir(path) == 0;
}

/* std_fs_remove_dir_all is fs::remove_dir_all: a directory removed with the files and directories in it. */
bool
std_fs_remove_dir_all(const char *path, size_t path_len, StdIoError *error)
{
	char	   *c_path = std_fe_fs_c_path(path, path_len);
	bool		ok = std_fe_fs_removed(c_path);

	pfree(c_path);
	return ok || std_fe_fs_failed(error);
}

/*
 * std_env_temp_dir is env::temp_dir: on Windows GetTempPathW's directory, a
 * separator after it, made UTF-8, and elsewhere TMPDIR, or /tmp when it is
 * unset.
 */
StdPathBuf
std_env_temp_dir(void)
{
	StdPathBuf	dir;

#ifdef WIN32
	WCHAR		wide[MAX_PATH + 1];
	DWORD		len = GetTempPathW(lengthof(wide), wide);
	int			bytes = WideCharToMultiByte(CP_UTF8, 0, wide, (int) len, NULL, 0, NULL, NULL);

	std_vec_u8_from_elem(&dir.bytes, 0, (size_t) Max(bytes, 0));
	(void) WideCharToMultiByte(CP_UTF8, 0, wide, (int) len, dir.bytes.ptr, bytes, NULL, NULL);
#else
	const char *tmpdir = getenv("TMPDIR");
	const char *chosen = tmpdir != NULL ? tmpdir : "/tmp";

	std_slice_to_vec_Checked(&dir.bytes, chosen, strlen(chosen), 1, 1);
#endif
	return dir;
}

/* std_process_id is process::id: the process's id. */
uint32
std_process_id(void)
{
	return (uint32) getpid();
}

/* test_std_fe_fs_tests_a_directory_is_made_written_in_and_removed: a temp directory's folder is made with its parent, a file is written in it, and the folder is removed with the file. */
void
test_std_fe_fs_tests_a_directory_is_made_written_in_and_removed(void)
{
	StdPathBuf	temp = std_env_temp_dir();
	char	   *nested = psprintf("%.*s/doltgres-fe-fs-%u/a", (int) temp.bytes.len, (const char *) temp.bytes.ptr,
								  std_process_id());
	char	   *top = pnstrdup(nested, strlen(nested) - 2);
	char	   *file = psprintf("%s/held", nested);
	StdIoError	error;

	STD_ASSERT(__LINE__, 5, std_fs_create_dir_all(nested, strlen(nested), &error), "the directory is made");
	STD_ASSERT(__LINE__, 5, std_fs_create_dir_all(nested, strlen(nested), &error), "a directory is made again");
	STD_ASSERT(__LINE__, 5, std_fs_write(file, strlen(file), (const uint8 *) "x", 1, &error), "a file is written");
	STD_ASSERT(__LINE__, 5, std_path_Path_exists(file, strlen(file)), "the file stands");
	STD_ASSERT(__LINE__, 5, std_fs_remove_dir_all(top, strlen(top), &error), "the folder is removed");
	STD_ASSERT(__LINE__, 5, !std_path_Path_exists(top, strlen(top)), "the folder is gone");
	STD_ASSERT(__LINE__, 5, !std_fs_remove_dir_all(top, strlen(top), &error) &&
			   std_io_Error_kind(&error) == STD_ERROR_KIND_NOT_FOUND, "a folder that is gone fails to be removed");
	pfree(file);
	pfree(top);
	pfree(nested);
	std_vec_free_Checked(&temp.bytes, 1, 1);
}

/* std_path_Path_exists is Path::exists: whether a stat of the path succeeds. */
bool
std_path_Path_exists(const char *path, size_t path_len)
{
	char	   *c_path = std_fe_fs_c_path(path, path_len);
	struct stat st;
	bool		there = stat(c_path, &st) == 0;

	pfree(c_path);
	return there;
}
