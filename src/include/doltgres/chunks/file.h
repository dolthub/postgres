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
 * writes goes through Postgres' file layer in the server (chunks' file.rs).
 */
#ifndef DOLTGRES_CHUNKS_FILE_H
#define DOLTGRES_CHUNKS_FILE_H

#include "doltgres/chunks/error.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/path.h"
#include "doltgres/std/vec.h"

/*
 * ChunksFileFallbackTag is Fallback's variants: why a read of a file
 * write_kept wrote read its previous generation, or kept only some of its
 * records.
 */
typedef enum ChunksFileFallbackTag
{
	/*
	 * The newest is missing, as a crash between a write's rotation of the
	 * newest and its new file leaves it.
	 */
	CHUNKS_FILE_FALLBACK_MISSING,
	/* The newest is there but fails its checksum. */
	CHUNKS_FILE_FALLBACK_DAMAGED,

	/*
	 * The file ends in damage that a search too long to finish could not tell
	 * from a last record a crash cut short, so its records before it are
	 * kept, with what of its previous generation they don't replace, the file
	 * as it was is kept beside it under damaged_name, and it is written again
	 * from those.
	 */
	CHUNKS_FILE_FALLBACK_UNCLEAR,

	/*
	 * Neither generation reads, so the file counts as empty, as only a list
	 * of files safe to delete may.
	 */
	CHUNKS_FILE_FALLBACK_UNREADABLE,

	/*
	 * The file of appended records is damaged at the offset past its first
	 * record, so only its records before the damage are kept, the file as it
	 * was is kept beside it under damaged_name, it is written again from
	 * those, and the WAL's replay restores what the records after the damage
	 * held.
	 */
	CHUNKS_FILE_FALLBACK_KEPT,
} ChunksFileFallbackTag;

/* ChunksFileFallback is Fallback: why a read fell back, with the offset of the damage that Kept names. */
typedef struct ChunksFileFallback
{
	ChunksFileFallbackTag tag;
	union
	{
		uint64		kept;
	}			u;
} ChunksFileFallback;

/*
 * ChunksFileWhole is Whole: where a read of a file found its whole records
 * end, with the generation its header names, which says what the file holds
 * and grants nothing.
 */
typedef struct ChunksFileWhole
{
	uint64		end;
	uint64		generation;
} ChunksFileWhole;

/*
 * ChunksFileDamage is Damage: where a file of appended records is damaged,
 * the offset of the first record that fails, and the whole records found
 * after it, which only the WAL can restore.
 */
typedef struct ChunksFileDamage
{
	uint64		offset;
	/* after is a Vec<Vec<u8>> of the whole records found after the damage. */
	StdVec		after;
} ChunksFileDamage;

/*
 * ChunksFileAppended is Appended: what a file of appended records holds, its
 * records in order, where the next record goes when records may be appended
 * to it, and why its records are its previous generation's, if they are.
 */
typedef struct ChunksFileAppended
{
	/* records is a Vec<Vec<u8>> of the file's records in order. */
	StdVec		records;
	bool		has_end;
	ChunksFileWhole end;
	bool		has_fallback;
	ChunksFileFallback fallback;

	/*
	 * kept is whether the records are the bytes of a file write_kept wrote,
	 * as one record.
	 */
	bool		kept;

	/*
	 * damage is where the file is damaged past the records, when it is, which
	 * keeps only the records before the damage.
	 */
	bool		has_damage;
	ChunksFileDamage damage;
} ChunksFileAppended;

/*
 * ChunksFileTornTail is TornTail: where in a file its whole records end when
 * all that follows them is a last record a crash cut short, which only scan
 * finds and the doors that write there return, consuming the one they write
 * at. Its storage is opaque, laid out by file.c alone, as the Rust's fields
 * are private to its module, so no other C names its parts, and t/117
 * refuses a file outside file.c that initializes one or copies bytes into
 * one. A door that takes one moves it out of its caller's storage before it
 * can fail, so any later use of that storage panics, as a use of a moved
 * value is refused.
 */
typedef struct ChunksFileTornTail
{
	uint64		opaque[7];
} ChunksFileTornTail;

/*
 * ChunksFileSynced is Synced: the generation of a file of records and the
 * length a checkpoint knew durable in it, as a repository's manifest records
 * it.
 */
typedef struct ChunksFileSynced
{
	uint64		generation;
	uint64		len;
} ChunksFileSynced;

/*
 * CHUNKS_FILE_GENERATION_HEADER is the length of the header that begins a
 * file of records whose synced length a manifest records: the kind's magic,
 * the generation of the write of the whole file, and an xxh3 of both.
 */
#define CHUNKS_FILE_GENERATION_HEADER ((size_t) 24)

/*
 * ChunksFileScan is Scan: what scan found in a file, the bytes it read, each
 * whole record, where the whole records from the first end unbroken, the
 * damaged regions that whole records follow, as places in the bytes read,
 * and where the whole records end when it found no damage.
 */
