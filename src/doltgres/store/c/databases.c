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
 * A database's repository made, dropped, and moved: the work of the entries
 * createdb, dropdb, check_db_file_conflict, and movedb call, the fork's
 * records of a creation, a drop, and a move's two steps with their redo, and
 * the first repository, which the start after bootstrap makes by the marker
 * bootstrap leaves.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/record.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/databases.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/state.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/store/fixtures.h"
#include "doltgres/store/open.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "databases.rs"

/* STORE_DATABASES_DATABASE is a record's field of the database's id, its repository's. */
#define STORE_DATABASES_DATABASE 1

/* STORE_DATABASES_TABLESPACE is a record's field of the tablespace that holds the repository's own directory. */
#define STORE_DATABASES_TABLESPACE 2

/* STORE_DATABASES_TEMPLATE is a creation's field of the template's id. */
#define STORE_DATABASES_TEMPLATE 3

/* STORE_DATABASES_ROOT is a creation's field of the template's base tree's root, absent for a template with no base. */
#define STORE_DATABASES_ROOT 4

/* STORE_DATABASES_FROM is a move's field of the tablespace it moves from. */
#define STORE_DATABASES_FROM 2

/* STORE_DATABASES_TO is a move's field of the tablespace it moves to. */
#define STORE_DATABASES_TO 3

/* STORE_DATABASES_MOVING is the suffix of a repository's copy in the tablespace it moves to, which no open reads as a repository. */
#define STORE_DATABASES_MOVING ".moving"

/* STORE_DATABASES_TEMPLATE1 is the id of template1, the database whose repository the start after bootstrap makes. */
#define STORE_DATABASES_TEMPLATE1 1

/* STORE_DATABASES_FIRST is the marker bootstrap leaves in the data directory's directory of repositories. */
#define STORE_DATABASES_FIRST "first"

/* StoreDatabasesCreated is the fork's record of a repository's creation. */
typedef struct StoreDatabasesCreated
{
	uint32		database;
	uint32		tablespace;
	uint32		template;
	bool		has_root;
	uint8		root[32];
} StoreDatabasesCreated;

/* StoreDatabasesDropped is the fork's record of a repository's drop. */
typedef struct StoreDatabasesDropped
{
	uint32		database;
	uint32		tablespace;
} StoreDatabasesDropped;

/* StoreDatabasesMoved is the fork's record of a move's copy or of its finish. */
typedef struct StoreDatabasesMoved
{
	uint32		database;
	uint32		from;
	uint32		to;
} StoreDatabasesMoved;

/* store_databases_corrupt returns the data corruption error of a repository record that doesn't read, taking the reason. */
static HostError
store_databases_corrupt(StdString *why)
{
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("a repository's WAL record is corrupt: "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{why, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	std_String_drop(why);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* store_databases_corrupt_text returns the data corruption error of a record that doesn't read, for a fixed reason. */
static HostError
store_databases_corrupt_text(const char *why)
{
	StdString	reason;

	std_string_from_Checked(&reason, why, strlen(why));
	return store_databases_corrupt(&reason);
}

/* store_databases_corrupt_field returns the data corruption error of a record's field, between two texts. */
static HostError
store_databases_corrupt_field(const char *before, uint32 number, const char *after)
{
	StdString	reason;
	StdStr		first = {before, strlen(before)};
	StdStr		last = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&first, std_str_Display_fmt}, {&number, std_u32_Display_fmt}, {&last, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&reason, &arguments);
	return store_databases_corrupt(&reason);
}

/* store_databases_raised returns chunks' error as Postgres' error, taking it. */
static HostError
store_databases_raised(ChunksErrorError *error)
{
	return chunks_packs_shared_raised(error);
}

/* store_databases_Read_of reads a body's fields, a Vec<Field>, refusing one whose number isn't among those given. */
static bool
store_databases_Read_of(const uint8 *body, size_t body_len, const uint32 *known, size_t known_len, StdVec *result,
						HostError *error)
{
	ChunksErrorError failed;

	if (!chunks_record_fields(body, body_len, result, &failed))
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	for (size_t k = 0, k_len = result->len; k < k_len; k++)
	{
		uint32		number = STD_VEC_AT(result, ChunksRecordField, k).number;
		bool		ours = false;

		for (size_t n = 0; n < known_len; n++)
			ours = ours || known[n] == number;
		if (!ours)
		{
			std_vec_free_Checked(result, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
			*error = store_databases_corrupt_field("it has field ", number, ", which no repository record has");
			return false;
		}
	}
	return true;
}

/* store_databases_Read_field returns the field of a read record with a number, or NULL when it is absent. */
static const ChunksRecordField *
store_databases_Read_field(const StdVec *fields, uint32 number)
{
	for (size_t k = 0, k_len = fields->len; k < k_len; k++)
		if (STD_VEC_AT(fields, ChunksRecordField, k).number == number)
			return &STD_VEC_AT(fields, ChunksRecordField, k);
	return NULL;
}

/* store_databases_Read_oid returns a read record's required field's object id. */
static bool
store_databases_Read_oid(const StdVec *fields, uint32 number, uint32 *result, HostError *error)
{
	const ChunksRecordField *field = store_databases_Read_field(fields, number);
	ChunksErrorError failed;
	uint64		value;

	if (field == NULL)
	{
		*error = store_databases_corrupt_field("it lacks field ", number, "");
		return false;
	}
	if (!chunks_record_uint(field, &value, &failed))
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	if (value > PG_UINT32_MAX)
	{
		*error = store_databases_corrupt_field("its field ", number, " is past an object id");
		return false;
	}
	*result = (uint32) value;
	return true;
}

/* store_databases_Read_database returns a read record's database's id, which is never 0. */
static bool
store_databases_Read_database(const StdVec *fields, uint32 *result, HostError *error)
{
	if (!store_databases_Read_oid(fields, STORE_DATABASES_DATABASE, result, error))
		return false;
	if (*result == 0)
	{
		*error = store_databases_corrupt_text("its database is 0");
		return false;
	}
	return true;
}

/* store_databases_Moved_encode returns a move's record's body. */
static StdVec
store_databases_Moved_encode(const StoreDatabasesMoved *self)
{
	ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();

	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, self->database), STORE_DATABASES_FROM, self->from), STORE_DATABASES_TO, self->to);
	return chunks_record_RecordWriter_finish(&w);
}

/* store_databases_Moved_decode reads a move's body, refusing a field it doesn't know, a missing one it requires, and a body cut short as data corruption. */
static bool
store_databases_Moved_decode(const uint8 *body, size_t body_len, StoreDatabasesMoved *result, HostError *error)
{
	static const uint32 known[] = {STORE_DATABASES_DATABASE, STORE_DATABASES_FROM, STORE_DATABASES_TO};
	StdVec		fields;
	bool		ok;

	if (!store_databases_Read_of(body, body_len, known, lengthof(known), &fields, error))
		return false;
	ok = store_databases_Read_database(&fields, &result->database, error) &&
		store_databases_Read_oid(&fields, STORE_DATABASES_FROM, &result->from, error) &&
		store_databases_Read_oid(&fields, STORE_DATABASES_TO, &result->to, error);
	std_vec_free_Checked(&fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	return ok;
}

/* store_databases_Created_encode returns a creation's record's body. */
static StdVec
store_databases_Created_encode(const StoreDatabasesCreated *self)
{
	ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();

	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, self->database), STORE_DATABASES_TABLESPACE, self->tablespace);
	chunks_record_RecordWriter_uint(&w, STORE_DATABASES_TEMPLATE, self->template);
	if (self->has_root)
		chunks_record_RecordWriter_field(&w, STORE_DATABASES_ROOT, self->root, sizeof(self->root));
	return chunks_record_RecordWriter_finish(&w);
}

/* store_databases_Created_decode reads a creation's body, refusing a field it doesn't know, a missing one it requires, a root of another length, and a body cut short as data corruption. */
static bool
store_databases_Created_decode(const uint8 *body, size_t body_len, StoreDatabasesCreated *result, HostError *error)
{
	static const uint32 known[] = {STORE_DATABASES_DATABASE, STORE_DATABASES_TABLESPACE, STORE_DATABASES_TEMPLATE,
	STORE_DATABASES_ROOT};
	StdVec		fields;
	const ChunksRecordField *root;
	bool		ok = false;

	if (!store_databases_Read_of(body, body_len, known, lengthof(known), &fields, error))
		return false;
	root = store_databases_Read_field(&fields, STORE_DATABASES_ROOT);
	result->has_root = root != NULL;
	if (root != NULL)
	{
		if (root->bytes_len != sizeof(result->root))
		{
			*error = store_databases_corrupt_text("its root isn't 32 bytes");
			goto done;
		}
		memcpy(result->root, root->bytes, sizeof(result->root));
	}
	ok = store_databases_Read_database(&fields, &result->database, error) &&
		store_databases_Read_oid(&fields, STORE_DATABASES_TABLESPACE, &result->tablespace, error) &&
		store_databases_Read_oid(&fields, STORE_DATABASES_TEMPLATE, &result->template, error);
done:
	std_vec_free_Checked(&fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	return ok;
}

/* store_databases_Dropped_encode returns a drop's record's body. */
static StdVec
store_databases_Dropped_encode(const StoreDatabasesDropped *self)
{
	ChunksRecordRecordWriter w = chunks_record_RecordWriter_new();

	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, self->database), STORE_DATABASES_TABLESPACE, self->tablespace);
	return chunks_record_RecordWriter_finish(&w);
}

/* store_databases_Dropped_decode reads a drop's body, refusing a field it doesn't know, a missing one it requires, and a body cut short as data corruption. */
static bool
store_databases_Dropped_decode(const uint8 *body, size_t body_len, StoreDatabasesDropped *result, HostError *error)
{
	static const uint32 known[] = {STORE_DATABASES_DATABASE, STORE_DATABASES_TABLESPACE};
	StdVec		fields;
	bool		ok;

	if (!store_databases_Read_of(body, body_len, known, lengthof(known), &fields, error))
		return false;
	ok = store_databases_Read_database(&fields, &result->database, error) &&
		store_databases_Read_oid(&fields, STORE_DATABASES_TABLESPACE, &result->tablespace, error);
	std_vec_free_Checked(&fields, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	return ok;
}

/* store_databases_id checks a database's id as a repository's, raising an internal error for 0, which Postgres never gives. */
static bool
store_databases_id(uint32 database, HostError *error)
{
	StdString	message;

	if (database != 0)
		return true;
	std_string_from_Checked(&message, "a database's id is 0", strlen("a database's id is 0"));
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* store_databases_failed returns an error of a SQLSTATE whose message names a database after a text. */
HostError
store_databases_failed(const uint8 *code, const char *said, uint32 database)
{
	StdString	message;
	StdStr		shown = {said, strlen(said)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&shown, std_str_Display_fmt}, {&database, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork(code, &message);
}

/* store_databases_there reports whether a directory stands, as a listing of it that is there. */
static bool
store_databases_there(HostIoReadDoors io, const StdPathBuf *dir, bool *result, HostError *error)
{
	StdVec		names;

	if (!host_list_dir_if_there(io, dir->bytes.ptr, dir->bytes.len, result, &names, error))
		return false;
	if (*result)
		std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	return true;
}

/* store_databases_spaces returns pg_default and every other tablespace, a Vec<u32>. */
static bool
store_databases_spaces(HostIoReadDoors io, StdVec *result, HostError *error)
{
	StdVec		others;
	uint32		space = HOST_PG_DEFAULT;

	if (!host_tablespaces(io, &others, error))
		return false;
	std_vec_with_capacity_Checked(result, others.len + 1, sizeof(uint32), _Alignof(uint32));
	std_vec_push_Checked(result, &space, sizeof(space), _Alignof(uint32));
	for (size_t k = 0, k_len = others.len; k < k_len; k++)
		std_vec_push_Checked(result, &STD_VEC_AT(&others, uint32, k), sizeof(uint32), _Alignof(uint32));
	std_vec_free_Checked(&others, sizeof(uint32), _Alignof(uint32));
	return true;
}

/* store_databases_moving returns the directory of a repository's copy in the tablespace it moves to. */
static StdPathBuf
store_databases_moving(uint32 database, uint32 to)
{
	StdPathBuf	dir = host_versioned_dir(to);
	StdPathBuf	result;
	StdString	name;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(STORE_DATABASES_MOVING)};
	StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&name, &arguments);
	std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, &result);
	std_String_drop(&name);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return result;
}

