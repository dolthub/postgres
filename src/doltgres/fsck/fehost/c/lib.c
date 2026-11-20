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
 * A frontend program's host: host's reads over Postgres' port layer, through
 * pg's frontend wrappers, for a data directory a program was given, with
 * each error worded as Postgres' frontend programs word it. It gives the
 * reads alone, so nothing a frontend program calls can write.
 */
#include "postgres_fe.h"

#include "doltgres/fehost/lib.h"
#include "doltgres/host/lib.h"
#include "doltgres/pg/frontend.h"
#include "doltgres/pg/zstd.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/any.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"
#include "fe_pg_calls.h"

#define RUST_FILE "fsck" STD_SEP "fehost" STD_SEP "src" STD_SEP "lib.rs"

/* FehostFeHost is FeHost: the frontend program's host, over the data directory it was given, and the name of a tablespace's version directory. */
typedef struct FehostFeHost
{
	StdPathBuf	datadir;
	StdString	version;
} FehostFeHost;

/* fehost_opened_t is an entry of the files the host opened: a file's number and its path, for its errors. */
typedef struct fehost_opened_t
{
	int32		file;
	StdPathBuf	path;
} fehost_opened_t;

/*
 * FEHOST_OPENED is OPENED: the path of each file the host opened, by its
 * number, a BTreeMap<i32, PathBuf> in number order; a close leaves its entry,
 * which the next open of that number replaces. The frontend's one thread is
 * its only user, so it takes no lock.
 */
static StdVec FEHOST_OPENED = {NULL, 0, 0};

/* FEHOST_HOST is HOST: the process's host, made once by install. */
static FehostFeHost FEHOST_HOST;

/* FEHOST_MADE is whether install made FEHOST_HOST, the OnceLock's state. */
static bool FEHOST_MADE;

/* fehost_failed returns a host error of a file's access with Postgres' wording, and the SQLSTATE errcode_for_file_access gives the errno; it takes what. */
static HostError
fehost_failed(PgFrontendFeError error, StdString *what)
{
	int64		missing = 0;
	PgFrontendFeError ignored;
	StdString	told;
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
	StdFmtArg	args[2];
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!pg_frontend_calls_is_missing(error.f0, &missing, &ignored))
		missing = 0;
	pg_frontend_FeError_text(error, &told);
	args[0].value = what;
	args[0].fmt = std_String_Display_fmt;
	args[1].value = &told;
	args[1].fmt = std_String_Display_fmt;
	std_fmt_format_Checked(&message, &arguments);
	std_String_drop(&told);
	std_String_drop(what);
	return host_Error_fork((const uint8 *) (missing == 1 ? "58P01" : "58030"), &message);
}

/* fehost_said returns a message of a text with a path in it, as format! with the path's display makes it. */
static StdString
fehost_said(const char *before, const char *path, size_t path_len, const char *after)
{
	StdString	said;
	StdStr		shown = {path, path_len};
	StdStr		head = {before, strlen(before)};
	StdStr		tail = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&head, std_str_Display_fmt}, {&shown, std_path_Display_fmt}, {&tail, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&said, &arguments);
	return said;
}

/* fehost_fork returns the fork's error of a SQLSTATE and a text. */
static HostError
fehost_fork(const char *code, const char *line)
{
	StdString	message;

	std_string_from_Checked(&message, line, strlen(line));
	return host_Error_fork((const uint8 *) code, &message);
}

/* fehost_c_path returns a path as Postgres takes it, refusing one with a NUL in it. */
static bool
fehost_c_path(const char *path, size_t path_len, StdCString *result, HostError *error)
{
	StdNulError nul;
	StdString	message;

	if (std_CString_new_bytes((const uint8 *) path, path_len, result, &nul))
		return true;
	std_vec_free_Checked(&nul.bytes, 1, 1);
	message = fehost_said("path \"", path, path_len, "\" holds a NUL byte");
	*error = host_Error_fork((const uint8 *) "22023", &message);
	return false;
}