typedef struct ChunksFileScan
{
	StdVec		bytes;
	/* records is a Vec<Range<usize>> of each whole record's place. */
	StdVec		records;
	size_t		end;
	/* damaged is a Vec<Range<usize>> of the damaged regions' places. */
	StdVec		damaged;
	bool		has_torn;
	ChunksFileWhole torn;
} ChunksFileScan;

/*
 * ChunksFileInterruptibleOps is Interruptible's table, its supertrait
 * ReadDoors' first: a capability that checks for an interrupt, failing with
 * Postgres' error once one is pending.
 */
typedef struct ChunksFileInterruptibleOps
{
	HostIoReadDoorsOps read_doors;
	bool		(*check_interrupt) (void *self, HostError *error);
} ChunksFileInterruptibleOps;

/* ChunksFileInterruptible is a capability that implements Interruptible. */
typedef struct ChunksFileInterruptible
{
	const ChunksFileInterruptibleOps *ops;
	void	   *self;
} ChunksFileInterruptible;

/* CHUNKS_FILE_APPENDED begins a file of appended records, which tells it from a file write_kept wrote. */
extern const uint8 CHUNKS_FILE_APPENDED[8];

#ifdef FRONTEND
extern const ChunksFileInterruptibleOps CHUNKS_FILE_READ_IO_INTERRUPTIBLE_OPS;
#endif
extern const ChunksFileInterruptibleOps CHUNKS_FILE_IO_INTERRUPTIBLE_OPS;
extern const ChunksFileInterruptibleOps CHUNKS_FILE_FLAG_IO_INTERRUPTIBLE_OPS;

extern bool chunks_file_read_at(HostIoReadDoors io, const HostFile *file, uint64 offset, size_t len, StdVec *result,
								ChunksErrorError *error);
extern void chunks_file_advise(HostIoReadDoors io, const HostFile *file, uint64 offset, uint64 len);
extern void chunks_file_set_read_only(bool read_only);
extern bool chunks_file_read_only(void);
extern bool chunks_file_writable(ChunksErrorAction *action, const char *path, size_t path_len,
								 ChunksErrorError *error);
extern bool chunks_file_write_at(HostIoIoDoors io, const HostFile *file, const char *path, size_t path_len,
								 uint64 offset, const uint8 *bytes, size_t bytes_len, ChunksErrorError *error);
extern bool chunks_file_sync(HostIoIoDoors io, const HostFile *file, ChunksErrorError *error);
extern bool chunks_file_sync_dir(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_file_write_file(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes,
								   size_t bytes_len, ChunksErrorError *error);
extern bool chunks_file_create(HostIoIoDoors io, const char *path, size_t path_len, HostFile *result,
							   ChunksErrorError *error);
extern bool chunks_file_open_rw(HostIoIoDoors io, const char *path, size_t path_len, bool create, HostFile *result,
								ChunksErrorError *error);
extern bool chunks_file_cut(HostIoIoDoors io, ChunksFileTornTail *tail, ChunksFileTornTail *result,
							ChunksErrorError *error);
extern bool chunks_file_exists(HostIoReadDoors io, const char *path, size_t path_len, bool *result,
							   ChunksErrorError *error);
extern StdPathBuf chunks_file_previous(const char *path, size_t path_len);
extern bool chunks_file_generations(HostIoReadDoors io, const char *path, size_t path_len, StdPathBuf *result_0,
									StdPathBuf *result_1, ChunksErrorError *error);
extern bool chunks_file_Fallback_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_file_Fallback_PartialEq_eq(const ChunksFileFallback *self, const ChunksFileFallback *other);
extern bool chunks_file_Damage_Debug_fmt(const void *value, StdFormatter *f);
extern void chunks_file_Damage_Default_default(ChunksFileDamage *result);
extern bool chunks_file_Damage_PartialEq_eq(const ChunksFileDamage *self, const ChunksFileDamage *other);
extern bool chunks_file_damaged_name(HostIoIoDoors io, const char *path, size_t path_len, StdPathBuf *result,
									 ChunksErrorError *error);
extern bool chunks_file_replace_records(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes,
										size_t bytes_len, bool keep, ChunksFileTornTail *result,
										ChunksErrorError *error);
extern bool chunks_file_write_kept(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *bytes,
								   size_t bytes_len, bool *result, ChunksErrorError *error);
extern bool chunks_file_read_kept(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
								  StdVec *result_0, bool *has_result_1, ChunksFileFallback *result_1,
								  ChunksErrorError *error);
extern bool chunks_file_read_previous(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
									  StdVec *result, ChunksErrorError *error);
extern uint64 chunks_file_TornTail_end(const ChunksFileTornTail *self);
extern const char *chunks_file_TornTail_path(const ChunksFileTornTail *self, size_t *result_len);
extern uint64 chunks_file_TornTail_generation(const ChunksFileTornTail *self);
extern ChunksFileTornTail chunks_file_TornTail_of_generation(ChunksFileTornTail *self, uint64 generation);
extern ChunksFileWhole chunks_file_Whole_of_generation(ChunksFileWhole self, uint64 generation);
extern ChunksFileTornTail chunks_file_tail(const char *path, size_t path_len, ChunksFileWhole whole);
extern bool chunks_file_Synced_cuts(const ChunksFileSynced *self, const ChunksFileWhole *whole);
extern void chunks_file_generation_header(const uint8 *magic, StdVec *out_0, uint64 *out_1);
extern bool chunks_file_read_generation(const uint8 *bytes, size_t bytes_len, const uint8 *magic, uint64 *result);
extern bool chunks_file_scan(HostIoIo *io, const char *path, size_t path_len, uint64 from, size_t stride,
							 bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len, size_t at,
											 size_t *result),
							 void *record_captures, void (*record_drop) (void *), ChunksFileScan *result,
							 ChunksErrorError *error);
