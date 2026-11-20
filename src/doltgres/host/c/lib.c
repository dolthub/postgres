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
 * The seams the storage crates reach Postgres through (design item 14), as
 * one trait, and the doors that call the host installed for the process: the
 * server's (pghost) or, in tests, the simulation's. The storage crates reach
 * a seam only through these doors, so the simulation runs the functions the
 * server runs with only the seams swapped. Its seams so far are the clock and
 * the process (the time of day, a monotonic clock, the process id, and
 * numbers no other call in the process returns), the file door (positioned
 * reads and writes, syncs, durable renames and removals, directories), the
 * WAL (inserting the fork's records, flushing, and holding off a
 * checkpoint's start), the check for a pending interrupt, zstd's compression
 * with trained dictionaries, and the simulation's actor that runs, whose
 * backend-local state is its own (local). Its doors that can raise take an
 * Io, the entry's capability (io), and return an Error the entry raises, and
 * its locks keep file I/O out from under the locks that don't allow it
 * (lock).
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/lib.h"
#include "doltgres/host/local.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "lib.rs"

/* host_MayRaise_of holds the capability an Io borrows, for a door's call of a seam. */
HostMayRaise
host_MayRaise_of(StdAny raise)
{
	return (HostMayRaise)
	{
		raise
	};
}

/* host_MayRaise_inner returns the capability of pg's it holds, for pghost's calls into Postgres. */
StdAny
host_MayRaise_inner(const HostMayRaise *self)
{
	return self->f0;
}

/* host_doors returns what a seam that can raise takes, from the Io or the view of one a door got. */
HostMayRaise
host_doors(HostIoReadDoors io)
{
	HostIoHeld *held;
	StdAny		raise;

	io.ops->sealed.parts(io.self, &held, &raise);
	return host_MayRaise_of(raise);
}


/*
 * host_Error_fork makes the fork's own error, with its SQLSTATE and message
 * in Postgres' wording; it takes the message.
 */
HostError
host_Error_fork(const uint8 *sqlstate, StdString *message)
{
	HostError	error;

	memcpy(error.sqlstate, sqlstate, 5);
	error.cause.tag = HOST_CAUSE_FORK;
	error.cause.u.fork.f0.tag = STD_COW_STR_OWNED;
	error.cause.u.fork.f0.u.owned = *message;
	error.cause.u.fork.f1 = (StdStr)
	{
		NULL, 0
	};
	return error;
}

/* host_Error_hinted makes the fork's own error with a hint, both in Postgres' wording. */
HostError
host_Error_hinted(const uint8 *sqlstate, const char *message, size_t message_len, const char *hint,
				  size_t hint_len)
{
	HostError	error;

	memcpy(error.sqlstate, sqlstate, 5);
	error.cause.tag = HOST_CAUSE_FORK;
	error.cause.u.fork.f0.tag = STD_COW_STR_BORROWED;
	error.cause.u.fork.f0.u.borrowed = (StdStr)
	{
		message, message_len
	};
	error.cause.u.fork.f1 = (StdStr)
	{
		hint, hint_len
	};
	return error;
}

/* host_Error_out_of_memory is Postgres' error of an allocation that failed, which it makes without allocating. */
HostError
host_Error_out_of_memory(void)
{
	HostError	error;

	memcpy(error.sqlstate, "53200", 5);
	error.cause.tag = HOST_CAUSE_FORK;
	error.cause.u.fork.f0.tag = STD_COW_STR_BORROWED;
	error.cause.u.fork.f0.u.borrowed = STD_STR("out of memory");
	error.cause.u.fork.f1 = (StdStr)
	{
		NULL, 0
	};
	return error;
}

/*
 * host_Error_postgres holds an error Postgres raised, with its SQLSTATE; only
 * pghost makes one; it takes the boxed error.
 */
HostError
host_Error_postgres(const uint8 *sqlstate, StdAny *error)
{
	HostError	made;

	memcpy(made.sqlstate, sqlstate, 5);
	made.cause.tag = HOST_CAUSE_POSTGRES;
	made.cause.u.postgres = *error;
	return made;
}

/* host_Error_sqlstate returns the error's SQLSTATE. */
const uint8 *
host_Error_sqlstate(const HostError *self)
{
	return self->sqlstate;
}

/* host_Error_message returns the fork's own error's message, and nothing for one Postgres raised. */
StdStr
host_Error_message(const HostError *self)
{
	switch (self->cause.tag)
	{
		case HOST_CAUSE_FORK:
			return std_cow_str_deref(&self->cause.u.fork.f0);
		case HOST_CAUSE_POSTGRES:
			return (StdStr)
			{
				NULL, 0
			};
	}
	pg_unreachable();
}

/* host_Error_hint returns the fork's own error's hint, and nothing for one with none or one Postgres raised. */
StdStr
host_Error_hint(const HostError *self)
{
	switch (self->cause.tag)
	{
		case HOST_CAUSE_FORK:
			return self->cause.u.fork.f1;
		case HOST_CAUSE_POSTGRES:
			return (StdStr)
			{
				NULL, 0
			};
	}
	pg_unreachable();
}

/*
 * host_Error_into_cause gives pghost what the error raises: Postgres' error,
 * or the fork's own message and hint; it takes the error.
 */
bool
host_Error_into_cause(HostError *self, StdAny *result, HostForked *error)
{
	switch (self->cause.tag)
	{
		case HOST_CAUSE_POSTGRES:
			*result = self->cause.u.postgres;
			return true;
		case HOST_CAUSE_FORK:
			memcpy(error->f0, self->sqlstate, 5);
			if (self->cause.u.fork.f0.tag == STD_COW_STR_OWNED)
				error->f1 = self->cause.u.fork.f0.u.owned;
			else
				std_string_from_Checked(&error->f1, self->cause.u.fork.f0.u.borrowed.ptr,
										self->cause.u.fork.f0.u.borrowed.len);
			error->f2 = self->cause.u.fork.f1;
			return false;
	}
	pg_unreachable();
}

/* host_Error_drop is an error's drop glue: its cause's, by its variant. */
void
host_Error_drop(HostError *self)
{
	switch (self->cause.tag)
	{
		case HOST_CAUSE_POSTGRES:
			std_any_box_drop(&self->cause.u.postgres);
			break;
		case HOST_CAUSE_FORK:
			std_cow_str_drop(&self->cause.u.fork.f0);
			break;
	}
}

/* host_OptionRefStr_Debug_fmt is the Debug of an error's Option<&str> message. */
static bool
host_OptionRefStr_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *message = (const StdStr *) value;
	StdDebugTuple b;

	if (message->ptr == NULL)
		return std_fmt_write_str(f, "None", 4);
	b = std_fmt_debug_tuple(f, "Some", 4);
	std_DebugTuple_field(&b, message, std_str_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/*
 * host_Error_Debug_fmt shows an error's SQLSTATE and, for the fork's own, its
 * message, as a test's failure reports it.
 */
bool
host_Error_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostError *self = (const HostError *) value;
	volatile StdCowStr sqlstate;
	volatile bool sqlstate_live = false;
	bool		ok;
	StdUnwind	unwind;

	std_string_from_utf8_lossy(self->sqlstate, 5, (StdCowStr *) &sqlstate);
	sqlstate_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		StdStr		shown = std_cow_str_deref((StdCowStr *) &sqlstate);
		StdStr		message;
		StdDebugStruct b;

		b = std_fmt_debug_struct(f, "Error", 5);
		std_DebugStruct_field(&b, "sqlstate", 8, &shown, std_str_Debug_fmt);
		message = host_Error_message(self);
		std_DebugStruct_field(&b, "message", 7, &message, host_OptionRefStr_Debug_fmt);
		ok = std_DebugStruct_finish(&b);
		sqlstate_live = false;
		std_cow_str_drop((StdCowStr *) &sqlstate);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (sqlstate_live)
			std_cow_str_drop((StdCowStr *) &sqlstate);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* host_File_number returns the host's number for the file, which a test compares across openings. */
int32
host_File_number(const HostFile *self)
{
	return self->number;
}

/* host_File_drop closes the file. */
void
host_File_drop(HostFile *self)
{
	self->host.ops->close(self->host.self, self->number);
}

/* host_ScratchFile_Deref_deref is a scratch file's File. */
const HostFile *
host_ScratchFile_Deref_deref(const HostScratchFile *self)
{
	return &self->file;
}

/* host_ScratchFile_drop is a scratch file's drop glue, its file's close. */
void
host_ScratchFile_drop(HostScratchFile *self)
{
	host_File_drop(&self->file);
}

/* host_Snapshot_made is a host's making of a snapshot from its own, for pghost and the simulation. */
HostSnapshot
host_Snapshot_made(HostSnapshotKind kind, HostXid xmin, HostXid xmax, HostCommand command,
				   bool recovery, bool overflowed, const uint32 *xip, size_t xip_len,
				   const uint32 *subxip, size_t subxip_len, StdAny of)
{
	return (HostSnapshot)
	{
		kind, xmin, xmax, command, recovery, overflowed, xip, xip_len,
			subxip, subxip_len, of
	};
}

/* host_Snapshot_of returns the host's own snapshot the value was read from, for that host's doors. */
StdAny
host_Snapshot_of(const HostSnapshot *self)
{
	return self->of;
}

/* host_WalKind_of returns the kind with a value, or nothing for a value no kind has. */
bool
host_WalKind_of(uint8 value, HostWalKind *result)
{
	switch (value)
	{
		case 4:
			*result = HOST_WAL_KIND_REPOSITORY_CREATED;
			return true;
		case 5:
			*result = HOST_WAL_KIND_REPOSITORY_DROPPED;
			return true;
		case 6:
			*result = HOST_WAL_KIND_REPOSITORY_COPIED;
			return true;
		case 7:
			*result = HOST_WAL_KIND_REPOSITORY_MOVED;
			return true;
		case 1:
			*result = HOST_WAL_KIND_ROW_CHANGE;
			return true;
		case 2:
			*result = HOST_WAL_KIND_PAGE_LEFT;
			return true;
		case 3:
			*result = HOST_WAL_KIND_CHUNK;
			return true;
#ifdef USE_INJECTION_POINTS
		case 15:
			*result = HOST_WAL_KIND_TEST;
			return true;
#endif
		case 8:
			*result = HOST_WAL_KIND_BASE_ADVANCED;
			return true;
		case 9:
			*result = HOST_WAL_KIND_STORAGE_EMPTIED;
			return true;
		case 10:
			*result = HOST_WAL_KIND_CONFIRM;
			return true;
		case 11:
			*result = HOST_WAL_KIND_TIDS_FREED;
			return true;
		case 12:
			*result = HOST_WAL_KIND_MAP_CARRIED;
			return true;
		case 13:
			*result = HOST_WAL_KIND_DATABASE_CHANGE;
			return true;
		default:
			return false;
	}
}

/* host_WalKind_value returns the kind's value. */
uint8
host_WalKind_value(HostWalKind self)
{
	switch (self)
	{
		case HOST_WAL_KIND_REPOSITORY_CREATED:
			return 4;
		case HOST_WAL_KIND_REPOSITORY_DROPPED:
			return 5;
		case HOST_WAL_KIND_REPOSITORY_COPIED:
			return 6;
		case HOST_WAL_KIND_REPOSITORY_MOVED:
			return 7;
		case HOST_WAL_KIND_ROW_CHANGE:
			return 1;
		case HOST_WAL_KIND_PAGE_LEFT:
			return 2;
		case HOST_WAL_KIND_CHUNK:
			return 3;
#ifdef USE_INJECTION_POINTS
		case HOST_WAL_KIND_TEST:
			return 15;
#endif
		case HOST_WAL_KIND_BASE_ADVANCED:
			return 8;
		case HOST_WAL_KIND_STORAGE_EMPTIED:
			return 9;
		case HOST_WAL_KIND_CONFIRM:
			return 10;
		case HOST_WAL_KIND_TIDS_FREED:
			return 11;
		case HOST_WAL_KIND_MAP_CARRIED:
			return 12;
		case HOST_WAL_KIND_DATABASE_CHANGE:
			return 13;
	}
	pg_unreachable();
}

/* host_WalKind_name returns the kind's name, which pg_walinspect shows as the record's type. */
const char *
host_WalKind_name(HostWalKind self)
{
	switch (self)
	{
		case HOST_WAL_KIND_REPOSITORY_CREATED:
			return "RepositoryCreated";
		case HOST_WAL_KIND_REPOSITORY_DROPPED:
			return "RepositoryDropped";
		case HOST_WAL_KIND_REPOSITORY_COPIED:
			return "RepositoryCopied";
		case HOST_WAL_KIND_REPOSITORY_MOVED:
			return "RepositoryMoved";
		case HOST_WAL_KIND_ROW_CHANGE:
			return "RowChange";
		case HOST_WAL_KIND_PAGE_LEFT:
			return "PageLeft";
		case HOST_WAL_KIND_CHUNK:
			return "Chunk";
		case HOST_WAL_KIND_BASE_ADVANCED:
			return "BaseAdvanced";
		case HOST_WAL_KIND_STORAGE_EMPTIED:
			return "StorageEmptied";
		case HOST_WAL_KIND_CONFIRM:
			return "Confirm";
		case HOST_WAL_KIND_TIDS_FREED:
			return "TidsFreed";
		case HOST_WAL_KIND_MAP_CARRIED:
			return "MapCarried";
		case HOST_WAL_KIND_DATABASE_CHANGE:
			return "DatabaseChange";
#ifdef USE_INJECTION_POINTS
		case HOST_WAL_KIND_TEST:
			return "Test";
#endif
	}
	pg_unreachable();
}

/* host_DelayCheckpoint_finish ends the window once its change is visible; it takes the window, whose drop follows. */
void
host_DelayCheckpoint_finish(HostDelayCheckpoint *self)
{
	self->finished = true;
	host_DelayCheckpoint_drop(self);
}

/* host_DelayCheckpoint_drop lets checkpoints start again, stopping the server for a window left unfinished. */
void
host_DelayCheckpoint_drop(HostDelayCheckpoint *self)
{
	self->host.ops->resume_checkpoints(self->host.self, self->finished);
}

/* host_CompressDictionary_new wraps a dictionary a host prepared; it takes the box. */
HostCompressDictionary
host_CompressDictionary_new(StdAny *prepared)
{
	return (HostCompressDictionary)
	{
		*prepared
	};
}

/* host_CompressDictionary_prepared returns the dictionary in the form its host prepared it. */
StdAny
host_CompressDictionary_prepared(const HostCompressDictionary *self)
{
	return self->f0;
}

/* host_CompressDictionary_drop is the dictionary's drop glue, its box's. */
void
host_CompressDictionary_drop(HostCompressDictionary *self)
{
	std_any_box_drop(&self->f0);
}

/* host_DecompressDictionary_new wraps a dictionary a host prepared; it takes the box. */
HostDecompressDictionary
host_DecompressDictionary_new(StdAny *prepared)
{
	return (HostDecompressDictionary)
	{
		*prepared
	};
}

/* host_DecompressDictionary_prepared returns the dictionary in the form its host prepared it. */
StdAny
host_DecompressDictionary_prepared(const HostDecompressDictionary *self)
{
	return self->f0;
}

/* host_DecompressDictionary_drop is the dictionary's drop glue, its box's. */
void
host_DecompressDictionary_drop(HostDecompressDictionary *self)
{
	std_any_box_drop(&self->f0);
}

/* HOST_HOST is the OnceLock of the host installed for the process. */
StdOnceLock HOST_HOST;

/* HOST_HOST_VALUE is HOST_HOST's value. */
HostHost	HOST_HOST_VALUE;

/* HOST_READING is the OnceLock of the reads of the process's host. */
static StdOnceLock HOST_READING;

/* HOST_READING_VALUE is HOST_READING's value. */
static HostReadHost HOST_READING_VALUE;

/* host_Storage_PartialEq_eq reports whether two storages have the same locator. */
bool
host_Storage_PartialEq_eq(const HostStorage *self, const HostStorage *other)
{
	return self->tablespace == other->tablespace && self->database == other->database &&
		self->relation == other->relation;
}

/* host_Storage_Debug_fmt is Storage's derived Debug. */
bool
host_Storage_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostStorage *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Storage", 7);

	std_DebugStruct_field(&s, "tablespace", 10, &self->tablespace, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "database", 8, &self->database, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "relation", 8, &self->relation, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* host_Xid_PartialEq_eq reports whether two xids are the same. */
bool
host_Xid_PartialEq_eq(const HostXid *self, const HostXid *other)
{
	return self->f0 == other->f0;
}

/* host_Xid_Debug_fmt is Xid's derived Debug. */
bool
host_Xid_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostXid *self = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, "Xid", 3);

	std_DebugTuple_field(&t, &self->f0, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* host_Row_PartialEq_eq reports whether two tuple ids are the same. */
bool
host_Row_PartialEq_eq(const HostRow *self, const HostRow *other)
{
	return self->f0 == other->f0;
}

/* host_Row_Debug_fmt is Row's derived Debug. */
bool
host_Row_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostRow *self = value;
	StdDebugTuple t = std_fmt_debug_tuple(f, "Row", 3);

	std_DebugTuple_field(&t, &self->f0, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* host_Refused_Debug_fmt is Refused's derived Debug: its name. */
bool
host_Refused_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write_str(f, "Refused", 7);
}

/* host_install installs the process's host, refusing to replace the one it has. */
bool
host_install(HostHost host, HostRefused *error)
{
	HostReadHost reading;

	if (!std_once_lock_set(&HOST_HOST, &HOST_HOST_VALUE, &host, sizeof(host)))
	{
		*error = (HostRefused)
		{
			0
		};
		return false;
	}
	reading.ops = &host.ops->read_host;
	reading.self = host.self;
	if (!std_once_lock_set(&HOST_READING, &HOST_READING_VALUE, &reading, sizeof(reading)))
	{
		*error = (HostRefused)
		{
			0
		};
		return false;
	}
	return true;
}

#ifdef FRONTEND
/*
 * host_install_reading installs a frontend program's host, which gives the
 * reads alone, refusing to replace the one it has.
 */
bool
host_install_reading(HostReadHost host, HostRefused *error)
{
	if (!std_once_lock_set(&HOST_READING, &HOST_READING_VALUE, &host, sizeof(host)))
	{
		*error = (HostRefused)
		{
			0
		};
		return false;
	}
	return true;
}
#endif

/* host_installed returns whether the process has a host. */
bool
host_installed(void)
{
	return std_once_lock_get(&HOST_HOST);
}

/* host_acting returns whether the running backend is one of the simulation's actors. */
bool
host_acting(void)
{
	uint64		actor;

	return std_once_lock_get(&HOST_HOST) && HOST_HOST_VALUE.ops->actor(HOST_HOST_VALUE.self, &actor);
}

/* host_yield_point passes the host's yield point before a backend-local lock's take, if a host is installed. */
void
host_yield_point(void)
{
	if (std_once_lock_get(&HOST_HOST))
		HOST_HOST_VALUE.ops->yield_point(HOST_HOST_VALUE.self);
}

/*
 * host_at_exit has the calling process run an exit work as it exits, once
 * however often it is registered. A crate registers its work before it takes
 * what the work cleans up, and a registration made once the process's exit
 * work has begun is refused.
 */
bool
host_at_exit(const StdTypeId *e_type, const HostExitOps *e, HostError *error)
{
	HostHost	host = host_current();

	return host.ops->at_exit(host.self, e_type, e->run, error);
}

/* host_current returns the process's host, which every entry installs before a storage crate runs. */
HostHost
host_current(void)
{
	if (!std_once_lock_get(&HOST_HOST))
		STD_OPTION_EXPECT_FAILED(1184, 25, "a host is installed before any storage crate runs");
	return HOST_HOST_VALUE;
}

/* host_reading returns the reads of the process's host. */
HostReadHost
host_reading(void)
{
	if (!std_once_lock_get(&HOST_READING))
		STD_OPTION_EXPECT_FAILED(1189, 28, "a host is installed before any storage crate runs");
	return HOST_READING_VALUE;
}

#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_HOST_TESTING)
/*
 * host_pause holds the calling process at a named place when the fork's tests
 * ask for it. A crate reaches it only through pause!, whose name is a
 * literal.
 */
bool
host_pause(HostSharedPauses at, const char *point, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_MayRaise_of(host_shared_raising(at));

	return host.ops->pause(host.self, &raise, point, error);
}

/*
 * host_injected says whether the fork's tests attached a named injection
 * point, where a crate takes a test's path.
 */
bool
host_injected(HostIoIoDoors io, const char *point, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->injected(host.self, &raise, point, result, error);
}
#else
/*
 * host_injected is false in a build with neither injection points nor the
 * tests' capability: a constant function, which the compiler holds to calling
 * no host.
 */
bool
host_injected(HostIoIoDoors io, const char *point, bool *result, HostError *error)
{
	*result = false;
	return true;
}

/*
 * host_pause is nothing in a build with neither injection points nor the
 * tests' capability: a constant function, which the compiler holds to calling
 * no host.
 */
bool
host_pause(HostSharedPauses at, const char *point, HostError *error)
{
	return true;
}
#endif

/* host_now returns the time of day. */
StdSystemTime
host_now(void)
{
	HostHost	host = host_current();

	return host.ops->now(host.self);
}

/* host_instant returns a reading of the monotonic clock. */
StdInstant
host_instant(void)
{
	HostHost	host = host_current();

	return host.ops->instant(host.self);
}

/* host_elapsed returns the time since a reading of instant, or zero for a reading after now. */
StdDuration
host_elapsed(StdInstant since)
{
	StdInstant	now = host_instant();

	return std_Instant_saturating_duration_since(&now, since);
}

/* host_process_id returns the process's id. */
uint32
host_process_id(void)
{
	HostHost	host = host_current();

	return host.ops->process_id(host.self);
}

/*
 * host_session_database returns the database the session is connected to, by
 * its id and the tablespace of its own directory; a process connected to no
 * database has none, an internal error, since only a connected backend asks.
 */
bool
host_session_database(uint32 *result_0, uint32 *result_1, HostError *error)
{
	HostHost	host = host_current();

	if (!host.ops->session_database(host.self, result_0, result_1))
	{
		static const char text[] = "the process is connected to no database";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	return true;
}

/*
 * host_exiting returns whether the process's exit has begun, after which a
 * frame a FATAL left standing never runs again.
 */
bool
host_exiting(void)
{
	HostHost	host = host_current();

	return host.ops->exiting(host.self);
}

/* host_unique returns a number no other call of it in the process returns. */
uint64
host_unique(void)
{
	HostHost	host = host_current();

	return host.ops->unique(host.self);
}

/* host_open opens a file to write, or creates one. */
bool
host_open(HostIoIoDoors io, const char *path, size_t path_len, HostOpen how, HostFile *result,
		  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);
	int32		number;

	if (!host.ops->open_(host.self, &raise, path, path_len, how, &number, error))
		return false;
	*result = (HostFile)
	{
		number,
		{
			&host.ops->read_host, host.self
		}
	};
	return true;
}

/* host_open_to_read opens an existing file to read. */
bool
host_open_to_read(HostIoReadDoors io, const char *path, size_t path_len, HostFile *result,
				  HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);
	int32		number;

	if (!host.ops->open_to_read(host.self, &raise, path, path_len, &number, error))
		return false;
	*result = (HostFile)
	{
		number, host
	};
	return true;
}

/*
 * host_open_if_there opens a file to read, or returns nothing when no file
 * has the path: an answer, with no error raised, for a caller that asks
 * whether the file is there.
 */
bool
host_open_if_there(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
				   HostFile *result, HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);
	bool		has_number;
	int32		number;

	if (!host.ops->open_if_there(host.self, &raise, path, path_len, &has_number, &number, error))
		return false;
	*has_result = has_number;
	if (has_number)
		*result = (HostFile)
	{
		number, host
	};
	return true;
}

/*
 * host_create_if_absent creates a file and opens it to read and write, or
 * returns nothing when a file already has the path: an answer, with no error
 * raised, for a caller that asks whether the file is already there.
 */
bool
host_create_if_absent(HostIoIoDoors io, const char *path, size_t path_len, bool *has_result,
					  HostFile *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);
	bool		has_number;
	int32		number;

	if (!host.ops->create_if_absent(host.self, &raise, path, path_len, &has_number, &number,
									error))
		return false;
	*has_result = has_number;
	if (has_number)
		*result = (HostFile)
	{
		number,
		{
			&host.ops->read_host, host.self
		}
	};
	return true;
}