/* fehost_FeHost_path returns the path of an open file, for its errors, an empty one for a number never opened. */
static StdPathBuf
fehost_FeHost_path(int32 file)
{
	StdPathBuf	path = {0};

	for (size_t k = 0, k_len = FEHOST_OPENED.len; k < k_len; k++)
		if (STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, k).file == file)
		{
			std_vec_clone(&path.bytes, &STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, k).path.bytes, 1, 1);
			return path;
		}
	std_vec_new(&path.bytes, 1, 1);
	return path;
}

/* fehost_opened_insert records an open file's path by its number, in place of an earlier file's of the number. */
static void
fehost_opened_insert(int32 file, const char *path, size_t path_len)
{
	fehost_opened_t entry = {.file = file};
	size_t		at = 0;

	std_slice_to_vec_Checked(&entry.path.bytes, path, path_len, 1, 1);
	while (at < FEHOST_OPENED.len && STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, at).file < file)
		at++;
	if (at < FEHOST_OPENED.len && STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, at).file == file)
	{
		std_vec_free_Checked(&STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, at).path.bytes, 1, 1);
		STD_VEC_AT(&FEHOST_OPENED, fehost_opened_t, at) = entry;
		return;
	}
	if (FEHOST_OPENED.ptr == NULL)
		std_vec_new(&FEHOST_OPENED, sizeof(fehost_opened_t), _Alignof(fehost_opened_t));
	std_vec_insert(&FEHOST_OPENED, at, &entry, sizeof(entry), _Alignof(fehost_opened_t), RUST_FILE, 0, 0);
}

/* fehost_FeHost_opened opens a file to read, or answers nothing when if_there is set and no file has the path. */
static bool
fehost_FeHost_opened(const char *path, size_t path_len, bool if_there, bool *has_result, int32 *result,
					 HostError *error)
{
	StdCString	c_path;
	PgFrontendFeError failed;
	PgFrontendFeError ignored;
	int64		file;
	int64		missing = 0;
	bool		opened;

	*has_result = false;
	if (!fehost_c_path(path, path_len, &c_path, error))
		return false;
	opened = pg_frontend_calls_open(std_CString_as_ptr(&c_path), &file, &failed);
	std_CString_drop(&c_path);
	if (opened)
	{
		if (file < PG_INT32_MIN || file > PG_INT32_MAX)
		{
			*error = fehost_fork("XX000", "a file's number is out of range");
			return false;
		}
		fehost_opened_insert((int32) file, path, path_len);
		*has_result = true;
		*result = (int32) file;
		return true;
	}
	if (if_there && pg_frontend_calls_is_missing(failed.f0, &missing, &ignored) && missing == 1)
		return true;
	{
		StdString	what = fehost_said("could not open file \"", path, path_len, "\"");

		*error = fehost_failed(failed, &what);
	}
	return false;
}

/* fehost_FeHost_ReadHost_open_to_read opens a file to read. */
static bool
fehost_FeHost_ReadHost_open_to_read(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
									int32 *result, HostError *error)
{
	bool		has;

	(void) self;
	(void) raise;
	if (!fehost_FeHost_opened(path, path_len, false, &has, result, error))
		return false;
	if (!has)
	{
		*error = fehost_fork("XX000", "an open found no file");
		return false;
	}
	return true;
}

/* fehost_FeHost_ReadHost_open_if_there opens a file to read, or answers nothing when no file has the path. */
static bool
fehost_FeHost_ReadHost_open_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
									 bool *has_result, int32 *result, HostError *error)
{
	(void) self;
	(void) raise;
	return fehost_FeHost_opened(path, path_len, true, has_result, result, error);
}

/* fehost_file_said returns a message of a text with an open file's path in it. */
static StdString
fehost_file_said(const char *before, int32 file, const char *after)
{
	StdPathBuf	path = fehost_FeHost_path(file);
	StdString	said = fehost_said(before, path.bytes.ptr, path.bytes.len, after);

	std_vec_free_Checked(&path.bytes, 1, 1);
	return said;
}

