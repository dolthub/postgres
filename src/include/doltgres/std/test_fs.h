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
 * The test-only forms of std::fs the crates' tests call, which simc, the Rust
 * test crate, exports as Rust's own std::fs, the owner's "same Rust code"
 * rule for sim (the port text's section 13): the shipped C never includes
 * this header, and simc is refused from the server's closure.
 */
#ifndef DOLTGRES_STD_TEST_FS_H
#define DOLTGRES_STD_TEST_FS_H

#include "doltgres/std/io.h"
#include "doltgres/std/vec.h"

/* StdFsMetadata is fs::Metadata, a Box<Metadata> simc made. */
typedef struct StdFsMetadata
{
	void	   *f0;
} StdFsMetadata;

/* StdFsOpenOptions is fs::OpenOptions, a Box<OpenOptions> simc made. */
typedef struct StdFsOpenOptions
{
	void	   *f0;
} StdFsOpenOptions;

/* StdFsFile is fs::File, a Box<File> simc made, which its drop closes. */
typedef struct StdFsFile
{
	void	   *f0;
} StdFsFile;

extern bool std_fs_read(const char *path, size_t path_len, StdVec *result, StdIoError *error);
extern bool std_fs_write(const char *path, size_t path_len, const uint8 *contents, size_t contents_len,
						 StdIoError *error);
extern bool std_fs_remove_file(const char *path, size_t path_len, StdIoError *error);
extern bool std_fs_rename(const char *from, size_t from_len, const char *to, size_t to_len, StdIoError *error);
extern bool std_fs_create_dir_all(const char *path, size_t path_len, StdIoError *error);
extern bool std_fs_read_dir_names(const char *path, size_t path_len, StdVec *result, bool *entry, StdIoError *error);
extern bool std_fs_metadata(const char *path, size_t path_len, StdFsMetadata *result, StdIoError *error);
extern uint64 std_fs_Metadata_len(const StdFsMetadata *self);
extern void std_fs_Metadata_drop(void *self);
extern StdFsOpenOptions std_fs_OpenOptions_new(void);
extern StdFsOpenOptions *std_fs_OpenOptions_append(StdFsOpenOptions *self, bool append);
extern bool std_fs_OpenOptions_open(const StdFsOpenOptions *self, const char *path, size_t path_len,
									StdFsFile *result, StdIoError *error);
extern void std_fs_OpenOptions_drop(void *self);
extern bool std_fs_File_Write_write_all(StdFsFile *self, const uint8 *buf, size_t buf_len, StdIoError *error);
extern void std_fs_File_drop(void *self);
extern bool std_path_Path_exists(const char *path, size_t path_len);

#endif							/* DOLTGRES_STD_TEST_FS_H */