/* host_open_scratch opens a temporary file of the current transaction's, for work that never becomes durable. */
bool
host_open_scratch(HostIoIo *io, HostScratchFile *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	int32		number;

	if (!host.ops->open_scratch(host.self, &raise, &number, error))
		return false;
	*result = (HostScratchFile)
	{
		{
			number,
			{
				&host.ops->read_host, host.self
			}
		}
	};
	return true;
}

/* host_read_at fills a buffer from a file at an offset, or fails on a short read. */
bool
host_read_at(HostIoReadDoors io, const HostFile *file, uint64 offset, uint8 *buffer,
			 size_t buffer_len, HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);

	return host.ops->read_at(host.self, &raise, file->number, offset, buffer, buffer_len, error);
}

/* host_write_at writes bytes to a file at an offset. */
bool
host_write_at(HostIoIoDoors io, const HostFile *file, uint64 offset, const uint8 *bytes,
			  size_t bytes_len, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->write_at(host.self, &raise, file->number, offset, bytes, bytes_len, error);
}

/* host_size returns a file's size. */
bool
host_size(HostIoReadDoors io, const HostFile *file, uint64 *result, HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);

	return host.ops->size(host.self, &raise, file->number, result, error);
}

/* host_truncate cuts a file to a length. */
bool
host_truncate(HostIoIoDoors io, const HostFile *file, uint64 len, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->truncate(host.self, &raise, file->number, len, error);
}

/* host_prefetch tells the operating system that bytes of a file will be read soon; it never fails. */
void
host_prefetch(const HostFile *file, uint64 offset, uint64 len)
{
	HostReadHost host = host_reading();

	host.ops->prefetch(host.self, file->number, offset, len);
}

/* host_sync makes a file's writes durable. */
bool
host_sync(HostIoIoDoors io, const HostFile *file, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->sync(host.self, &raise, file->number, error);
}

/* host_rename renames a file, durably once its directory's sync is. */
bool
host_rename(HostIoIoDoors io, const char *from, size_t from_len, const char *to, size_t to_len,
			HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->rename_(host.self, &raise, from, from_len, to, to_len, error);
}

/* host_rename_durably renames a file, durably once it returns. */
bool
host_rename_durably(HostIoIoDoors io, const char *from, size_t from_len, const char *to,
					size_t to_len, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->rename_durably(host.self, &raise, from, from_len, to, to_len, error);
}

/* host_remove_durably removes a file, durably once it returns. */
bool
host_remove_durably(HostIoIoDoors io, const char *path, size_t path_len, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->remove_durably(host.self, &raise, path, path_len, error);
}

/*
 * host_remove_durably_if_there removes a file and reports whether one was
 * there, an answer with no error raised, with the directory's entries durable
 * once it returns either way.
 */
bool
host_remove_durably_if_there(HostIoIoDoors io, const char *path, size_t path_len, bool *result,
							 HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->remove_durably_if_there(host.self, &raise, path, path_len, result, error);
}