/* fehost_FeHost_ReadHost_read_at reads a buffer's length of a file's bytes from an offset, failing on a file that ends first. */
static bool
fehost_FeHost_ReadHost_read_at(void *self, const HostMayRaise *raise, int32 file, uint64 offset, uint8 *buffer,
							   size_t buffer_len, HostError *error)
{
	size_t		done = 0;

	(void) self;
	(void) raise;
	while (done < buffer_len)
	{
		uint64		at = STD_ADD(u64, offset, (uint64) done, 108, 22);
		PgFrontendFeError failed;
		int64		read;

		if (!pg_frontend_calls_pread(file, buffer + done, buffer_len - done, at, &read, &failed))
		{
			StdString	what = fehost_file_said("could not read file \"", file, "\"");

			*error = fehost_failed(failed, &what);
			return false;
		}
		if (read == 0)
		{
			StdPathBuf	path = fehost_FeHost_path(file);
			StdStr		shown = {path.bytes.ptr, path.bytes.len};
			StdString	message;
			StdFmtPiece pieces[] = {STD_FMT_LIT("could not read file \""), STD_FMT_ARG(0), STD_FMT_LIT("\": read "),
			STD_FMT_ARG(1), STD_FMT_LIT(" of "), STD_FMT_ARG(2)};
			StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&done, std_usize_Display_fmt},
			{&buffer_len, std_usize_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&message, &arguments);
			std_vec_free_Checked(&path.bytes, 1, 1);
			*error = host_Error_fork((const uint8 *) "08P01", &message);
			return false;
		}
		done = STD_ADD(usize, done, read < 0 ? 0 : (size_t) read, 118, 13);
	}
	return true;
}

/* fehost_FeHost_ReadHost_size returns a file's size. */
static bool
fehost_FeHost_ReadHost_size(void *self, const HostMayRaise *raise, int32 file, uint64 *result, HostError *error)
{
	PgFrontendFeError failed;
	int64		size;

	(void) self;
	(void) raise;
	if (!pg_frontend_calls_size(file, &size, &failed))
	{
		StdString	what = fehost_file_said("could not stat file \"", file, "\"");

		*error = fehost_failed(failed, &what);
		return false;
	}
	*result = size < 0 ? 0 : (uint64) size;
	return true;
}

/* fehost_FeHost_ReadHost_prefetch does nothing: a frontend program reads in order. */
static void
fehost_FeHost_ReadHost_prefetch(void *self, int32 file, uint64 offset, uint64 len)
{
	(void) self;
	(void) file;
	(void) offset;
	(void) len;
}

/* fehost_FeHost_ReadHost_close closes a file, its failure dropped. */
static void
fehost_FeHost_ReadHost_close(void *self, int32 file)
{
	PgFrontendFeError failed;
	int64		closed;

	(void) self;
	(void) pg_frontend_calls_close(file, &closed, &failed);
}

/* fehost_FeHost_ReadHost_tablespace_root returns the root of a tablespace's files: the data directory for pg_default, and otherwise its link's version directory. */
static StdPathBuf
fehost_FeHost_ReadHost_tablespace_root(void *self, uint32 tablespace)
{
	const FehostFeHost *host = self;
	StdPathBuf	root;
	StdPathBuf	links;
	StdPathBuf	space;
	StdString	number;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&tablespace, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (tablespace == HOST_PG_DEFAULT)
	{
		std_vec_clone(&root.bytes, &host->datadir.bytes, 1, 1);
		return root;
	}
	std_fmt_format_Checked(&number, &arguments);
	std_path_join(host->datadir.bytes.ptr, host->datadir.bytes.len, "pg_tblspc", 9, &links);
	std_path_join(links.bytes.ptr, links.bytes.len, number.ptr, number.len, &space);
	std_path_join(space.bytes.ptr, space.bytes.len, host->version.ptr, host->version.len, &root);
	std_vec_free_Checked(&space.bytes, 1, 1);
	std_vec_free_Checked(&links.bytes, 1, 1);
	std_String_drop(&number);
	return root;
}

