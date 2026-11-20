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
 * The server's storage across its repositories: what each checkpoint does for
 * every repository's packs at once. A checkpoint writes each idle
 * repository's newest manifest once more where its two generations may
 * differ, and keeps the WAL from the redo point of the checkpoint before it
 * while any repository's generations may still differ, reading that WAL
 * through once and warning of a segment it can't read.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/generation.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/num.h"
#include "doltgres/store/databases.h"
#include "doltgres/store/lib.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/store/fixtures.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "lib.rs"

/* STORE_STORE_STATE_SINGLE_OPS is StoreState's Shape and Single: store's single record, of one atomic, the kept redo point. */
const HostSharedSingleOps STORE_STORE_STATE_SINGLE_OPS = {
	.shape = {.id = UINT64CONST(0x73746f7265000001), .atomics = 1},
	.words = host_shared_Single_words,
};

static bool store_dropped_generations(HostIoIo *io, HostError *error);

/*
 * store_checkpoint is the fork's work at each checkpoint and restartpoint,
 * at its redo point: it writes each marked repository's newest manifest once
 * more, then stores the redo point of the last checkpoint completed as the
 * point the WAL is kept from, and while a manifest is still marked it reads
 * the WAL kept, from there to this checkpoint's redo point, warning of any it
 * can't read. With no base backup counted in, it first removes the
 * generations backups kept (bugs row 178), as a collection that a backup's
 * start waits for. A bootstrap process only leaves the marker that tells the
 * start after it to make template1's repository.
 */