/*
 * host_remove_leftover removes a file a failure left behind, if it is there,
 * for the code that returns that failure: it never fails, the host reporting
 * a removal that didn't happen in its log, and it makes nothing durable, so
 * it is only for a file whose name startup's cleanup also removes. It takes
 * the capability of file I/O as every file door does, and raises nothing
 * through it.
 */
void
host_remove_leftover(HostIoIoDoors io, const char *path, size_t path_len)
{
	HostHost	host = host_current();

	host.ops->remove_leftover(host.self, path, path_len);
}

/*
 * host_tablespace_dir returns the directory of a database's versioned files
 * in a tablespace, by their ids, the same in every process:
 * pg_versioned/<database> in the data directory for pg_default, and in the
 * tablespace's version directory for any other. It does no I/O, so a
 * directory that isn't there is found by whoever opens a file in it.
 */
StdPathBuf
host_tablespace_dir(uint32 database, uint32 tablespace)
{
	volatile StdPathBuf tmp_1;
	volatile bool tmp_1_live = false;
	volatile StdString tmp_2;
	volatile bool tmp_2_live = false;
	StdPathBuf	result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		*(StdPathBuf *) &tmp_1 = host_versioned_dir(tablespace);
		tmp_1_live = true;
		std_u32_to_string(database, (StdString *) &tmp_2);
		tmp_2_live = true;
		std_path_join(((StdPathBuf *) &tmp_1)->bytes.ptr, ((StdPathBuf *) &tmp_1)->bytes.len, ((StdString *) &tmp_2)->ptr,
					  ((StdString *) &tmp_2)->len, &result);
		tmp_2_live = false;
		std_vec_free_Checked((StdString *) &tmp_2, 1, 1);
		tmp_1_live = false;
		std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_2_live)
			std_vec_free_Checked((StdString *) &tmp_2, 1, 1);
		if (tmp_1_live)
			std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		std_unwind_resume(&unwind);
	}
	return result;
}

/*
 * host_versioned_dir returns the directory of a tablespace that holds each
 * database's versioned files in a directory of its own, pg_versioned in the
 * data directory for pg_default and in the tablespace's version directory for
 * any other.
 */
StdPathBuf
host_versioned_dir(uint32 tablespace)
{
	volatile StdPathBuf tmp_1;
	volatile bool tmp_1_live = false;
	StdPathBuf	result;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostReadHost host = host_reading();

		*(StdPathBuf *) &tmp_1 = host.ops->tablespace_root(host.self, tablespace);
		tmp_1_live = true;
		std_path_join(((StdPathBuf *) &tmp_1)->bytes.ptr, ((StdPathBuf *) &tmp_1)->bytes.len, HOST_VERSIONED,
					  strlen(HOST_VERSIONED), &result);
		tmp_1_live = false;
		std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_1_live)
			std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		std_unwind_resume(&unwind);
	}
	return result;
}

/* host_OptionVecString_drop drops host_tablespaces' names, an Option<Vec<String>>. */
static void
host_OptionVecString_drop(bool has_names, StdVec *names)
{
	if (!has_names)
		return;
	for (size_t i = 0, i_len = names->len; i < i_len; i++)
		std_vec_free_Checked(&STD_VEC_AT(names, StdString, i), 1, 1);
	std_vec_free_Checked(names, sizeof(StdString), _Alignof(StdString));
}

/*
 * std_u32_PartialOrd_lt_FnMut_call_mut is the fn item u32's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_u32_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_u32_PartialOrd_lt(a, b);
}

/* sort_unstable at u32, which tablespaces sorts its ids with. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U32
#define STD_SORT_T uint32
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT std_u32_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/sort_template.h"

/*
 * host_tablespaces returns the ids of the server's tablespaces that have a
 * directory of their own, in order, read from the names in the data
 * directory's pg_tblspc: every tablespace but pg_default and pg_global.
 */
bool
host_tablespaces(HostIoReadDoors io, StdVec *result, HostError *error)
{
	volatile StdPathBuf tmp_1;
	volatile bool tmp_1_live = false;
	volatile StdPathBuf tmp_2;
	volatile bool tmp_2_live = false;
	volatile bool has_names;
	volatile StdVec names;
	volatile bool names_live = false;
	volatile StdVec ids;
	volatile bool ids_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostReadHost host = host_reading();
		bool		listed;
		size_t		i = 0;
		size_t		i_len;

		*(StdPathBuf *) &tmp_1 = host.ops->tablespace_root(host.self, HOST_PG_DEFAULT);
		tmp_1_live = true;
		std_path_join(((StdPathBuf *) &tmp_1)->bytes.ptr, ((StdPathBuf *) &tmp_1)->bytes.len, "pg_tblspc", 9,
					  (StdPathBuf *) &tmp_2);
		tmp_2_live = true;
		listed = host_list_dir_if_there(io, ((StdPathBuf *) &tmp_2)->bytes.ptr, ((StdPathBuf *) &tmp_2)->bytes.len,
										(bool *) &has_names, (StdVec *) &names, error);
		tmp_2_live = false;
		std_vec_free_Checked(&((StdPathBuf *) &tmp_2)->bytes, 1, 1);
		tmp_1_live = false;
		std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		if (!listed)
		{
			std_unwind_pop(&unwind);
			return false;
		}
		names_live = true;
		std_vec_new((StdVec *) &ids, sizeof(uint32), _Alignof(uint32));
		ids_live = true;
		i_len = has_names ? names.len : 0;
		for (; i < i_len; i++)
		{
			const StdString *name = &STD_VEC_AT((StdVec *) &names, StdString, i);
			uint32		id;
			StdParseIntError parse_error;

			if (std_str_parse_u32(name->ptr, name->len, &id, &parse_error))
			{
				std_vec_collect_first((StdVec *) &ids, &id, 0, sizeof(uint32), _Alignof(uint32));
				i++;
				break;
			}
		}
		for (; i < i_len; i++)
		{
			const StdString *name = &STD_VEC_AT((StdVec *) &names, StdString, i);
			uint32		id;
			StdParseIntError parse_error;

			if (std_str_parse_u32(name->ptr, name->len, &id, &parse_error))
				std_vec_extend_one((StdVec *) &ids, &id, 0, sizeof(uint32), _Alignof(uint32));
		}
		std_slice_sort_unstable_U32((uint32 *) ((StdVec *) &ids)->ptr, ((StdVec *) &ids)->len);
		names_live = false;
		host_OptionVecString_drop(has_names, (StdVec *) &names);
		ids_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (ids_live)
			std_vec_free_Checked((StdVec *) &ids, sizeof(uint32), _Alignof(uint32));
		if (names_live)
			host_OptionVecString_drop(has_names, (StdVec *) &names);
		if (tmp_2_live)
			std_vec_free_Checked(&((StdPathBuf *) &tmp_2)->bytes, 1, 1);
		if (tmp_1_live)
			std_vec_free_Checked(&((StdPathBuf *) &tmp_1)->bytes, 1, 1);
		std_unwind_resume(&unwind);
	}
	*result = *(StdVec *) &ids;
	return true;
}

/*
 * host_rename_back takes a rename back durably after a failure, for the code
 * that returns that failure, and reports whether it did: it never fails, the
 * host reporting a rename that didn't happen in its log, so it is only for
 * names that read correctly with the rename not taken back, as they would
 * after a crash at that step. It takes the capability of file I/O as every
 * file door does, and raises nothing through it.
 */
bool
host_rename_back(HostIoIoDoors io, const char *from, size_t from_len, const char *to,
				 size_t to_len)
{
	HostHost	host = host_current();

	return host.ops->rename_back(host.self, from, from_len, to, to_len);
}

/*
 * host_remove_tree removes a directory and everything in it, as Postgres
 * removes a dropped database's, and reports whether all of it is gone: true
 * also for a directory that was never there, and false when something was
 * left, which the host reports in its log with Postgres' wording. A tree
 * another process removes while this one is removing it is answered as
 * something left, and as gone when asked again. It makes nothing durable, so
 * it is only for a directory nothing reads again, and its one error of its
 * own is a path that can't be passed.
 */
bool
host_remove_tree(HostIoIoDoors io, const char *path, size_t path_len, bool *result,
				 HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->remove_tree(host.self, &raise, path, path_len, result, error);
}

/* host_sync_path makes a file's or directory's contents durable. */
bool
host_sync_path(HostIoIoDoors io, const char *path, size_t path_len, bool directory,
			   HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->sync_path(host.self, &raise, path, path_len, directory, error);
}

/*
 * host_directory_of returns the directory a path is in, . for a one-part
 * relative path, as Postgres' fsync_parent_path takes it.
 */
StdStr
host_directory_of(const char *path, size_t path_len)
{
	StdStr		dir;

	if (std_path_parent(path, path_len, &dir) && !(dir.len == 0))
		return dir;
	return (StdStr)
	{
		".", 1
	};
}

/* host_make_dir makes a directory, if it doesn't exist. */
bool
host_make_dir(HostIoIoDoors io, const char *path, size_t path_len, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->make_dir(host.self, &raise, path, path_len, error);
}

/* host_list_dir returns the names in a directory in byte order, without . and ... */
bool
host_list_dir(HostIoReadDoors io, const char *path, size_t path_len, StdVec *result,
			  HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);

	return host.ops->list_dir(host.self, &raise, path, path_len, result, error);
}

/*
 * host_list_dir_if_there is list_dir, or nothing when no directory has the
 * path: an answer, with no error raised, for a caller that asks whether the
 * directory is there.
 */
bool
host_list_dir_if_there(HostIoReadDoors io, const char *path, size_t path_len, bool *has_result,
					   StdVec *result, HostError *error)
{
	HostReadHost host = host_reading();
	HostMayRaise raise = host_doors(io);

	return host.ops->list_dir_if_there(host.self, &raise, path, path_len, has_result, result,
									   error);
}

/*
 * host_disk_space returns the bytes still free to the server on a directory's
 * volume, and the bytes it holds, which the collector's schedule compares
 * with collector_min_free_disk (the collector text's §8.3).
 */
bool
host_disk_space(HostIoIoDoors io, const char *path, size_t path_len, uint64 *result_0, uint64 *result_1,
				HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->disk_space(host.self, &raise, path, path_len, result_0, result_1, error);
}

/*
 * host_tree_bytes returns the bytes of the files under a path, through its
 * subdirectories, 0 when nothing is there, which a database's and a
 * tablespace's sizes add for their repositories (design item 5).
 */
bool
host_tree_bytes(HostIoIoDoors io, const char *path, size_t path_len, uint64 *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->tree_bytes(host.self, &raise, path, path_len, result, error);
}

/*
 * host_nap waits on the process's latch for a while, ending early when it is
 * set and raising a pending cancel, as a collection paced by
 * collector_io_limit does (the collector text's §8.3).
 */
bool
host_nap(HostIoIoDoors io, StdDuration duration, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->nap(host.self, &raise, duration, error);
}

/*
 * host_wal_insert appends a record of a kind to the WAL, returning the
 * position just past it; a record holds at least one byte.
 */
bool
host_wal_insert(HostIoIoDoors io, HostWalKind kind, const uint8 *body, size_t body_len,
				HostLsn *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->wal_insert(host.self, &raise, kind, body, body_len, result, error);
}

/*
 * host_wal_new_cid appends heap's own record of the command ids of a row a
 * transaction wrote or changed in a relation logical decoding reads, so
 * decoding finds the row's command ids as it finds heap's (PLAN item 7's
 * historic snapshots); it returns the position just past the record.
 */
bool
host_wal_new_cid(HostIoIoDoors io, const HostStorage *storage, HostRow tid, HostNewCid cid,
				 HostLsn *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->wal_new_cid(host.self, &raise, storage, tid, cid, result, error);
}

/* host_wal_flush makes the WAL durable up to a position. */
bool
host_wal_flush(HostIoIoDoors io, HostLsn lsn, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->wal_flush(host.self, &raise, lsn, error);
}

/*
 * host_fold_point returns a fold's point, m: the WAL's insert position once
 * every commit whose record lies below it has marked the commit log and its
 * transaction's entry, so a fold asks each version's outcome once and every
 * answer agrees (the fold text's 3.1).
 */
bool
host_fold_point(HostIoIoDoors io, HostLsn *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->fold_point(host.self, &raise, result, error);
}

/*
 * host_checkpoint_now requests a fast checkpoint, forced though nothing was
 * written since the last, and waits for it.
 */
bool
host_checkpoint_now(HostIoIoDoors io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->checkpoint_now(host.self, &raise, error);
}

/*
 * host_quiet_database takes a database's lock and ends its sessions, on a hot
 * standby only, as the redo of Postgres' drop of a database does, so that a
 * redo can change the database's files under no session.
 */
bool
host_quiet_database(HostIoIoDoors io, uint32 database, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->quiet_database(host.self, &raise, database, error);
}

/* host_unquiet_database releases the lock quiet_database took, on a hot standby only. */
bool
host_unquiet_database(HostIoIoDoors io, uint32 database, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->unquiet_database(host.self, &raise, database, error);
}

/* host_last_redo returns the redo point of the last checkpoint or restartpoint completed. */
bool
host_last_redo(HostIoIoDoors io, HostLsn *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->last_redo(host.self, &raise, result, error);
}

/*
 * host_status says whether a transaction or a subtransaction is in progress,
 * committed, or aborted, in one call, so that Postgres' order of checks, in
 * progress before committed, is the door's and no caller makes them the other
 * way round (design item 7's first transaction-state seam). It checks no
 * range of the xid: every xid a caller asks is one an unfolded version or a
 * pending swap names, which item 7's rule for relfrozenxid keeps from the
 * future and from the commit log's truncation.
 */
bool
host_status(HostIoIoDoors io, HostXid xid, HostStatus *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->xid_status(host.self, &raise, xid, result, error);
}