/* fehost_String_PartialOrd_lt_FnMut_call_mut is the fn item String's PartialOrd::lt as the stable sort's is_less. */
static bool
fehost_String_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	(void) captures;
	return std_String_PartialOrd_lt(a, b);
}

/* The stable sort of a listing's names, Vec<String>::sort: core's shared sort parts and sort at String. */
#define STD_SORT_SCOPE static
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_TS String
#define STD_SORT_T StdString
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS String
#define STD_STABLE_SORT_T StdString
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT fehost_String_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/* fehost_FeHost_ReadHost_list_dir_if_there returns a directory's names, sorted, or nothing for one that isn't there. */
static bool
fehost_FeHost_ReadHost_list_dir_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
										 bool *has_result, StdVec *result, HostError *error)
{
	StdCString	c_path;
	StdVec		names;

	(void) self;
	(void) raise;
	*has_result = false;
	if (!fehost_c_path(path, path_len, &c_path, error))
		return false;
	std_vec_u8_from_elem(&names, 0, 4096);
	for (;;)
	{
		PgFrontendFeError failed;
		PgFrontendFeError ignored;
		int64		len;
		int64		missing = 0;

		if (pg_frontend_calls_list_dir(std_CString_as_ptr(&c_path), names.ptr, names.len, &len, &failed))
		{
			size_t		listed_len = len < 0 ? 0 : (size_t) len;
			size_t		start = 0;

			if (listed_len > names.len)
			{
				uint8		zero = 0;

				std_vec_resize(&names, listed_len, &zero, 1, 1);
				continue;
			}
			std_vec_new(result, sizeof(StdString), _Alignof(StdString));
			for (size_t at = 0; at <= listed_len; at++)
			{
				if (at < listed_len && ((const uint8 *) names.ptr)[at] != 0)
					continue;
				if (at > start)
				{
					StdCowStr	lossy;
					StdString	name;

					std_string_from_utf8_lossy((const uint8 *) names.ptr + start, at - start, &lossy);
					std_CowStr_into_owned(&lossy, &name);
					std_vec_push_Checked(result, &name, sizeof(name), _Alignof(StdString));
				}
				start = at + 1;
			}
			std_slice_sort_String(result->ptr, result->len);
			*has_result = true;
			break;
		}
		if (pg_frontend_calls_is_missing(failed.f0, &missing, &ignored) && missing == 1)
			break;
		{
			StdString	what = fehost_said("could not read directory \"", path, path_len, "\"");

			*error = fehost_failed(failed, &what);
		}
		std_vec_free_Checked(&names, 1, 1);
		std_CString_drop(&c_path);
		return false;
	}
	std_vec_free_Checked(&names, 1, 1);
	std_CString_drop(&c_path);
	return true;
}

/* fehost_FeHost_ReadHost_list_dir returns a directory's names, sorted, failing for one that isn't there. */
static bool
fehost_FeHost_ReadHost_list_dir(void *self, const HostMayRaise *raise, const char *path, size_t path_len,
								StdVec *result, HostError *error)
{
	bool		has;

	if (!fehost_FeHost_ReadHost_list_dir_if_there(self, raise, path, path_len, &has, result, error))
		return false;
	if (!has)
	{
		StdString	message = fehost_said("could not open directory \"", path, path_len,
										  "\": No such file or directory");

		*error = host_Error_fork((const uint8 *) "58P01", &message);
		return false;
	}
	return true;
}

