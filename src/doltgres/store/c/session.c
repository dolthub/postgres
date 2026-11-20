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
 * The session's repository: the repository of the database a backend is
 * connected to, opened at the backend's first use of the fork's storage and
 * held until it exits. It is shared, since an entry is entered again inside
 * itself, as an index build's table scan is.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs/shared.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/store/databases.h"
#include "doltgres/store/session.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/shared.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/store/fixtures.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "session.rs"

/* store_session_Repo_drop is Repo's drop glue, as an Arc's value takes it. */
static void
store_session_Repo_drop(void *repo)
{
	repo_Repo_drop(repo);
}

/* store_session_Held_Default_default is Option<Arc<Repo>>'s Default: none. */
static void
store_session_Held_Default_default(StoreSessionHeld *result)
{
	result->has = false;
	result->repo = NULL;
}

/* store_session_Held_drop is Option<Arc<Repo>>'s drop: the Arc's, when it has one. */
static void
store_session_Held_drop(StoreSessionHeld *self)
{
	if (!self->has)
		return;
	self->has = false;
	std_arc_drop(self->repo, sizeof(RepoRepo), _Alignof(RepoRepo), store_session_Repo_drop);
}

/* taken at Option<Arc<Repo>> and IoMutex<Option<Arc<Repo>>>, which session.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Held
#define HOST_LOCK_TAKEN_T StoreSessionHeld
#define HOST_LOCK_TAKEN_DEFAULT(place) store_session_Held_Default_default(place)
#define HOST_LOCK_TAKEN_DROP(place) store_session_Held_drop(place)
#define HOST_LOCK_TAKEN_TYPE STORE_SESSION_SESSION_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Held
#define HOST_LOCK_T StoreSessionHeld
#define HOST_LOCK_DEFAULT(place) store_session_Held_Default_default(place)
#include "doltgres/host/lock_template.h"

/* store_session_IoMutexHeld_drop is IoMutex<Option<Arc<Repo>>>'s drop glue: its value's. */
static void
store_session_IoMutexHeld_drop(void *value)
{
	HostLockIoMutexHeld *self = value;

	store_session_Held_drop(&self->value);
}

/* STORE_SESSION_SESSION_TYPE is IoMutex<Option<Arc<Repo>>>'s size, Default, and drop. */
static const StdDefaultType STORE_SESSION_SESSION_TYPE = {sizeof(HostLockIoMutexHeld), _Alignof(HostLockIoMutexHeld),
host_lock_IoMutexHeld_Default_default, store_session_IoMutexHeld_drop};

/* STORE_SESSION_SESSION is SESSION: the backend's session repository once it opened it, its BackendLocal header and the process's value. */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexHeld process;
}			STORE_SESSION_SESSION = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/* store_session_session_repo_frame_0_t is session_repo's state across its pad: the backend's value, its lock, and the repository opened. */
typedef struct store_session_session_repo_frame_0_t
{
	HostIoIo   *io;
	StdArcInner **result;
	HostError  *error;
	bool		ok;
	HostLocalLocal local;
	bool		local_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	RepoRepo	repo;
	StdArcInner *opened;
	bool		opened_live;
} store_session_session_repo_frame_0_t;

/* store_session_session_repo_pad_0 is session_repo's pad: the lock, the repository's Arc, then the backend's value. */
static void
store_session_session_repo_pad_0(void *frame_)
{
	store_session_session_repo_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->opened_live)
	{
		frame->opened_live = false;
		std_arc_drop(frame->opened, sizeof(RepoRepo), _Alignof(RepoRepo), store_session_Repo_drop);
	}
	if (frame->local_live)
	{
		frame->local_live = false;
		host_local_Local_drop(&frame->local, &STORE_SESSION_SESSION_TYPE);
	}
}