/*
 * host_in_progress says whether a transaction or a subtransaction is in
 * progress, by the running transactions, for an xid nothing keeps the commit
 * log for (the status table's text, check 10). Postgres' check reads the
 * commit log and pg_subtrans only while a running xid at or below the asked
 * one exists, and both are truncated only below every running xid, so it
 * never reads a part they dropped. Every running xid is within 2^31 of the
 * next one, since Postgres stops assigning xids before wraparound reaches a
 * running one, so the host narrows an xid only in that range and answers no
 * for an older one.
 */
bool
host_in_progress(HostIoIoDoors io, HostXid xid, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->xid_in_progress(host.self, &raise, xid, result, error);
}

/*
 * host_xids_answered says whether in_progress answers for every running and
 * prepared transaction: outside recovery, and in a hot standby, whose
 * known-assigned xids hold the primary's, but not in crash or archive
 * recovery, whose proc array holds neither the transactions the crash cut
 * short nor the prepared ones before the end of recovery. There Postgres'
 * check answers yes for every xid only because latestCompletedXid is zero
 * until the end of recovery sets it (xlog.c's StartupXLOG, "also initialize
 * latestCompletedXid"), which nothing promises, so the fork asks this rather
 * than rest on it (bugs row 90).
 */
bool
host_xids_answered(void)
{
	HostHost	host = host_current();

	return host.ops->xids_answered(host.self);
}

/*
 * host_removable_in says whether a NonVacuumable snapshot's horizon has
 * passed an xid (seam 2's door for its kind), so that a version the xid
 * deleted is dead to everyone.
 */
bool
host_removable_in(HostIoIoDoors io, const HostSnapshot *snapshot, HostXid xid, bool *result,
				  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->removable_in(host.self, &raise, host_Snapshot_of(snapshot), xid, result,
								  error);
}

/*
 * host_current_writer returns the xid a version the current command writes
 * names as its writer, the current subtransaction's, and its top-level
 * transaction's, which the commit status table keys it by (design item 7's
 * first transaction-state seam), assigning each that has none, Postgres'
 * GetCurrentFullTransactionId and GetTopFullTransactionId. It raises where
 * Postgres assigns none: in recovery, in a parallel worker, and where the xid
 * limits stop assignment near wraparound.
 */
bool
host_current_writer(HostIoIoDoors io, HostXid *result_0, HostXid *result_1, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->current_writer(host.self, &raise, result_0, result_1, error);
}

/*
 * host_btree_compare returns how a btree class's comparison function orders
 * two values. It takes the entry's whole Io, which every lock guard of the
 * fork's borrows, so it can't be called while one is held: the function is
 * user code (design item 13).
 */
bool
host_btree_compare(HostIoIo *io, const HostCompared *compared, const uint8 *a, size_t a_len,
				   const uint8 *b, size_t b_len, int32 *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->btree_compare(host.self, &raise, compared, a, a_len, b, b_len, result,
								   error);
}

/*
 * host_reformed re-forms a fold's stored row into its table's layout in the
 * catalog (STORAGE_DESIGN §II.10). It takes the entry's whole Io, as
 * btree_compare does, so it can't be called while a lock guard of the fork's
 * is held.
 */
bool
host_reformed(HostIoIo *io, const HostReformRow *row, HostReformedRow *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->reformed(host.self, &raise, row, result, error);
}

/*
 * host_sort_key returns a text's sort key under a collation. It takes the
 * entry's whole Io, as btree_compare does, so it can't be called while a lock
 * guard of the fork's is held.
 */
bool
host_sort_key(HostIoIo *io, uint32 collation, const uint8 *text, size_t text_len, StdVec *result,
			  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->sort_key(host.self, &raise, collation, text, text_len, result, error);
}

/*
 * host_current_command returns the current command's id, Postgres'
 * GetCurrentCommandId, marking it used when the command writes, which raises
 * in a parallel worker.
 */
bool
host_current_command(HostIoIoDoors io, bool used, HostCommand *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->current_command(host.self, &raise, used, result, error);
}

/*
 * host_is_current reports whether a version's writer is the current
 * transaction or one of its subtransactions that hasn't aborted, Postgres'
 * TransactionIdIsCurrentTransactionId, which a version's command is read by.
 * It checks no range of the xid, which an unfolded version names, as status
 * checks none.
 */
bool
host_is_current(HostIoIoDoors io, HostXid xid)
{
	HostHost	host = host_current();

	return host.ops->xid_is_current(host.self, xid);
}

/*
 * host_combo_command returns the combo command id of a change at cmax of a
 * row its own transaction's writer wrote at cmin, which the change's version
 * shows as cmin and cmax of the row ((4b)'s §6): heap's
 * HeapTupleHeaderAdjustCmax, whose combo table hands out ids in the order of
 * new pairs.
 */
bool
host_combo_command(HostIoIoDoors io, HostXid writer, HostCommand cmin, HostCommand cmax,
				   HostCommand *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->combo_command(host.self, &raise, writer, cmin, cmax, result, error);
}

/*
 * host_KeyGuard_io lends the capability for the stripes' work: file I/O,
 * transactions' status, the WAL, the checkpoint window, pauses, and the
 * memtable's locks.
 */
HostIoKeyIo
host_KeyGuard_io(HostKeyGuard *self)
{
	return host_io_KeyIo_new(self->held, self->raise, &self->pushed);
}

/*
 * host_key_count reads a stripe's push count, which a resolve over a
 * per-storage stripe reads before its candidates and again under the stripe,
 * starting over when another writer pushed under the stripe between ((4b)'s
 * §3).
 */
uint64
host_key_count(uint32 stripe)
{
	HostHost	host = host_current();

	return host.ops->key_count(host.self, STD_REM_FOLDED(u32, stripe, HOST_STRIPES));
}

/*
 * host_pushed marks a push a view made, which a key lock's view keeps so its
 * stripes' release moves their push counts ((4b)'s §3), and any other view
 * ignores.
 */
void
host_pushed(HostIoTakesShort io)
{
	io.ops->io_doors.read_doors.sealed.pushed(io.self);
}

/* host_KeyHold_drop clears the stripe's hold-back flag and wakes the writers asleep on it. */
void
host_KeyHold_drop(HostKeyHold *self)
{
	self->host.ops->key_release(self->host.self, self->stripe);
}

/*
 * host_key_hold sets a stripe's hold-back flag under the stripe, whose KeyIo
 * it takes, unless another process's is set, and returns it set for the
 * running process.
 */
bool
host_key_hold(HostIoKeyIo *io, uint32 stripe, HostKeyHold *result)
{
	HostHost	host = host_current();
	uint32		stripe_2 = STD_REM_FOLDED(u32, stripe, HOST_STRIPES);
	bool		held = host.ops->key_hold(host.self, stripe_2);
	HostKeyHold made = {host, stripe_2};

	if (held)
	{
		*result = made;
		return true;
	}
	host_KeyHold_drop(&made);
	return false;
}

/*
 * host_key_wait sleeps before a writer's read while another process's
 * hold-back flag of its stripe is set ((4b)'s §3), outside every stripe,
 * since it takes the entry's Waits; a cancel and statement_timeout end it.
 */
bool
host_key_wait(HostIoWaits *waits, HostIoIoDoors io, uint32 stripe, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->key_held_wait(host.self, &raise, STD_REM_FOLDED(u32, stripe, HOST_STRIPES),
								   error);
}

/*
 * host_key_lock takes the stripes of up to two keys, each exclusively, in
 * their index order and once when both are the same, as a key-moving update
 * takes its old key's and its new key's ((4b)'s key lock).
 */
bool
host_key_lock(HostIoStriping *striping, HostIoIo *io, const uint32 *stripes, size_t stripes_len,
			  HostKeyGuard *result, HostError *error)
{
	uint32		sorted[2];
	size_t		count;
	HostHost	host;
	HostMayRaise raise;
	HostIoHeld *held;
	StdAny		raise_2;

	if (stripes_len == 1)
	{
		sorted[0] = stripes[0];
		sorted[1] = 0;
		count = 1;
	}
	else if (stripes_len == 2 && stripes[0] == stripes[1])
	{
		sorted[0] = stripes[0];
		sorted[1] = 0;
		count = 1;
	}
	else if (stripes_len == 2)
	{
		sorted[0] = std_min_u32(stripes[0], stripes[1]);
		sorted[1] = std_max_u32(stripes[0], stripes[1]);
		count = 2;
	}
	else
	{
		static const char text[] = "a key lock names other than one or two stripes";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
		return false;
	}
	std_range_to_check(count, 2, RUST_FILE, 1716, 14);
	for (size_t i = 0; i < count; i++)
	{
		if (sorted[i] >= HOST_STRIPES)
		{
			static const char text[] = "a key lock names a stripe past the last";
			StdString	message;

			std_string_from_Checked(&message, text, sizeof(text) - 1);
			*error = host_Error_fork((const uint8 *) "XX000", &message);
			return false;
		}
	}
	host = host_current();
	raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	std_range_to_check(count, 2, RUST_FILE, 1720, 38);
	if (!host.ops->key_lock(host.self, &raise, sorted, count, error))
		return false;
	host_io_Io_Sealed_parts(io, &held, &raise_2);
	*result = (HostKeyGuard)
	{
		host,
		{
			sorted[0], sorted[1]
		}, count, held, raise_2, false
	};
	return true;
}

/* host_KeyGuard_drop releases the stripes, moving their push counts when a push was made under them. */
void
host_KeyGuard_drop(HostKeyGuard *self)
{
	std_range_to_check(self->count, 2, RUST_FILE, 1727, 43);
	self->host.ops->key_unlock(self->host.self, self->stripes, self->count, self->pushed);
}

/*
 * host_xact_wait waits until a transaction ends, as XactLockTableWait does,
 * or answers whether it ended with wait false; it takes the entry's Waits, so
 * it can't be called under a key lock's stripe.
 */
bool
host_xact_wait(HostIoWaits *waits, HostIoIoDoors io, HostXid xid, bool has_on, HostOperation on,
			   bool wait, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->xact_wait(host.self, &raise, xid, has_on, on, wait, result, error);
}

/*
 * host_speculative_wait waits until a running speculative insert of a
 * transaction with a token completes, as the btree's _bt_check_unique waits
 * with SpeculativeInsertionWait; it takes the entry's Waits, so it can't be
 * called under a key lock's stripe.
 */
bool
host_speculative_wait(HostIoWaits *waits, HostIoIoDoors io, HostXid xid, uint32 token,
					  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->speculative_wait(host.self, &raise, xid, token, error);
}

/*
 * host_lock_tuple takes a row's tuple lock, as heap_acquire_tuplock does, or
 * with wait false answers none when another holds a conflicting one; it takes
 * the entry's Waits, so it can't be called under a key lock's stripe.
 */
bool
host_lock_tuple(HostIoWaits *waits, HostIoIoDoors io, HostTupleLock tuple, bool wait,
				bool *has_result, HostTupleGuard *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);
	bool		took;
	HostTupleGuard made;

	if (!host.ops->lock_tuple(host.self, &raise, tuple, wait, &took, error))
		return false;
	made = (HostTupleGuard)
	{
		host, tuple
	};
	*has_result = took;
	if (took)
		*result = made;
	else
		host_TupleGuard_drop(&made);
	return true;
}

/* host_TupleGuard_drop releases the row's tuple lock. */
void
host_TupleGuard_drop(HostTupleGuard *self)
{
	self->host.ops->unlock_tuple(self->host.self, self->tuple);
}

/* host_wal_stand returns where the server's WAL stands. */
HostWalStand
host_wal_stand(HostIoIoDoors io)
{
	HostHost	host = host_current();

	return host.ops->wal_stand(host.self);
}

/*
 * host_warn_kept_wal reads the WAL from one position to another, on a
 * timeline and up to a position, and reports at WARNING, naming the segment,
 * where it can't be read.
 */
bool
host_warn_kept_wal(HostIoIoDoors io, HostLsn from, HostLsn to, uint32 timeline, HostLsn upto,
				   HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->warn_kept_wal(host.self, &raise, from, to, timeline, upto, error);
}

/*
 * host_next_fork_record reads the first of the fork's records whose kind is
 * in a mask past a record's end, up to the replayed WAL in recovery and the
 * flushed WAL otherwise (PLAN item 8's "Standbys' roots").
 */
bool
host_next_fork_record(HostIoIoDoors io, HostLsn from, uint32 kinds, bool *has_result, HostForkRecord *result,
					  HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->next_fork_record(host.self, &raise, from, kinds, has_result, result, error);
}

/*
 * host_warn reports an error at WARNING with a context before its message,
 * for work that goes on past it in place of raising it; it takes the error.
 */
bool
host_warn(HostIoIoDoors io, HostError *error_, const char *context, HostError *error)
{
	volatile bool error_live = true;
	bool		ok;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		HostHost	host = host_current();
		HostMayRaise raise = host_doors_of_io_doors(io);

		error_live = false;
		ok = host.ops->warn(host.self, &raise, error_, context, error);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (error_live)
			host_Error_drop(error_);
		std_unwind_resume(&unwind);
	}
	return ok;
}

/* host_log reports a message in the server's log at LOG. */
bool
host_log(HostIoIoDoors io, const char *message, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	return host.ops->log(host.self, &raise, message, error);
}

/* host_refuse_recovery publishes a standby's root mismatch, with the detail of the FATAL that ends the startup process. */
void
host_refuse_recovery(HostIoIoDoors io, const char *detail, size_t detail_len)
{
	HostHost	host = host_current();

	host.ops->refuse_recovery(host.self, detail, detail_len);
}

/*
 * host_delay_checkpoint opens a window holding off the start of a checkpoint
 * until it finishes, with the names of the pauses inside it, which its open
 * loads.
 */
bool
host_delay_checkpoint(HostIoIoDoors io, const char *const *pauses, size_t pauses_len,
					  HostDelayCheckpoint *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors_of_io_doors(io);

	if (!host.ops->delay_checkpoints(host.self, &raise, pauses, pauses_len, error))
		return false;
	*result = (HostDelayCheckpoint)
	{
		host, false
	};
	return true;
}