/* fehost_FeHost_ReadHost_prepare_decompressing prepares a dictionary for decompressing, in pg's form. */
static bool
fehost_FeHost_ReadHost_prepare_decompressing(void *self, const uint8 *bytes, size_t bytes_len,
											 HostDecompressDictionary *result, HostError *error)
{
	PgZstdDecompressDictionary prepared;
	PgZstdDecompressDictionary *boxed;
	StdAny		any;

	(void) self;
	if (!pg_zstd_DecompressDictionary_new(bytes, bytes_len, &prepared))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	boxed = std_box_new_uninit_Checked(sizeof(PgZstdDecompressDictionary), _Alignof(PgZstdDecompressDictionary));
	*boxed = prepared;
	any.type = &PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID;
	any.value = boxed;
	*result = host_DecompressDictionary_new(&any);
	return true;
}

/* fehost_FeHost_ReadHost_frame_size returns the length a frame records of its bytes. */
static bool
fehost_FeHost_ReadHost_frame_size(void *self, const uint8 *frame, size_t frame_len, uint64 *result)
{
	(void) self;
	return pg_zstd_frame_size(frame, frame_len, result);
}

/* fehost_FeHost_ReadHost_decompress returns a frame's bytes, nothing for a damaged frame, or an error when libzstd can't allocate. */
static bool
fehost_FeHost_ReadHost_decompress(void *self, const uint8 *frame, size_t frame_len, size_t size,
								  const HostDecompressDictionary *dictionary, bool *has_result, StdVec *result,
								  HostError *error)
{
	const PgZstdDecompressDictionary *prepared = NULL;
	PgZstdFailed failed;

	(void) self;
	if (dictionary != NULL)
		prepared = std_any_downcast(host_DecompressDictionary_prepared(dictionary),
									&PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID);
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

/* FEHOST_FE_HOST_READ_HOST_OPS is FeHost's ReadHost table. */
static const HostReadHostOps FEHOST_FE_HOST_READ_HOST_OPS = {
	.open_to_read = fehost_FeHost_ReadHost_open_to_read,
	.open_if_there = fehost_FeHost_ReadHost_open_if_there,
	.read_at = fehost_FeHost_ReadHost_read_at,
	.size = fehost_FeHost_ReadHost_size,
	.prefetch = fehost_FeHost_ReadHost_prefetch,
	.close = fehost_FeHost_ReadHost_close,
	.tablespace_root = fehost_FeHost_ReadHost_tablespace_root,
	.list_dir = fehost_FeHost_ReadHost_list_dir,
	.list_dir_if_there = fehost_FeHost_ReadHost_list_dir_if_there,
	.prepare_decompressing = fehost_FeHost_ReadHost_prepare_decompressing,
	.frame_size = fehost_FeHost_ReadHost_frame_size,
	.decompress = fehost_FeHost_ReadHost_decompress,
};

/*
 * fehost_install installs the host for a data directory and returns the
 * capability its reads take, or the error of a second install, which makes
 * no host.
 */
bool
fehost_install(const char *datadir, size_t datadir_len, HostIoReadIo *result, StdString *error)
{
	uint8		version[64];
	int64		len;
	PgFrontendFeError failed;
	HostReadHost host;
	HostRefused refused;
	static const char installed[] = "a host is installed already";

	if (!pg_frontend_calls_version_directory(version, sizeof(version), &len, &failed))
	{
		pg_frontend_FeError_text(failed, error);
		return false;
	}
	if (FEHOST_MADE)
	{
		std_string_from_Checked(error, installed, strlen(installed));
		return false;
	}
	{
		StdCowStr	lossy;

		std_string_from_utf8_lossy(version, len < 0 ? 0 : (size_t) len, &lossy);
		std_CowStr_into_owned(&lossy, &FEHOST_HOST.version);
	}
	std_slice_to_vec_Checked(&FEHOST_HOST.datadir.bytes, datadir, datadir_len, 1, 1);
	FEHOST_MADE = true;
	host.ops = &FEHOST_FE_HOST_READ_HOST_OPS;
	host.self = &FEHOST_HOST;
	if (!host_install_reading(host, &refused))
	{
		std_string_from_Checked(error, installed, strlen(installed));
		return false;
	}
	*result = host_io_ReadIo_new_for_fehost();
	return true;
}