/* store_session_locked takes the lock of the backend's session repository. */
static StoreSessionHeld *
store_session_locked(store_session_session_repo_frame_0_t *frame)
{
	HostLockIoMutex *table = (HostLockIoMutex *) host_local_Local_Deref_deref(&frame->local,
																			  &STORE_SESSION_SESSION_TYPE);

	frame->guard = host_lock_IoMutex_lock(table, &STORE_SESSION_SESSION_LOCK_TYPE, &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/* store_session_unlocked releases the lock of the backend's session repository. */
static void
store_session_unlocked(store_session_session_repo_frame_0_t *frame)
{
	frame->guard_live = false;
	host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
}

/* store_session_session_repo_body_0 is session_repo's body: the stored repository cloned, or one opened and stored. */
static void
store_session_session_repo_body_0(void *frame_)
{
	store_session_session_repo_frame_0_t *frame = frame_;
	StoreSessionHeld *held;
	ChunksErrorError failed;
	uint32		database;
	uint32		tablespace;
	uint32		own;

	frame->ok = false;
	frame->local = host_local_BackendLocal_get(&STORE_SESSION_SESSION.local, &STORE_SESSION_SESSION_TYPE);
	frame->local_live = true;
	held = store_session_locked(frame);
	if (held->has)
	{
		*frame->result = std_arc_clone(held->repo);
		frame->ok = true;
		goto done;
	}
	store_session_unlocked(frame);
	if (!host_session_database(&database, &tablespace, frame->error) ||
		!store_databases_committed(frame->io, database, tablespace, &own, frame->error))
		goto done;
	if (!repo_Repo_open(frame->io, database, own, &frame->repo, &failed))
	{
		*frame->error = chunks_packs_shared_raised(&failed);
		goto done;
	}
	frame->opened = std_arc_new_dropping(&frame->repo, sizeof(RepoRepo), _Alignof(RepoRepo), store_session_Repo_drop);
	frame->opened_live = true;
	held = store_session_locked(frame);
	store_session_Held_drop(held);
	held->has = true;
	held->repo = std_arc_clone(frame->opened);
	store_session_unlocked(frame);
	*frame->result = frame->opened;
	frame->opened_live = false;
	frame->ok = true;
done:
	store_session_session_repo_pad_0(frame);
}

/*
 * store_session_session_repo returns the repository of the session's
 * database, opened at the backend's first call with no lock held. It clones
 * the table's Arc, since the table and the caller share the repository; an
 * open that fails stores nothing, so the next call opens again.
 */
bool
store_session_session_repo(HostIoIo *io, StdArcInner **result, HostError *error)
{
	store_session_session_repo_frame_0_t frame = {.io = io, .result = result, .error = error};

	std_landing(&frame, store_session_session_repo_body_0, store_session_session_repo_pad_0);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_SESSION_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_SESSION_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* STORE_SESSION_TESTS_UNWRAP_ERR is Result::unwrap_err's panic text. */
#define STORE_SESSION_TESTS_UNWRAP_ERR "called `Result::unwrap_err()` on an `Ok` value"

/* store_session_tests_Arc_drop drops an Arc<Repo>. */
static void
store_session_tests_Arc_drop(StdArcInner *repo)
{
	std_arc_drop(repo, sizeof(RepoRepo), _Alignof(RepoRepo), store_session_Repo_drop);
}

/*
 * store_session_tests_ended closes the running backend's session repository
 * and frees its record, as its exit and a restart leave it, and removes the
 * repository's directory, so that no other test's start finds it.
 */
static void
store_session_tests_ended(HostIoIo *io, uint32 number)
{
	HostLocalLocal local = host_local_BackendLocal_get(&STORE_SESSION_SESSION.local, &STORE_SESSION_SESSION_TYPE);
	HostLockIoMutex *table = (HostLockIoMutex *) host_local_Local_Deref_deref(&local, &STORE_SESSION_SESSION_TYPE);
	HostLockLockedRwLockWriteGuard guard = host_lock_IoMutex_lock(table, &STORE_SESSION_SESSION_LOCK_TYPE, &io->held);
	StoreSessionHeld *place = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&guard);
	StoreSessionHeld held = *place;
	StdPathBuf	dir;
	StdIoError	io_error;

	store_session_Held_Default_default(place);
	host_lock_LockedRwLockWriteGuard_drop(&guard);
	host_local_Local_drop(&local, &STORE_SESSION_SESSION_TYPE);
	if (held.has)
	{
		HostSharedReader reading;
		HostSharedOwned owned;
		ChunksErrorError failed;
		HostError	error;
		RepoRepo	repo;
		uint64		location;
		bool		has;

		if (!host_shared_read(io, &reading, &error))
			STD_RESULT_EXPECT_FAILED(58, 63, STORE_SESSION_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
		location = chunks_packs_backend_Packs_location(&((const RepoRepo *) std_arc_Deref_deref(held.repo,
																								_Alignof(RepoRepo)))->packs);
		if (!std_arc_into_inner(held.repo, sizeof(RepoRepo), _Alignof(RepoRepo), &repo))
			std_option_unwrap_failed(RUST_FILE, 59, 35);
		if (!repo_Repo_close(&repo, io, &reading, &failed))
			STD_RESULT_EXPECT_FAILED(59, 64, STORE_SESSION_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
		if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location, &has,
							   &owned, &error))
			STD_RESULT_EXPECT_FAILED(60, 79, STORE_SESSION_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
		if (!has)
			std_option_unwrap_failed(RUST_FILE, 60, 88);
		host_shared_free(&owned);
		host_shared_Reader_drop(&reading);
	}
	dir = host_tablespace_dir(number, HOST_PG_DEFAULT);
	if (!std_fs_remove_dir_all(dir.bytes.ptr, dir.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(62, 81, STORE_SESSION_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_vec_free_Checked(&dir.bytes, 1, 1);
}

/* store_session_tests_repo returns the session's repository, which unwraps at a line and column. */
static StdArcInner *
store_session_tests_repo(HostIoIo *io, uint32 line, uint32 col)
{
	StdArcInner *repo;
	HostError	error;

	if (!store_session_session_repo(io, &repo, &error))
		STD_RESULT_EXPECT_FAILED(line, col, STORE_SESSION_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	return repo;
}

/* store_session_tests_shared is calls_in_one_backend_share_its_repository's backend: two calls, then its end. */
static void
store_session_tests_shared(void *captures)
{
	HostIoIo   *io = captures;
	StdArcInner *outer;
	StdArcInner *inner;

	simc_connect(store_fixtures_id(931), HOST_PG_DEFAULT);
	outer = store_session_tests_repo(io, 73, 42);
	inner = store_session_tests_repo(io, 74, 42);
	STD_ASSERT(75, 13, outer == inner, "Arc::ptr_eq(&outer, &inner)");
	store_session_tests_Arc_drop(outer);
	store_session_tests_Arc_drop(inner);
	store_session_tests_ended(io, 931);
}

/*
 * test_store_session_tests_calls_in_one_backend_share_its_repository: two
 * calls in one backend, as an entry and an entry entered inside it make,
 * share one hold of its repository.
 */
void
test_store_session_tests_calls_in_one_backend_share_its_repository(void)
{
	SimcTestIo	test_io = simc_io();

	store_fixtures_made(&test_io.io, 931);
	simc_as_actor(31, store_session_tests_shared, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* store_session_tests_failing is a_failed_open_stores_nothing's backend: a call before the repository is there, one after, then its end. */
static void
store_session_tests_failing(void *captures)
{
	HostIoIo   *io = captures;
	StdArcInner *repo;
	HostError	error;
	bool		failed;

	simc_connect(store_fixtures_id(932), HOST_PG_DEFAULT);
	failed = !store_session_session_repo(io, &repo, &error);
	if (failed)
		host_Error_drop(&error);
	else
		store_session_tests_Arc_drop(repo);
	STD_ASSERT(89, 13, failed, "session_repo(io).is_err()");
	store_fixtures_made(io, 932);
	store_session_tests_Arc_drop(store_session_tests_repo(io, 91, 35));
	store_session_tests_ended(io, 932);
}

/*
 * test_store_session_tests_a_failed_open_stores_nothing: a first call that
 * can't open the repository raises and stores nothing, so a call once the
 * repository is there opens it.
 */
void
test_store_session_tests_a_failed_open_stores_nothing(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(32, store_session_tests_failing, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* store_session_tests_refused returns the error of a call of the session's repository, whose success panics at a line and column. */
static HostError
store_session_tests_refused(HostIoIo *io, uint32 line, uint32 col)
{
	StdArcInner *repo;
	HostError	error;

	if (store_session_session_repo(io, &repo, &error))
	{
		store_session_tests_Arc_drop(repo);
		STD_RESULT_EXPECT_FAILED(line, col, STORE_SESSION_TESTS_UNWRAP_ERR, STD_DANGLING, std_unit_Debug_fmt);
	}
	return error;
}


/* store_session_tests_refusing_t is a_refused_repository_raises_its_refusal's backend's captures: the test's door and the pack table's file. */
typedef struct store_session_tests_refusing_t
{
	HostIoIo   *io;
	const StdPathBuf *table;
} store_session_tests_refusing_t;

/* store_session_tests_refusing is a_refused_repository_raises_its_refusal's backend: a call the refusal fails, then its end. */
static void
store_session_tests_refusing(void *captures)
{
	const store_session_tests_refusing_t *refusing = captures;
	HostIoIo   *io = refusing->io;
	HostError	error;
	StdString	refusal;
	StdStr		shown = {refusing->table->bytes.ptr, refusing->table->bytes.len};
	StdFmtPiece pieces[] = {STD_FMT_LIT("required file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" does not exist")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	simc_connect(store_fixtures_id(933), HOST_PG_DEFAULT);
	error = store_session_tests_refused(io, 108, 52);
	std_fmt_format_Checked(&refusal, &arguments);
	store_tests_raised_eq(RUST_FILE, &error, "XX001", refusal.ptr, refusal.len, 110, 13);
	std_String_drop(&refusal);
	store_session_tests_ended(io, 933);
}

/*
 * test_store_session_tests_a_refused_repository_raises_its_refusal: a
 * repository the startup refuses, its pack table's file gone with its
 * previous generation, raises the refusal, Postgres' data-corrupted SQLSTATE
 * naming the file.
 */
void
test_store_session_tests_a_refused_repository_raises_its_refusal(void)
{
	SimcTestIo	test_io = simc_io();
	StdPathBuf	dir;
	StdPathBuf	table;
	StdPathBuf	previous;
	StdIoError	io_error;
	store_session_tests_refusing_t refusing = {&test_io.io, &table};

	store_fixtures_made(&test_io.io, 933);
	dir = host_tablespace_dir(933, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	if (!std_fs_remove_file(table.bytes.ptr, table.bytes.len, &io_error))
		STD_RESULT_EXPECT_FAILED(104, 38, STORE_SESSION_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	previous = chunks_file_previous(table.bytes.ptr, table.bytes.len);
	(void) std_fs_remove_file(previous.bytes.ptr, previous.bytes.len, &io_error);
	std_vec_free_Checked(&previous.bytes, 1, 1);
	simc_as_actor(33, store_session_tests_refusing, &refusing);
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_session_tests_unconnected is a_process_with_no_database_has_no_session_repository's process: a call with no database. */
static void
store_session_tests_unconnected(void *captures)
{
	static const char message[] = "the process is connected to no database";
	HostError	error = store_session_tests_refused(captures, 121, 52);

	store_tests_raised_eq(RUST_FILE, &error, "XX000", message, strlen(message), 122, 13);
}

/*
 * test_store_session_tests_a_process_with_no_database_has_no_session_repository:
 * a process connected to no database has no session repository, an
 * internal error.
 */
void
test_store_session_tests_a_process_with_no_database_has_no_session_repository(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(34, store_session_tests_unconnected, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* store_session_tests_failed is the exiting backend's map of chunks' error to an internal error with its text, which drops it. */
static HostError
store_session_tests_failed(ChunksErrorError *error)
{
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{error, chunks_error_Error_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	chunks_error_Error_drop(error);
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/*
 * store_session_tests_exiting is a_backends_exit_appends_its_session_repositorys_deferred_adds's
 * backend: it makes a pack of one chunk through its session repository's
 * packs, defers its add, and is terminated.
 */
static bool
store_session_tests_exiting(void *captures, HostIoIo *io, HostError *error)
{
	uint64	   *told = captures;
	StdArcInner *repo;
	const RepoRepo *held;
	HostSharedReader reading;
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF, .addresses = {.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS}};
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
	ChunksPackPackWriterInDir writer;
	ChunksTrailerLocator at;
	ChunksPackPack pack;
	ChunksErrorError failed;
	StdVec		hashed;
	StdVec		bytes;
	bool		ok;

	simc_connect(store_fixtures_id(935), HOST_PG_DEFAULT);
	if (!store_session_session_repo(io, &repo, error))
		return false;
	held = std_arc_Deref_deref(repo, _Alignof(RepoRepo));
	*told = chunks_packs_backend_Packs_location(&held->packs);
	if (!host_shared_read(io, &reading, error))
	{
		store_session_tests_Arc_drop(repo);
		return false;
	}
	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, (const uint8 *) "one", 3, &hashed);
	chunks_trailer_store(hashed.ptr, hashed.len, NULL, 0, &stamps, &bytes);
	chunks_trailer_Stamps_drop(&stamps);
	ok = chunks_packs_backend_Packs_writer(&held->packs, io, &reading, 0, &writer, &failed);
	if (ok)
	{
		ok = chunks_pack_PackWriterInDir_write(&writer, io, chunks_address_Address_of(hashed.ptr, hashed.len),
											   bytes.ptr, bytes.len, false, &at, &failed);
		if (!ok)
			chunks_pack_PackWriterInDir_drop(&writer);
		else
			ok = chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &failed) &&
				chunks_packs_backend_Packs_add_deferred(&held->packs, HOST_IO_IO_TAKES_SHORT(io), &reading, &pack,
														&failed);
	}
	if (!ok)
	{
		*error = store_session_tests_failed(&failed);
		goto made;
	}
	store_session_tests_Arc_drop(repo);
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-case-exiting", error))
		goto dropped;
	for (;;)
		(void) host_unique();
made:
	store_session_tests_Arc_drop(repo);
dropped:
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&hashed, 1, 1);
	host_shared_Reader_drop(&reading);
	return false;
}

/* store_session_tests_Ending_pair_Debug_fmt is an actor's (String, Ending)'s Debug. */
static bool
store_session_tests_Ending_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const simc_TupleStringEnding_t *pair = value;
	StdTupleRef tuple = {2, {STD_ITEM(&pair->f0, std_String_Debug_fmt), STD_ITEM(&pair->f1, simc_Ending_Debug_fmt)}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* store_session_tests_Endings_Debug_fmt is a Vec<(String, Ending)>'s Debug. */
static bool
store_session_tests_Endings_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *endings = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = endings->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(endings, simc_TupleStringEnding_t, k),
							store_session_tests_Ending_pair_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * test_store_session_tests_a_backends_exit_appends_its_session_repositorys_deferred_adds:
 * a backend terminated holding its session repository, whose packs defer an
 * add, appends the add to the list files as it exits: a record made again
 * from them lists the pack beside the one the repository's save wrote.
 */
void
test_store_session_tests_a_backends_exit_appends_its_session_repositorys_deferred_adds(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint64		at = 0;
	SimcActor	backend = {store_session_tests_exiting, &at};
	SimcEnding	terminated = {SIMC_ENDING_TERMINATED};
	SimcRun		run;
	SimcReport	report;
	const StdVec *endings;
	HostSharedReader reading;
	HostSharedOwned owned;
	ChunksPacksBackendPacks again;
	ChunksErrorError failed;
	HostError	error;
	StdVec		numbers;
	size_t		listed;
	size_t		two = 2;
	bool		has;

	store_fixtures_made(io, 935);
	run = simc_Run_new(41);
	simc_Run_actor(&run, "backend", 7, backend);
	simc_Run_until(&run, "backend", 7, "doltgres-store-case-exiting");
	simc_Run_terminate_at(&run, "backend", 7, 5000);
	report = simc_Run_finish(run);
	if (report.outcome.tag != SIMC_OUTCOME_ENDED)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 163, 67, &arguments);
	}
	endings = &report.outcome.u.ended;
	std_index_check(0, endings->len, RUST_FILE, 164, 20);
	if (!simc_Ending_PartialEq_eq(&STD_VEC_AT(endings, simc_TupleStringEnding_t, 0).f1, &terminated))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{endings, store_session_tests_Endings_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_assert_failed(RUST_FILE, 164, 9, "==", &STD_VEC_AT(endings, simc_TupleStringEnding_t, 0).f1,
						  simc_Ending_Debug_fmt, &terminated, simc_Ending_Debug_fmt, &arguments);
	}
	if (!host_shared_read(io, &reading, &error))
		STD_RESULT_EXPECT_FAILED(165, 46, STORE_SESSION_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	if (!host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, at, &has, &owned,
						   &error))
		STD_RESULT_EXPECT_FAILED(166, 93, STORE_SESSION_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, 166, 102);
	host_shared_free(&owned);
	if (!chunks_packs_open_Packs_open(io, store_fixtures_id(935), HOST_PG_DEFAULT, &again, &failed))
		STD_RESULT_EXPECT_FAILED(167, 72, STORE_SESSION_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	if (!chunks_packs_backend_Packs_numbers(&again, io, &reading, &numbers, &failed))
		STD_RESULT_EXPECT_FAILED(168, 48, STORE_SESSION_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	listed = numbers.len;
	STD_ASSERT_EQ_MSG(168, 9, listed == two, &listed, std_usize_Debug_fmt, &two, std_usize_Debug_fmt,
					  "the exit left an add deferred");
	std_vec_free_Checked(&numbers, sizeof(uint64), _Alignof(uint64));
	chunks_packs_backend_Packs_drop(&again);
	host_shared_Reader_drop(&reading);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_STORE */