/*
 * host_prepare_compressing prepares a zstd dictionary for compressing at a
 * level, failing with Postgres' out of memory error when it can't allocate
 * it.
 */
bool
host_prepare_compressing(const uint8 *bytes, size_t bytes_len, int32 level,
						 HostCompressDictionary *result, HostError *error)
{
	HostHost	host = host_current();

	return host.ops->prepare_compressing(host.self, bytes, bytes_len, level, result, error);
}

/*
 * host_prepare_decompressing prepares a zstd dictionary for decompressing,
 * failing with Postgres' out of memory error when it can't allocate it.
 */
bool
host_prepare_decompressing(const uint8 *bytes, size_t bytes_len,
						   HostDecompressDictionary *result, HostError *error)
{
	HostReadHost host = host_reading();

	return host.ops->prepare_decompressing(host.self, bytes, bytes_len, result, error);
}

/*
 * host_compress returns bytes compressed as one zstd frame that records their
 * length, at a level or with a dictionary, failing with Postgres' out of
 * memory error when it can't allocate its memory.
 */
bool
host_compress(const uint8 *bytes, size_t bytes_len, int32 level,
			  const HostCompressDictionary *dictionary, StdVec *result, HostError *error)
{
	HostHost	host = host_current();

	return host.ops->compress(host.self, bytes, bytes_len, level, dictionary, result, error);
}

/* host_frame_size returns the length a zstd frame records of its bytes, or None when it records none or is no frame. */
bool
host_frame_size(const uint8 *frame, size_t frame_len, uint64 *result)
{
	HostReadHost host = host_reading();

	return host.ops->frame_size(host.self, frame, frame_len, result);
}

/*
 * host_decompress returns a zstd frame's size bytes, decompressed with a
 * dictionary if it names one, or None when the frame is damaged or holds
 * another size, failing with Postgres' out of memory error when it can't
 * allocate its memory.
 */
bool
host_decompress(const uint8 *frame, size_t frame_len, size_t size,
				const HostDecompressDictionary *dictionary, bool *has_result, StdVec *result,
				HostError *error)
{
	HostReadHost host = host_reading();

	return host.ops->decompress(host.self, frame, frame_len, size, dictionary, has_result,
								result, error);
}

/*
 * host_train returns a zstd dictionary of at most capacity bytes trained on
 * samples, or None when it can't train one on them.
 */
bool
host_train(const StdBytes *samples, size_t samples_len, size_t capacity, StdVec *result)
{
	HostHost	host = host_current();

	return host.ops->train(host.self, samples, samples_len, capacity, result);
}

/*
 * host_backup_begin counts a base backup in and then waits while a collection
 * runs, from do_pg_backup_start, so no collection changes the pack lists, the
 * forwards, or the files while the backup copies them (the collector text's
 * §3.3); a cancel ends the wait with Postgres' error, the backup left counted
 * in until host_backup_end counts it out.
 */
bool
host_backup_begin(HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->backup_begin(host.self, &raise, error);
}

/* host_backup_end counts a base backup out, from do_pg_backup_stop and do_pg_abort_backup. */
void
host_backup_end(void)
{
	HostHost	host = host_current();

	host.ops->backup_end(host.self);
}

/*
 * host_backups_running reports whether a base backup is counted in, which a
 * collection reads at its safe points and the removal queue before it removes
 * a file.
 */
bool
host_backups_running(void)
{
	HostHost	host = host_current();

	return host.ops->backups_running(host.self);
}

/*
 * host_collection_begin sets the collection's word unless a base backup is
 * counted in, answering whether the collection may run; one that may ends
 * with host_collection_end.
 */
bool
host_collection_begin(void)
{
	HostHost	host = host_current();

	return host.ops->collection_begin(host.self);
}

/* host_collection_end clears the collection's word and wakes the base backups waiting for it. */
void
host_collection_end(void)
{
	HostHost	host = host_current();

	host.ops->collection_end(host.self);
}

/*
 * host_collection_ask asks the collector worker for a full collection of a
 * database's repository and waits for its answer, the packs it retired, those
 * it copied out, and its live bytes, each PG_UINT64_MAX when it failed (the
 * collector text's §8.2); a cancel ends the wait with Postgres' error, the ask
 * taken back, and the collection goes on.
 */
bool
host_collection_ask(HostIoIo *io, uint32 database, uint64 *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->collection_ask(host.self, &raise, database, result, error);
}

/*
 * host_collection_asked returns the unanswered ask's word and the database it
 * asks for, which the collector worker reads.
 */
bool
host_collection_asked(uint64 *result_0, uint32 *result_1)
{
	HostHost	host = host_current();

	return host.ops->collection_asked(host.self, result_0, result_1);
}

/* host_collection_answer answers an ask, by its word, with the collection's counts, and wakes its asker. */
void
host_collection_answer(uint64 word, const uint64 *counts)
{
	HostHost	host = host_current();

	host.ops->collection_answer(host.self, word, counts);
}

/* host_collector_started records the running process as the collector worker, whose latch an ask sets. */
void
host_collector_started(void)
{
	HostHost	host = host_current();

	host.ops->collector_started(host.self);
}

/*
 * host_database_locked takes a database's lock in session mode without
 * waiting, answering whether it took it, so the collector worker opens the
 * database's repository only while no DROP DATABASE or ALTER DATABASE SET
 * TABLESPACE holds or waits for that database's lock, as Postgres' own
 * commands take it.
 */
bool
host_database_locked(HostIoIo *io, uint32 database, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->database_locked(host.self, &raise, database, result, error);
}

/* host_database_unlocked gives back a database's lock that host_database_locked took. */
void
host_database_unlocked(uint32 database)
{
	HostHost	host = host_current();

	host.ops->database_unlocked(host.self, database);
}

/*
 * host_database_lock_waited reports whether a session waits for the lock this
 * process holds on a database, which a collection reads at its safe points to
 * stop for a drop or a move of the database.
 */
bool
host_database_lock_waited(HostIoIo *io, uint32 database, bool *result, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	return host.ops->database_lock_waited(host.self, &raise, database, result, error);
}

#ifdef DOLTGRES_TEST_HOST

#include "port/atomics.h"

/* HostTestsTest is a host of fixed answers, whose file seams these tests can't reach. */
typedef struct HostTestsTest
{
	/* next is the next unique number. */
	pg_atomic_uint64 next;

	/*
	 * base is the monotonic clock's one reading, a OnceLock; base_value is
	 * its value.
	 */
	StdOnceLock base;
	StdInstant	base_value;
} HostTestsTest;

/* host_tests_Test_ReadHost_open_to_read is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_open_to_read(void *self, const HostMayRaise *raise, const char *path, size_t path_len, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1963, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_read_at is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_read_at(void *self, const HostMayRaise *raise, int32 file, uint64 offset, uint8 *buffer, size_t buffer_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1967, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_size is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_size(void *self, const HostMayRaise *raise, int32 file, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1971, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_prefetch does nothing. */
static void
host_tests_Test_ReadHost_prefetch(void *self, int32 file, uint64 offset, uint64 len)
{
}

/* host_tests_Test_ReadHost_close does nothing. */
static void
host_tests_Test_ReadHost_close(void *self, int32 file)
{
}

/* host_tests_Test_ReadHost_tablespace_root returns an empty path. */
static StdPathBuf
host_tests_Test_ReadHost_tablespace_root(void *self, uint32 tablespace)
{
	StdPathBuf	path;

	std_PathBuf_new(&path);
	return path;
}

/* host_tests_Test_ReadHost_open_if_there is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_open_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *has_result, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1981, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_list_dir_if_there is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_list_dir_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *has_result, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1985, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_list_dir is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_ReadHost_list_dir(void *self, const HostMayRaise *raise, const char *path, size_t path_len, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 1989, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_prepare_decompressing is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_ReadHost_prepare_decompressing(void *self, const uint8 *bytes, size_t bytes_len, HostDecompressDictionary *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 1993, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_frame_size is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_ReadHost_frame_size(void *self, const uint8 *frame, size_t frame_len, uint64 *result)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 1997, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_ReadHost_decompress is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_ReadHost_decompress(void *self, const uint8 *frame, size_t frame_len, size_t size, const HostDecompressDictionary *dictionary, bool *has_result, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 2001, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_now returns the epoch and 42 seconds. */
static StdSystemTime
host_tests_Test_Host_now(void *self)
{
	return std_SystemTime_add(STD_UNIX_EPOCH, std_Duration_from_secs(42), RUST_FILE, 2007, 13);
}

/* host_tests_Test_Host_instant returns the monotonic clock's one reading, taken the first time. */
static StdInstant
host_tests_Test_Host_instant(void *self)
{
	HostTestsTest *test = self;

	std_once_lock_get_or_init(&test->base, &test->base_value, std_Instant_now_call, NULL);
	return test->base_value;
}

/* host_tests_Test_Host_process_id returns 7. */
static uint32
host_tests_Test_Host_process_id(void *self)
{
	return 7;
}

/* host_tests_Test_Host_session_database returns none. */
static bool
host_tests_Test_Host_session_database(void *self, uint32 *out_0, uint32 *out_1)
{
	return false;
}

/* host_tests_Test_Host_exiting returns false. */
static bool
host_tests_Test_Host_exiting(void *self)
{
	return false;
}

/* host_tests_Test_Host_unique returns the next unique number. */
static uint64
host_tests_Test_Host_unique(void *self)
{
	return pg_atomic_fetch_add_u64(&((HostTestsTest *) self)->next, 1);
}