/* store_databases_made_gather is made's gathering: the initial schemas' pairs, cloned, since a refused write gathers again. */
static bool
store_databases_made_gather(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							ChunksErrorError *error)
{
	const StdVec *initial = captures;

	(void) io;
	(void) read;
	(void) error;
	std_vec_with_capacity_Checked(result, initial->len, sizeof(RepoGathered), _Alignof(RepoGathered));
	for (size_t k = 0, k_len = initial->len; k < k_len; k++)
	{
		const TxnIdentityPaired *pair = &STD_VEC_AT(initial, TxnIdentityPaired, k);
		RepoGathered gathered = {.tag = REPO_GATHERED_PAIR, .u.pair.has_value = true};

		std_vec_clone(&gathered.u.pair.key, &pair->f0, 1, 1);
		std_vec_clone(&gathered.u.pair.value, &pair->f1, 1, 1);
		std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	}
	return true;
}

/* store_databases_saved is a save's after-write that does nothing. */
static bool
store_databases_saved(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/* store_databases_Paired_drop drops an identity pair. */
static void
store_databases_Paired_drop(void *pair)
{
	std_vec_free_Checked(&((TxnIdentityPaired *) pair)->f0, 1, 1);
	std_vec_free_Checked(&((TxnIdentityPaired *) pair)->f1, 1, 1);
}

/* store_databases_made_frame_0_t is made's state across its pad: the repository, the reader, and the initial pairs. */
typedef struct store_databases_made_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		tablespace;
	HostError  *error;
	bool		ok;
	RepoRepo	repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
	StdVec		initial;
	bool		initial_live;
} store_databases_made_frame_0_t;

/* store_databases_made_pad_0 is made's pad: the initial pairs, the reader, then the repository. */
static void
store_databases_made_pad_0(void *frame_)
{
	store_databases_made_frame_0_t *frame = frame_;

	if (frame->initial_live)
	{
		frame->initial_live = false;
		std_vec_drop_Checked(&frame->initial, sizeof(TxnIdentityPaired), _Alignof(TxnIdentityPaired),
							 store_databases_Paired_drop);
	}
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
}