bool
store_checkpoint(HostIoIo *io, HostLsn redo, HostError *error)
{
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};
	ChunksErrorError failed;
	HostLsn		kept;
	bool		marked;

	if (!host_shared_ready())
		return store_databases_bootstrapped(io, error);
	if (host_collection_begin())
	{
		bool		dropped = store_dropped_generations(io, error);

		host_collection_end();
		if (!dropped)
			return false;
	}
	if (!chunks_packs_shared_even_manifests(io, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-evened", error) ||
		!host_last_redo(HOST_IO_IO_IO_DOORS(io), &kept, error) ||
		!host_shared_single(&STORE_STORE_STATE_SINGLE_OPS, &record, error))
		return false;
	exchange.u.exchange.f0 = host_shared_Record_atomic(record, STORE_KEPT_REDO_INDEX, load);
	exchange.u.exchange.f1 = kept;
	(void) host_shared_Record_atomic(record, STORE_KEPT_REDO_INDEX, exchange);
	if (kept == 0)
		return true;
	if (!chunks_packs_shared_marked(io, &marked, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	return !marked || store_read_kept_wal(io, kept, redo, error);
}

/* store_dropped_generations_frame_0_t is dropped_generations' state across its pad: the repositories. */
typedef struct store_dropped_generations_frame_0_t
{
	HostIoIo   *io;
	HostError  *error;
	bool		ok;
	StdVec		repositories;
} store_dropped_generations_frame_0_t;

/* store_dropped_generations_pad_0 is dropped_generations' pad: the repositories. */
static void
store_dropped_generations_pad_0(void *frame_)
{
	store_dropped_generations_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_vec_new(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
}

/* store_dropped_generations_body_0 is dropped_generations' body. */
static void
store_dropped_generations_body_0(void *frame_)
{
	store_dropped_generations_frame_0_t *frame = frame_;
	ChunksErrorError failed;

	frame->ok = false;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	for (size_t k = 0, k_len = frame->repositories.len; k < k_len; k++)
	{
		const std_TupleU32U32_t *found = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);

		if (!chunks_packs_generation_Packs_drop_generations(HOST_IO_IO_IO_DOORS(frame->io), found->f0, found->f1,
															&failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
	}
	frame->ok = true;
done:
	store_dropped_generations_pad_0(frame);
}

/* store_dropped_generations removes the generations base backups kept in every repository's directories. */
static bool
store_dropped_generations(HostIoIo *io, HostError *error)
{
	store_dropped_generations_frame_0_t frame = {.io = io, .error = error};

	std_vec_new(&frame.repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_landing(&frame, store_dropped_generations_body_0, store_dropped_generations_pad_0);
	return frame.ok;
}

/* store_kept_frame_0_t is kept's state across its pad: the repositories, the packs, and the reader. */
typedef struct store_kept_frame_0_t
{
	HostIoIo   *io;
	HostLsn		checkpoint;
	HostError  *error;
	bool		ok;
	StdVec		repositories;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} store_kept_frame_0_t;

/* store_kept_pad_0 is kept's pad: the reader, the packs, then the repositories. */
static void
store_kept_pad_0(void *frame_)
{
	store_kept_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_vec_new(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
}

/* store_kept_body_0 is kept's body: each repository's packs opened, kept, and closed, a failure warned of before the close. */
static void
store_kept_body_0(void *frame_)
{
	store_kept_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	HostError	failure;
	uint8		code[5];

	frame->ok = false;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(frame->io), &frame->repositories, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	for (size_t k = 0, k_len = frame->repositories.len; k < k_len; k++)
	{
		const std_TupleU32U32_t *found = &STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);
		bool		dropping;
		bool		closed;

		if (!chunks_packs_open_dropping(HOST_IO_IO_READ_DOORS(frame->io), found->f0, found->f1, &dropping, &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
		if (dropping)
			continue;
		if (!chunks_packs_open_Packs_open(frame->io, found->f0, found->f1, &frame->packs, &failed))
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
		frame->packs_live = true;
		if (!host_shared_read(frame->io, &frame->reading, frame->error))
			goto done;
		frame->reading_live = true;
		if (!chunks_packs_generation_Packs_keep(&frame->packs, frame->io, frame->checkpoint, &failed))
		{
			failure = chunks_packs_shared_raised(&failed);
			if (!store_databases_warned(frame->io, &failure, "the base backup's start", code, frame->error))
				goto done;
			frame->packs_live = false;
			if (!chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed))
			{
				*frame->error = chunks_packs_shared_raised(&failed);
				goto done;
			}
			*frame->error = store_databases_failed(code, "could not keep the backup's generation of database ",
												   found->f0);
			goto done;
		}
		frame->packs_live = false;
		closed = chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed);
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
		if (!closed)
		{
			*frame->error = chunks_packs_shared_raised(&failed);
			goto done;
		}
	}
	frame->ok = true;
done:
	store_kept_pad_0(frame);
}

/*
 * store_kept keeps each repository's generation of its list files and
 * manifest under the name of a base backup's checkpoint, in
 * do_pg_backup_start after the checkpoint, for a server restored from the
 * backup to settle from (§III.9, bugs row 178); a dropped repository keeps
 * none. A repository whose generation it can't keep is reported at WARNING
 * before its hold is closed, and fails the backup's start.
 */
bool
store_kept(HostIoIo *io, HostLsn checkpoint, HostError *error)
{
	store_kept_frame_0_t frame = {.io = io, .checkpoint = checkpoint, .error = error};

	std_vec_new(&frame.repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_landing(&frame, store_kept_body_0, store_kept_pad_0);
	return frame.ok;
}

/*
 * store_stand returns the timeline, and the position up to which the WAL can
 * be read, that a read of the kept WAL ending at a position stands on: once
 * recovery has ended, the insertion timeline up to the flushed position;
 * while WAL is replayed, replay's timeline up to its position, or the
 * insertion timeline once recovery's end has set it; and before replay
 * begins, recovery's target timeline up to the position itself.
 */
static void
store_stand(const HostWalStand *stood, HostLsn to, uint32 *timeline, HostLsn *upto)
{
	if (!stood->recovering)
	{
		*timeline = stood->insertion_timeline;
		*upto = stood->flushed;
	}
	else if (stood->insertion_timeline == 0 && stood->replayed_timeline == 0)
	{
		*timeline = stood->target_timeline;
		*upto = to;
	}
	else if (stood->insertion_timeline == 0)
	{
		*timeline = stood->replayed_timeline;
		*upto = stood->replayed;
	}
	else
	{
		*timeline = stood->insertion_timeline;
		*upto = stood->replayed;
	}
}

/* store_read_kept_wal reads the WAL kept for a manifest's fallback, from one position to another, on the stand the server's WAL is at, warning where it can't be read. */
bool
store_read_kept_wal(HostIoIo *io, HostLsn from, HostLsn to, HostError *error)
{
	HostWalStand stood = host_wal_stand(HOST_IO_IO_IO_DOORS(io));
	uint32		timeline;
	HostLsn		upto;

	store_stand(&stood, to, &timeline, &upto);
	return host_warn_kept_wal(HOST_IO_IO_IO_DOORS(io), from, to, timeline, upto, error);
}

/*
 * store_kept_segment returns the oldest WAL segment to keep, given the
 * oldest Postgres keeps: the segment of the stored redo point when a
 * repository's manifest generations may differ as the WAL is counted, and
 * otherwise the one given, as in a bootstrap process.
 */
bool
store_kept_segment(HostIoIo *io, uint64 segment, uint64 segment_size, uint64 *result, HostError *error)
{
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	ChunksErrorError failed;
	HostLsn		kept;
	bool		marked;

	*result = segment;
	if (!host_shared_ready())
		return true;
	if (!host_shared_single(&STORE_STORE_STATE_SINGLE_OPS, &record, error))
		return false;
	kept = host_shared_Record_atomic(record, STORE_KEPT_REDO_INDEX, load);
	if (kept == 0)
		return true;
	if (!chunks_packs_shared_marked(io, &marked, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	if (marked)
		*result = Min(segment, STD_DIV(u64, kept, segment_size, 128, 20));
	return true;
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* store_tests_ok asserts a call's success, which unwraps at a line and column. */
static void
store_tests_ok(bool ok, const HostError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* store_tests_chunks_ok asserts a call of chunks' success, which unwraps at a line and column. */
static void
store_tests_chunks_ok(bool ok, const ChunksErrorError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/*
 * store_tests_packs makes a repository's packs by its id in pg_default with a
 * manifest written twice, with the same bytes or with others, so that its
 * record is marked only when they differ.
 */
static ChunksPacksBackendPacks
store_tests_packs(HostIoIo *io, uint32 id, bool differ)
{
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;

	store_tests_chunks_ok(chunks_packs_open_Packs_create(io, id, HOST_PG_DEFAULT, &packs, &failed), &failed, 141, 87);
	store_tests_chunks_ok(chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
																(const uint8 *) "first", 5, &failed), &failed, 142, 56);
	store_tests_chunks_ok(chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST,
																(const uint8 *) (differ ? "other" : "first"), 5,
																&failed), &failed, 143, 88);
	return packs;
}

/* store_tests_even reports whether a repository's manifest's two generations hold the same bytes. */
static bool
store_tests_even(HostIoIo *io, uint32 id)
{
	StdPathBuf	dir = host_tablespace_dir(id, HOST_PG_DEFAULT);
	StdPathBuf	path;
	ChunksErrorError failed;
	ChunksFileFallback fallback;
	StdVec		newest;
	StdVec		previous;
	bool		has;
	bool		has_fallback;
	bool		even;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
	store_tests_chunks_ok(chunks_file_read_kept(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has,
												&newest, &has_fallback, &fallback, &failed), &failed, 150, 57);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, 150, 66);
	store_tests_chunks_ok(chunks_file_read_previous(HOST_IO_IO_READ_DOORS(io), path.bytes.ptr, path.bytes.len, &has,
													&previous, &failed), &failed, 151, 48);
	even = has && previous.len == newest.len && memcmp(previous.ptr, newest.ptr, newest.len) == 0;
	if (has)
		std_vec_free_Checked(&previous, 1, 1);
	std_vec_free_Checked(&newest, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return even;
}

/* store_tests_checkpoint runs a checkpoint at a redo point, which unwraps at a line and column. */
static void
store_tests_checkpoint(HostIoIo *io, HostLsn redo, uint32 line, uint32 col)
{
	HostError	error;

	store_tests_ok(store_checkpoint(io, redo, &error), &error, line, col);
}

/* store_tests_segment_eq asserts at a line that the oldest segment kept is the one given, which unwraps at a column. */
static void
store_tests_segment_eq(HostIoIo *io, uint64 segment, uint64 segment_size, uint64 want, uint32 line, uint32 col)
{
	HostError	error;
	uint64		got;

	store_tests_ok(store_kept_segment(io, segment, segment_size, &got, &error), &error, line, col);
	STD_ASSERT_EQ_U64(line, 9, got, want);
}

/* store_tests_kept_redo_eq asserts at a line that the stored redo point is the one given, whose single unwraps at column 43. */
static void
store_tests_kept_redo_eq(uint64 want, uint32 line)
{
	HostSharedRecord record;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostError	error;

	store_tests_ok(host_shared_single(&STORE_STORE_STATE_SINGLE_OPS, &record, &error), &error, line, 43);
	STD_ASSERT_EQ_U64(line, 9, host_shared_Record_atomic(record, STORE_KEPT_REDO_INDEX, load), want);
}


/*
 * test_store_tests_a_checkpoint_evens_each_marked_manifest_and_stores_the_last_redo_point:
 * a checkpoint writes the manifest of each repository whose generations
 * differ once more, so both are even after it, and stores the redo point of
 * the last checkpoint completed; with nothing marked after it, the WAL kept
 * is what Postgres keeps.
 */
void
test_store_tests_a_checkpoint_evens_each_marked_manifest_and_stores_the_last_redo_point(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostLsn		from;
	HostLsn		to;
	ChunksPacksBackendPacks marked;
	ChunksPacksBackendPacks idle;

	(void) simc_kept_wal_read(&from, &to);
	marked = store_tests_packs(io, 801, true);
	idle = store_tests_packs(io, 802, false);
	STD_ASSERT(163, 9, !store_tests_even(io, 801) && store_tests_even(io, 802), "!even(io, 801) && even(io, 802)");
	simc_checkpointed(77);
	store_tests_checkpoint(io, 99, 165, 28);
	store_tests_kept_eq(RUST_FILE, false, 0, 0, 166, NULL);
	STD_ASSERT(167, 9, store_tests_even(io, 801) && store_tests_even(io, 802), "even(io, 801) && even(io, 802)");
	store_tests_kept_redo_eq(77, 168);
	store_tests_segment_eq(io, 10, 4, 10, 169, 44);
	chunks_packs_backend_Packs_drop(&marked);
	chunks_packs_backend_Packs_drop(&idle);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_tests_the_wal_is_kept_from_the_stored_redo_point_only_while_a_manifest_is_marked:
 * while a repository's manifest generations differ as the WAL is counted,
 * the WAL is kept from the stored redo point's segment, never past the
 * oldest Postgres keeps; once they are even again, from the oldest Postgres
 * keeps.
 */
void
test_store_tests_the_wal_is_kept_from_the_stored_redo_point_only_while_a_manifest_is_marked(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs;

	simc_checkpointed(40);
	store_tests_checkpoint(io, 60, 180, 28);
	packs = store_tests_packs(io, 803, true);
	store_tests_segment_eq(io, 100, 8, 5, 182, 45);
	store_tests_segment_eq(io, 3, 8, 3, 183, 43);
	simc_checkpointed(90);
	store_tests_checkpoint(io, 120, 185, 29);
	store_tests_segment_eq(io, 100, 8, 100, 186, 45);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_tests_before_the_first_checkpoint_nothing_is_kept_past_what_postgres_keeps:
 * before a run's first checkpoint the stored redo point is zero, which
 * lowers nothing, even with a manifest marked.
 */
void
test_store_tests_before_the_first_checkpoint_nothing_is_kept_past_what_postgres_keeps(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks packs = store_tests_packs(io, 804, true);

	store_tests_segment_eq(io, 100, 8, 100, 197, 45);
	chunks_packs_backend_Packs_drop(&packs);
	simc_TestIo_drop(&test_io);
}

/* store_tests_checkpointer is the checkpointing actor: a checkpoint at redo point 70. */
static bool
store_tests_checkpointer(void *captures, HostIoIo *io, HostError *error)
{
	(void) captures;
	return store_checkpoint(io, 70, error);
}

/* store_tests_saver is the saving actor: it writes repository 805's manifest once more. */
static bool
store_tests_saver(void *captures, HostIoIo *io, HostError *error)
{
	ChunksPacksBackendPacks packs;
	ChunksErrorError failed;
	bool		ok;

	(void) captures;
	if (!chunks_packs_open_Packs_open(io, 805, HOST_PG_DEFAULT, &packs, &failed))
	{
		*error = chunks_packs_shared_raised(&failed);
		return false;
	}
	ok = chunks_packs_backend_Packs_write_kept(&packs, io, CHUNKS_PACKS_BACKEND_KEPT_MANIFEST, (const uint8 *) "saved",
											   5, &failed);
	if (!ok)
		*error = chunks_packs_shared_raised(&failed);
	chunks_packs_backend_Packs_drop(&packs);
	return ok;
}

/*
 * test_store_tests_a_checkpoint_reads_the_wal_it_keeps_only_while_a_manifest_is_marked:
 * a checkpoint reads the WAL kept for a fallback, from the stored redo point
 * to its own, only while a manifest is still marked after its evening, as a
 * save that writes one during the checkpoint leaves it, and not when its
 * evening left nothing marked.
 */
void
test_store_tests_a_checkpoint_reads_the_wal_it_keeps_only_while_a_manifest_is_marked(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	SimcActor	checkpointer = {store_tests_checkpointer};
	SimcActor	saver = {store_tests_saver};
	SimcStep	steps[] = {{"saver", 5, PG_UINT64_MAX}, {"checkpointer", 12, PG_UINT64_MAX}};
	ChunksPacksBackendPacks packs;
	HostLsn		from;
	HostLsn		to;
	SimcRun		run;
	SimcReport	report;

	(void) simc_kept_wal_read(&from, &to);
	packs = store_tests_packs(io, 805, true);
	chunks_packs_backend_Packs_drop(&packs);
	simc_checkpointed(30);
	store_tests_checkpoint(io, 50, 210, 28);
	store_tests_kept_eq(RUST_FILE, false, 0, 0, 211, "a checkpoint that left nothing marked read the WAL");
	simc_checkpointed(50);
	run = simc_Run_new(1);
	simc_Run_actor(&run, "checkpointer", 12, checkpointer);
	simc_Run_actor(&run, "saver", 5, saver);
	simc_Run_until(&run, "checkpointer", 12, "doltgres-store-evened");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	store_tests_ended(RUST_FILE, &report, 223, 9);
	simc_Report_drop(&report);
	store_tests_kept_eq(RUST_FILE, true, 50, 70, 229, NULL);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_tests_a_checkpoint_warns_of_a_manifest_it_cant_write_once_more_and_evens_the_others:
 * a checkpoint that can't write one repository's manifest once more, its
 * newest damaged, warns of it once with Postgres' data-corrupted SQLSTATE,
 * naming the database, evens the others, stores the redo point, and
 * completes, and the damaged repository's record stays marked, keeping its
 * WAL.
 */
void
test_store_tests_a_checkpoint_warns_of_a_manifest_it_cant_write_once_more_and_evens_the_others(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	ChunksPacksBackendPacks damaged = store_tests_packs(io, 806, true);
	ChunksPacksBackendPacks other = store_tests_packs(io, 807, true);
	StdPathBuf	dir = host_tablespace_dir(806, HOST_PG_DEFAULT);
	StdPathBuf	path;
	StdIoError	io_error;
	StdVec		bytes;
	StdVec		warnings;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &path);
	if (!std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(241, 46, STORE_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_index_check(0, bytes.len, RUST_FILE, 242, 9);
	STD_VEC_AT(&bytes, uint8, 0) ^= 0xff;
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(243, 38, STORE_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, 1, 1);
	warnings = simc_take_warnings();
	store_tests_Warnings_drop(&warnings);
	simc_checkpointed(20);
	store_tests_checkpoint(io, 40, 246, 28);
	warnings = simc_take_warnings();
	store_tests_warned_once(RUST_FILE, &warnings, "database 806", 248);
	STD_ASSERT(252, 9, store_tests_even(io, 807), "even(io, 807)");
	store_tests_kept_redo_eq(20, 253);
	store_tests_segment_eq(io, 100, 8, 2, 254, 45);
	store_tests_kept_eq(RUST_FILE, true, 20, 40, 255, NULL);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	chunks_packs_backend_Packs_drop(&damaged);
	chunks_packs_backend_Packs_drop(&other);
	simc_TestIo_drop(&test_io);
}

/* store_tests_stand_eq asserts at a line that a read ending at 55 stands on the timeline and up to the position given. */
static void
store_tests_stand_eq(const HostWalStand *stood, uint32 timeline, HostLsn upto, uint32 line)
{
	uint32		got_timeline;
	HostLsn		got_upto;

	store_stand(stood, 55, &got_timeline, &got_upto);
	if (got_timeline != timeline || got_upto != upto)
	{
		StdTupleRef left = {2, {STD_ITEM(&got_timeline, std_u32_Debug_fmt), STD_ITEM(&got_upto, std_u64_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(&timeline, std_u32_Debug_fmt), STD_ITEM(&upto, std_u64_Debug_fmt)}};

		std_assert_failed(RUST_FILE, line, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
}

/*
 * test_store_tests_a_read_of_the_kept_wal_stands_where_the_servers_wal_does:
 * a read of the kept WAL stands on the insertion timeline up to the flushed
 * position once recovery has ended, on replay's timeline up to its position
 * while WAL is replayed, on the insertion timeline once recovery's end has
 * set it, and on recovery's target timeline up to the read's own end before
 * replay begins.
 */
void
test_store_tests_a_read_of_the_kept_wal_stands_where_the_servers_wal_does(void)
{
	HostWalStand ended = {.insertion_timeline = 3, .flushed = 90, .target_timeline = 3};
	HostWalStand replaying = {.recovering = true, .replayed = 70, .replayed_timeline = 2, .target_timeline = 4};
	HostWalStand set = replaying;
	HostWalStand begun = replaying;

	store_tests_stand_eq(&ended, 3, 90, 272);
	store_tests_stand_eq(&replaying, 2, 70, 281);
	set.insertion_timeline = 4;
	store_tests_stand_eq(&set, 4, 70, 282);
	begun.replayed = 0;
	begun.replayed_timeline = 0;
	store_tests_stand_eq(&begun, 4, 55, 283);
}

#endif							/* DOLTGRES_TEST_STORE */