/* host_tests_Test_Host_check_interrupts is unreachable here: only `pghost` makes the capability a seam that can raise takes. */
static bool
host_tests_Test_Host_check_interrupts(void *self, const HostMayRaise *raise, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a seam that can raise takes";

	std_panic(RUST_FILE, 2032, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_open is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_open(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostOpen how, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2036, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_open_scratch is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_open_scratch(void *self, const HostMayRaise *raise, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2040, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_write_at is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_write_at(void *self, const HostMayRaise *raise, int32 file, uint64 offset, const uint8 *bytes, size_t bytes_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2044, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_truncate is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_truncate(void *self, const HostMayRaise *raise, int32 file, uint64 len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2048, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_sync is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_sync(void *self, const HostMayRaise *raise, int32 file, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2052, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_rename is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_rename(void *self, const HostMayRaise *raise, const char *from, size_t from_len, const char *to, size_t to_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2056, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_rename_durably is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_rename_durably(void *self, const HostMayRaise *raise, const char *from, size_t from_len, const char *to, size_t to_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2060, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_remove_durably is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_remove_durably(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2064, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_remove_durably_if_there is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_remove_durably_if_there(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2068, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_remove_leftover does nothing. */
static void
host_tests_Test_Host_remove_leftover(void *self, const char *path, size_t path_len)
{
}

/* host_tests_Test_Host_rename_back returns false. */
static bool
host_tests_Test_Host_rename_back(void *self, const char *from, size_t from_len, const char *to,
								 size_t to_len)
{
	return false;
}

/* host_tests_Test_Host_remove_tree is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_remove_tree(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2077, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_create_if_absent is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_create_if_absent(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool *has_result, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2081, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_sync_path is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_sync_path(void *self, const HostMayRaise *raise, const char *path, size_t path_len, bool directory, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2085, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_make_dir is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_make_dir(void *self, const HostMayRaise *raise, const char *path, size_t path_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2089, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_disk_space is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_disk_space(void *self, const HostMayRaise *raise, const char *path, size_t path_len, uint64 *result_0, uint64 *result_1, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2097, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_tree_bytes is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_tree_bytes(void *self, const HostMayRaise *raise, const char *path, size_t path_len, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2153, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_nap is unreachable here: only `pghost` makes the capability a file seam takes. */
static bool
host_tests_Test_Host_nap(void *self, const HostMayRaise *raise, StdDuration duration, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a file seam takes";

	std_panic(RUST_FILE, 2101, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_wal_insert is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_wal_insert(void *self, const HostMayRaise *raise, HostWalKind kind, const uint8 *body, size_t body_len, HostLsn *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2105, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_wal_new_cid is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_wal_new_cid(void *self, const HostMayRaise *raise, const HostStorage *storage, HostRow tid, HostNewCid cid, HostLsn *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2109, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_wal_flush is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_wal_flush(void *self, const HostMayRaise *raise, HostLsn lsn, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2113, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_fold_point is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_fold_point(void *self, const HostMayRaise *raise, HostLsn *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2117, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_checkpoint_now is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_checkpoint_now(void *self, const HostMayRaise *raise, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2121, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_quiet_database is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_quiet_database(void *self, const HostMayRaise *raise, uint32 database, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2125, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_unquiet_database is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_unquiet_database(void *self, const HostMayRaise *raise, uint32 database, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2129, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_last_redo is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_last_redo(void *self, const HostMayRaise *raise, HostLsn *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2133, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_wal_stand is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static HostWalStand
host_tests_Test_Host_wal_stand(void *self)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2137, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_warn_kept_wal is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_warn_kept_wal(void *self, const HostMayRaise *raise, HostLsn from, HostLsn to, uint32 timeline, HostLsn upto, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2141, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_next_fork_record is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_next_fork_record(void *self, const HostMayRaise *raise, HostLsn from, uint32 kinds, bool *has_result, HostForkRecord *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2145, 13, text, sizeof(text) - 1);
}

/*
 * host_tests_Test_Host_warn is unreachable here: only `pghost` makes the
 * capability a report takes. Its panic's unwind drops the error it owns.
 */
static bool
host_tests_Test_Host_warn(void *self, const HostMayRaise *raise, HostError *error_, const char *context, HostError *error)
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a report takes";

		std_panic(RUST_FILE, 2149, 13, text, sizeof(text) - 1);
	}
	host_Error_drop(error_);
	std_unwind_resume(&unwind);
}

/* host_tests_Test_Host_log is unreachable here: only `pghost` makes the capability a report takes. */
static bool
host_tests_Test_Host_log(void *self, const HostMayRaise *raise, const char *message, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a report takes";

	std_panic(RUST_FILE, 2153, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_refuse_recovery is unreachable here: only `pghost` recovers. */
static void
host_tests_Test_Host_refuse_recovery(void *self, const char *detail, size_t detail_len)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` recovers";

	std_panic(RUST_FILE, 2157, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delay_checkpoints is unreachable here: only `pghost` makes the capability a WAL seam takes. */
static bool
host_tests_Test_Host_delay_checkpoints(void *self, const HostMayRaise *raise, const char *const *pauses, size_t pauses_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a WAL seam takes";

	std_panic(RUST_FILE, 2161, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_resume_checkpoints does nothing. */
static void
host_tests_Test_Host_resume_checkpoints(void *self, bool finished)
{
}

/* host_tests_Test_Host_xid_status is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_xid_status(void *self, const HostMayRaise *raise, HostXid xid, HostStatus *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2167, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_xid_in_progress is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_xid_in_progress(void *self, const HostMayRaise *raise, HostXid xid, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2171, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_xids_answered returns true. */
static bool
host_tests_Test_Host_xids_answered(void *self)
{
	return true;
}

/* host_tests_Test_Host_removable_in is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_removable_in(void *self, const HostMayRaise *raise, StdAny snapshot, HostXid xid, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2179, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_current_writer is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_current_writer(void *self, const HostMayRaise *raise, HostXid *result_0, HostXid *result_1, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2183, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_current_command is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_current_command(void *self, const HostMayRaise *raise, bool used, HostCommand *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2187, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_key_lock is unreachable here: only `pghost` makes the capability a key lock takes. */
static bool
host_tests_Test_Host_key_lock(void *self, const HostMayRaise *raise, const uint32 *stripes, size_t stripes_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a key lock takes";

	std_panic(RUST_FILE, 2191, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_key_unlock does nothing. */
static void
host_tests_Test_Host_key_unlock(void *self, const uint32 *stripes, size_t stripes_len, bool pushed)
{
}

/* host_tests_Test_Host_key_count returns 0. */
static uint64
host_tests_Test_Host_key_count(void *self, uint32 stripe)
{
	return 0;
}

/* host_tests_Test_Host_key_hold returns false. */
static bool
host_tests_Test_Host_key_hold(void *self, uint32 stripe)
{
	return false;
}

/* host_tests_Test_Host_key_release does nothing. */
static void
host_tests_Test_Host_key_release(void *self, uint32 stripe)
{
}

/* host_tests_Test_Host_key_held_wait is unreachable here: only `pghost` makes the capability a wait takes. */
static bool
host_tests_Test_Host_key_held_wait(void *self, const HostMayRaise *raise, uint32 stripe, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a wait takes";

	std_panic(RUST_FILE, 2207, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_xact_wait is unreachable here: only `pghost` makes the capability a wait takes. */
static bool
host_tests_Test_Host_xact_wait(void *self, const HostMayRaise *raise, HostXid xid, bool has_on, HostOperation on, bool wait, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a wait takes";

	std_panic(RUST_FILE, 2211, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_lock_tuple is unreachable here: only `pghost` makes the capability a wait takes. */
static bool
host_tests_Test_Host_lock_tuple(void *self, const HostMayRaise *raise, HostTupleLock tuple, bool wait, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a wait takes";

	std_panic(RUST_FILE, 2215, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_speculative_wait is unreachable here: only `pghost` makes the capability a wait takes. */
static bool
host_tests_Test_Host_speculative_wait(void *self, const HostMayRaise *raise, HostXid xid, uint32 token, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a wait takes";

	std_panic(RUST_FILE, 2219, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_unlock_tuple does nothing. */
static void
host_tests_Test_Host_unlock_tuple(void *self, HostTupleLock tuple)
{
}

/* host_tests_Test_Host_xid_is_current is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_xid_is_current(void *self, HostXid xid)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2225, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_combo_command is unreachable here: only `pghost` makes the capability a transaction seam takes. */
static bool
host_tests_Test_Host_combo_command(void *self, const HostMayRaise *raise, HostXid writer, HostCommand cmin, HostCommand cmax, HostCommand *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a transaction seam takes";

	std_panic(RUST_FILE, 2229, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_actor returns none: the process's one backend. */
static bool
host_tests_Test_Host_actor(void *self, uint64 *result)
{
	return false;
}

/* host_tests_Test_Host_yield_point does nothing. */
static void
host_tests_Test_Host_yield_point(void *self)
{
}

/* host_tests_Test_Host_pause is unreachable here: only `pghost` makes the capability a pause takes. */
static bool
host_tests_Test_Host_pause(void *self, const HostMayRaise *raise, const char *point, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a pause takes";

	std_panic(RUST_FILE, 2238, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_injected is unreachable here: only `pghost` makes the capability an injection point takes. */
static bool
host_tests_Test_Host_injected(void *self, const HostMayRaise *raise, const char *point, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability an injection point takes";

	std_panic(RUST_FILE, 2242, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_at_exit is unreachable here: no test of `host` registers an exit work. */
static bool
host_tests_Test_Host_at_exit(void *self, const StdTypeId *work, bool (*run) (HostIoIo *io, HostError *error), HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` registers an exit work";

	std_panic(RUST_FILE, 2246, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_prepare_compressing is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_Host_prepare_compressing(void *self, const uint8 *bytes, size_t bytes_len, int32 level, HostCompressDictionary *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 2250, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_compress is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_Host_compress(void *self, const uint8 *bytes, size_t bytes_len, int32 level, const HostCompressDictionary *dictionary, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 2254, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_train is unreachable here: these tests compress nothing. */
static bool
host_tests_Test_Host_train(void *self, const StdBytes *samples, size_t samples_len, size_t capacity, StdVec *result)
{
	static const char text[] = "internal error: entered unreachable code: these tests compress nothing";

	std_panic(RUST_FILE, 2258, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_make is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_make(void *self, const HostMayRaise *raise, const HostSharedKind *kind, bool *has_result, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2262, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_publish is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static uint64
host_tests_Test_Host_shared_publish(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2266, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_claim is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_claim(void *self, const HostSharedKind *kind, uint64 location, bool *has_result, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2270, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_free is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_shared_free(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2274, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_give_up is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_shared_give_up(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2278, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_open is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_open(void *self, const HostMayRaise *raise, const HostSharedKind *kind, uint64 location, bool *has_result, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2282, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_close is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_shared_close(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2286, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_reclaim is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_reclaim(void *self, const HostSharedKind *kind, uint64 location, HostSharedLooked *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2290, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_reclaimed is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_reclaimed(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2294, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_unreclaim is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_shared_unreclaim(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2298, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_sweep is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_sweep(void *self, const HostMayRaise *raise, const HostSharedKind *kind, size_t atomic, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2302, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_single is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_single(void *self, const HostSharedKind *kind, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2306, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_ready returns false. */
static bool
host_tests_Test_Host_shared_ready(void *self)
{
	return false;
}

/* host_tests_Test_Host_shared_lock is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_lock(void *self, const HostMayRaise *raise, uint64 place, size_t lock, bool exclusive, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2314, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_unlock is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_shared_unlock(void *self, uint64 place, size_t lock)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2318, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_flag_take is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_flag_take(void *self, const HostMayRaise *raise, uint64 place, size_t flag, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2322, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_flag_clear is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_flag_clear(void *self, uint64 place, size_t flag)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2326, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_atomic is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static uint64
host_tests_Test_Host_atomic(void *self, uint64 place, size_t atomic, HostSharedOp op)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2330, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_atomics is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static size_t
host_tests_Test_Host_shared_atomics(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2334, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_blob_read is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static StdVec
host_tests_Test_Host_blob_read(void *self, uint64 place, size_t blob)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2338, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_blob_write is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_blob_write(void *self, uint64 place, size_t blob, const uint8 *bytes, size_t bytes_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2342, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_entry_read is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_entry_read(void *self, uint64 place, size_t array, size_t entry, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2346, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_entry_write is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_entry_write(void *self, uint64 place, size_t array, size_t entry, const uint8 *bytes, size_t bytes_len, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2350, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_location is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static uint64
host_tests_Test_Host_shared_location(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2354, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_live is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_shared_live(void *self, uint64 location)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2358, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_shared_gone is unreachable here: no test of `host` asks whether a record is gone. */
static bool
host_tests_Test_Host_shared_gone(void *self, const HostSharedKind *kind, uint64 location, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` asks whether a record is gone";

	std_panic(RUST_FILE, 2362, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_slots_name is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static uint64
host_tests_Test_Host_slots_name(void *self, uint64 place, size_t array)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2366, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_slots_rename is unreachable here: these tests reach no shared memory. */
static bool
host_tests_Test_Host_slots_rename(void *self, uint64 place, size_t array, uint64 loaded, uint64 name)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2370, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_words_processes is unreachable here: these tests reach no shared memory. */
static bool
host_tests_Test_Host_words_processes(void *self, const HostSharedKind *kind, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2374, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_slots_empty is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_slots_empty(void *self, uint64 place, size_t array)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2378, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_slots_claim is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_slots_claim(void *self, const HostMayRaise *raise, uint64 place, size_t array, const uint8 *bytes, size_t bytes_len, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2382, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_slots_read is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static StdVec
host_tests_Test_Host_slots_read(void *self, uint64 place, size_t array)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2386, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_words_mine is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_words_mine(void *self, const HostSharedKind *kind, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2390, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_word_store is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_word_store(void *self, uint64 place, size_t word, uint64 value)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2394, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_word_store_own is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static void
host_tests_Test_Host_word_store_own(void *self, size_t index, const HostSharedKind *kind, size_t word, uint64 value)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2398, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_words_place is unreachable here: no test of `host` exchanges a process's word. */
static bool
host_tests_Test_Host_words_place(void *self, const HostSharedKind *kind, uint32 proc, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` exchanges a process's word";

	std_panic(RUST_FILE, 2402, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_word_load is unreachable here: these tests reach no shared memory. */
static bool
host_tests_Test_Host_word_load(void *self, const HostSharedKind *kind, uint32 proc, size_t word, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2406, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_lock is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_epochs_lock(void *self, const HostMayRaise *raise, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2410, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_unlock is unreachable here: these tests reach no shared memory. */
static void
host_tests_Test_Host_epochs_unlock(void *self)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2414, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_advance is unreachable here: these tests reach no shared memory. */
static uint64
host_tests_Test_Host_epochs_advance(void *self)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2418, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_oldest is unreachable here: these tests reach no shared memory. */
static uint64
host_tests_Test_Host_epochs_oldest(void *self)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2422, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_read is unreachable here: only `pghost` makes the capability a shared-memory seam takes. */
static bool
host_tests_Test_Host_epochs_read(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a shared-memory seam takes";

	std_panic(RUST_FILE, 2426, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_epochs_end is unreachable here: these tests reach no shared memory. */
static void
host_tests_Test_Host_epochs_end(void *self, uint64 epoch)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no shared memory";

	std_panic(RUST_FILE, 2430, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_proc_number returns 0. */
static uint32
host_tests_Test_Host_proc_number(void *self)
{
	return 0;
}

/* host_tests_Test_Host_delta_hold is unreachable here: only `pghost` makes the capability a hold takes. */
static bool
host_tests_Test_Host_delta_hold(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a hold takes";

	std_panic(RUST_FILE, 2438, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_release is unreachable here: no test of `host` holds the delta's pages. */
static void
host_tests_Test_Host_delta_release(void *self, uint64 epoch)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` holds the delta's pages";

	std_panic(RUST_FILE, 2442, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_open is unreachable here: no test of `host` holds the delta's pages. */
static bool
host_tests_Test_Host_delta_open(void *self, HostDeltaPagePage name, uint64 epoch, HostDeltaDirectoryOpened *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` holds the delta's pages";

	std_panic(RUST_FILE, 2446, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_bytes is unreachable here: no test of `host` opens a page of the delta. */
static bool
host_tests_Test_Host_delta_bytes(void *self, const HostDeltaDirectoryOpened *opened, uint64 offset, uint64 len, HostCacheBytes *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2450, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_room is unreachable here: no test of `host` opens a page of the delta. */
static uint64
host_tests_Test_Host_delta_room(void *self, const HostDeltaDirectoryOpened *opened)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2454, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_filled is unreachable here: no test of `host` opens a page of the delta. */
static uint64
host_tests_Test_Host_delta_filled(void *self, const HostDeltaDirectoryOpened *opened)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2458, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_word is unreachable here: no test of `host` opens a page of the delta. */
static uint64
host_tests_Test_Host_delta_word(void *self, uint64 body, uint64 index, HostDeltaWordOp op)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2462, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_alloc is unreachable here: only `pghost` makes the capability an allocation takes. */
static bool
host_tests_Test_Host_delta_alloc(void *self, const HostMayRaise *raise, const HostDeltaPageSize *sizes, size_t sizes_len, StdVec *names, bool *has_result, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability an allocation takes";

	std_panic(RUST_FILE, 2471, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_alloc_replacing is unreachable here: only `pghost` makes the capability an allocation takes. */
static bool
host_tests_Test_Host_delta_alloc_replacing(void *self, const HostMayRaise *raise, HostDeltaPageSize size,
										   HostDeltaPagePage replaced, StdVec *names, bool *has_result, uint64 *result,
										   HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability an allocation takes";

	std_panic(RUST_FILE, 2481, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_give_back is unreachable here: no test of `host` owns a page of the delta. */
static void
host_tests_Test_Host_delta_give_back(void *self, HostDeltaPagePage name)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` owns a page of the delta";

	std_panic(RUST_FILE, 2485, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_owned_append is unreachable here: no test of `host` owns a page of the delta. */
static bool
host_tests_Test_Host_delta_owned_append(void *self, HostDeltaPagePage name, const uint8 *record, size_t record_len, uint32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` owns a page of the delta";

	std_panic(RUST_FILE, 2489, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_owned_store is unreachable here: no test of `host` owns a page of the delta. */
static bool
host_tests_Test_Host_delta_owned_store(void *self, HostDeltaPagePage name, uint64 index, uint64 value, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` owns a page of the delta";

	std_panic(RUST_FILE, 2493, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_link is unreachable here: no test of `host` owns a page of the delta. */
static void
host_tests_Test_Host_delta_link(void *self, HostDeltaPagePage name, HostDeltaDirectoryOwner owner)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` owns a page of the delta";

	std_panic(RUST_FILE, 2497, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_append is unreachable here: no test of `host` opens a page of the delta. */
static bool
host_tests_Test_Host_delta_append(void *self, const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner, const uint8 *record, size_t record_len, uint32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2506, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_seal is unreachable here: no test of `host` opens a page of the delta. */
static bool
host_tests_Test_Host_delta_seal(void *self, const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a page of the delta";

	std_panic(RUST_FILE, 2510, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_retire is unreachable here: no test of `host` retires a page of the delta. */
static void
host_tests_Test_Host_delta_retire(void *self, const HostDeltaLeaving *records, size_t records_len, const HostDeltaPagePage *names, size_t names_len)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` retires a page of the delta";

	std_panic(RUST_FILE, 2514, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_make is unreachable here: no test of `host` makes a record of the delta. */
static bool
host_tests_Test_Host_delta_make(void *self, const HostMayRaise *raise, const HostSharedKind *kind, const char *name, size_t name_len, bool *result_ok, uint64 *result_0, uint64 *result_1, uint64 *result_err, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` makes a record of the delta";

	std_panic(RUST_FILE, 2518, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_record is unreachable here: no test of `host` opens a record of the delta. */
static bool
host_tests_Test_Host_delta_record(void *self, const HostSharedKind *kind, uint64 location, uint64 epoch, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` opens a record of the delta";

	std_panic(RUST_FILE, 2522, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_publish is unreachable here: no test of `host` makes a record of the delta. */
static void
host_tests_Test_Host_delta_publish(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` makes a record of the delta";

	std_panic(RUST_FILE, 2526, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_unmake is unreachable here: no test of `host` makes a record of the delta. */
static void
host_tests_Test_Host_delta_unmake(void *self, uint64 place)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` makes a record of the delta";

	std_panic(RUST_FILE, 2530, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_pass is unreachable here: no test of `host` retires a page of the delta. */
static void
host_tests_Test_Host_delta_pass(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` retires a page of the delta";

	std_panic(RUST_FILE, 2534, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_advanced is unreachable here: no test of `host` advances the delta's epoch. */
static void
host_tests_Test_Host_delta_advanced(void *self, uint64 *result_0, uint64 *result_1)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` advances the delta's epoch";

	std_panic(RUST_FILE, 2538, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_used is unreachable here: no test of `host` reads the delta's count. */
static uint64
host_tests_Test_Host_delta_used(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` reads the delta's count";

	std_panic(RUST_FILE, 2542, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_budget is unreachable here: no test of `host` reads the delta's budget. */
static uint64
host_tests_Test_Host_delta_budget(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` reads the delta's budget";

	std_panic(RUST_FILE, 2546, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_seed is unreachable here: no test of `host` reads the delta's seed. */
static void
host_tests_Test_Host_delta_seed(void *self, uint8 *result)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` reads the delta's seed";

	std_panic(RUST_FILE, 2550, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_delta_wait is unreachable here: only `pghost` makes the capability a wait takes. */
static bool
host_tests_Test_Host_delta_wait(void *self, const HostMayRaise *raise, uint64 frees, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a wait takes";

	std_panic(RUST_FILE, 2554, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_backup_begin is unreachable here: no test of `host` starts a base backup. */
static bool
host_tests_Test_Host_backup_begin(void *self, const HostMayRaise *raise, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` starts a base backup";

	std_panic(RUST_FILE, 2558, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_backup_end is unreachable here: no test of `host` ends a base backup. */
static void
host_tests_Test_Host_backup_end(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` ends a base backup";

	std_panic(RUST_FILE, 2562, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_backups_running is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_backups_running(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2566, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collection_begin is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_collection_begin(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2570, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collection_end is unreachable here: no test of `host` collects. */
static void
host_tests_Test_Host_collection_end(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2574, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collection_ask is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_collection_ask(void *self, const HostMayRaise *raise, uint32 database, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2578, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collection_asked is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_collection_asked(void *self, uint64 *result_0, uint32 *result_1)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2582, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collection_answer is unreachable here: no test of `host` collects. */
static void
host_tests_Test_Host_collection_answer(void *self, uint64 word, const uint64 *counts)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2586, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_collector_started is unreachable here: no test of `host` collects. */
static void
host_tests_Test_Host_collector_started(void *self)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2590, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_database_locked is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_database_locked(void *self, const HostMayRaise *raise, uint32 database, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2594, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_database_unlocked is unreachable here: no test of `host` collects. */
static void
host_tests_Test_Host_database_unlocked(void *self, uint32 database)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2598, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_database_lock_waited is unreachable here: no test of `host` collects. */
static bool
host_tests_Test_Host_database_lock_waited(void *self, const HostMayRaise *raise, uint32 database, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: no test of `host` collects";

	std_panic(RUST_FILE, 2602, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_cache_get is unreachable here: only `pghost` makes the capability a node-cache seam takes. */
static bool
host_tests_Test_Host_cache_get(void *self, const HostMayRaise *raise, HostCachePlacement placement, const HostCacheKey *key, bool *has_result, HostCacheFound *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a node-cache seam takes";

	std_panic(RUST_FILE, 2611, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_cache_install is unreachable here: only `pghost` makes the capability a node-cache seam takes. */
static bool
host_tests_Test_Host_cache_install(void *self, const HostMayRaise *raise, HostCachePlacement placement, const HostCacheKey *key, size_t len, void (*fill) (void *, uint8 *, size_t), void *fill_captures, bool *has_result, HostCacheFound *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a node-cache seam takes";

	std_panic(RUST_FILE, 2622, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_cache_contains is unreachable here: only `pghost` makes the capability a node-cache seam takes. */
static bool
host_tests_Test_Host_cache_contains(void *self, const HostMayRaise *raise, HostCachePlacement placement, const HostCacheKey *key, bool *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a node-cache seam takes";

	std_panic(RUST_FILE, 2626, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_cache_unpin is unreachable here: these tests reach no node cache. */
static void
host_tests_Test_Host_cache_unpin(void *self, HostCachePlacement placement, uint32 frame)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no node cache";

	std_panic(RUST_FILE, 2630, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_cache_make_local is unreachable here: only `pghost` makes the capability a node-cache seam takes. */
static bool
host_tests_Test_Host_cache_make_local(void *self, const HostMayRaise *raise, size_t bytes, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a node-cache seam takes";

	std_panic(RUST_FILE, 2634, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owner_enlarge is unreachable here: only `pghost` makes the capability a resource-owner seam takes. */
static bool
host_tests_Test_Host_owner_enlarge(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a resource-owner seam takes";

	std_panic(RUST_FILE, 2638, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owner_top_enlarge is unreachable here: only `pghost` makes the capability a resource-owner seam takes. */
static bool
host_tests_Test_Host_owner_top_enlarge(void *self, const HostMayRaise *raise, uint64 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: only `pghost` makes the capability a resource-owner seam takes";

	std_panic(RUST_FILE, 2642, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_btree_compare is unreachable here: these tests compare by no class's function. */
static bool
host_tests_Test_Host_btree_compare(void *self, const HostMayRaise *raise, const HostCompared *compared, const uint8 *a, size_t a_len, const uint8 *b, size_t b_len, int32 *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests compare by no class's function";

	std_panic(RUST_FILE, 2646, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_sort_key is unreachable here: these tests sort no text by a collation. */
static bool
host_tests_Test_Host_sort_key(void *self, const HostMayRaise *raise, uint32 collation, const uint8 *text_, size_t text_len, StdVec *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests sort no text by a collation";

	std_panic(RUST_FILE, 2650, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_reformed is unreachable here: these tests re-form no row. */
static bool
host_tests_Test_Host_reformed(void *self, const HostMayRaise *raise, const HostReformRow *row, HostReformedRow *result, HostError *error)
{
	static const char text[] = "internal error: entered unreachable code: these tests re-form no row";

	std_panic(RUST_FILE, 2654, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owner_remember is unreachable here: these tests reach no resource owner. */
static void
host_tests_Test_Host_owner_remember(void *self, uint64 owner, uint64 pin)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no resource owner";

	std_panic(RUST_FILE, 2658, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owner_forget is unreachable here: these tests reach no resource owner. */
static void
host_tests_Test_Host_owner_forget(void *self, uint64 owner, uint64 pin)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no resource owner";

	std_panic(RUST_FILE, 2662, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owned_remember is unreachable here: these tests reach no resource owner. */
static void
host_tests_Test_Host_owned_remember(void *self, uint64 owner, uint64 record, void (*release) (uint32))
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no resource owner";

	std_panic(RUST_FILE, 2666, 13, text, sizeof(text) - 1);
}

/* host_tests_Test_Host_owned_forget is unreachable here: these tests reach no resource owner. */
static void
host_tests_Test_Host_owned_forget(void *self, uint64 owner, uint64 record)
{
	static const char text[] = "internal error: entered unreachable code: these tests reach no resource owner";

	std_panic(RUST_FILE, 2670, 13, text, sizeof(text) - 1);
}

/* HOST_TESTS_TEST_HOST_OPS is Test's tables of ReadHost and Host. */
static const HostHostOps HOST_TESTS_TEST_HOST_OPS = {
	.read_host = {
		.open_to_read = host_tests_Test_ReadHost_open_to_read,
		.open_if_there = host_tests_Test_ReadHost_open_if_there,
		.read_at = host_tests_Test_ReadHost_read_at,
		.size = host_tests_Test_ReadHost_size,
		.prefetch = host_tests_Test_ReadHost_prefetch,
		.close = host_tests_Test_ReadHost_close,
		.tablespace_root = host_tests_Test_ReadHost_tablespace_root,
		.list_dir = host_tests_Test_ReadHost_list_dir,
		.list_dir_if_there = host_tests_Test_ReadHost_list_dir_if_there,
		.prepare_decompressing = host_tests_Test_ReadHost_prepare_decompressing,
		.frame_size = host_tests_Test_ReadHost_frame_size,
		.decompress = host_tests_Test_ReadHost_decompress,
	},
	.now = host_tests_Test_Host_now,
	.instant = host_tests_Test_Host_instant,
	.process_id = host_tests_Test_Host_process_id,
	.session_database = host_tests_Test_Host_session_database,
	.exiting = host_tests_Test_Host_exiting,
	.unique = host_tests_Test_Host_unique,
	.check_interrupts = host_tests_Test_Host_check_interrupts,
	.open_ = host_tests_Test_Host_open,
	.create_if_absent = host_tests_Test_Host_create_if_absent,
	.open_scratch = host_tests_Test_Host_open_scratch,
	.write_at = host_tests_Test_Host_write_at,
	.truncate = host_tests_Test_Host_truncate,
	.sync = host_tests_Test_Host_sync,
	.rename_ = host_tests_Test_Host_rename,
	.rename_durably = host_tests_Test_Host_rename_durably,
	.remove_durably = host_tests_Test_Host_remove_durably,
	.remove_durably_if_there = host_tests_Test_Host_remove_durably_if_there,
	.remove_leftover = host_tests_Test_Host_remove_leftover,
	.rename_back = host_tests_Test_Host_rename_back,
	.remove_tree = host_tests_Test_Host_remove_tree,
	.sync_path = host_tests_Test_Host_sync_path,
	.make_dir = host_tests_Test_Host_make_dir,
	.disk_space = host_tests_Test_Host_disk_space,
	.tree_bytes = host_tests_Test_Host_tree_bytes,
	.nap = host_tests_Test_Host_nap,
	.wal_insert = host_tests_Test_Host_wal_insert,
	.wal_new_cid = host_tests_Test_Host_wal_new_cid,
	.wal_flush = host_tests_Test_Host_wal_flush,
	.fold_point = host_tests_Test_Host_fold_point,
	.checkpoint_now = host_tests_Test_Host_checkpoint_now,
	.quiet_database = host_tests_Test_Host_quiet_database,
	.unquiet_database = host_tests_Test_Host_unquiet_database,
	.last_redo = host_tests_Test_Host_last_redo,
	.wal_stand = host_tests_Test_Host_wal_stand,
	.warn_kept_wal = host_tests_Test_Host_warn_kept_wal,
	.next_fork_record = host_tests_Test_Host_next_fork_record,
	.warn = host_tests_Test_Host_warn,
	.log = host_tests_Test_Host_log,
	.refuse_recovery = host_tests_Test_Host_refuse_recovery,
	.delay_checkpoints = host_tests_Test_Host_delay_checkpoints,
	.resume_checkpoints = host_tests_Test_Host_resume_checkpoints,
	.xid_status = host_tests_Test_Host_xid_status,
	.xid_in_progress = host_tests_Test_Host_xid_in_progress,
	.xids_answered = host_tests_Test_Host_xids_answered,
	.removable_in = host_tests_Test_Host_removable_in,
	.current_writer = host_tests_Test_Host_current_writer,
	.current_command = host_tests_Test_Host_current_command,
	.xid_is_current = host_tests_Test_Host_xid_is_current,
	.combo_command = host_tests_Test_Host_combo_command,
	.key_lock = host_tests_Test_Host_key_lock,
	.key_unlock = host_tests_Test_Host_key_unlock,
	.key_count = host_tests_Test_Host_key_count,
	.key_hold = host_tests_Test_Host_key_hold,
	.key_release = host_tests_Test_Host_key_release,
	.key_held_wait = host_tests_Test_Host_key_held_wait,
	.xact_wait = host_tests_Test_Host_xact_wait,
	.speculative_wait = host_tests_Test_Host_speculative_wait,
	.lock_tuple = host_tests_Test_Host_lock_tuple,
	.unlock_tuple = host_tests_Test_Host_unlock_tuple,
	.actor = host_tests_Test_Host_actor,
	.yield_point = host_tests_Test_Host_yield_point,
	.pause = host_tests_Test_Host_pause,
	.injected = host_tests_Test_Host_injected,
	.at_exit = host_tests_Test_Host_at_exit,
	.prepare_compressing = host_tests_Test_Host_prepare_compressing,
	.compress = host_tests_Test_Host_compress,
	.train = host_tests_Test_Host_train,
	.shared_make = host_tests_Test_Host_shared_make,
	.shared_publish = host_tests_Test_Host_shared_publish,
	.shared_claim = host_tests_Test_Host_shared_claim,
	.shared_free = host_tests_Test_Host_shared_free,
	.shared_give_up = host_tests_Test_Host_shared_give_up,
	.shared_open = host_tests_Test_Host_shared_open,
	.shared_close = host_tests_Test_Host_shared_close,
	.shared_reclaim = host_tests_Test_Host_shared_reclaim,
	.shared_reclaimed = host_tests_Test_Host_shared_reclaimed,
	.shared_unreclaim = host_tests_Test_Host_shared_unreclaim,
	.shared_sweep = host_tests_Test_Host_shared_sweep,
	.shared_single = host_tests_Test_Host_shared_single,
	.shared_ready = host_tests_Test_Host_shared_ready,
	.shared_lock = host_tests_Test_Host_shared_lock,
	.shared_unlock = host_tests_Test_Host_shared_unlock,
	.flag_take = host_tests_Test_Host_flag_take,
	.flag_clear = host_tests_Test_Host_flag_clear,
	.atomic = host_tests_Test_Host_atomic,
	.shared_atomics = host_tests_Test_Host_shared_atomics,
	.blob_read = host_tests_Test_Host_blob_read,
	.blob_write = host_tests_Test_Host_blob_write,
	.entry_read = host_tests_Test_Host_entry_read,
	.entry_write = host_tests_Test_Host_entry_write,
	.shared_location = host_tests_Test_Host_shared_location,
	.shared_live = host_tests_Test_Host_shared_live,
	.shared_gone = host_tests_Test_Host_shared_gone,
	.slots_name = host_tests_Test_Host_slots_name,
	.slots_rename = host_tests_Test_Host_slots_rename,
	.slots_empty = host_tests_Test_Host_slots_empty,
	.slots_claim = host_tests_Test_Host_slots_claim,
	.slots_read = host_tests_Test_Host_slots_read,
	.words_processes = host_tests_Test_Host_words_processes,
	.words_mine = host_tests_Test_Host_words_mine,
	.word_store = host_tests_Test_Host_word_store,
	.word_store_own = host_tests_Test_Host_word_store_own,
	.word_load = host_tests_Test_Host_word_load,
	.words_place = host_tests_Test_Host_words_place,
	.epochs_lock = host_tests_Test_Host_epochs_lock,
	.epochs_unlock = host_tests_Test_Host_epochs_unlock,
	.epochs_advance = host_tests_Test_Host_epochs_advance,
	.epochs_oldest = host_tests_Test_Host_epochs_oldest,
	.epochs_read = host_tests_Test_Host_epochs_read,
	.epochs_end = host_tests_Test_Host_epochs_end,
	.proc_number = host_tests_Test_Host_proc_number,
	.delta_hold = host_tests_Test_Host_delta_hold,
	.delta_release = host_tests_Test_Host_delta_release,
	.delta_open = host_tests_Test_Host_delta_open,
	.delta_bytes = host_tests_Test_Host_delta_bytes,
	.delta_room = host_tests_Test_Host_delta_room,
	.delta_filled = host_tests_Test_Host_delta_filled,
	.delta_word = host_tests_Test_Host_delta_word,
	.delta_alloc = host_tests_Test_Host_delta_alloc,
	.delta_alloc_replacing = host_tests_Test_Host_delta_alloc_replacing,
	.delta_give_back = host_tests_Test_Host_delta_give_back,
	.delta_owned_append = host_tests_Test_Host_delta_owned_append,
	.delta_owned_store = host_tests_Test_Host_delta_owned_store,
	.delta_link = host_tests_Test_Host_delta_link,
	.delta_append = host_tests_Test_Host_delta_append,
	.delta_seal = host_tests_Test_Host_delta_seal,
	.delta_retire = host_tests_Test_Host_delta_retire,
	.delta_make = host_tests_Test_Host_delta_make,
	.delta_record = host_tests_Test_Host_delta_record,
	.delta_publish = host_tests_Test_Host_delta_publish,
	.delta_unmake = host_tests_Test_Host_delta_unmake,
	.delta_pass = host_tests_Test_Host_delta_pass,
	.delta_advanced = host_tests_Test_Host_delta_advanced,
	.delta_used = host_tests_Test_Host_delta_used,
	.delta_budget = host_tests_Test_Host_delta_budget,
	.delta_seed = host_tests_Test_Host_delta_seed,
	.delta_wait = host_tests_Test_Host_delta_wait,
	.backup_begin = host_tests_Test_Host_backup_begin,
	.backup_end = host_tests_Test_Host_backup_end,
	.backups_running = host_tests_Test_Host_backups_running,
	.collection_begin = host_tests_Test_Host_collection_begin,
	.collection_end = host_tests_Test_Host_collection_end,
	.collection_ask = host_tests_Test_Host_collection_ask,
	.collection_asked = host_tests_Test_Host_collection_asked,
	.collection_answer = host_tests_Test_Host_collection_answer,
	.collector_started = host_tests_Test_Host_collector_started,
	.database_locked = host_tests_Test_Host_database_locked,
	.database_unlocked = host_tests_Test_Host_database_unlocked,
	.database_lock_waited = host_tests_Test_Host_database_lock_waited,
	.cache_get = host_tests_Test_Host_cache_get,
	.cache_install = host_tests_Test_Host_cache_install,
	.cache_contains = host_tests_Test_Host_cache_contains,
	.cache_unpin = host_tests_Test_Host_cache_unpin,
	.cache_make_local = host_tests_Test_Host_cache_make_local,
	.owner_enlarge = host_tests_Test_Host_owner_enlarge,
	.owner_top_enlarge = host_tests_Test_Host_owner_top_enlarge,
	.owner_remember = host_tests_Test_Host_owner_remember,
	.owner_forget = host_tests_Test_Host_owner_forget,
	.owned_remember = host_tests_Test_Host_owned_remember,
	.owned_forget = host_tests_Test_Host_owned_forget,
	.btree_compare = host_tests_Test_Host_btree_compare,
	.sort_key = host_tests_Test_Host_sort_key,
	.reformed = host_tests_Test_Host_reformed,
};

/* HOST_TESTS_TEST is the tests' host. */
static HostTestsTest HOST_TESTS_TEST = {{0}, STD_ONCE_LOCK_NEW};

/* HOST_TESTS_OTHER is another host. */
static HostTestsTest HOST_TESTS_OTHER = {{0}, STD_ONCE_LOCK_NEW};

/* HOST_TESTS_INSTALL installs the tests' host once, whichever test runs first. */
static StdOnce HOST_TESTS_INSTALL = {STD_ONCE_INCOMPLETE};

/* host_tests_with_test_host_closure_0 installs the tests' host, which no host may be installed before. */
static void
host_tests_with_test_host_closure_0(void *captures)
{
	HostHost	test = {&HOST_TESTS_TEST_HOST_OPS, &HOST_TESTS_TEST};
	HostRefused refused;

	if (!host_install(test, &refused))
		STD_RESULT_EXPECT_FAILED(2612, 45, "no host is installed yet", &refused, host_Refused_Debug_fmt);
}

/* host_tests_with_test_host installs the tests' host, if no test has yet. */
static void
host_tests_with_test_host(void)
{
	std_once_call_once(&HOST_TESTS_INSTALL, host_tests_with_test_host_closure_0, NULL, RUST_FILE, 2685, 17);
}

/*
 * test_host_tests_the_doors_answer_from_the_installed_host: the doors answer
 * from the installed host, and its unique numbers don't repeat.
 */
void
test_host_tests_the_doors_answer_from_the_installed_host(void)
{
	host_tests_with_test_host();
	{
		StdSystemTime left = host_now();
		StdSystemTime right = std_SystemTime_add(STD_UNIX_EPOCH, std_Duration_from_secs(42), RUST_FILE, 2692, 27);

		STD_ASSERT_EQ(2619, 9, std_SystemTime_eq(&left, &right), &left, std_SystemTime_Debug_fmt, &right,
					  std_SystemTime_Debug_fmt);
	}
	{
		uint32		left = host_process_id();
		uint32		right = 7;

		STD_ASSERT_EQ(2620, 9, left == right, &left, std_u32_Debug_fmt, &right, std_u32_Debug_fmt);
	}
	{
		uint64		left = host_unique();
		uint64		right = host_unique();

		if (left == right)
			std_assert_failed(RUST_FILE, 2694, 9, "!=", &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt, NULL);
	}
}

/* test_host_tests_a_second_host_is_refused: a process has one host, so installing another is refused. */
void
test_host_tests_a_second_host_is_refused(void)
{
	HostHost	other = {&HOST_TESTS_TEST_HOST_OPS, &HOST_TESTS_OTHER};
	HostRefused refused;

	host_tests_with_test_host();
	STD_ASSERT(2628, 9, !host_install(other, &refused),
			   "install(&OTHER).is_err()");
}

/* test_host_tests_elapsed_saturates: the time since a reading after now is zero. */
void
test_host_tests_elapsed_saturates(void)
{
	StdDuration left;
	StdDuration right = STD_DURATION_ZERO;

	host_tests_with_test_host();
	{
		StdInstant	tmp_1 = host_instant();
		StdDuration tmp_2 = std_Duration_from_secs(1);

		left = host_elapsed(std_Instant_add(tmp_1, tmp_2, RUST_FILE, 2708, 28));
	}
	STD_ASSERT_EQ(2635, 9, std_Duration_eq(&left, &right), &left, std_Duration_Debug_fmt, &right,
				  std_Duration_Debug_fmt);
}

/* host_tests_AtomicU64_Default_default is AtomicU64's Default, zero. */
static void
host_tests_AtomicU64_Default_default(void *result, const StdLocation *caller)
{
	pg_atomic_init_u64((pg_atomic_uint64 *) result, 0);
}

/* HOST_TESTS_ATOMIC is AtomicU64's size, Default, and drop. */
static const StdDefaultType HOST_TESTS_ATOMIC = {sizeof(pg_atomic_uint64), _Alignof(pg_atomic_uint64), host_tests_AtomicU64_Default_default,
NULL};

/* HOST_TESTS_THE_PROCESS_KEEPS_ONE_BACKEND_LOCAL_VALUE_COUNT is the test's backend-local count, and its process value. */
static struct
{
	HostLocalBackendLocal local;
	pg_atomic_uint64 process;
}			HOST_TESTS_THE_PROCESS_KEEPS_ONE_BACKEND_LOCAL_VALUE_COUNT = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * test_host_tests_the_process_keeps_one_backend_local_value: the process's
 * one backend, which every server process is, keeps one value of a
 * backend-local static.
 */
void
test_host_tests_the_process_keeps_one_backend_local_value(void)
{
	HostLocalLocal tmp_1;
	volatile HostLocalLocal tmp_3;
	volatile bool tmp_3_live = false;
	uint64		left;
	uint64		right = 2;
	StdUnwind	unwind;

	host_tests_with_test_host();
	tmp_1 = host_local_BackendLocal_get(&HOST_TESTS_THE_PROCESS_KEEPS_ONE_BACKEND_LOCAL_VALUE_COUNT.local,
										&HOST_TESTS_ATOMIC);
	(void) pg_atomic_fetch_add_u64((pg_atomic_uint64 *) host_local_Local_Deref_deref(&tmp_1, &HOST_TESTS_ATOMIC), 1);
	host_local_Local_drop(&tmp_1, &HOST_TESTS_ATOMIC);
	tmp_1 = host_local_BackendLocal_get(&HOST_TESTS_THE_PROCESS_KEEPS_ONE_BACKEND_LOCAL_VALUE_COUNT.local,
										&HOST_TESTS_ATOMIC);
	(void) pg_atomic_fetch_add_u64((pg_atomic_uint64 *) host_local_Local_Deref_deref(&tmp_1, &HOST_TESTS_ATOMIC), 1);
	host_local_Local_drop(&tmp_1, &HOST_TESTS_ATOMIC);
	*(HostLocalLocal *) &tmp_3 = host_local_BackendLocal_get(&HOST_TESTS_THE_PROCESS_KEEPS_ONE_BACKEND_LOCAL_VALUE_COUNT.local,
															 &HOST_TESTS_ATOMIC);
	tmp_3_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		left = pg_atomic_read_u64((pg_atomic_uint64 *) host_local_Local_Deref_deref((HostLocalLocal *) &tmp_3, &HOST_TESTS_ATOMIC));
		STD_ASSERT_EQ(2645, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
		tmp_3_live = false;
		host_local_Local_drop((HostLocalLocal *) &tmp_3, &HOST_TESTS_ATOMIC);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (tmp_3_live)
			host_local_Local_drop((HostLocalLocal *) &tmp_3, &HOST_TESTS_ATOMIC);
		std_unwind_resume(&unwind);
	}
}

#endif							/* DOLTGRES_TEST_HOST */