extern bool chunks_file_scan_locked(HostIoReadDoors io, const char *path, size_t path_len, uint64 from,
									size_t stride,
									bool (*record) (void *captures, const uint8 *bytes, size_t bytes_len,
													size_t at, size_t *result),
									void *record_captures, void (*record_drop) (void *), ChunksFileScan *result,
									ChunksErrorError *error);
extern bool chunks_file_appended_between(HostIoReadDoors io, const char *path, size_t path_len, uint64 from,
										 uint64 to, bool *has_result, StdVec *result, ChunksErrorError *error);
extern bool chunks_file_previous_damaged(HostIoReadDoors io, const char *path, size_t path_len, bool *result,
										 ChunksErrorError *error);
extern bool chunks_file_read_appended(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
									  ChunksFileAppended *result, ChunksErrorError *error);
extern bool chunks_file_write_appended(HostIoIoDoors io, const char *path, size_t path_len, const uint8 *record,
									   size_t record_len, bool *placing, ChunksFileTornTail *result,
									   ChunksErrorError *error);
extern bool chunks_file_append(HostIoIoDoors io, ChunksFileTornTail *end, const uint8 *record, size_t record_len,
							   ChunksFileTornTail *result, ChunksErrorError *error);
extern bool chunks_file_write_end(HostIoIoDoors io, ChunksFileTornTail *end, const uint8 *bytes, size_t bytes_len,
								  bool create, ChunksFileTornTail *result, ChunksErrorError *error);
extern bool chunks_file_rename(HostIoIoDoors io, const char *from, size_t from_len, const char *to, size_t to_len,
							   ChunksErrorError *error);
extern bool chunks_file_file_names(HostIoReadDoors io, const char *dir, size_t dir_len, StdVec *result,
								   ChunksErrorError *error);
extern bool chunks_file_dir_there(HostIoReadDoors io, const char *dir, size_t dir_len, bool *result,
								  ChunksErrorError *error);
extern bool chunks_file_read_file(HostIoReadDoors io, const char *path, size_t path_len, StdVec *result,
								  ChunksErrorError *error);
extern bool chunks_file_create_dir_all(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_file_create_dir(HostIoIoDoors io, const char *dir, size_t dir_len, ChunksErrorError *error);
extern bool chunks_file_remove_file(HostIoIoDoors io, const char *path, size_t path_len, ChunksErrorError *error);
extern bool chunks_file_remove_if_there(HostIoIoDoors io, const char *path, size_t path_len, bool *result,
										ChunksErrorError *error);
extern void chunks_file_Appended_drop(void *self_);
extern void chunks_file_Damage_drop(void *self_);
extern void chunks_file_TornTail_drop(void *self_);
extern void chunks_file_Scan_drop(void *self_);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_file_tests_one_backends_read_only_mode_leaves_another_backends_writes_alone(void);
extern void test_chunks_file_tests_a_kept_file_whose_newest_generation_is_damaged_reads_its_previous_and_keeps_it_through_the_next_write(void);
extern void test_chunks_file_tests_a_cut_by_the_tail_an_append_returns_keeps_what_the_append_wrote(void);
extern void test_chunks_file_tests_a_scan_of_a_file_of_records_stops_within_a_record_once_a_cancel_is_pending(void);
extern void test_chunks_file_tests_a_scan_of_a_file_that_is_not_there_finds_nothing(void);
extern void test_chunks_file_tests_a_rotation_that_fails_at_any_step_leaves_the_old_records_or_the_new(void);
extern void test_chunks_file_tests_a_read_between_two_places_refuses_bytes_that_are_not_whole_records(void);
extern void test_chunks_file_tests_a_kept_files_write_answers_whether_its_two_generations_are_the_same(void);
extern void test_chunks_file_tests_a_one_part_relative_path_syncs_the_directory_it_is_in(void);
extern void test_chunks_file_tests_a_failed_allocation_in_a_read_of_appended_records_leaks_nothing(void);
extern void test_chunks_file_tests_a_torn_tail_a_door_took_panics_at_its_next_use_and_a_failed_door_still_takes_it(void);
#endif

#endif							/* DOLTGRES_CHUNKS_FILE_H */