/* store_databases_made_body_0 is made's body: the repository made, saved with the initial pairs, and closed. */
static void
store_databases_made_body_0(void *frame_)
{
	store_databases_made_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	bool		closed;

	frame->ok = false;
	if (!repo_Repo_create(frame->io, frame->repository, frame->tablespace, &frame->repo, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->repo_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (!txn_identity_initial(&frame->initial, frame->error))
		goto done;
	frame->initial_live = true;
	if (!repo_Repo_save(&frame->repo, frame->io, &frame->reading, false, NULL, store_databases_made_gather,
						&frame->initial, store_databases_saved, NULL, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->repo_live = false;
	closed = repo_Repo_close(&frame->repo, frame->io, &frame->reading, &failed);
	if (!closed)
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->ok = true;
done:
	store_databases_made_pad_0(frame);
}

/*
 * store_databases_made makes a repository in a tablespace, its identity tree
 * holding only the database's initial schemas' pairs, and closes the
 * process's hold of it.
 */
static bool
store_databases_made(HostIoIo *io, uint32 repository, uint32 tablespace, HostError *error)
{
	store_databases_made_frame_0_t frame = {.io = io, .repository = repository, .tablespace = tablespace,
	.error = error};

	std_landing(&frame, store_databases_made_body_0, store_databases_made_pad_0);
	return frame.ok;
}

/* store_databases_inserted appends a fork's record of a kind, taking its body. */
static bool
store_databases_inserted(HostIoIo *io, HostWalKind kind, StdVec *body, HostError *error)
{
	HostLsn		lsn;
	bool		ok = host_wal_insert(HOST_IO_IO_IO_DOORS(io), kind, body->ptr, body->len, &lsn, error);

	std_vec_free_Checked(body, 1, 1);
	return ok;
}

/* store_databases_warned reports an error at WARNING with a context, taking it, and returns its SQLSTATE. */
bool
store_databases_warned(HostIoIo *io, HostError *failure, const char *context, uint8 *code, HostError *error)
{
	memcpy(code, host_Error_sqlstate(failure), 5);
	return host_warn(HOST_IO_IO_IO_DOORS(io), failure, context, error);
}

/*
 * store_databases_created makes a new database's repository in its default
 * tablespace and appends the fork's record of it, in createdb's cleanup
 * block after Postgres' copy. A making that fails part way is removed, so the
 * failure callback finds nothing of the fork's: the failure is reported at
 * WARNING first, so no error is held through the removal, and the error
 * raised after it carries the failure's SQLSTATE.
 */
bool
store_databases_created(HostIoIo *io, uint32 template, uint32 database, uint32 tablespace, HostError *error)
{
	StoreDatabasesCreated record = {.database = database, .tablespace = tablespace, .template = template};
	HostError	failure;
	ChunksErrorError failed;
	StdVec		body;
	uint8		code[5];
	bool		removed;

	if (!store_databases_id(database, error))
		return false;
	if (store_databases_made(io, database, tablespace, &failure))
	{
		body = store_databases_Created_encode(&record);
		if (store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_CREATED, &body, &failure))
			return true;
	}
	if (!store_databases_warned(io, &failure, "CREATE DATABASE", code, error))
		return false;
	if (!chunks_packs_open_Packs_dropped(io, database, tablespace, &removed, &failed))
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	*error = store_databases_failed(code, "could not make the repository of database ", database);
	return false;
}

/* store_databases_held_t is what a function holds across its calls: the tablespaces, and two paths. */
typedef struct store_databases_held_t
{
	/* spaces is a Vec<u32>. */
	StdVec		spaces;
	bool		spaces_live;
	StdPathBuf	first;
	bool		first_live;
	StdPathBuf	second;
	bool		second_live;
} store_databases_held_t;

/* store_databases_held_drop drops what a function holds: its paths, then the tablespaces. */
static void
store_databases_held_drop(store_databases_held_t *held)
{
	if (held->second_live)
	{
		held->second_live = false;
		std_vec_free_Checked(&held->second.bytes, 1, 1);
	}
	if (held->first_live)
	{
		held->first_live = false;
		std_vec_free_Checked(&held->first.bytes, 1, 1);
	}
	if (held->spaces_live)
	{
		held->spaces_live = false;
		std_vec_free_Checked(&held->spaces, sizeof(uint32), _Alignof(uint32));
	}
}

/* store_databases_held_first sets the first path held. */
static void
store_databases_held_first(store_databases_held_t *held, StdPathBuf path)
{
	held->first = path;
	held->first_live = true;
}

/* store_databases_held_second sets the second path held. */
static void
store_databases_held_second(store_databases_held_t *held, StdPathBuf path)
{
	held->second = path;
	held->second_live = true;
}

/* store_databases_held_unset frees the paths held. */
static void
store_databases_held_unset(store_databases_held_t *held)
{
	if (held->second_live)
	{
		held->second_live = false;
		std_vec_free_Checked(&held->second.bytes, 1, 1);
	}
	if (held->first_live)
	{
		held->first_live = false;
		std_vec_free_Checked(&held->first.bytes, 1, 1);
	}
}

/* store_databases_frame_t is a function's state across its pad: its door, its arguments, what it holds, and its outcome. */
typedef struct store_databases_frame_t
{
	HostIoIo   *io;
	HostIoReadDoors read;
	uint32		database;
	uint32		from;
	uint32		to;
	const uint8 *body;
	size_t		body_len;
	bool	   *result;
	/* record is the record a replay reads, while it runs. */
	const void *record;
	uint32	   *space;
	HostError  *error;
	bool		ok;
	store_databases_held_t held;
	/* found is a Vec<(NonZeroU32, u32)> of the repositories. */
	StdVec		found;
	bool		found_live;
	/* names is a Vec<String> of a directory's names. */
	StdVec		names;
	bool		names_live;
} store_databases_frame_t;

/* store_databases_pad is a function's pad: the repositories found, then what it holds. */
static void
store_databases_pad(void *frame_)
{
	store_databases_frame_t *frame = frame_;

	if (frame->names_live)
	{
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
	store_databases_held_drop(&frame->held);
}

/* store_databases_found returns the tablespace of a repository among those found, reporting whether it is there. */
static bool
store_databases_found(const StdVec *found, uint32 repository, uint32 *tablespace)
{
	for (size_t k = 0, k_len = found->len; k < k_len; k++)
		if (STD_VEC_AT(found, std_TupleU32U32_t, k).f0 == repository)
		{
			*tablespace = STD_VEC_AT(found, std_TupleU32U32_t, k).f1;
			return true;
		}
	return false;
}

/* store_databases_stale removes every copy of a database's repository a move left in any tablespace. */
static bool
store_databases_stale(HostIoIo *io, store_databases_held_t *held, uint32 database, HostError *error)
{
	if (!store_databases_spaces(HOST_IO_IO_READ_DOORS(io), &held->spaces, error))
		return false;
	held->spaces_live = true;
	for (size_t k = 0, k_len = held->spaces.len; k < k_len; k++)
	{
		bool		removed;
		bool		ok;

		store_databases_held_first(held, store_databases_moving(database, STD_VEC_AT(&held->spaces, uint32, k)));
		ok = host_remove_tree(HOST_IO_IO_IO_DOORS(io), held->first.bytes.ptr, held->first.bytes.len, &removed, error);
		held->first_live = false;
		std_vec_free_Checked(&held->first.bytes, 1, 1);
		if (!ok)
			return false;
	}
	held->spaces_live = false;
	std_vec_free_Checked(&held->spaces, sizeof(uint32), _Alignof(uint32));
	return true;
}

/* store_databases_dropped_body_0 is dropped's body: the record, the removal, the emptied directories, and the checkpoint. */
static void
store_databases_dropped_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;
	ChunksErrorError failed;
	StoreDatabasesDropped record = {.database = frame->database};
	StdVec		body;
	bool		removed;

	frame->ok = false;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(io), &frame->found, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->found_live = true;
	if (!store_databases_stale(io, &frame->held, frame->database, frame->error))
		goto done;
	if (!store_databases_found(&frame->found, frame->database, &record.tablespace))
	{
		frame->ok = true;
		goto done;
	}
	body = store_databases_Dropped_encode(&record);
	if (!store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_DROPPED, &body, frame->error))
		goto done;
	if (!chunks_packs_open_Packs_dropped(io, frame->database, record.tablespace, &removed, &failed) ||
		!chunks_packs_open_emptied(io, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->ok = host_checkpoint_now(HOST_IO_IO_IO_DOORS(io), frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_dropped drops a database's repository in dropdb, after
 * Postgres' removal: it appends the fork's record of the drop, removes the
 * repository's directories, the mark last, and each tablespace's directory of
 * repositories left empty, and then checkpoints, so that no replay spans the
 * drop and a later database given its id. A database with no repository has
 * nothing to remove, which is no error.
 */
bool
store_databases_dropped(HostIoIo *io, uint32 database, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .database = database, .error = error};

	if (!store_databases_id(database, error))
		return false;
	std_landing(&frame, store_databases_dropped_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_conflicts_body_0 is conflicts' body: each tablespace's directory of the id and of a move's copy of it. */
static void
store_databases_conflicts_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;

	frame->ok = false;
	*frame->result = false;
	if (!store_databases_spaces(frame->read, &frame->held.spaces, frame->error))
		goto done;
	frame->held.spaces_live = true;
	for (size_t k = 0, k_len = frame->held.spaces.len; k < k_len && !*frame->result; k++)
	{
		uint32		space = STD_VEC_AT(&frame->held.spaces, uint32, k);

		store_databases_held_first(&frame->held, host_tablespace_dir(frame->database, space));
		store_databases_held_second(&frame->held, store_databases_moving(frame->database, space));
		if (!store_databases_there(frame->read, &frame->held.first, frame->result, frame->error) ||
			(!*frame->result &&
			 !store_databases_there(frame->read, &frame->held.second, frame->result, frame->error)))
			goto done;
		store_databases_held_unset(&frame->held);
	}
	frame->ok = true;
done:
	store_databases_pad(frame);
}

/*
 * store_databases_conflicts reports whether a directory of a repository of
 * a database's id, or a move's copy of one, stands in any tablespace, so that
 * the id isn't given to a new database while a removal or a move has left
 * something of it.
 */
bool
store_databases_conflicts(HostIoReadDoors io, uint32 database, bool *result, HostError *error)
{
	store_databases_frame_t frame = {.read = io, .database = database, .result = result, .error = error};

	std_landing(&frame, store_databases_conflicts_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_standing_body_0 is standing's body: each tablespace's directory of the repository, its drop finished or found standing. */
static void
store_databases_standing_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;

	frame->ok = false;
	*frame->result = false;
	if (!store_databases_spaces(HOST_IO_IO_READ_DOORS(io), &frame->held.spaces, frame->error))
		goto done;
	frame->held.spaces_live = true;
	for (size_t k = 0, k_len = frame->held.spaces.len; k < k_len; k++)
	{
		uint32		space = STD_VEC_AT(&frame->held.spaces, uint32, k);
		ChunksErrorError failed;
		bool		there;
		bool		dropping;
		bool		removed;

		store_databases_held_first(&frame->held, host_tablespace_dir(frame->database, space));
		if (!store_databases_there(HOST_IO_IO_READ_DOORS(io), &frame->held.first, &there, frame->error))
			goto done;
		store_databases_held_unset(&frame->held);
		if (!there)
			continue;
		if (!chunks_packs_open_dropping(HOST_IO_IO_READ_DOORS(io), frame->database, space, &dropping, &failed) ||
			(dropping && !chunks_packs_open_Packs_dropped(io, frame->database, space, &removed, &failed)))
		{
			*frame->error = store_databases_raised(&failed);
			goto done;
		}
		if (!dropping)
			*frame->result = true;
	}
	frame->ok = true;
done:
	store_databases_pad(frame);
}

/*
 * store_databases_standing finishes the removal of each directory of a
 * repository that carries a drop's mark and reports whether one without it
 * stands in any tablespace, which is the database's own repository.
 */
static bool
store_databases_standing(HostIoIo *io, uint32 repository, bool *result, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .database = repository, .result = result, .error = error};

	std_landing(&frame, store_databases_standing_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_redo_created_body_0 is redo_created's body: the leftovers finished, then the repository made unless it stands or its tablespace is gone. */
static void
store_databases_redo_created_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	StoreDatabasesCreated record;
	StdPathBuf	directory;
	StdStr		parent;
	bool		stands;
	bool		there;

	frame->ok = false;
	if (!store_databases_Created_decode(frame->body, frame->body_len, &record, frame->error) ||
		!store_databases_standing(frame->io, record.database, &stands, frame->error))
		goto done;
	if (stands)
	{
		frame->ok = true;
		goto done;
	}
	store_databases_held_first(&frame->held, host_versioned_dir(record.tablespace));
	parent = host_directory_of(frame->held.first.bytes.ptr, frame->held.first.bytes.len);
	std_slice_to_vec_Checked(&directory.bytes, parent.ptr, parent.len, 1, 1);
	store_databases_held_second(&frame->held, directory);
	if (!store_databases_there(HOST_IO_IO_READ_DOORS(frame->io), &frame->held.second, &there, frame->error))
		goto done;
	if (!there)
	{
		frame->ok = true;
		goto done;
	}
	if (record.has_root)
	{
		StdString	message;
		static const char refused[] = "a repository's creation from a template's base can't be replayed yet";

		/*
		 * TODO: a template's base is copied with the fold's half of createdb,
		 * which no build has yet.
		 */
		std_string_from_Checked(&message, refused, sizeof(refused) - 1);
		*frame->error = host_Error_fork((const uint8 *) "XX000", &message);
		goto done;
	}
	frame->ok = store_databases_made(frame->io, record.database, record.tablespace, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_redo_created replays a creation: it finishes the removal of
 * a drop's marked leftovers of the id in any tablespace, makes nothing when
 * another directory of the id stands in any, and makes nothing either when
 * the tablespace's version directory is gone, since a later record drops it;
 * otherwise it makes the repository there.
 */
bool
store_databases_redo_created(HostIoIo *io, const uint8 *body, size_t body_len, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .body = body, .body_len = body_len, .error = error};

	std_landing(&frame, store_databases_redo_created_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_quieted ends a database's sessions and keeps them out around a replay, which it runs with captures; a replay that raises keeps the lock. */
static bool
store_databases_quieted(HostIoIo *io, uint32 repository, bool (*replay) (void *captures, HostIoIo *io, HostError *error),
						void *captures, HostError *error)
{
	return host_quiet_database(HOST_IO_IO_IO_DOORS(io), repository, error) && replay(captures, io, error) &&
		host_unquiet_database(HOST_IO_IO_IO_DOORS(io), repository, error);
}

/* store_databases_redo_dropped_closure_0 is redo_dropped's replay: the states freed, the directories removed when its own stands, and the emptied ones. */
static bool
store_databases_redo_dropped_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	store_databases_frame_t *frame = captures;
	const StoreDatabasesDropped *record = frame->record;
	ChunksErrorError failed;
	bool		there;
	bool		removed;

	if (!txn_runs_dropped(io, record->database, error))
		return false;
	store_databases_held_first(&frame->held, host_tablespace_dir(record->database, record->tablespace));
	if (!store_databases_there(HOST_IO_IO_READ_DOORS(io), &frame->held.first, &there, error))
		return false;
	store_databases_held_unset(&frame->held);
	if ((there && !chunks_packs_open_Packs_dropped(io, record->database, record->tablespace, &removed, &failed)) ||
		!chunks_packs_open_emptied(io, &failed))
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	return true;
}

/* store_databases_redo_dropped_body_0 is redo_dropped's body: the record read, then its replay under the database's lock. */
static void
store_databases_redo_dropped_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	StoreDatabasesDropped record;

	frame->ok = store_databases_Dropped_decode(frame->body, frame->body_len, &record, frame->error);
	if (frame->ok)
	{
		frame->record = &record;
		frame->ok = store_databases_quieted(frame->io, record.database, store_databases_redo_dropped_closure_0, frame,
											frame->error);
		frame->record = NULL;
	}
	store_databases_pad(frame);
}

/*
 * store_databases_redo_dropped replays a drop under the database's lock: it
 * frees the repository's states, removes the repository's directories when
 * its own directory stands, and each tablespace's directory of repositories
 * left empty.
 */
bool
store_databases_redo_dropped(HostIoIo *io, const uint8 *body, size_t body_len, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .body = body, .body_len = body_len, .error = error};

	std_landing(&frame, store_databases_redo_dropped_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_copy_frame_0_t is copy's state across its pad: the repository's packs and the reader. */
typedef struct store_databases_copy_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		from;
	const StdPathBuf *into;
	HostError  *error;
	bool		ok;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} store_databases_copy_frame_0_t;

/* store_databases_copy_pad_0 is copy's pad: the reader, then the packs. */
static void
store_databases_copy_pad_0(void *frame_)
{
	store_databases_copy_frame_0_t *frame = frame_;

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
}

/* store_databases_copy_body_0 is copy's body: the packs opened, copied into the directory, and closed, a failure warned of before the close. */
static void
store_databases_copy_body_0(void *frame_)
{
	store_databases_copy_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	HostError	failure;
	uint8		code[5];

	frame->ok = false;
	if (!chunks_packs_open_Packs_open(frame->io, frame->repository, frame->from, &frame->packs, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->packs_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (chunks_packs_backend_Packs_copied(&frame->packs, frame->io, &frame->reading, frame->into->bytes.ptr,
										  frame->into->bytes.len, &failed))
	{
		frame->packs_live = false;
		frame->ok = chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed);
		if (!frame->ok)
			*frame->error = store_databases_raised(&failed);
		goto done;
	}
	failure = store_databases_raised(&failed);
	if (!store_databases_warned(frame->io, &failure, "ALTER DATABASE SET TABLESPACE", code, frame->error))
		goto done;
	frame->packs_live = false;
	if (!chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	*frame->error = store_databases_failed(code, "could not copy the repository of database ", frame->repository);
done:
	store_databases_copy_pad_0(frame);
}

/*
 * store_databases_copy copies a repository's own directory, the files its
 * pack record owes written first, into a directory. A failed copy is
 * reported at WARNING before the hold is closed, so no error is held through
 * the close.
 */
static bool
store_databases_copy(HostIoIo *io, uint32 repository, uint32 from, const StdPathBuf *into, HostError *error)
{
	store_databases_copy_frame_0_t frame = {.io = io, .repository = repository, .from = from, .into = into,
	.error = error};

	std_landing(&frame, store_databases_copy_body_0, store_databases_copy_pad_0);
	return frame.ok;
}

/* store_databases_moved_copy_body_0 is moved_copy's body: the stale copies removed, the copy made and recorded, or removed again on its failure. */
static void
store_databases_moved_copy_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;
	StoreDatabasesMoved record = {frame->database, frame->from, frame->to};
	HostError	failure;
	StdVec		body;
	uint8		code[5];
	bool		removed;

	frame->ok = false;
	store_databases_held_second(&frame->held, store_databases_moving(frame->database, frame->to));
	if (!store_databases_stale(io, &frame->held, frame->database, frame->error))
		goto done;
	if (store_databases_copy(io, frame->database, frame->from, &frame->held.second, &failure))
	{
		body = store_databases_Moved_encode(&record);
		if (store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_COPIED, &body, &failure))
		{
			frame->ok = host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-copied", frame->error);
			goto done;
		}
	}
	if (!store_databases_warned(io, &failure, "ALTER DATABASE SET TABLESPACE", code, frame->error) ||
		!host_remove_tree(HOST_IO_IO_IO_DOORS(io), frame->held.second.bytes.ptr, frame->held.second.bytes.len,
						  &removed, frame->error))
		goto done;
	*frame->error = store_databases_failed(code, "could not copy the repository of database ", frame->database);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_moved_copy copies a moving database's repository into the
 * tablespace it moves to, under a name no open reads, in movedb's cleanup
 * block after Postgres' copy, and appends the fork's record of it: a
 * leftover copy of an earlier try is removed first, and a copy that fails is
 * removed before its error is returned.
 */
bool
store_databases_moved_copy(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .database = database, .from = from, .to = to, .error = error};

	if (!store_databases_id(database, error))
		return false;
	std_landing(&frame, store_databases_moved_copy_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_finish_frame_0_t is finish's state across its pad: the old directory, the copy, the new directory, the repository, and the reader. */
typedef struct store_databases_finish_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		from;
	uint32		to;
	HostError  *error;
	bool		ok;
	StdPathBuf	old;
	bool		old_live;
	StdPathBuf	copy;
	bool		copy_live;
	StdPathBuf	new;
	bool		new_live;
	RepoRepo	repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
} store_databases_finish_frame_0_t;

/* store_databases_finish_pad_0 is finish's pad: the reader, the repository, then the paths. */
static void
store_databases_finish_pad_0(void *frame_)
{
	store_databases_finish_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
	if (frame->new_live)
	{
		frame->new_live = false;
		std_vec_free_Checked(&frame->new.bytes, 1, 1);
	}
	if (frame->copy_live)
	{
		frame->copy_live = false;
		std_vec_free_Checked(&frame->copy.bytes, 1, 1);
	}
	if (frame->old_live)
	{
		frame->old_live = false;
		std_vec_free_Checked(&frame->old.bytes, 1, 1);
	}
}

/* store_databases_finish_body_0 is finish's body: each step of a move's finish that isn't done. */
static void
store_databases_finish_body_0(void *frame_)
{
	store_databases_finish_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	ChunksErrorError failed;
	HostError	failure;
	uint8		code[5];

	frame->ok = false;
	frame->old = host_tablespace_dir(frame->repository, frame->from);
	frame->old_live = true;
	if (!chunks_packs_open_moving_from(io, frame->old.bytes.ptr, frame->old.bytes.len, &failed))
		goto raised;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-marked", frame->error))
		goto done;
	if (!chunks_packs_open_forgot(io, frame->repository, frame->from, &failed))
		goto raised;
	frame->copy = store_databases_moving(frame->repository, frame->to);
	frame->copy_live = true;
	frame->new = host_tablespace_dir(frame->repository, frame->to);
	frame->new_live = true;
	if (!chunks_packs_open_moved_in(io, frame->copy.bytes.ptr, frame->copy.bytes.len, frame->new.bytes.ptr,
									frame->new.bytes.len, &failed))
		goto raised;
	if (!txn_state_rehomed(io, frame->repository, frame->from, frame->to, frame->error))
		goto done;
	if (!repo_Repo_open(io, frame->repository, frame->to, &frame->repo, &failed))
		goto raised;
	frame->repo_live = true;
	if (!host_shared_read(io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (repo_Repo_rekeyed(&frame->repo, io, &frame->reading, frame->from, frame->to, &failed))
	{
		frame->repo_live = false;
		if (!repo_Repo_close(&frame->repo, io, &frame->reading, &failed))
			goto raised;
	}
	else
	{
		failure = store_databases_raised(&failed);
		if (!store_databases_warned(io, &failure, "ALTER DATABASE SET TABLESPACE", code, frame->error))
			goto done;
		frame->repo_live = false;
		if (!repo_Repo_close(&frame->repo, io, &frame->reading, &failed))
			goto raised;
		*frame->error = store_databases_failed(code, "could not move the repository of database ", frame->repository);
		goto done;
	}
	if (!chunks_packs_open_moved_from(io, frame->old.bytes.ptr, frame->old.bytes.len, &failed) ||
		!chunks_packs_open_emptied(io, &failed))
		goto raised;
	frame->ok = true;
	goto done;
raised:
	*frame->error = store_databases_raised(&failed);
done:
	store_databases_finish_pad_0(frame);
}

/*
 * store_databases_finish does whichever step of a move's finish isn't done,
 * each answering one another process did meanwhile: the mark in the old
 * directory, the pack record of the old directory freed, the copy renamed to
 * the repository's own name in its new tablespace, the database's states made
 * to name it there, its manifest's storages of the old tablespace put under
 * the new one, and the old directory removed, the mark last.
 */
static bool
store_databases_finish(HostIoIo *io, uint32 repository, uint32 from, uint32 to, HostError *error)
{
	store_databases_finish_frame_0_t frame = {.io = io, .repository = repository, .from = from, .to = to,
	.error = error};

	std_landing(&frame, store_databases_finish_body_0, store_databases_finish_pad_0);
	return frame.ok;
}

/*
 * store_databases_moved_finish makes a moved database's copy its repository
 * in movedb after Postgres' drop record of the old directory and before the
 * database's lock is released: it rekeys the database's states first, then
 * appends the record, finishes the move, and checkpoints.
 */
bool
store_databases_moved_finish(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StoreDatabasesMoved record = {database, from, to};
	StdVec		body;

	if (!store_databases_id(database, error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-finishing", error) ||
		!txn_state_rekeyed(io, database, from, to, error))
		return false;
	body = store_databases_Moved_encode(&record);
	return store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_MOVED, &body, error) &&
		store_databases_finish(io, database, from, to, error) &&
		host_checkpoint_now(HOST_IO_IO_IO_DOORS(io), error);
}

/* store_databases_unfinished_body_0 is unfinished's body: the move's copy standing and the repository's own directory not. */
static void
store_databases_unfinished_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	bool		own;

	frame->ok = false;
	store_databases_held_first(&frame->held, store_databases_moving(frame->database, frame->to));
	store_databases_held_second(&frame->held, host_tablespace_dir(frame->database, frame->to));
	if (!store_databases_there(frame->read, &frame->held.first, frame->result, frame->error))
		goto done;
	if (*frame->result)
	{
		if (!store_databases_there(frame->read, &frame->held.second, &own, frame->error))
			goto done;
		*frame->result = !own;
	}
	frame->ok = true;
done:
	store_databases_pad(frame);
}

/*
 * store_databases_unfinished reports whether a repository stands in the
 * tablespace its database's row names only as a move's copy, the move
 * committed and its finish never run.
 */
bool
store_databases_unfinished(HostIoReadDoors io, uint32 repository, uint32 tablespace, bool *result, HostError *error)
{
	store_databases_frame_t frame = {.read = io, .database = repository, .to = tablespace, .result = result,
	.error = error};

	std_landing(&frame, store_databases_unfinished_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_committed_body_0 is committed's body: the move's finish recorded and done, or the old directory on a standby. */
static void
store_databases_committed_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;
	uint32	   *result = frame->space;
	ChunksErrorError failed;
	StoreDatabasesMoved record = {.database = frame->database, .to = frame->to};
	StdVec		body;

	frame->ok = false;
	*result = frame->to;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(io), &frame->found, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->found_live = true;
	if (!store_databases_found(&frame->found, frame->database, &record.from))
	{
		frame->ok = true;
		goto done;
	}
	if (host_wal_stand(HOST_IO_IO_IO_DOORS(io)).recovering)
	{
		*result = record.from;
		frame->ok = true;
		goto done;
	}
	body = store_databases_Moved_encode(&record);
	frame->ok = store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_MOVED, &body, frame->error) &&
		store_databases_finish(io, frame->database, record.from, frame->to, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_committed returns the tablespace a session opens its
 * database's repository in: the one its row names, after finishing a move
 * that committed and whose finish never ran, when the repository stands
 * there only as its copy; on a hot standby, which can't log, the
 * repository's old directory.
 */
bool
store_databases_committed(HostIoIo *io, uint32 repository, uint32 tablespace, uint32 *result, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .database = repository, .to = tablespace, .space = result,
	.error = error};
	bool		unfinished;

	if (!store_databases_unfinished(HOST_IO_IO_READ_DOORS(io), repository, tablespace, &unfinished, error))
		return false;
	if (!unfinished)
	{
		*result = tablespace;
		return true;
	}
	std_landing(&frame, store_databases_committed_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_redo_copied_closure_0 is redo_copied's replay: nothing when the move's copy or its repository stands, or its source doesn't; otherwise the copy. */
static bool
store_databases_redo_copied_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	store_databases_frame_t *frame = captures;
	const StoreDatabasesMoved *record = frame->record;
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(io);
	bool		there;

	store_databases_held_first(&frame->held, host_tablespace_dir(record->database, record->to));
	store_databases_held_second(&frame->held, store_databases_moving(record->database, record->to));
	if (!store_databases_there(read, &frame->held.first, &there, error) ||
		(!there && !store_databases_there(read, &frame->held.second, &there, error)))
		return false;
	if (!there)
	{
		std_vec_free_Checked(&frame->held.first.bytes, 1, 1);
		frame->held.first = host_tablespace_dir(record->database, record->from);
		if (!store_databases_there(read, &frame->held.first, &there, error))
			return false;
		if (there)
			return store_databases_copy(io, record->database, record->from, &frame->held.second, error);
	}
	return true;
}

/* store_databases_redo_copied_body_0 is redo_copied's body: the record read, then its replay under the database's lock. */
static void
store_databases_redo_copied_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	StoreDatabasesMoved record;

	frame->ok = store_databases_Moved_decode(frame->body, frame->body_len, &record, frame->error);
	if (frame->ok)
	{
		frame->record = &record;
		frame->ok = store_databases_quieted(frame->io, record.database, store_databases_redo_copied_closure_0, frame,
											frame->error);
		frame->record = NULL;
	}
	store_databases_pad(frame);
}

/*
 * store_databases_redo_copied replays a move's copy under the database's
 * lock: it makes nothing when the repository already stands, or its copy
 * does, in the tablespace it moves to, or when the tablespace it moves from
 * has no directory of it; otherwise it makes the same copy from the
 * standby's own files.
 */
bool
store_databases_redo_copied(HostIoIo *io, const uint8 *body, size_t body_len, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .body = body, .body_len = body_len, .error = error};

	std_landing(&frame, store_databases_redo_copied_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_redo_moved_closure_0 is redo_moved's replay: the move's finish. */
static bool
store_databases_redo_moved_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const StoreDatabasesMoved *record = captures;

	return store_databases_finish(io, record->database, record->from, record->to, error);
}

/*
 * store_databases_redo_moved replays a move's finish under the database's
 * lock, doing whichever of its steps isn't done.
 */
bool
store_databases_redo_moved(HostIoIo *io, const uint8 *body, size_t body_len, HostError *error)
{
	StoreDatabasesMoved record;

	return store_databases_Moved_decode(body, body_len, &record, error) &&
		store_databases_quieted(io, record.database, store_databases_redo_moved_closure_0, &record, error);
}

/* store_databases_describe_moved returns a move's copy's or finish's description for pg_walinspect, raising a record that doesn't read. */
bool
store_databases_describe_moved(const uint8 *body, size_t body_len, StdString *result, HostError *error)
{
	StoreDatabasesMoved record;
	StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0), STD_FMT_LIT(" from tablespace "),
	STD_FMT_ARG(1), STD_FMT_LIT(" to "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&record.database, std_u32_Display_fmt}, {&record.from, std_u32_Display_fmt},
	{&record.to, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!store_databases_Moved_decode(body, body_len, &record, error))
		return false;
	std_fmt_format_Checked(result, &arguments);
	return true;
}

/* store_databases_describe_created returns a creation's description for pg_walinspect, raising a record that doesn't read. */
bool
store_databases_describe_created(const uint8 *body, size_t body_len, StdString *result, HostError *error)
{
	StoreDatabasesCreated record;
	StdStr		from;
	StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0), STD_FMT_LIT(" in tablespace "),
	STD_FMT_ARG(1), STD_FMT_LIT(" from template "), STD_FMT_ARG(2), STD_FMT_ARG(3)};
	StdFmtArg	args[] = {{&record.database, std_u32_Display_fmt}, {&record.tablespace, std_u32_Display_fmt},
	{&record.template, std_u32_Display_fmt}, {&from, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!store_databases_Created_decode(body, body_len, &record, error))
		return false;
	from.ptr = record.has_root ? " and its base" : "";
	from.len = strlen(from.ptr);
	std_fmt_format_Checked(result, &arguments);
	return true;
}

/* store_databases_describe_dropped returns a drop's description for pg_walinspect, raising a record that doesn't read. */
bool
store_databases_describe_dropped(const uint8 *body, size_t body_len, StdString *result, HostError *error)
{
	StoreDatabasesDropped record;
	StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0), STD_FMT_LIT(" in tablespace "),
	STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&record.database, std_u32_Display_fmt}, {&record.tablespace, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!store_databases_Dropped_decode(body, body_len, &record, error))
		return false;
	std_fmt_format_Checked(result, &arguments);
	return true;
}

/* store_databases_bootstrapped_body_0 is bootstrapped's body: the directory and the marker made, and both synced. */
static void
store_databases_bootstrapped_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	ChunksErrorError failed;
	HostFile	marker;

	frame->ok = false;
	store_databases_held_first(&frame->held, host_versioned_dir(HOST_PG_DEFAULT));
	if (!chunks_file_create_dir(doors, frame->held.first.bytes.ptr, frame->held.first.bytes.len, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	std_path_join(frame->held.first.bytes.ptr, frame->held.first.bytes.len, STORE_DATABASES_FIRST,
				  strlen(STORE_DATABASES_FIRST), &frame->held.second);
	frame->held.second_live = true;
	if (!chunks_file_create(doors, frame->held.second.bytes.ptr, frame->held.second.bytes.len, &marker, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	host_File_drop(&marker);
	frame->ok = host_sync_path(doors, frame->held.second.bytes.ptr, frame->held.second.bytes.len, false,
							   frame->error) &&
		host_sync_path(doors, frame->held.first.bytes.ptr, frame->held.first.bytes.len, true, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_bootstrapped leaves the marker that tells the start after
 * bootstrap to make template1's repository, in bootstrap's shutdown
 * checkpoint, which has no shared memory of the fork's: an empty file in the
 * data directory's directory of repositories, made when absent, and synced
 * by its path.
 */
bool
store_databases_bootstrapped(HostIoIo *io, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .error = error};

	std_landing(&frame, store_databases_bootstrapped_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_first_body_0 is first's body: template1's repository made when the marker stands and no repository does, then the marker removed. */
static void
store_databases_first_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	StdPathBuf	dir = host_versioned_dir(HOST_PG_DEFAULT);
	HostFile	marker;
	bool		has;
	bool		stands;

	frame->ok = false;
	std_path_join(dir.bytes.ptr, dir.bytes.len, STORE_DATABASES_FIRST, strlen(STORE_DATABASES_FIRST),
				  &frame->held.first);
	frame->held.first_live = true;
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!host_open_if_there(HOST_IO_IO_READ_DOORS(frame->io), frame->held.first.bytes.ptr, frame->held.first.bytes.len,
							&has, &marker, frame->error))
		goto done;
	if (!has)
	{
		frame->ok = true;
		goto done;
	}
	host_File_drop(&marker);
	if (!store_databases_standing(frame->io, STORE_DATABASES_TEMPLATE1, &stands, frame->error) ||
		(!stands && !store_databases_made(frame->io, STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT, frame->error)))
		goto done;
	frame->ok = host_remove_durably(HOST_IO_IO_IO_DOORS(frame->io), frame->held.first.bytes.ptr,
									frame->held.first.bytes.len, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_first makes template1's repository in the start after
 * bootstrap, when bootstrap's marker stands and no repository of template1
 * does, and then removes the marker; a crash between the two makes nothing
 * twice.
 */
bool
store_databases_first(HostIoIo *io, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .error = error};

	std_landing(&frame, store_databases_first_body_0, store_databases_pad);
	return frame.ok;
}

/* store_databases_repository_of reads a directory's name as a repository's id, reporting whether it is one: a u32's parse, never 0. */
static bool
store_databases_repository_of(const StdString *name, uint32 *result)
{
	StdParseIntError parse;
	uint64		value;

	if (!std_str_parse_u64(name->ptr, name->len, &value, &parse) || value > PG_UINT32_MAX || value == 0)
		return false;
	*result = (uint32) value;
	return true;
}

/* store_databases_moved_to finds the tablespace other than one a moved repository's copy, or its own directory, stands in. */
static bool
store_databases_moved_to(store_databases_frame_t *frame, uint32 repository, uint32 from, bool *found, uint32 *to)
{
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(frame->io);

	*found = false;
	for (size_t k = 0, k_len = frame->held.spaces.len; k < k_len && !*found; k++)
	{
		uint32		space = STD_VEC_AT(&frame->held.spaces, uint32, k);
		bool		there;

		if (space == from)
			continue;
		store_databases_held_second(&frame->held, store_databases_moving(repository, space));
		if (!store_databases_there(read, &frame->held.second, &there, frame->error))
			return false;
		std_vec_free_Checked(&frame->held.second.bytes, 1, 1);
		frame->held.second_live = false;
		if (!there)
		{
			store_databases_held_second(&frame->held, host_tablespace_dir(repository, space));
			if (!store_databases_there(read, &frame->held.second, &there, frame->error))
				return false;
			std_vec_free_Checked(&frame->held.second.bytes, 1, 1);
			frame->held.second_live = false;
		}
		if (there)
		{
			*found = true;
			*to = space;
		}
	}
	return true;
}

/* store_databases_marked_moved finishes the move of a repository whose old directory in a tablespace carries the finish's mark, or warns that its copy is nowhere. */
static bool
store_databases_marked_moved(store_databases_frame_t *frame, uint32 repository, uint32 from)
{
	HostIoIo   *io = frame->io;
	StdPathBuf	dir = host_tablespace_dir(repository, from);
	HostFile	mark;
	bool		has;
	bool		found;
	uint32		to;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_OPEN_MOVED_FROM, strlen(CHUNKS_PACKS_OPEN_MOVED_FROM),
				  &frame->held.first);
	frame->held.first_live = true;
	std_vec_free_Checked(&dir.bytes, 1, 1);
	if (!host_open_if_there(HOST_IO_IO_READ_DOORS(io), frame->held.first.bytes.ptr, frame->held.first.bytes.len, &has,
							&mark, frame->error))
		return false;
	frame->held.first_live = false;
	std_vec_free_Checked(&frame->held.first.bytes, 1, 1);
	if (!has)
		return true;
	host_File_drop(&mark);
	if (!store_databases_moved_to(frame, repository, from, &found, &to))
		return false;
	if (found)
		return store_databases_finish(io, repository, from, to, frame->error);
	{
		HostError	nowhere;
		StdString	message;
		StdFmtPiece pieces[] = {STD_FMT_LIT("the moved repository of database "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is in no tablespace")};
		StdFmtArg	args[] = {{&repository, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&message, &arguments);
		nowhere = host_Error_fork((const uint8 *) "XX001", &message);
		return host_warn(HOST_IO_IO_IO_DOORS(io), &nowhere, "the startup's open", frame->error);
	}
}

/* store_databases_marked_moves_body_0 is marked_moves' body: each tablespace's directories of repositories, each marked one's move finished. */
static void
store_databases_marked_moves_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;

	frame->ok = false;
	if (!store_databases_spaces(HOST_IO_IO_READ_DOORS(io), &frame->held.spaces, frame->error))
		goto done;
	frame->held.spaces_live = true;
	for (size_t k = 0, k_len = frame->held.spaces.len; k < k_len; k++)
	{
		uint32		from = STD_VEC_AT(&frame->held.spaces, uint32, k);
		StdPathBuf	versioned = host_versioned_dir(from);
		bool		has;
		bool		listed;

		listed = host_list_dir_if_there(HOST_IO_IO_READ_DOORS(io), versioned.bytes.ptr, versioned.bytes.len, &has,
										&frame->names, frame->error);
		std_vec_free_Checked(&versioned.bytes, 1, 1);
		if (!listed)
			goto done;
		if (!has)
			continue;
		frame->names_live = true;
		for (size_t n = 0, n_len = frame->names.len; n < n_len; n++)
		{
			uint32		repository;

			if (!store_databases_repository_of(&STD_VEC_AT(&frame->names, StdString, n), &repository))
				continue;
			if (!store_databases_marked_moved(frame, repository, from))
				goto done;
		}
		frame->names_live = false;
		std_vec_drop_Checked(&frame->names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	}
	frame->ok = true;
done:
	store_databases_pad(frame);
}

/*
 * store_databases_marked_moves finishes each move whose old directory
 * carries the finish's mark, in the startup's open: the copy, under its
 * moving name or its own, is found in another tablespace, and the finish's
 * remaining steps are done. A marked directory whose copy is nowhere is left,
 * and warned of.
 */
bool
store_databases_marked_moves(HostIoIo *io, HostError *error)
{
	store_databases_frame_t frame = {.io = io, .error = error};

	std_landing(&frame, store_databases_marked_moves_body_0, store_databases_pad);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_DATABASES_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_DATABASES_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* store_databases_tests_unwrapped panics at a line and column with a host error, as unwrap does. */
static pg_noreturn void
store_databases_tests_unwrapped(const HostError *error, uint32 line, uint32 col)
{
	STD_RESULT_EXPECT_FAILED(line, col, STORE_DATABASES_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* store_databases_tests_chunks_unwrapped panics at a line and column with chunks' error, as unwrap does. */
static pg_noreturn void
store_databases_tests_chunks_unwrapped(const ChunksErrorError *error, uint32 line, uint32 col)
{
	STD_RESULT_EXPECT_FAILED(line, col, STORE_DATABASES_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/* store_databases_tests_io_unwrapped panics at a line and column with an I/O error, as unwrap does. */
static pg_noreturn void
store_databases_tests_io_unwrapped(const StdIoError *error, uint32 line, uint32 col)
{
	STD_RESULT_EXPECT_FAILED(line, col, STORE_DATABASES_TESTS_UNWRAP, error, std_io_Error_Debug_fmt);
}

/* store_databases_tests_space makes a tablespace's version directory, as CREATE TABLESPACE leaves it, and returns the tablespace. */
static uint32
store_databases_tests_space(uint32 tablespace)
{
	StdPathBuf	version = host_versioned_dir(tablespace);
	StdStr		dir = host_directory_of(version.bytes.ptr, version.bytes.len);
	StdIoError	io_error;

	if (!std_fs_create_dir_all(dir.ptr, dir.len, &io_error))
		store_databases_tests_io_unwrapped(&io_error, 565, 87);
	std_vec_free_Checked(&version.bytes, 1, 1);
	return tablespace;
}

/* store_databases_tests_opens reports whether a repository opens in a tablespace. */
static bool
store_databases_tests_opens(HostIoIo *io, uint32 database, uint32 tablespace)
{
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_open(io, database, tablespace, &repo, &failed))
	{
		chunks_error_Error_drop(&failed);
		return false;
	}
	if (!host_shared_read(io, &reading, &error))
		store_databases_tests_unwrapped(&error, 572, 46);
	if (!repo_Repo_close(&repo, io, &reading, &failed))
		store_databases_tests_chunks_unwrapped(&failed, 573, 34);
	host_shared_Reader_drop(&reading);
	return true;
}

/* store_databases_tests_there reports whether a repository's directory stands in a tablespace. */
static bool
store_databases_tests_there(uint32 database, uint32 tablespace)
{
	StdPathBuf	dir = host_tablespace_dir(database, tablespace);
	bool		there = std_path_Path_exists(dir.bytes.ptr, dir.bytes.len);

	std_vec_free_Checked(&dir.bytes, 1, 1);
	return there;
}

/* store_databases_tests_exists reports whether a path stands, taking it. */
static bool
store_databases_tests_exists(StdPathBuf path)
{
	bool		there = std_path_Path_exists(path.bytes.ptr, path.bytes.len);

	std_vec_free_Checked(&path.bytes, 1, 1);
	return there;
}

/* store_databases_tests_table reads the pack table's file of a repository in a tablespace, which unwraps at a line and column. */
static StdVec
store_databases_tests_table(uint32 database, uint32 tablespace, uint32 line, uint32 col)
{
	StdPathBuf	dir = host_tablespace_dir(database, tablespace);
	StdPathBuf	path;
	StdVec		bytes;
	StdIoError	io_error;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &path);
	if (!std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error))
		store_databases_tests_io_unwrapped(&io_error, line, col);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return bytes;
}

/* store_databases_tests_bytes_eq asserts at a line that two byte vectors, which it frees, are the same. */
static void
store_databases_tests_bytes_eq(StdVec *got, StdVec *want, uint32 line)
{
	if (got->len != want->len || memcmp(got->ptr, want->ptr, got->len) != 0)
	{
		StdBytes	left = {got->ptr, got->len};
		StdBytes	right = {want->ptr, want->len};

		std_assert_failed(RUST_FILE, line, 9, "==", &left, std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt, NULL);
	}
	std_vec_free_Checked(got, 1, 1);
	std_vec_free_Checked(want, 1, 1);
}

/* store_databases_tests_ok asserts a call's success, which unwraps at a line and column. */
static void
store_databases_tests_ok(bool ok, const HostError *error, uint32 line, uint32 col)
{
	if (!ok)
		store_databases_tests_unwrapped(error, line, col);
}

/* store_databases_tests_conflicts reports whether a database's id conflicts, which unwraps at a line and column. */
static bool
store_databases_tests_conflicts(HostIoIo *io, uint32 database, uint32 line, uint32 col)
{
	HostError	error;
	bool		result;

	store_databases_tests_ok(store_databases_conflicts(HOST_IO_IO_READ_DOORS(io), database, &result, &error), &error,
							 line, col);
	return result;
}

/* store_databases_tests_dropping reports whether a repository's directory carries a drop's mark, which unwraps at a line and column. */
static bool
store_databases_tests_dropping(HostIoIo *io, uint32 database, uint32 tablespace, uint32 line, uint32 col)
{
	ChunksErrorError failed;
	bool		result;

	if (!chunks_packs_open_dropping(HOST_IO_IO_READ_DOORS(io), database, tablespace, &result, &failed))
		store_databases_tests_chunks_unwrapped(&failed, line, col);
	return result;
}

/*
 * test_store_databases_tests_a_creation_makes_an_empty_repository_and_leaves_its_template:
 * a creation makes a repository that opens, in its tablespace's directory of
 * repositories, which it makes when absent, and leaves the template's
 * repository byte for byte; its id then conflicts with a new database's.
 */
void
test_store_databases_tests_a_creation_makes_an_empty_repository_and_leaves_its_template(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdVec		template;
	StdVec		after;
	uint32		tablespace;

	store_databases_tests_ok(store_databases_created(io, 0, 931, HOST_PG_DEFAULT, &error), &error, 588, 47);
	template = store_databases_tests_table(931, HOST_PG_DEFAULT, 589, 103);
	tablespace = store_databases_tests_space(70001);
	store_databases_tests_ok(store_databases_created(io, 931, 932, tablespace, &error), &error, 591, 43);
	STD_ASSERT(592, 9, store_databases_tests_opens(io, 932, tablespace), "opens(io, 932, tablespace)");
	after = store_databases_tests_table(931, HOST_PG_DEFAULT, 593, 99);
	store_databases_tests_bytes_eq(&after, &template, 593);
	STD_ASSERT(594, 9, store_databases_tests_conflicts(io, 932, 594, 36) && !store_databases_tests_conflicts(io, 933, 594, 68),
			   "conflicts(io, 932).unwrap() && !conflicts(io, 933).unwrap()");
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_a_failed_creation_removes_what_it_made: a
 * creation that fails part way removes what it made, and a removal that
 * can't finish leaves the directory marked, which the id's conflict sees.
 */
void
test_store_databases_tests_a_failed_creation_removes_what_it_made(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir = host_tablespace_dir(934, HOST_PG_DEFAULT);
	StdPathBuf	table;
	StdIoError	io_error;
	HostError	error;
	bool		failed;

	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_TABLE, strlen(CHUNKS_PACKS_TABLE), &table);
	if (!std_fs_create_dir_all(table.bytes.ptr, table.bytes.len, &io_error))
		store_databases_tests_io_unwrapped(&io_error, 603, 98);
	failed = !store_databases_created(io, 0, 934, HOST_PG_DEFAULT, &error);
	if (failed)
		host_Error_drop(&error);
	STD_ASSERT(604, 9, failed, "created(io, 0, 934, host::PG_DEFAULT).is_err()");
	STD_ASSERT(605, 9, store_databases_tests_dropping(io, 934, HOST_PG_DEFAULT, 605, 61),
			   "chunks::dropping(io, 934, host::PG_DEFAULT).unwrap()");
	STD_ASSERT(606, 9, store_databases_tests_conflicts(io, 934, 606, 36), "conflicts(io, 934).unwrap()");
	STD_ASSERT(607, 9, !store_databases_tests_opens(io, 934, HOST_PG_DEFAULT), "!opens(io, 934, host::PG_DEFAULT)");
	std_vec_free_Checked(&table.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_probe appends a drop's record of one byte and returns its end, which unwraps at a line and column. */
static HostLsn
store_databases_tests_probe(HostIoIo *io, uint32 line, uint32 col)
{
	static const uint8 body[] = {1};
	HostError	error;
	HostLsn		end;

	store_databases_tests_ok(host_wal_insert(HOST_IO_IO_IO_DOORS(io), HOST_WAL_KIND_REPOSITORY_DROPPED, body,
											 lengthof(body), &end, &error), &error, line, col);
	return end;
}

/*
 * test_store_databases_tests_a_drop_removes_the_repository_and_checkpoints_after_it:
 * a drop logs its record, removes the repository and the tablespace's
 * directory of repositories it emptied, but never the data directory's, and
 * checkpoints after the removal; a database with no repository drops with
 * nothing to remove and nothing logged.
 */
void
test_store_databases_tests_a_drop_removes_the_repository_and_checkpoints_after_it(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		tablespace = store_databases_tests_space(70002);
	HostLsn		before;
	HostLsn		redo;
	HostLsn		first;
	HostLsn		second;
	HostLsn		third;
	uint64		left;
	uint64		right;

	store_databases_tests_ok(store_databases_created(io, 0, 935, tablespace, &error), &error, 618, 41);
	before = store_databases_tests_probe(io, 619, 80);
	store_databases_tests_ok(store_databases_dropped(io, 935, &error), &error, 620, 26);
	STD_ASSERT(621, 9, !store_databases_tests_there(935, tablespace) &&
			   !store_databases_tests_exists(host_versioned_dir(tablespace)),
			   "!there(935, tablespace) && !host::versioned_dir(tablespace).exists()");
	STD_ASSERT(622, 9, store_databases_tests_exists(host_versioned_dir(HOST_PG_DEFAULT)),
			   "host::versioned_dir(host::PG_DEFAULT).exists()");
	store_databases_tests_ok(host_last_redo(HOST_IO_IO_IO_DOORS(io), &redo, &error), &error, 623, 37);
	STD_ASSERT(623, 9, redo > before, "host::last_redo(io).unwrap() > before");
	first = store_databases_tests_probe(io, 624, 97);
	second = store_databases_tests_probe(io, 624, 97);
	store_databases_tests_ok(store_databases_dropped(io, 935, &error), &error, 626, 26);
	third = store_databases_tests_probe(io, 624, 97);
	left = STD_SUB(u64, third, second, 627, 20);
	right = STD_SUB(u64, second, first, 627, 40);
	STD_ASSERT_EQ_U64(627, 9, left, right);
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_creation returns a creation's record body. */
static StdVec
store_databases_tests_creation(uint32 database, uint32 tablespace)
{
	StoreDatabasesCreated record = {.database = database, .tablespace = tablespace, .template = 1};

	return store_databases_Created_encode(&record);
}

/* store_databases_tests_redo_created replays a creation of a database in a tablespace, which unwraps at a line and column. */
static void
store_databases_tests_redo_created(HostIoIo *io, uint32 database, uint32 tablespace, uint32 line, uint32 col)
{
	StdVec		body = store_databases_tests_creation(database, tablespace);
	HostError	error;

	store_databases_tests_ok(store_databases_redo_created(io, body.ptr, body.len, &error), &error, line, col);
	std_vec_free_Checked(&body, 1, 1);
}

/*
 * test_store_databases_tests_a_creation_and_a_drop_replay_twice_as_once: a
 * creation's and a drop's redo each run twice leave what one run leaves: the
 * second creation finds the repository standing and makes nothing, and the
 * second drop finds nothing to remove.
 */
void
test_store_databases_tests_a_creation_and_a_drop_replay_twice_as_once(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		tablespace = store_databases_tests_space(70003);
	StoreDatabasesDropped dropped = {936, 0};
	StdVec		table;
	StdVec		again;
	StdVec		body;

	store_databases_tests_redo_created(io, 936, tablespace, 642, 54);
	table = store_databases_tests_table(936, tablespace, 643, 94);
	store_databases_tests_redo_created(io, 936, tablespace, 644, 54);
	again = store_databases_tests_table(936, tablespace, 645, 93);
	store_databases_tests_bytes_eq(&again, &table, 645);
	STD_ASSERT(646, 9, store_databases_tests_opens(io, 936, tablespace), "opens(io, 936, tablespace)");
	dropped.tablespace = tablespace;
	body = store_databases_Dropped_encode(&dropped);
	store_databases_tests_ok(store_databases_redo_dropped(io, body.ptr, body.len, &error), &error, 648, 33);
	STD_ASSERT(649, 9, !store_databases_tests_there(936, tablespace) &&
			   !store_databases_tests_exists(host_versioned_dir(tablespace)),
			   "!there(936, tablespace) && !host::versioned_dir(tablespace).exists()");
	store_databases_tests_ok(store_databases_redo_dropped(io, body.ptr, body.len, &error), &error, 650, 33);
	STD_ASSERT(651, 9, !store_databases_tests_there(936, tablespace), "!there(936, tablespace)");
	std_vec_free_Checked(&body, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_written writes a file's bytes in a directory, which unwraps at a line and column. */
static void
store_databases_tests_written(const StdPathBuf *dir, const char *name, const char *contents, uint32 line, uint32 col)
{
	StdPathBuf	path;
	StdIoError	io_error;

	std_path_join(dir->bytes.ptr, dir->bytes.len, name, strlen(name), &path);
	if (!std_fs_write(path.bytes.ptr, path.bytes.len, (const uint8 *) contents, strlen(contents), &io_error))
		store_databases_tests_io_unwrapped(&io_error, line, col);
	std_vec_free_Checked(&path.bytes, 1, 1);
}

/*
 * test_store_databases_tests_a_creation_replays_over_a_marked_leftover_and_not_over_its_moved_repository:
 * a creation's redo over a drop's marked leftover of its id finishes that
 * removal and makes the repository anew, and one over the repository
 * standing in another tablespace, where a later move put it, makes nothing.
 */
void
test_store_databases_tests_a_creation_replays_over_a_marked_leftover_and_not_over_its_moved_repository(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdPathBuf	leftover = host_tablespace_dir(937, HOST_PG_DEFAULT);
	StdPathBuf	stray;
	StdIoError	io_error;
	uint32		moved;

	if (!std_fs_create_dir_all(leftover.bytes.ptr, leftover.bytes.len, &io_error))
		store_databases_tests_io_unwrapped(&io_error, 662, 44);
	store_databases_tests_written(&leftover, "dropped", "", 663, 55);
	store_databases_tests_written(&leftover, "leftover", "x", 664, 57);
	STD_ASSERT(665, 9, store_databases_tests_dropping(io, 937, HOST_PG_DEFAULT, 665, 61),
			   "chunks::dropping(io, 937, host::PG_DEFAULT).unwrap()");
	store_databases_tests_redo_created(io, 937, HOST_PG_DEFAULT, 666, 60);
	STD_ASSERT(667, 9, !store_databases_tests_dropping(io, 937, HOST_PG_DEFAULT, 667, 62),
			   "!chunks::dropping(io, 937, host::PG_DEFAULT).unwrap()");
	std_path_join(leftover.bytes.ptr, leftover.bytes.len, "leftover", 8, &stray);
	STD_ASSERT(668, 9, !store_databases_tests_exists(stray),
			   "!host::tablespace_dir(937, host::PG_DEFAULT).join(\"leftover\").exists()");
	STD_ASSERT(669, 9, store_databases_tests_opens(io, 937, HOST_PG_DEFAULT), "opens(io, 937, host::PG_DEFAULT)");
	moved = store_databases_tests_space(70004);
	store_databases_tests_ok(store_databases_created(io, 0, 938, moved, &error), &error, 671, 36);
	store_databases_tests_redo_created(io, 938, HOST_PG_DEFAULT, 672, 60);
	STD_ASSERT(673, 9, !store_databases_tests_there(938, HOST_PG_DEFAULT) && store_databases_tests_there(938, moved),
			   "!there(938, host::PG_DEFAULT) && there(938, moved)");
	std_vec_free_Checked(&leftover.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_a_creation_replays_into_a_dropped_tablespace_as_nothing:
 * a creation's redo into a tablespace a later record dropped, whose version
 * directory is gone, makes nothing and fails nothing.
 */
void
test_store_databases_tests_a_creation_replays_into_a_dropped_tablespace_as_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;

	store_databases_tests_redo_created(io, 939, 70005, 682, 50);
	STD_ASSERT(683, 9, !store_databases_tests_there(939, 70005) && !store_databases_tests_conflicts(io, 939, 683, 60),
			   "!there(939, 70_005) && !conflicts(io, 939).unwrap()");
	simc_TestIo_drop(&test_io);
}


/* store_databases_tests_corrupt_eq asserts at a line and column that a decode's outcome is an error of the data-corrupted SQLSTATE, which it drops. */
static void
store_databases_tests_corrupt_eq(bool ok, HostError *error, uint32 line, uint32 col)
{
	static const uint8 corrupt[5] = {'X', 'X', '0', '0', '1'};
	uint8		code[5] = {0};
	bool		same;

	if (!ok)
	{
		memcpy(code, host_Error_sqlstate(error), 5);
		host_Error_drop(error);
	}
	same = !ok && memcmp(code, corrupt, 5) == 0;
	if (!same)
	{
		StdOptionRef got = {!ok, STD_ITEM(code, store_tests_Sqlstate_Debug_fmt)};
		StdOptionRef want = {true, STD_ITEM(corrupt, store_tests_Sqlstate_Debug_fmt)};

		std_assert_failed(RUST_FILE, line, col, "==", &got, std_option_Debug_fmt, &want, std_option_Debug_fmt, NULL);
	}
}

/*
 * test_store_databases_tests_a_record_that_doesnt_read_is_corrupt: a record
 * with a field no repository record has, one missing a field it requires, a
 * root of another length, and a body cut short are data corruption.
 */
void
test_store_databases_tests_a_record_that_doesnt_read_is_corrupt(void)
{
	ChunksRecordRecordWriter w;
	StdVec		bodies[4];
	StdVec		creation = store_databases_tests_creation(940, 1663);
	StoreDatabasesCreated created;
	StoreDatabasesDropped dropped = {940, 1663};
	HostError	error;
	StdVec		body;
	StdString	shown;
	bool		ok;
	static const char described[] = "database 940 in tablespace 1663";

	w = chunks_record_RecordWriter_new();
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, 940), STORE_DATABASES_TABLESPACE, 1663), 9, 1);
	bodies[0] = chunks_record_RecordWriter_finish(&w);
	w = chunks_record_RecordWriter_new();
	chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, 940);
	bodies[1] = chunks_record_RecordWriter_finish(&w);
	w = chunks_record_RecordWriter_new();
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(&w, 1, 940), 2, 1663), 3, 1);
	chunks_record_RecordWriter_field(&w, STORE_DATABASES_ROOT, (const uint8 *) "short", 5);
	bodies[2] = chunks_record_RecordWriter_finish(&w);
	std_slice_to_vec_Checked(&bodies[3], creation.ptr, 3, 1, 1);
	for (size_t k = 0; k < lengthof(bodies); k++)
	{
		ok = store_databases_Created_decode(bodies[k].ptr, bodies[k].len, &created, &error);
		store_databases_tests_corrupt_eq(ok, &error, 695, 13);
	}
	ok = store_databases_Dropped_decode(bodies[0].ptr, bodies[0].len, &dropped, &error);
	store_databases_tests_corrupt_eq(ok, &error, 697, 9);
	dropped.database = 940;
	dropped.tablespace = 1663;
	body = store_databases_Dropped_encode(&dropped);
	store_databases_tests_ok(store_databases_describe_dropped(body.ptr, body.len, &shown, &error), &error, 699, 97);
	if (shown.len != strlen(described) || memcmp(shown.ptr, described, shown.len) != 0)
	{
		StdStr		want = {described, strlen(described)};

		std_assert_failed(RUST_FILE, 698, 9, "==", &shown, std_String_Debug_fmt, &want, std_str_Debug_fmt, NULL);
	}
	std_String_drop(&shown);
	std_vec_free_Checked(&body, 1, 1);
	for (size_t k = 0; k < lengthof(bodies); k++)
		std_vec_free_Checked(&bodies[k], 1, 1);
	std_vec_free_Checked(&creation, 1, 1);
}

/* store_databases_tests_logs_empty reports whether the simulation reported nothing at LOG since its reports were last taken, taking them. */
static bool
store_databases_tests_logs_empty(void)
{
	StdVec		logs = simc_take_logs();
	bool		empty = logs.len == 0;

	store_tests_Logs_drop(&logs);
	return empty;
}





/* store_databases_tests_marker returns the path of bootstrap's marker. */
static StdPathBuf
store_databases_tests_marker(void)
{
	StdPathBuf	dir = host_versioned_dir(HOST_PG_DEFAULT);
	StdPathBuf	marker;

	std_path_join(dir.bytes.ptr, dir.bytes.len, STORE_DATABASES_FIRST, strlen(STORE_DATABASES_FIRST), &marker);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return marker;
}

/* store_databases_tests_unmade removes template1's repository's directory, its outcome dropped. */
static void
store_databases_tests_unmade(void)
{
	StdPathBuf	dir = host_tablespace_dir(STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT);
	StdIoError	io_error;

	(void) std_fs_remove_dir_all(dir.bytes.ptr, dir.bytes.len, &io_error);
	std_vec_free_Checked(&dir.bytes, 1, 1);
}

/*
 * test_store_databases_tests_the_first_repository_is_made_once_by_bootstraps_marker:
 * the start after bootstrap makes template1's repository by bootstrap's
 * marker, silently, and removes the marker; with the repository standing it
 * makes nothing and removes the marker, and with no marker it makes nothing.
 */
void
test_store_databases_tests_the_first_repository_is_made_once_by_bootstraps_marker(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdVec		table;
	StdVec		again;
	StdVec		logs;

	store_databases_tests_unmade();
	store_databases_tests_ok(store_databases_first(io, &error), &error, 713, 19);
	STD_ASSERT(714, 9, !store_databases_tests_there(STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT),
			   "!there(TEMPLATE1, host::PG_DEFAULT)");
	store_databases_tests_ok(store_databases_bootstrapped(io, &error), &error, 715, 26);
	logs = simc_take_logs();
	store_tests_Logs_drop(&logs);
	store_databases_tests_ok(store_databases_first(io, &error), &error, 717, 19);
	STD_ASSERT(718, 9, store_databases_tests_opens(io, STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT) &&
			   !store_databases_tests_exists(store_databases_tests_marker()),
			   "opens(io, TEMPLATE1, host::PG_DEFAULT) && !marker.exists()");
	STD_ASSERT(719, 9, store_databases_tests_logs_empty(), "sim::take_logs().is_empty()");
	table = store_databases_tests_table(STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT, 720, 106);
	store_databases_tests_ok(store_databases_bootstrapped(io, &error), &error, 721, 26);
	store_databases_tests_ok(store_databases_first(io, &error), &error, 722, 19);
	STD_ASSERT(723, 9, !store_databases_tests_exists(store_databases_tests_marker()) &&
			   store_databases_tests_logs_empty(), "!marker.exists() && sim::take_logs().is_empty()");
	again = store_databases_tests_table(STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT, 725, 98);
	store_databases_tests_bytes_eq(&again, &table, 724);
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_moved moves a repository from one tablespace to another, as movedb does: its copy, then its finish. */
static void
store_databases_tests_moved(HostIoIo *io, uint32 database, uint32 from, uint32 to)
{
	HostError	error;

	store_databases_tests_ok(store_databases_moved_copy(io, database, from, to, &error), &error, 732, 44);
	store_databases_tests_ok(store_databases_moved_finish(io, database, from, to, &error), &error, 733, 46);
}

/* store_databases_tests_moving_there reports whether a repository's copy stands in the tablespace it moves to. */
static bool
store_databases_tests_moving_there(uint32 database, uint32 to)
{
	return store_databases_tests_exists(store_databases_moving(database, to));
}

/* store_databases_tests_probe_moved appends a move's record of one byte and returns its end, which unwraps at a line and column. */
static HostLsn
store_databases_tests_probe_moved(HostIoIo *io, uint32 line, uint32 col)
{
	static const uint8 body[] = {1};
	HostError	error;
	HostLsn		end;

	store_databases_tests_ok(host_wal_insert(HOST_IO_IO_IO_DOORS(io), HOST_WAL_KIND_REPOSITORY_MOVED, body,
											 lengthof(body), &end, &error), &error, line, col);
	return end;
}

/*
 * test_store_databases_tests_a_move_puts_the_repository_in_its_new_tablespace:
 * a move copies the repository into its new tablespace under a name no open
 * reads, and its finish makes the copy the repository, removes the old
 * directory with the old tablespace's directory of repositories it emptied,
 * and checkpoints; the repository then opens in its new tablespace.
 */
void
test_store_databases_tests_a_move_puts_the_repository_in_its_new_tablespace(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70011);
	uint32		to = store_databases_tests_space(70012);
	StdVec		table;
	StdVec		after;
	HostLsn		before;
	HostLsn		redo;

	store_databases_tests_ok(store_databases_created(io, 0, 951, from, &error), &error, 745, 35);
	table = store_databases_tests_table(951, from, 746, 88);
	store_databases_tests_ok(store_databases_moved_copy(io, 951, from, to, &error), &error, 747, 39);
	STD_ASSERT(748, 9, store_databases_tests_moving_there(951, to) && store_databases_tests_there(951, from) &&
			   !store_databases_tests_there(951, to), "moving(951, to).exists() && there(951, from) && !there(951, to)");
	STD_ASSERT(749, 9, store_databases_tests_conflicts(io, 951, 749, 36), "conflicts(io, 951).unwrap()");
	before = store_databases_tests_probe_moved(io, 750, 78);
	store_databases_tests_ok(store_databases_moved_finish(io, 951, from, to, &error), &error, 751, 41);
	STD_ASSERT(752, 9, !store_databases_tests_moving_there(951, to) && !store_databases_tests_there(951, from) &&
			   !store_databases_tests_exists(host_versioned_dir(from)),
			   "!moving(951, to).exists() && !there(951, from) && !host::versioned_dir(from).exists()");
	store_databases_tests_ok(host_last_redo(HOST_IO_IO_IO_DOORS(io), &redo, &error), &error, 753, 37);
	STD_ASSERT(753, 9, redo > before, "host::last_redo(io).unwrap() > before");
	after = store_databases_tests_table(951, to, 754, 82);
	STD_ASSERT(754, 9, after.len >= table.len && memcmp(after.ptr, table.ptr, table.len) == 0,
			   "std::fs::read(host::tablespace_dir(951, to).join(chunks::TABLE)).unwrap().starts_with(&table)");
	std_vec_free_Checked(&after, 1, 1);
	std_vec_free_Checked(&table, 1, 1);
	STD_ASSERT(755, 9, store_databases_tests_opens(io, 951, to), "opens(io, 951, to)");
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_redo_moving replays a move's copy or finish, which unwraps at a line and column. */
static void
store_databases_tests_redo_moving(HostIoIo *io, bool copied, uint32 database, uint32 from, uint32 to, uint32 line,
								  uint32 col)
{
	StoreDatabasesMoved record = {database, from, to};
	StdVec		body = store_databases_Moved_encode(&record);
	HostError	error;
	bool		ok = copied ? store_databases_redo_copied(io, body.ptr, body.len, &error) :
		store_databases_redo_moved(io, body.ptr, body.len, &error);

	store_databases_tests_ok(ok, &error, line, col);
	std_vec_free_Checked(&body, 1, 1);
}

/*
 * test_store_databases_tests_a_moves_records_replay_twice_as_once_and_after_it_finished:
 * a move's copy and finish each replayed twice leave what one run leaves;
 * replayed after the move finished and its old tablespace's directory went,
 * they make nothing and fail nothing; and a copy replayed after the finish
 * renamed the copy, its old directory still standing marked, makes no second
 * copy.
 */
void
test_store_databases_tests_a_moves_records_replay_twice_as_once_and_after_it_finished(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70013);
	uint32		to = store_databases_tests_space(70014);
	StdPathBuf	old;
	StdPathBuf	copy;
	StdPathBuf	own;
	StdIoError	io_error;

	store_databases_tests_ok(store_databases_created(io, 0, 952, from, &error), &error, 766, 35);
	store_databases_tests_redo_moving(io, true, 952, from, to, 768, 34);
	store_databases_tests_redo_moving(io, true, 952, from, to, 769, 34);
	STD_ASSERT(770, 9, store_databases_tests_moving_there(952, to) && store_databases_tests_there(952, from),
			   "moving(952, to).exists() && there(952, from)");
	store_databases_tests_redo_moving(io, false, 952, from, to, 771, 33);
	store_databases_tests_redo_moving(io, false, 952, from, to, 772, 33);
	STD_ASSERT(773, 9, !store_databases_tests_moving_there(952, to) && !store_databases_tests_there(952, from) &&
			   store_databases_tests_opens(io, 952, to),
			   "!moving(952, to).exists() && !there(952, from) && opens(io, 952, to)");
	store_databases_tests_redo_moving(io, true, 952, from, to, 774, 34);
	store_databases_tests_redo_moving(io, false, 952, from, to, 775, 33);
	STD_ASSERT(776, 9, !store_databases_tests_moving_there(952, to) && store_databases_tests_opens(io, 952, to),
			   "!moving(952, to).exists() && opens(io, 952, to)");
	store_databases_tests_ok(store_databases_created(io, 0, 956, from, &error), &error, 777, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 956, from, to, &error), &error, 778, 39);
	old = host_tablespace_dir(956, from);
	store_databases_tests_written(&old, CHUNKS_PACKS_OPEN_MOVED_FROM, "", 779, 87);
	copy = store_databases_moving(956, to);
	own = host_tablespace_dir(956, to);
	if (!std_fs_rename(copy.bytes.ptr, copy.bytes.len, own.bytes.ptr, own.bytes.len, &io_error))
		store_databases_tests_io_unwrapped(&io_error, 780, 73);
	store_databases_tests_redo_moving(io, true, 956, from, to, 781, 82);
	STD_ASSERT(782, 9, !store_databases_tests_moving_there(956, to), "!moving(956, to).exists()");
	std_vec_free_Checked(&own.bytes, 1, 1);
	std_vec_free_Checked(&copy.bytes, 1, 1);
	std_vec_free_Checked(&old.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_started runs the startup process's open at a redo point, with no label, which unwraps at a line and column 40. */
static void
store_databases_tests_started(HostIoIo *io, uint32 line)
{
	HostError	error;

	store_databases_tests_ok(store_open_startup_open(io, 50, 0, &error), &error, line, 40);
}

/*
 * test_store_databases_tests_a_start_finishes_a_marked_move_and_leaves_an_unmarked_copy:
 * a start finishes a move whose old directory carries the finish's mark,
 * whether its copy still has its moving name or was renamed already, and
 * leaves an unmarked copy, which only a backend that reads the row can judge.
 */
void
test_store_databases_tests_a_start_finishes_a_marked_move_and_leaves_an_unmarked_copy(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70015);
	uint32		to = store_databases_tests_space(70016);
	StdPathBuf	old;

	store_databases_tests_ok(store_databases_created(io, 0, 953, from, &error), &error, 792, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 953, from, to, &error), &error, 793, 39);
	store_databases_tests_started(io, 794);
	STD_ASSERT(795, 9, store_databases_tests_moving_there(953, to) && store_databases_tests_there(953, from),
			   "moving(953, to).exists() && there(953, from)");
	old = host_tablespace_dir(953, from);
	store_databases_tests_written(&old, CHUNKS_PACKS_OPEN_MOVED_FROM, "", 796, 87);
	std_vec_free_Checked(&old.bytes, 1, 1);
	store_databases_tests_started(io, 797);
	STD_ASSERT(798, 9, !store_databases_tests_moving_there(953, to) && !store_databases_tests_there(953, from) &&
			   store_databases_tests_opens(io, 953, to),
			   "!moving(953, to).exists() && !there(953, from) && opens(io, 953, to)");
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_committed_eq asserts at a line that a session's open of a database answers the tablespace given, which unwraps at column 55. */
static void
store_databases_tests_committed_eq(HostIoIo *io, uint32 database, uint32 tablespace, uint32 line)
{
	HostError	error;
	uint32		own;

	store_databases_tests_ok(store_databases_committed(io, database, tablespace, &own, &error), &error, line, 55);
	STD_ASSERT_EQ(line, 9, own == tablespace, &own, std_u32_Debug_fmt, &tablespace, std_u32_Debug_fmt);
}

/*
 * test_store_databases_tests_an_open_finishes_a_committed_move: a session's
 * open of a database whose row names a tablespace where the repository
 * stands only as its copy finishes the move that committed, and a second
 * open finds it done.
 */
void
test_store_databases_tests_an_open_finishes_a_committed_move(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70017);
	uint32		to = store_databases_tests_space(70018);

	store_databases_tests_ok(store_databases_created(io, 0, 954, from, &error), &error, 808, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 954, from, to, &error), &error, 809, 39);
	store_databases_tests_committed_eq(io, 954, to, 810);
	STD_ASSERT(811, 9, !store_databases_tests_moving_there(954, to) && !store_databases_tests_there(954, from) &&
			   store_databases_tests_opens(io, 954, to),
			   "!moving(954, to).exists() && !there(954, from) && opens(io, 954, to)");
	store_databases_tests_committed_eq(io, 954, to, 812);
	STD_ASSERT(813, 9, store_databases_tests_opens(io, 954, to), "opens(io, 954, to)");
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_a_drop_and_a_move_remove_a_stale_copy: a drop
 * of a database, and its next move, remove a copy an earlier move left in
 * any tablespace.
 */
void
test_store_databases_tests_a_drop_and_a_move_remove_a_stale_copy(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70019);
	uint32		to = store_databases_tests_space(70020);
	uint32		other = store_databases_tests_space(70021);

	store_databases_tests_ok(store_databases_created(io, 0, 955, from, &error), &error, 822, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 955, from, other, &error), &error, 823, 42);
	store_databases_tests_moved(io, 955, from, to);
	STD_ASSERT(825, 9, !store_databases_tests_moving_there(955, other) && store_databases_tests_opens(io, 955, to),
			   "!moving(955, other).exists() && opens(io, 955, to)");
	store_databases_tests_ok(store_databases_moved_copy(io, 955, to, other, &error), &error, 826, 40);
	store_databases_tests_ok(store_databases_dropped(io, 955, &error), &error, 827, 26);
	STD_ASSERT(828, 9, !store_databases_tests_moving_there(955, other) && !store_databases_tests_there(955, to) &&
			   !store_databases_tests_conflicts(io, 955, 828, 88),
			   "!moving(955, other).exists() && !there(955, to) && !conflicts(io, 955).unwrap()");
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_repo opens a repository, which unwraps at a line and column. */
static void
store_databases_tests_repo(HostIoIo *io, uint32 database, uint32 tablespace, RepoRepo *result, uint32 line,
						   uint32 col)
{
	ChunksErrorError failed;

	if (!repo_Repo_open(io, database, tablespace, result, &failed))
		store_databases_tests_chunks_unwrapped(&failed, line, col);
}

/* store_databases_tests_finished finishes a move, which unwraps at a line and column 47. */
static void
store_databases_tests_finished(HostIoIo *io, uint32 database, uint32 from, uint32 to, uint32 line)
{
	HostError	error;

	store_databases_tests_ok(store_databases_finish(io, database, from, to, &error), &error, line, 47);
}

/*
 * test_store_databases_tests_two_finishes_of_one_move_leave_one_pack_record:
 * two backends that both found a committed move unfinished finish it once:
 * the second's finish, run after the first's open made the repository's
 * record in its new tablespace, frees no record of it, so both open one
 * record.
 */
void
test_store_databases_tests_two_finishes_of_one_move_leave_one_pack_record(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	ChunksErrorError failed;
	uint32		from = store_databases_tests_space(70022);
	uint32		to = store_databases_tests_space(70023);
	RepoRepo	first;
	RepoRepo	second;
	HostSharedReader reading;
	uint64		a;
	uint64		b;

	store_databases_tests_ok(store_databases_created(io, 0, 957, from, &error), &error, 838, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 957, from, to, &error), &error, 839, 39);
	store_databases_tests_committed_eq(io, 957, to, 840);
	store_databases_tests_repo(io, 957, to, &first, 841, 57);
	store_databases_tests_finished(io, 957, from, to, 842);
	store_databases_tests_repo(io, 957, to, &second, 843, 58);
	a = chunks_packs_backend_Packs_location(&first.packs);
	b = chunks_packs_backend_Packs_location(&second.packs);
	STD_ASSERT_EQ_U64(844, 9, a, b);
	store_databases_tests_ok(host_shared_read(io, &reading, &error), &error, 845, 46);
	if (!repo_Repo_close(&first, io, &reading, &failed))
		store_databases_tests_chunks_unwrapped(&failed, 846, 35);
	if (!repo_Repo_close(&second, io, &reading, &failed))
		store_databases_tests_chunks_unwrapped(&failed, 847, 36);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_a_finish_after_another_removed_the_old_directory_ends_without_error:
 * a finish run after another finish removed the old directory ends without
 * error and leaves no mark.
 */
void
test_store_databases_tests_a_finish_after_another_removed_the_old_directory_ends_without_error(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	uint32		from = store_databases_tests_space(70024);
	uint32		to = store_databases_tests_space(70025);

	store_databases_tests_ok(store_databases_created(io, 0, 958, from, &error), &error, 857, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 958, from, to, &error), &error, 858, 39);
	store_databases_tests_finished(io, 958, from, to, 859);
	STD_ASSERT(860, 9, !store_databases_tests_there(958, from), "!there(958, from)");
	store_databases_tests_finished(io, 958, from, to, 861);
	STD_ASSERT(862, 9, !store_databases_tests_there(958, from) && store_databases_tests_opens(io, 958, to),
			   "!there(958, from) && opens(io, 958, to)");
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_the_start_after_bootstrap_is_silent: the start
 * after bootstrap makes template1's repository and reports nothing, at LOG
 * or at WARNING, as stock initdb writes nothing.
 */
void
test_store_databases_tests_the_start_after_bootstrap_is_silent(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	StdVec		logs;
	StdVec		warnings;

	store_databases_tests_unmade();
	store_databases_tests_ok(store_databases_bootstrapped(io, &error), &error, 872, 26);
	logs = simc_take_logs();
	store_tests_Logs_drop(&logs);
	warnings = simc_take_warnings();
	store_tests_Warnings_drop(&warnings);
	store_databases_tests_started(io, 874);
	logs = simc_take_logs();
	warnings = simc_take_warnings();
	if (logs.len != 0 || warnings.len != 0)
	{
		StdVec		none = {0};
		StdTupleRef left = {2, {STD_ITEM(&logs, store_tests_Logs_Debug_fmt),
		STD_ITEM(&warnings, store_tests_Warnings_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(&none, store_tests_Logs_Debug_fmt),
		STD_ITEM(&none, store_tests_Warnings_Debug_fmt)}};

		std_assert_failed(RUST_FILE, 875, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	store_tests_Logs_drop(&logs);
	store_tests_Warnings_drop(&warnings);
	STD_ASSERT(876, 9, store_databases_tests_opens(io, STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT),
			   "opens(io, TEMPLATE1, host::PG_DEFAULT)");
	simc_TestIo_drop(&test_io);
}

/* store_databases_tests_spaces_t is a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository's actors' captures: the move's two tablespaces. */
typedef struct store_databases_tests_spaces_t
{
	uint32		from;
	uint32		to;
} store_databases_tests_spaces_t;

/* store_databases_tests_opener is the opener: it opens the repository in its old tablespace, its outcome dropped. */
static bool
store_databases_tests_opener(void *captures, HostIoIo *io, HostError *error)
{
	const store_databases_tests_spaces_t *spaces = captures;
	RepoRepo	repo;
	ChunksErrorError failed;

	(void) error;
	if (repo_Repo_open(io, 963, spaces->from, &repo, &failed))
		repo_Repo_drop(&repo);
	else
		chunks_error_Error_drop(&failed);
	return true;
}

/* store_databases_tests_mover is the mover: the move's finish. */
static bool
store_databases_tests_mover(void *captures, HostIoIo *io, HostError *error)
{
	const store_databases_tests_spaces_t *spaces = captures;

	return store_databases_moved_finish(io, 963, spaces->from, spaces->to, error);
}


/*
 * test_store_databases_tests_a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository:
 * an open that made a repository's pack record from its old tablespace's
 * files before the move's finish, and publishes it after, leaves no record of
 * the old directory: the repository opens in its new tablespace.
 */
void
test_store_databases_tests_a_record_made_before_a_moves_finish_is_no_record_of_the_moved_repository(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostError	error;
	ChunksErrorError failed;
	store_databases_tests_spaces_t spaces = {store_databases_tests_space(70031), store_databases_tests_space(70032)};
	SimcActor	opener = {store_databases_tests_opener, &spaces};
	SimcActor	mover = {store_databases_tests_mover, &spaces};
	SimcStep	steps[] = {{"mover", 5, PG_UINT64_MAX}, {"opener", 6, PG_UINT64_MAX}};
	SimcRun		run;
	SimcReport	report;

	store_databases_tests_ok(store_databases_created(io, 0, 963, spaces.from, &error), &error, 887, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 963, spaces.from, spaces.to, &error), &error, 888, 39);
	if (!chunks_packs_open_forgot(io, 963, spaces.from, &failed))
		store_databases_tests_chunks_unwrapped(&failed, 889, 51);
	run = simc_Run_new(23);
	simc_Run_actor(&run, "opener", 6, opener);
	simc_Run_actor(&run, "mover", 5, mover);
	simc_Run_until(&run, "opener", 6, "doltgres-chunks-record-made");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	store_tests_ended(RUST_FILE, &report, 900, 9);
	simc_Report_drop(&report);
	STD_ASSERT(906, 9, store_databases_tests_opens(io, 963, spaces.to), "opens(io, 963, to)");
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_STORE */
