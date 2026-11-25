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
 * Repositories (repo's lib.rs): each database's packs, its list files, and
 * its manifest, a kept file that names the tree of its storages' bases as
 * its last save left it.
 */
#ifndef DOLTGRES_REPO_LIB_H
#define DOLTGRES_REPO_LIB_H

#include "doltgres/chunks/error.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/path.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"

/*
 * RepoSpilled is Spilled: a transaction's spill file among a checkpoint's
 * runs, by its top-level xid and its number, for a free space map's spill its
 * transaction's abort kept, the point a fold passes it at, and, for a fold's
 * residual of a map's spills, the fold's point, from which readers read it.
 */
typedef struct RepoSpilled
{
	uint64		xid;
	uint32		number;
	bool		has_passed;
	uint64		passed;
	bool		has_from;
	uint64		from;
} RepoSpilled;

/*
 * RepoRunFile is RunFile: one file of a checkpoint's runs, its tree's kind,
 * its storage's tablespace and relation, its length, the xxh3 its trailer
 * holds, for a transaction's spill, its xid and number, and the tablespace
 * its names carry, which no move of its database changes (bugs rows 438 and
 * 442).
 */
typedef struct RepoRunFile
{
	uint8		kind;
	uint32		tablespace;
	uint32		relation;
	uint64		len;
	uint64		hash;
	bool		has_spilled;
	RepoSpilled spilled;
	uint32		space;
} RepoRunFile;

/* RepoRuns is Runs: the files of the delta's runs a checkpoint wrote at its redo point, one for each tree it wrote. */
typedef struct RepoRuns
{
	HostLsn		redo;
	/* files is a Vec<RunFile>. */
	StdVec		files;
} RepoRuns;

/*
 * RepoManifest is Manifest: what a repository's last save left, the tree of
 * its storages' bases, the highest fold point its saves wrote, the redo
 * point of the last checkpoint completed when it was written, the runs of
 * the delta its checkpoint wrote, and the database's identity tree.
 */
typedef struct RepoManifest
{
	/*
	 * The base tree's root, `bases`, or none in a repository nothing was
	 * folded in.
	 */
	bool		has_bases;
	TreesPublishedPublished bases;
	HostLsn		folded;
	HostLsn		redo;
	bool		has_runs;
	RepoRuns	runs;

	/*
	 * The identity tree's root, `identity`, or none in a repository whose
	 * saves folded no pair.
	 */
	bool		has_identity;
	TreesPublishedPublished identity;
} RepoManifest;

/* RepoStorage is Storage: a storage's name within its database, the tablespace and relation number of its relfilenode's locator, which keys its entry in the base tree. */
typedef struct RepoStorage
{
	uint32		tablespace;
	uint32		relation;
} RepoStorage;

/* RepoBaseRecord is BaseRecord: a storage's entry in the base tree, its base's record and the chunks the record names. */
typedef struct RepoBaseRecord
{
	StdVec		bytes;
	/* addresses is a Vec<(u32, Locator)>. */
	StdVec		addresses;
} RepoBaseRecord;

/* RepoGatheredTag is Gathered's variants. */
typedef enum RepoGatheredTag
{
	/* A storage's newest base: its record, and the fold point the base is at. */
	REPO_GATHERED_BASE,
	/* An unlinked storage, whose entry the save takes out. */
	REPO_GATHERED_UNLINKED,

	/*
	 * A key of the identity tree with the value a fold gives it, or none for
	 * its removal.
	 */
	REPO_GATHERED_PAIR,
} RepoGatheredTag;

/* RepoGathered is Gathered: a save's edit of one storage's entry in the base tree, or of one key of the identity tree. */
typedef struct RepoGathered
{
	RepoGatheredTag tag;
	union
	{
		struct
		{
			RepoStorage storage;
			RepoBaseRecord record;
			HostLsn		folded;
		}			base;
		RepoStorage unlinked;
		struct
		{
			StdVec		key;
			bool		has_value;
			StdVec		value;
		}			pair;
	}			u;
} RepoGathered;

/* RepoRead is Read: a save's read of the newest manifest, by the count of rotations it was read at. */
typedef struct RepoRead
{
	uint64		rotations;
} RepoRead;

/* repo_OptionTupleU64ArcManifest_t is an Option<(u64, Arc<Manifest>)>: a manifest's share with the count of rotations it was read at. */
typedef struct repo_OptionTupleU64ArcManifest_t
{
	bool		has;
	uint64		f0;
	StdArcInner *f1;
} repo_OptionTupleU64ArcManifest_t;

/* The IoMutex<Option<(u64, Arc<Manifest>)>> of a Repo's copy of its manifest. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_TAKEN_TS RepoCopy
#define HOST_LOCK_TAKEN_T repo_OptionTupleU64ArcManifest_t
#define HOST_LOCK_TAKEN_TYPE REPO_COPY_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS RepoCopy
#define HOST_LOCK_T repo_OptionTupleU64ArcManifest_t
#define HOST_LOCK_DEFAULT(place) ((place)->has = false)
#include "doltgres/host/lock_template.h"

/*
 * RepoRepo is Repo: a process's own handle on one repository, its hold of
 * the repository's packs, and its copy of the manifest as it last read it.
 */
typedef struct RepoRepo
{
	ChunksPacksBackendPacks packs;
	StdPathBuf	dir;
	/* own is the tablespace of the repository's own directory. */
	uint32		own;

	/*
	 * The manifest as last read, with the packs' count of rotations it was
	 * read at, or none to read it again.
	 */
	HostLockIoMutexRepoCopy copy;
} RepoRepo;

extern void repo_Manifest_encode(const RepoManifest *self, StdVec *result);
extern bool repo_Manifest_decode(const uint8 *bytes, size_t bytes_len, TreesPublishedRepoId repo,
								 RepoManifest *result, ChunksErrorError *error);
extern void repo_Manifest_drop(RepoManifest *self);
extern bool repo_Spilled_Debug_fmt(const void *value, StdFormatter *f);
extern bool repo_Spilled_PartialEq_eq(const RepoSpilled *self, const RepoSpilled *other);
extern bool repo_RunFile_Debug_fmt(const void *value, StdFormatter *f);
extern bool repo_RunFile_PartialEq_eq(const RepoRunFile *self, const RepoRunFile *other);
extern bool repo_Runs_Debug_fmt(const void *value, StdFormatter *f);
extern bool repo_Runs_PartialEq_eq(const RepoRuns *self, const RepoRuns *other);
extern void repo_Runs_Clone_clone(const RepoRuns *self, RepoRuns *result);
extern void repo_Runs_drop(RepoRuns *self);
extern void repo_ArcManifest_drop(StdArcInner *self);
extern void repo_Repo_drop(RepoRepo *self);
extern bool repo_Repo_create(HostIoIo *io, uint32 repository, uint32 own, RepoRepo *result, ChunksErrorError *error);
extern bool repo_Repo_open(HostIoIo *io, uint32 repository, uint32 own, RepoRepo *result, ChunksErrorError *error);
extern bool repo_Repo_close(RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, ChunksErrorError *error);
extern TreesPublishedRepoId repo_Repo_id(const RepoRepo *self);
extern uint32 repo_Repo_own(const RepoRepo *self);
extern const char *repo_Repo_dir(const RepoRepo *self, size_t *result_len);
extern TreesStoreReader repo_Repo_reader(const RepoRepo *self, HostCachePlacement placement,
										 const HostSharedReader *reading);
extern bool repo_Repo_manifest(const RepoRepo *self, HostIoIo *io, StdArcInner **result, ChunksErrorError *error);
extern const RepoManifest *repo_Repo_newest(StdArcInner *manifest);
extern bool repo_Repo_previous(const RepoRepo *self, HostIoIo *io, bool *has_result, RepoManifest *result,
							   ChunksErrorError *error);
extern bool repo_Repo_roots(const RepoRepo *self, HostIoIo *io, StdVec *result, ChunksErrorError *error);
extern void repo_BaseRecord_drop(RepoBaseRecord *self);
extern bool repo_Repo_base(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading,
						   const RepoStorage *storage, bool *has_result, RepoBaseRecord *result,
						   ChunksErrorError *error);
extern bool repo_Repo_identity(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, const uint8 *key,
							   size_t key_len, bool *has_result, StdVec *result, ChunksErrorError *error);
extern void repo_Gathered_drop(RepoGathered *self);
extern bool repo_Repo_save(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, bool has_runs,
						   RepoRuns *runs,
						   bool (*gather) (void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
										   ChunksErrorError *error),
						   void *gather_captures,
						   bool (*saved) (void *captures, HostIoIo *io, ChunksErrorError *error),
						   void *saved_captures, ChunksErrorError *error);
extern bool repo_Repo_save_between(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading,
								   RepoRuns *runs,
								   bool (*gather) (void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
												   ChunksErrorError *error),
								   void *gather_captures,
								   bool (*saved) (void *captures, HostIoIo *io, ChunksErrorError *error),
								   void *saved_captures, ChunksErrorError *error);
extern bool repo_Repo_records(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, StdVec *result,
							  ChunksErrorError *error);
extern bool repo_Repo_rekeyed(const RepoRepo *self, HostIoIo *io, const HostSharedReader *reading, uint32 from,
							  uint32 to, ChunksErrorError *error);
#ifdef DOLTGRES_TEST_REPO
extern void test_repo_tests_a_manifest_reads_back_what_it_was_written_with(void);
extern void test_repo_tests_a_manifest_that_is_not_one_is_refused(void);
extern void test_repo_tests_a_repository_keeps_its_manifest_and_chunks(void);
extern void test_repo_tests_a_handle_reads_the_manifest_another_saved(void);
extern void test_repo_tests_a_manifest_that_is_not_whole_is_refused(void);
extern void test_repo_tests_a_manifests_runs_read_back_and_unknown_fields_are_refused(void);
extern void test_repo_tests_a_saves_fold_point_is_the_highest_written_and_its_redo_point_the_last_checkpoints(void);
extern void test_repo_tests_a_checkpoints_runs_are_written_and_kept_by_the_next_save(void);
extern void test_repo_tests_a_saved_base_reads_back_with_its_chunks_and_a_storage_taken_out_is_gone(void);
extern void test_repo_tests_a_save_refused_by_another_starts_over_and_its_first_pack_is_named_by_no_manifest(void);
extern void test_repo_tests_a_saves_pairs_fold_into_the_identity_tree(void);
extern void test_repo_tests_a_refused_save_starts_over_with_its_own_runs_or_the_ones_it_reads_again(void);
extern void test_repo_tests_a_save_cut_short_by_a_crash_at_any_step_leaves_one_whole_manifest(void);
#endif

#endif							/* DOLTGRES_REPO_LIB_H */
