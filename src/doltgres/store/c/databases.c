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
#include "doltgres/chunks/packs/dropped.h"
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
#include "doltgres/txn/fold.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/memtable/branch.h"
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

/* STORE_DATABASES_DIGEST is a creation's field of its template's digest, which a standby's copy must match. */
#define STORE_DATABASES_DIGEST 4

/*
 * STORE_DATABASES_KINDS is a creation's field of the storages its template's
 * digest folded, in order, each its tablespace and relation, four bytes
 * big-endian each, and its kind, one byte.
 */
#define STORE_DATABASES_KINDS 5

/* STORE_DATABASES_KIND is the length of one storage's entry in a creation's kinds. */
#define STORE_DATABASES_KIND 9

/* STORE_DATABASES_FROM is a move's field of the tablespace it moves from. */
#define STORE_DATABASES_FROM 2

/* STORE_DATABASES_TO is a move's field of the tablespace it moves to. */
#define STORE_DATABASES_TO 3

/* STORE_DATABASES_MOVING is the suffix of a repository's copy in the tablespace it moves to, which no open reads as a repository. */
#define STORE_DATABASES_MOVING ".moving"

/* STORE_DATABASES_COPYING is the suffix of a copy while it is written, renamed to the copy's own name only whole (bugs row 449). */
#define STORE_DATABASES_COPYING ".copying"

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

	/*
	 * digest is the template's digest, and kinds a Vec<Digested> of the
	 * storages it folded (bugs row 404).
	 */
	uint8		digest[TXN_FOLD_DIGEST_LEN];
	StdVec		kinds;
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
	StdVec		kinds;

	chunks_record_RecordWriter_uint(&w, STORE_DATABASES_DATABASE, self->database);
	chunks_record_RecordWriter_uint(&w, STORE_DATABASES_TABLESPACE, self->tablespace);
	chunks_record_RecordWriter_uint(&w, STORE_DATABASES_TEMPLATE, self->template);
	chunks_record_RecordWriter_field(&w, STORE_DATABASES_DIGEST, self->digest, sizeof(self->digest));
	std_vec_with_capacity_Checked(&kinds, self->kinds.len * STORE_DATABASES_KIND, 1, 1);
	for (size_t k = 0; k < self->kinds.len; k++)
	{
		const TxnFoldDigested *kind = &STD_VEC_AT(&self->kinds, TxnFoldDigested, k);
		uint8		entry[STORE_DATABASES_KIND];

		std_u32_to_be_bytes(kind->tablespace, entry);
		std_u32_to_be_bytes(kind->relation, entry + 4);
		entry[8] = kind->kind;
		std_vec_extend_from_slice_Checked(&kinds, entry, sizeof(entry), 1, 1);
	}
	chunks_record_RecordWriter_field(&w, STORE_DATABASES_KINDS, kinds.ptr, kinds.len);
	std_vec_free_Checked(&kinds, 1, 1);
	return chunks_record_RecordWriter_finish(&w);
}

/*
 * store_databases_Created_kinds reads a creation's kinds, refusing a field of
 * another length, a kind past unknown, and storages out of order or repeated
 * as data corruption.
 */
static bool
store_databases_Created_kinds(const ChunksRecordField *field, StdVec *result, HostError *error)
{
	if (field->bytes_len % STORE_DATABASES_KIND != 0)
	{
		*error = store_databases_corrupt_text("its kinds aren't whole");
		return false;
	}
	std_vec_with_capacity_Checked(result, field->bytes_len / STORE_DATABASES_KIND, sizeof(TxnFoldDigested),
								  _Alignof(TxnFoldDigested));
	for (size_t at = 0; at < field->bytes_len; at += STORE_DATABASES_KIND)
	{
		const uint8 *entry = field->bytes + at;
		TxnFoldDigested kind = {std_u32_from_be_bytes(entry), std_u32_from_be_bytes(entry + 4), entry[8]};
		const TxnFoldDigested *last = result->len > 0 ?
			&STD_VEC_AT(result, TxnFoldDigested, result->len - 1) : NULL;

		if (kind.kind > TXN_FOLD_DIGEST_UNKNOWN ||
			(last != NULL && (last->tablespace > kind.tablespace ||
							  (last->tablespace == kind.tablespace && last->relation >= kind.relation))))
		{
			std_vec_free_Checked(result, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
			*error = store_databases_corrupt_text("its kinds aren't storages in order with a kind each");
			return false;
		}
		std_vec_push_Checked(result, &kind, sizeof(kind), _Alignof(TxnFoldDigested));
	}
	return true;
}

/* store_databases_Created_decode reads a creation's body, refusing a field it doesn't know, a missing one it requires, a digest of another length, kinds that don't read, and a body cut short as data corruption. */
static bool
store_databases_Created_decode(const uint8 *body, size_t body_len, StoreDatabasesCreated *result, HostError *error)
{
	static const uint32 known[] = {STORE_DATABASES_DATABASE, STORE_DATABASES_TABLESPACE, STORE_DATABASES_TEMPLATE,
	STORE_DATABASES_DIGEST, STORE_DATABASES_KINDS};
	StdVec		fields;
	const ChunksRecordField *digest;
	const ChunksRecordField *kinds;
	bool		ok = false;

	if (!store_databases_Read_of(body, body_len, known, lengthof(known), &fields, error))
		return false;
	digest = store_databases_Read_field(&fields, STORE_DATABASES_DIGEST);
	kinds = store_databases_Read_field(&fields, STORE_DATABASES_KINDS);
	if (digest == NULL || kinds == NULL)
	{
		*error = store_databases_corrupt_field("it lacks field ", digest == NULL ? STORE_DATABASES_DIGEST :
											   STORE_DATABASES_KINDS, "");
		goto done;
	}
	if (digest->bytes_len != sizeof(result->digest))
	{
		*error = store_databases_corrupt_text("its digest isn't 32 bytes");
		goto done;
	}
	memcpy(result->digest, digest->bytes, sizeof(result->digest));
	if (!store_databases_Read_database(&fields, &result->database, error) ||
		!store_databases_Read_oid(&fields, STORE_DATABASES_TABLESPACE, &result->tablespace, error) ||
		!store_databases_Read_oid(&fields, STORE_DATABASES_TEMPLATE, &result->template, error))
		goto done;
	ok = store_databases_Created_kinds(kinds, &result->kinds, error);
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

/* store_databases_beside returns a directory named by a repository and a suffix in a tablespace's directory of repositories. */
static StdPathBuf
store_databases_beside(uint32 database, uint32 to, const char *suffix)
{
	StdPathBuf	dir = host_versioned_dir(to);
	StdPathBuf	result;
	StdString	name;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(suffix)};
	StdFmtArg	args[] = {{&database, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&name, &arguments);
	std_path_join(dir.bytes.ptr, dir.bytes.len, name.ptr, name.len, &result);
	std_String_drop(&name);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	return result;
}

/* store_databases_moving returns the directory of a repository's copy in the tablespace it moves to. */
static StdPathBuf
store_databases_moving(uint32 database, uint32 to)
{
	return store_databases_beside(database, to, STORE_DATABASES_MOVING);
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

/*
 * store_databases_inserted appends a fork's record of a kind, taking its
 * body, and when asked flushes the WAL through it, so the record is durable
 * before the files it orders change; it returns the record's end.
 */
static bool
store_databases_inserted(HostIoIo *io, HostWalKind kind, StdVec *body, bool flushed, HostLsn *end, HostError *error)
{
	bool		ok = host_wal_insert(HOST_IO_IO_IO_DOORS(io), kind, body->ptr, body->len, end, error) &&
		(!flushed || host_wal_flush(HOST_IO_IO_IO_DOORS(io), *end, error));

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

static bool store_databases_found(const StdVec *found, uint32 repository, uint32 *tablespace);

/*
 * store_databases_pending_t is a creation the startup process found marked
 * or replayed whose transaction it hasn't seen commit: the new repository,
 * its tablespace, and the creating transaction's xid.
 */
typedef struct store_databases_pending_t
{
	uint32		database;
	uint32		tablespace;
	uint64		xid;
} store_databases_pending_t;

/*
 * store_databases_pending is the startup process's creations its replay of
 * their transactions' commits or aborts settles, or the end of redo settles
 * as aborted (bugs row 404).
 */
static StdVec store_databases_pending = {.ptr = NULL};

/* store_databases_pending_kept makes the startup process's list of kept creations once. */
static void
store_databases_pending_kept(void)
{
	if (store_databases_pending.ptr == NULL)
		std_vec_new(&store_databases_pending, sizeof(store_databases_pending_t),
					_Alignof(store_databases_pending_t));
}

/*
 * store_databases_created_from makes a new database's repository from its
 * template's, as a backend's creation or, at a replayed record's end, as its
 * replay by the creating transaction's xid (bugs row 404).
 */
static bool store_databases_created_from(HostIoIo *io, uint32 template, uint32 database, uint32 tablespace,
										 HostLsn position, uint64 xid, const StoreDatabasesCreated *expected,
										 HostError *error);

/*
 * store_databases_creation is the repository the backend's transaction is
 * creating from a template, which the transaction's end settles (bugs row
 * 404).
 */
static struct
{
	bool		live;
	uint32		database;
	uint32		tablespace;
}			store_databases_creation;

/* store_databases_created_gather is created's save's gathering, which gathers nothing: the copy's identity tree is the template's. */
static bool
store_databases_created_gather(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result,
							   ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) read;
	(void) error;
	std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
	return true;
}

/*
 * store_databases_created_frame_0_t is created's state across its pad: the
 * repositories found, the template's packs, the new repository, its newest
 * manifest, and the reader, while live, and the runs its save takes.
 */
typedef struct store_databases_created_frame_0_t
{
	HostIoIo   *io;
	uint32		template;
	uint32		database;
	uint32		tablespace;

	/*
	 * position is a replayed creation's record's end, or 0 for a creation a
	 * backend runs, and xid its creating transaction's, which a backend's
	 * creation reads as its own.
	 */
	HostLsn		position;
	uint64		xid;

	/*
	 * expected is a replayed creation's record, whose digest and kinds the
	 * replay's copy must match, or NULL; digest and kinds are the template's.
	 */
	const StoreDatabasesCreated *expected;
	uint8		digest[TXN_FOLD_DIGEST_LEN];
	StdVec		kinds;
	HostError  *error;
	bool		ok;
	StdVec		found;
	bool		found_live;
	RepoRepo	template_repo;
	bool		template_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	RepoRepo	repo;
	bool		repo_live;
	StdArcInner *manifest;
	bool		manifest_live;
	RepoRuns	runs;
	HostSharedReader reading;
	bool		reading_live;
	StdPathBuf	dir;
} store_databases_created_frame_0_t;

/* store_databases_created_pad_0 is created's pad: the reader, the manifest, the repository, the packs, then the repositories found and the directory. */
static void
store_databases_created_pad_0(void *frame_)
{
	store_databases_created_frame_0_t *frame = frame_;

	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->manifest_live)
	{
		frame->manifest_live = false;
		repo_ArcManifest_drop(frame->manifest);
	}
	if (frame->repo_live)
	{
		frame->repo_live = false;
		repo_Repo_drop(&frame->repo);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->template_live)
	{
		frame->template_live = false;
		repo_Repo_drop(&frame->template_repo);
	}
	if (frame->found_live)
	{
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
	std_vec_free_Checked(&frame->kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	std_vec_new(&frame->kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	std_vec_free_Checked(&frame->dir.bytes, 1, 1);
	std_PathBuf_new(&frame->dir);
}

/* store_databases_kind_name returns a digested storage's kind's name for a refusal's detail. */
static const char *
store_databases_kind_name(uint8 kind)
{
	return kind == TXN_FOLD_DIGEST_KEYED ? "keyed" : kind == TXN_FOLD_DIGEST_KEYLESS ? "keyless" : "unknown";
}

/* store_databases_hex writes a digest's bytes as hexadecimal digits and a terminator. */
static void
store_databases_hex(const uint8 *digest, char *result)
{
	for (int k = 0; k < TXN_FOLD_DIGEST_LEN; k++)
		snprintf(result + 2 * k, 3, "%02x", digest[k]);
}

/*
 * store_databases_refused refuses a standby's recovery whose replay of a
 * creation differs from its primary's (bugs row 404): a storage whose known
 * key contradicts the kind the primary folded it as, or a digest of its own
 * template other than the primary's, naming both.
 */
static void
store_databases_refused(store_databases_created_frame_0_t *frame, bool disagreed, const TxnFoldDigested *at)
{
	char		detail[320];
	char		ours[2 * TXN_FOLD_DIGEST_LEN + 1];
	char		theirs[2 * TXN_FOLD_DIGEST_LEN + 1];

	if (disagreed)
	{
		uint8		primary = TXN_FOLD_DIGEST_UNKNOWN;

		for (size_t k = 0; k < frame->expected->kinds.len; k++)
			if (STD_VEC_AT(&frame->expected->kinds, TxnFoldDigested, k).tablespace == at->tablespace &&
				STD_VEC_AT(&frame->expected->kinds, TxnFoldDigested, k).relation == at->relation)
				primary = STD_VEC_AT(&frame->expected->kinds, TxnFoldDigested, k).kind;
		snprintf(detail, sizeof(detail),
				 "Storage %u/%u of template database %u is %s here and was %s on the primary.",
				 at->tablespace, at->relation, frame->template, store_databases_kind_name(at->kind),
				 store_databases_kind_name(primary));
	}
	else
	{
		store_databases_hex(frame->digest, ours);
		store_databases_hex(frame->expected->digest, theirs);
		snprintf(detail, sizeof(detail),
				 "Template database %u of database %u digests %s here and digested %s on the primary.",
				 frame->template, frame->database, ours, theirs);
	}
	host_refuse_recovery(HOST_IO_IO_IO_DOORS(frame->io), detail, strlen(detail));
}

/*
 * store_databases_created_body_0 is created's body: the template saved, the
 * new repository's directory made with the creation's mark, the template's
 * directories copied into it, the template's delta written as its runs, and
 * its manifest saved naming them; a backend's creation then appends the
 * record, and a replayed one is kept for its transaction's outcome.
 */
static void
store_databases_created_body_0(void *frame_)
{
	store_databases_created_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	StoreDatabasesCreated record = {.database = frame->database, .tablespace = frame->tablespace,
	.template = frame->template};
	ChunksErrorError failed;
	uint32		own;
	bool		took;
	bool		cleaned;
	HostXid		writer;
	HostXid		top;
	HostLsn		redo;
	HostLsn		point;
	HostLsn		end;
	StdVec		body;
	bool		disagreed;
	bool		diverged = false;
	TxnFoldDigested at;

	frame->ok = false;
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(io), &frame->found, &failed))
		goto raised;
	frame->found_live = true;
	if (!store_databases_found(&frame->found, frame->template, &own))
	{
		*frame->error = store_databases_failed((const uint8 *) "XX000", "found no repository of template database ",
											   frame->template);
		goto done;
	}
	if (frame->position != 0)
	{
		if (!txn_runs_saved_at(io, frame->template, own, frame->position, &took, &cleaned, frame->error))
			goto done;
	}
	else
	{
		if (!txn_runs_save(io, frame->template, own, &took, &cleaned, frame->error) ||
			!host_current_writer(HOST_IO_IO_IO_DOORS(io), &writer, &top, frame->error))
			goto done;
		frame->xid = top.f0;
	}
	if (!host_shared_read(io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (!repo_Repo_open(io, frame->template, own, &frame->template_repo, &failed))
		goto raised;
	frame->template_live = true;
	if (!txn_fold_digest(io, &frame->template_repo, &frame->reading, frame->template,
						 frame->expected != NULL ? &frame->expected->kinds : NULL, &frame->kinds, frame->digest,
						 &disagreed, &at, frame->error))
		goto done;
	frame->template_live = false;
	if (!repo_Repo_close(&frame->template_repo, io, &frame->reading, &failed))
		goto raised;
	if (frame->expected != NULL &&
		!host_injected(HOST_IO_IO_IO_DOORS(io), "doltgres-store-creation-diverged", &diverged, frame->error))
		goto done;
	if (diverged)
		frame->digest[0] ^= 1;
	if (frame->expected != NULL &&
		(disagreed || memcmp(frame->digest, frame->expected->digest, TXN_FOLD_DIGEST_LEN) != 0))
	{
		store_databases_refused(frame, disagreed, &at);
		frame->ok = true;
		goto done;
	}
	frame->dir = host_tablespace_dir(frame->database, frame->tablespace);
	if (!chunks_packs_open_made_dir(io, frame->dir.bytes.ptr, frame->dir.bytes.len, &failed) ||
		!chunks_packs_dropped_creation_mark(HOST_IO_IO_IO_DOORS(io), frame->database, frame->tablespace, frame->xid,
											&failed))
		goto raised;
	if (frame->position == 0)
	{
		store_databases_creation.live = true;
		store_databases_creation.database = frame->database;
		store_databases_creation.tablespace = frame->tablespace;
	}
	if (!chunks_packs_open_Packs_open(io, frame->template, own, &frame->packs, &failed))
		goto raised;
	frame->packs_live = true;
	if (!chunks_packs_backend_Packs_created(&frame->packs, io, &frame->reading, frame->database, frame->tablespace,
											&failed))
		goto raised;
	frame->packs_live = false;
	if (!chunks_packs_backend_Packs_close(&frame->packs, io, &frame->reading, &failed) ||
		!repo_Repo_open(io, frame->database, frame->tablespace, &frame->repo, &failed))
		goto raised;
	frame->repo_live = true;
	if (!repo_Repo_manifest(&frame->repo, io, &frame->manifest, &failed))
		goto raised;
	frame->manifest_live = true;
	point = repo_Repo_newest(frame->manifest)->folded;
	frame->manifest_live = false;
	repo_ArcManifest_drop(frame->manifest);
	if (!host_last_redo(HOST_IO_IO_IO_DOORS(io), &redo, frame->error) ||
		!txn_runs_copied(io, frame->template, own, frame->database, frame->tablespace, redo, point, &frame->repo,
						 &frame->reading, &frame->runs, frame->error))
		goto done;
	if (!repo_Repo_save(&frame->repo, io, &frame->reading, true, &frame->runs, store_databases_created_gather, NULL,
						store_databases_saved, NULL, &failed) ||
		(own != frame->tablespace &&
		 !repo_Repo_rekeyed(&frame->repo, io, &frame->reading, own, frame->tablespace, &failed)))
		goto raised;
	frame->repo_live = false;
	if (!repo_Repo_close(&frame->repo, io, &frame->reading, &failed))
		goto raised;
	if (frame->position != 0)
	{
		store_databases_pending_t pending = {.database = frame->database, .tablespace = frame->tablespace,
		.xid = frame->xid};

		store_databases_pending_kept();
		std_vec_push_Checked(&store_databases_pending, &pending, sizeof(pending),
							 _Alignof(store_databases_pending_t));
		frame->ok = true;
		goto done;
	}
	memcpy(record.digest, frame->digest, sizeof(record.digest));
	record.kinds = frame->kinds;
	body = store_databases_Created_encode(&record);
	frame->ok = store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_CREATED, &body, false, &end, frame->error) &&
		host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-creation-copied", frame->error);
	goto done;
raised:
	*frame->error = store_databases_raised(&failed);
done:
	store_databases_created_pad_0(frame);
}

/*
 * store_databases_created makes a new database's repository from its
 * template's, in its default tablespace, and appends the fork's record of
 * it, in createdb's cleanup block after Postgres' copy (bugs row 404): it
 * saves the template, makes the new repository's own directory holding the
 * creation's mark, copies the template's directories into the new
 * repository's, writes the template's committed delta into it as its runs,
 * and saves its manifest naming them, its storages of the template's own
 * tablespace put under the new one's; the record carries the template's
 * digest and the kinds its storages were folded as. The template has no
 * session while createdb runs, since Postgres refuses one, and the copy holds
 * the files' flag that every whole write of the template's kept files holds.
 * A failure leaves the marked repository to the transaction's abort, which
 * removes it.
 */
bool
store_databases_created(HostIoIo *io, uint32 template, uint32 database, uint32 tablespace, HostError *error)
{
	return store_databases_id(database, error) &&
		store_databases_created_from(io, template, database, tablespace, 0, 0, NULL, error);
}

/* store_databases_created_from makes a new database's repository from its template's, by created's body. */
static bool
store_databases_created_from(HostIoIo *io, uint32 template, uint32 database, uint32 tablespace, HostLsn position,
							 uint64 xid, const StoreDatabasesCreated *expected, HostError *error)
{
	store_databases_created_frame_0_t frame = {.io = io, .template = template, .database = database,
	.tablespace = tablespace, .position = position, .xid = xid, .expected = expected, .error = error};

	std_PathBuf_new(&frame.dir);
	std_vec_new(&frame.kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	std_landing(&frame, store_databases_created_body_0, store_databases_created_pad_0);
	return frame.ok;
}

/*
 * store_databases_settled settles a repository's creation by its creating
 * transaction's outcome (bugs row 404): a committed one's mark is taken away,
 * and an aborted one's states are freed and the repository is marked dropped,
 * its creation's mark is taken away, and its directories are removed, or
 * left to the next settle while a process holds it.
 */
bool
store_databases_settled(HostIoIo *io, uint32 database, uint32 tablespace, bool committed, HostError *error)
{
	ChunksErrorError failed;
	StdPathBuf	dir;
	bool		removed;
	bool		ok;

	if (committed)
		ok = chunks_packs_dropped_created(HOST_IO_IO_IO_DOORS(io), database, tablespace, &failed);
	else
	{
		if (!txn_runs_dropped(io, database, error))
			return false;
		dir = host_tablespace_dir(database, tablespace);
		ok = chunks_packs_dropped_mark(HOST_IO_IO_IO_DOORS(io), dir.bytes.ptr, dir.bytes.len, &failed);
		std_vec_free_Checked(&dir.bytes, 1, 1);
		ok = ok && chunks_packs_dropped_created(HOST_IO_IO_IO_DOORS(io), database, tablespace, &failed) &&
			chunks_packs_open_Packs_dropped(io, database, tablespace, &removed, &failed);
	}
	if (!ok)
		*error = store_databases_raised(&failed);
	return ok;
}

/*
 * store_databases_creation_ended settles the repository the backend's
 * transaction created, at its commit or its abort, which can't raise: a
 * failure is reported at WARNING, and the next checkpoint settles the
 * creation by its outcome (bugs row 404); settled says whether there was
 * one.
 */
bool
store_databases_creation_ended(HostIoIo *io, bool committed, bool *settled, HostError *error)
{
	HostError	failure;
	uint8		code[5];

	*settled = store_databases_creation.live;
	if (!store_databases_creation.live)
		return true;
	store_databases_creation.live = false;
	if (committed && !host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-creation-ending", &failure))
		return store_databases_warned(io, &failure, "CREATE DATABASE", code, error);
	if (store_databases_settled(io, store_databases_creation.database, store_databases_creation.tablespace,
								committed, &failure))
		return true;
	return store_databases_warned(io, &failure, "CREATE DATABASE", code, error);
}

/*
 * store_databases_loaded loads the runs a committed creation's copy wrote
 * into the delta and then settles it, so no listing of repositories names it
 * before its runs' versions are in the delta, where the collector counts
 * them as roots.
 */
static bool
store_databases_loaded(HostIoIo *io, const TxnInsertPage *page, uint32 database, uint32 tablespace,
					   HostError *error)
{
	return txn_runs_loaded_one(io, page, database, tablespace, error) &&
		store_databases_settled(io, database, tablespace, true, error);
}

/*
 * store_databases_started_frame_0_t is started's state across its pad: the
 * marked repositories found.
 */
typedef struct store_databases_started_frame_0_t
{
	HostIoIo   *io;

	/*
	 * page is what the startup process loads a committed creation's runs
	 * with, or none at a checkpoint, which only settles each ended creation.
	 */
	const TxnInsertPage *page;

	/*
	 * loaded says whether the startup process loaded a committed creation's
	 * runs.
	 */
	bool	   *loaded;
	HostError  *error;
	bool		ok;
	StdVec		found;
} store_databases_started_frame_0_t;

/* store_databases_started_pad_0 is started's pad: the marked repositories found. */
static void
store_databases_started_pad_0(void *frame_)
{
	store_databases_started_frame_0_t *frame = frame_;

	std_vec_free_Checked(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_vec_new(&frame->found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
}

/* store_databases_started_body_0 is started's body. */
static void
store_databases_started_body_0(void *frame_)
{
	store_databases_started_frame_0_t *frame = frame_;
	HostIoIo   *io = frame->io;
	ChunksErrorError failed;

	if (!chunks_packs_open_creations(HOST_IO_IO_READ_DOORS(io), &frame->found, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	for (size_t k = 0, k_len = frame->found.len; k < k_len; k++)
	{
		std_TupleU32U32_t found = STD_VEC_AT(&frame->found, std_TupleU32U32_t, k);
		store_databases_pending_t pending = {.database = found.f0, .tablespace = found.f1};
		bool		marked;
		HostXid		xid;
		HostStatus	status;

		if (!chunks_packs_dropped_creating(HOST_IO_IO_READ_DOORS(io), found.f0, found.f1, &marked, &pending.xid,
										   &failed))
		{
			*frame->error = store_databases_raised(&failed);
			goto done;
		}
		xid.f0 = pending.xid;
		if (!host_status(HOST_IO_IO_IO_DOORS(io), xid, &status, frame->error))
			goto done;
		if (status == HOST_STATUS_COMMITTED && frame->page != NULL)
		{
			if (!store_databases_loaded(io, frame->page, found.f0, found.f1, frame->error))
				goto done;
			*frame->loaded = true;
		}
		else if (frame->page != NULL)
			std_vec_push_Checked(&store_databases_pending, &pending, sizeof(pending),
								 _Alignof(store_databases_pending_t));
		else if (status != HOST_STATUS_IN_PROGRESS &&
				 !store_databases_settled(io, found.f0, found.f1, status == HOST_STATUS_COMMITTED, frame->error))
			goto done;
	}
	frame->ok = true;
done:
	store_databases_started_pad_0(frame);
}

/*
 * store_databases_started settles, in the startup process before redo, each
 * creation whose mark stands by its transaction's outcome in the commit log,
 * which the checkpoint at the redo point made durable for every commit before
 * it (bugs row 404): a committed one's mark is taken away and its runs are
 * loaded, ahead of the replay of any write to it, and any other is kept for
 * the replay of its transaction's commit or abort, or for the end of redo,
 * since a commit log that shows a transaction neither running nor committed
 * may only not have replayed its commit yet; loaded says whether it loaded
 * one.
 */
bool
store_databases_started(HostIoIo *io, const TxnInsertPage *page, bool *loaded, HostError *error)
{
	store_databases_started_frame_0_t frame = {.io = io, .page = page, .loaded = loaded, .error = error};

	*loaded = false;
	store_databases_pending_kept();
	std_vec_new(&frame.found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_landing(&frame, store_databases_started_body_0, store_databases_started_pad_0);
	return frame.ok;
}

/*
 * store_databases_checkpointed settles, at a checkpoint outside recovery,
 * each creation whose mark stands and whose transaction has ended, as its
 * transaction's end does, for an end that couldn't (bugs row 404); recovery
 * settles its own.
 */
bool
store_databases_checkpointed(HostIoIo *io, HostError *error)
{
	store_databases_started_frame_0_t frame = {.io = io, .error = error};

	if (host_wal_stand(HOST_IO_IO_IO_DOORS(io)).recovering)
		return true;
	std_vec_new(&frame.found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	std_landing(&frame, store_databases_started_body_0, store_databases_started_pad_0);
	return frame.ok;
}

/*
 * store_databases_redo_settled settles a kept creation whose transaction's
 * commit or abort the startup process replays, and loads a committed one's
 * runs, answering through loaded whether it did (bugs row 404).
 */
bool
store_databases_redo_settled(HostIoIo *io, const TxnInsertPage *page, uint64 xid, bool committed, bool *loaded,
							 HostError *error)
{
	*loaded = false;
	for (size_t k = 0; store_databases_pending.ptr != NULL && k < store_databases_pending.len; k++)
	{
		store_databases_pending_t pending = STD_VEC_AT(&store_databases_pending, store_databases_pending_t, k);

		if (pending.xid != xid)
			continue;
		STD_VEC_AT(&store_databases_pending, store_databases_pending_t, k) =
			STD_VEC_AT(&store_databases_pending, store_databases_pending_t, store_databases_pending.len - 1);
		store_databases_pending.len--;
		*loaded = committed;
		return committed ? store_databases_loaded(io, page, pending.database, pending.tablespace, error) :
			store_databases_settled(io, pending.database, pending.tablespace, false, error);
	}
	return true;
}

/*
 * store_databases_redo_ended settles each creation still kept at the end of
 * redo as aborted, since its transaction never committed (bugs row 404).
 */
bool
store_databases_redo_ended(HostIoIo *io, HostError *error)
{
	while (store_databases_pending.ptr != NULL && store_databases_pending.len > 0)
	{
		store_databases_pending_t pending = STD_VEC_AT(&store_databases_pending, store_databases_pending_t,
													   store_databases_pending.len - 1);

		store_databases_pending.len--;
		if (!store_databases_settled(io, pending.database, pending.tablespace, false, error))
			return false;
	}
	return true;
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

	/*
	 * position is a replayed creation's record's end, and xid its creating
	 * transaction's.
	 */
	HostLsn		position;
	uint64		xid;
	/* kinds is a replayed creation's Vec<Digested>, while kinds_live. */
	StdVec		kinds;
	bool		kinds_live;
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

	if (frame->kinds_live)
	{
		frame->kinds_live = false;
		std_vec_free_Checked(&frame->kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	}
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

/* store_databases_stale removes every copy of a database's repository a move left in any tablespace, and every copy's temporary. */
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

		for (size_t s = 0; s < 2; s++)
		{
			store_databases_held_first(held, store_databases_beside(database, STD_VEC_AT(&held->spaces, uint32, k),
																	s == 0 ? STORE_DATABASES_MOVING :
																	STORE_DATABASES_COPYING));
			ok = host_remove_tree(HOST_IO_IO_IO_DOORS(io), held->first.bytes.ptr, held->first.bytes.len, &removed,
								  error);
			held->first_live = false;
			std_vec_free_Checked(&held->first.bytes, 1, 1);
			if (!ok)
				return false;
		}
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
	HostLsn		end;
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
	if (!store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_DROPPED, &body, false, &end, frame->error))
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
	if (!store_databases_Created_decode(frame->body, frame->body_len, &record, frame->error))
		goto done;
	frame->kinds = record.kinds;
	frame->kinds_live = true;
	if (!store_databases_standing(frame->io, record.database, &stands, frame->error))
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
	frame->ok = store_databases_created_from(frame->io, record.template, record.database, record.tablespace,
											 frame->position, frame->xid, &record, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_redo_created replays a creation: it finishes the removal of
 * a drop's marked leftovers of the id in any tablespace, makes nothing when
 * another directory of the id stands in any, and makes nothing either when
 * the tablespace's version directory is gone, since a later record drops it;
 * otherwise it makes the repository there from the template's as this
 * server's replay holds it, kept for its transaction's outcome, once the
 * template's digest matches the record's, and refuses recovery when it
 * doesn't (bugs row 404).
 */
bool
store_databases_redo_created(HostIoIo *io, const uint8 *body, size_t body_len, uint64 xid, HostLsn end,
							 HostError *error)
{
	store_databases_frame_t frame = {.io = io, .body = body, .body_len = body_len, .position = end, .xid = xid,
	.error = error};

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
	uint32		to;
	HostError  *error;
	bool		ok;
	StdPathBuf	copying;
	bool		copying_live;
	StdPathBuf	into;
	bool		into_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	HostSharedReader reading;
	bool		reading_live;
} store_databases_copy_frame_0_t;

/* store_databases_copy_pad_0 is copy's pad: the reader, then the packs, then the paths. */
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
	if (frame->into_live)
	{
		frame->into_live = false;
		std_vec_free_Checked(&frame->into.bytes, 1, 1);
	}
	if (frame->copying_live)
	{
		frame->copying_live = false;
		std_vec_free_Checked(&frame->copying.bytes, 1, 1);
	}
}

/* store_databases_copy_renamed renames a whole copy's temporary to the copy's own name, synced with its parent. */
static bool
store_databases_copy_renamed(store_databases_copy_frame_0_t *frame)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	StdStr		parent = host_directory_of(frame->into.bytes.ptr, frame->into.bytes.len);

	return host_rename(doors, frame->copying.bytes.ptr, frame->copying.bytes.len, frame->into.bytes.ptr,
					   frame->into.bytes.len, frame->error) &&
		host_sync_path(doors, frame->into.bytes.ptr, frame->into.bytes.len, true, frame->error) &&
		host_sync_path(doors, parent.ptr, parent.len, true, frame->error);
}

/*
 * store_databases_copy_body_0 is copy's body: a leftover temporary removed,
 * the packs opened, copied into the temporary, and closed, a failure warned
 * of before the close, and the whole copy renamed to its own name.
 */
static void
store_databases_copy_body_0(void *frame_)
{
	store_databases_copy_frame_0_t *frame = frame_;
	ChunksErrorError failed;
	HostError	failure;
	uint8		code[5];
	bool		removed;

	frame->ok = false;
	frame->copying = store_databases_beside(frame->repository, frame->to, STORE_DATABASES_COPYING);
	frame->copying_live = true;
	frame->into = store_databases_moving(frame->repository, frame->to);
	frame->into_live = true;
	if (!host_remove_tree(HOST_IO_IO_IO_DOORS(frame->io), frame->copying.bytes.ptr, frame->copying.bytes.len,
						  &removed, frame->error))
		goto done;
	if (!chunks_packs_open_Packs_open(frame->io, frame->repository, frame->from, &frame->packs, &failed))
	{
		*frame->error = store_databases_raised(&failed);
		goto done;
	}
	frame->packs_live = true;
	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		goto done;
	frame->reading_live = true;
	if (chunks_packs_backend_Packs_copied(&frame->packs, frame->io, &frame->reading, frame->copying.bytes.ptr,
										  frame->copying.bytes.len, &failed))
	{
		frame->packs_live = false;
		if (!chunks_packs_backend_Packs_close(&frame->packs, frame->io, &frame->reading, &failed))
		{
			*frame->error = store_databases_raised(&failed);
			goto done;
		}
		frame->ok = store_databases_copy_renamed(frame);
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
	if (!host_remove_tree(HOST_IO_IO_IO_DOORS(frame->io), frame->copying.bytes.ptr, frame->copying.bytes.len,
						  &removed, frame->error))
		goto done;
	*frame->error = store_databases_failed(code, "could not copy the repository of database ", frame->repository);
done:
	store_databases_copy_pad_0(frame);
}

/*
 * store_databases_copy copies a repository's own directory, the files its
 * pack record owes written first, into its copy's name in the tablespace it
 * moves to: under a temporary name, synced, then renamed, so a copy that
 * stands is whole (bugs row 449). A failed copy is reported at WARNING
 * before the hold is closed, so no error is held through the close, and its
 * temporary is removed.
 */
static bool
store_databases_copy(HostIoIo *io, uint32 repository, uint32 from, uint32 to, HostError *error)
{
	store_databases_copy_frame_0_t frame = {.io = io, .repository = repository, .from = from, .to = to,
	.error = error};

	std_landing(&frame, store_databases_copy_body_0, store_databases_copy_pad_0);
	return frame.ok;
}

/* store_databases_moved_copy_body_0 is moved_copy's body: the stale copies removed, then the copy made. */
static void
store_databases_moved_copy_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	HostIoIo   *io = frame->io;

	frame->ok = store_databases_stale(io, &frame->held, frame->database, frame->error) &&
		store_databases_copy(io, frame->database, frame->from, frame->to, frame->error) &&
		host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-copied", frame->error);
	store_databases_pad(frame);
}

/*
 * store_databases_moved_copy copies a moving database's repository into the
 * tablespace it moves to, under a name no open reads, in movedb's cleanup
 * block after Postgres' copy, so a copy that fails fails the move before its
 * commit: a leftover copy of an earlier try is removed first. No record
 * repeats it: the move's finish makes the copy itself where none stands
 * (bugs row 449).
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

/* STORE_DATABASES_MOVES is the name of a repository's move log in its own directory (bugs row 443). */
#define STORE_DATABASES_MOVES "moves"

/* STORE_DATABASES_MOVES_VERSION is the version byte that begins a move log. */
#define STORE_DATABASES_MOVES_VERSION 1

/* STORE_DATABASES_MOVE is the length of a move log's entry: the tablespace moved from, then its record's end. */
#define STORE_DATABASES_MOVE 12

/* StoreDatabasesMove is a move log's entry: the tablespace a repository moved from and the end of its move's record. */
typedef struct StoreDatabasesMove
{
	uint32		from;
	HostLsn		moved;
} StoreDatabasesMove;

/* StoreDatabasesMoves is a repository's move log the startup process loaded, with the tablespace it stands in. */
typedef struct StoreDatabasesMoves
{
	uint32		repository;
	uint32		own;

	/*
	 * moves is the log's entries in the order of their moves, a
	 * Vec<StoreDatabasesMove>.
	 */
	StdVec		moves;
} StoreDatabasesMoves;

/*
 * store_databases_moves_loaded is the startup process's move logs, which
 * recovery loads before redo and frees when redo ends: a
 * Vec<StoreDatabasesMoves>, one for each repository that has a log.
 */
static struct
{
	bool		live;
	StdVec		repositories;
}			store_databases_moves_loaded;

/*
 * store_databases_moves_read returns the entries of the move log in a
 * repository's directory, a Vec<StoreDatabasesMove>, empty when it has none;
 * a log whose newest generation doesn't read is corrupt, since its previous
 * one lacks the newest move.
 */
static bool
store_databases_moves_read(HostIoReadDoors io, uint32 repository, const StdPathBuf *dir, StdVec *result,
						   HostError *error)
{
	StdPathBuf	path;
	StdVec		bytes;
	bool		has;
	bool		fell_back;
	ChunksFileFallback why;
	ChunksErrorError failed;
	bool		ok;

	std_path_join(dir->bytes.ptr, dir->bytes.len, STORE_DATABASES_MOVES, strlen(STORE_DATABASES_MOVES), &path);
	ok = chunks_file_read_kept(io, path.bytes.ptr, path.bytes.len, &has, &bytes, &fell_back, &why, &failed);
	std_vec_free_Checked(&path.bytes, 1, 1);
	if (!ok)
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	std_vec_new(result, sizeof(StoreDatabasesMove), _Alignof(StoreDatabasesMove));
	if (!has)
		return true;
	if (fell_back || bytes.len == 0 || ((const uint8 *) bytes.ptr)[0] != STORE_DATABASES_MOVES_VERSION ||
		(bytes.len - 1) % STORE_DATABASES_MOVE != 0)
	{
		std_vec_free_Checked(&bytes, 1, 1);
		*error = store_databases_failed((const uint8 *) "XX001", "the move log is corrupt in the repository of database ",
										repository);
		return false;
	}
	for (size_t at = 1; at < bytes.len; at += STORE_DATABASES_MOVE)
	{
		const uint8 *entry = STD_PTR_AT(bytes.ptr, at, 1);
		StoreDatabasesMove move = {std_u32_from_le_bytes(entry), std_u64_from_le_bytes(entry + 4)};

		std_vec_push_Checked(result, &move, sizeof(move), _Alignof(StoreDatabasesMove));
	}
	std_vec_free_Checked(&bytes, 1, 1);
	return true;
}

/*
 * store_databases_moves_written writes a move's log into its copy, durably:
 * the log of the directory it moves from, without the moves whose records end
 * at or before the redo point of the last checkpoint or restartpoint
 * completed, from which every replay starts, unless a base backup runs, whose
 * start may precede them, and then the move's entry, so the copy's rename
 * makes the repository's new tablespace and the entry that tells redo where it
 * stood before the move durable at once (bugs row 443). A kept file's write
 * replaces it whole by a rename, so a crash inside it leaves the copy's log
 * as it was before.
 */
static bool
store_databases_moves_written(HostIoIo *io, uint32 repository, const StdPathBuf *old, const StdPathBuf *copy,
							  uint32 from, HostLsn moved, HostError *error)
{
	StdVec		moves;
	StdVec		bytes;
	StdPathBuf	path;
	ChunksErrorError failed;
	StoreDatabasesMove move = {from, moved};
	HostLsn		durable = 0;
	uint8		version = STORE_DATABASES_MOVES_VERSION;
	uint8		le[8];
	bool		same;
	bool		ok;

	if ((!host_backups_running() && !host_last_redo(HOST_IO_IO_IO_DOORS(io), &durable, error)) ||
		!store_databases_moves_read(HOST_IO_IO_READ_DOORS(io), repository, old, &moves, error))
		return false;
	std_vec_push_Checked(&moves, &move, sizeof(move), _Alignof(StoreDatabasesMove));
	std_vec_with_capacity_Checked(&bytes, 1 + moves.len * STORE_DATABASES_MOVE, 1, 1);
	std_vec_push_Checked(&bytes, &version, 1, 1);
	for (size_t k = 0; k < moves.len; k++)
	{
		const StoreDatabasesMove *kept = &STD_VEC_AT(&moves, StoreDatabasesMove, k);

		if (kept->moved <= durable)
			continue;
		std_u32_to_le_bytes(kept->from, le);
		std_vec_extend_from_slice_Checked(&bytes, le, 4, 1, 1);
		std_u64_to_le_bytes(kept->moved, le);
		std_vec_extend_from_slice_Checked(&bytes, le, 8, 1, 1);
	}
	std_vec_free_Checked(&moves, sizeof(StoreDatabasesMove), _Alignof(StoreDatabasesMove));
	std_path_join(copy->bytes.ptr, copy->bytes.len, STORE_DATABASES_MOVES, strlen(STORE_DATABASES_MOVES), &path);
	ok = chunks_file_write_kept(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &same,
								&failed);
	std_vec_free_Checked(&path.bytes, 1, 1);
	std_vec_free_Checked(&bytes, 1, 1);
	if (!ok)
		*error = store_databases_raised(&failed);
	return ok;
}

/*
 * store_databases_moves_load loads, in the startup process before redo, the
 * move log of each repository that has one, which redo's storages are
 * translated by (bugs row 443).
 */
bool
store_databases_moves_load(HostIoReadDoors io, HostError *error)
{
	ChunksErrorError failed;
	StdVec		found;

	if (!chunks_packs_open_repositories(io, &found, &failed))
	{
		*error = store_databases_raised(&failed);
		return false;
	}
	std_vec_new(&store_databases_moves_loaded.repositories, sizeof(StoreDatabasesMoves),
				_Alignof(StoreDatabasesMoves));
	store_databases_moves_loaded.live = true;
	for (size_t k = 0; k < found.len; k++)
	{
		const std_TupleU32U32_t *listed = &STD_VEC_AT(&found, std_TupleU32U32_t, k);
		StoreDatabasesMoves loaded = {listed->f0, listed->f1};
		StdPathBuf	dir = host_tablespace_dir(listed->f0, listed->f1);
		bool		ok = store_databases_moves_read(io, listed->f0, &dir, &loaded.moves, error);

		std_vec_free_Checked(&dir.bytes, 1, 1);
		if (!ok)
		{
			std_vec_free_Checked(&found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
			return false;
		}
		if (loaded.moves.len == 0)
			std_vec_free_Checked(&loaded.moves, sizeof(StoreDatabasesMove), _Alignof(StoreDatabasesMove));
		else
			std_vec_push_Checked(&store_databases_moves_loaded.repositories, &loaded, sizeof(loaded),
								 _Alignof(StoreDatabasesMoves));
	}
	std_vec_free_Checked(&found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	return true;
}

/* store_databases_moves_freed frees the startup process's move logs once redo ends. */
void
store_databases_moves_freed(void)
{
	if (!store_databases_moves_loaded.live)
		return;
	for (size_t k = 0; k < store_databases_moves_loaded.repositories.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&store_databases_moves_loaded.repositories, StoreDatabasesMoves, k).moves,
							 sizeof(StoreDatabasesMove), _Alignof(StoreDatabasesMove));
	std_vec_free_Checked(&store_databases_moves_loaded.repositories, sizeof(StoreDatabasesMoves),
						 _Alignof(StoreDatabasesMoves));
	store_databases_moves_loaded.live = false;
}

/*
 * store_databases_moves_hold reports whether the startup process's loaded
 * log of a repository holds a move's entry: the tablespace it moved from and
 * the end of its record.
 */
static bool
store_databases_moves_hold(uint32 repository, uint32 from, HostLsn moved)
{
	for (size_t k = 0; store_databases_moves_loaded.live && k < store_databases_moves_loaded.repositories.len; k++)
	{
		const StoreDatabasesMoves *loaded = &STD_VEC_AT(&store_databases_moves_loaded.repositories,
														StoreDatabasesMoves, k);

		for (size_t m = 0; loaded->repository == repository && m < loaded->moves.len; m++)
		{
			const StoreDatabasesMove *move = &STD_VEC_AT(&loaded->moves, StoreDatabasesMove, m);

			if (move->from == from && move->moved == moved)
				return true;
		}
	}
	return false;
}

/*
 * store_databases_replayed_storage gives a storage of a record the startup
 * process replays the tablespace its repository stands in now when its
 * tablespace was the repository's own at that record, which a kept move from
 * it after the record says (bugs row 443): a crash after a finished move
 * replays the records before it over the moved repository. Postgres' movedb
 * refuses a tablespace that holds any of the database's relations, so a
 * tablespace a later move left names the repository's own up to that move
 * and never a relation placed in it after, and moves from F to A to B or to F
 * each translate right.
 */
void
store_databases_replayed_storage(HostStorage *storage, HostLsn replayed)
{
	if (!store_databases_moves_loaded.live)
		return;
	for (size_t k = 0; k < store_databases_moves_loaded.repositories.len; k++)
	{
		const StoreDatabasesMoves *loaded = &STD_VEC_AT(&store_databases_moves_loaded.repositories,
														StoreDatabasesMoves, k);

		if (loaded->repository != storage->database)
			continue;
		for (size_t m = 0; m < loaded->moves.len; m++)
		{
			const StoreDatabasesMove *move = &STD_VEC_AT(&loaded->moves, StoreDatabasesMove, m);

			if (move->moved > replayed)
			{
				if (storage->tablespace == move->from)
					storage->tablespace = loaded->own;
				return;
			}
		}
		return;
	}
}

/* store_databases_finish_frame_0_t is finish's state across its pad: the old directory, the copy, the new directory, the repository, and the reader. */
typedef struct store_databases_finish_frame_0_t
{
	HostIoIo   *io;
	uint32		repository;
	uint32		from;
	uint32		to;
	HostLsn		moved;
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

/* store_databases_marked reports whether the directory a move leaves holds the mark of its finish. */
static bool
store_databases_marked(store_databases_finish_frame_0_t *frame, bool *result)
{
	ChunksErrorError failed;
	StdPathBuf	mark;
	bool		ok;

	std_path_join(frame->old.bytes.ptr, frame->old.bytes.len, CHUNKS_PACKS_OPEN_MOVED_FROM,
				  strlen(CHUNKS_PACKS_OPEN_MOVED_FROM), &mark);
	ok = chunks_file_exists(HOST_IO_IO_READ_DOORS(frame->io), mark.bytes.ptr, mark.bytes.len, result, &failed);
	std_vec_free_Checked(&mark.bytes, 1, 1);
	if (!ok)
		*frame->error = store_databases_raised(&failed);
	return ok;
}

/*
 * store_databases_finished reports whether a move's finish the startup
 * process replays ran to its end before: the repository's loaded log holds
 * the move's own entry, which only the rename of the move's copy makes the
 * repository's, and the directory it moved from holds no mark, which only the
 * finish's last step removes, with that directory (bugs row 454). An entry a
 * later finish pruned ends at or before the durable redo point, so no replay
 * meets its record; outside the startup process no log is loaded, and every
 * finish runs its steps.
 */
static bool
store_databases_finished(store_databases_finish_frame_0_t *frame, bool *result)
{
	bool		marked;

	*result = false;
	if (!store_databases_moves_hold(frame->repository, frame->from, frame->moved))
		return true;
	if (!store_databases_marked(frame, &marked))
		return false;
	*result = !marked;
	return true;
}

/*
 * store_databases_logged writes the move's log into its copy while the copy
 * stands and the directory it moves from has no mark, so the log is written
 * before the mark; a finish that finds the mark finds the log written (bugs
 * row 443).
 */
static bool
store_databases_logged(store_databases_finish_frame_0_t *frame)
{
	bool		copied;
	bool		marked;

	if (!store_databases_there(HOST_IO_IO_READ_DOORS(frame->io), &frame->copy, &copied, frame->error))
		return false;
	if (!copied)
		return true;
	if (!store_databases_marked(frame, &marked))
		return false;
	return marked || store_databases_moves_written(frame->io, frame->repository, &frame->old, &frame->copy,
												   frame->from, frame->moved, frame->error);
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
	bool		there;
	bool		finished;

	frame->ok = false;
	frame->old = host_tablespace_dir(frame->repository, frame->from);
	frame->old_live = true;
	frame->copy = store_databases_moving(frame->repository, frame->to);
	frame->copy_live = true;
	frame->new = host_tablespace_dir(frame->repository, frame->to);
	frame->new_live = true;
	if (!store_databases_finished(frame, &finished))
		goto done;
	if (finished)
	{
		if (!chunks_packs_open_emptied(io, &failed))
			goto raised;
		frame->ok = true;
		goto done;
	}
	if (!store_databases_there(HOST_IO_IO_READ_DOORS(io), &frame->new, &there, frame->error) ||
		(!there && !store_databases_there(HOST_IO_IO_READ_DOORS(io), &frame->copy, &there, frame->error)) ||
		(!there && !store_databases_copy(io, frame->repository, frame->from, frame->to, frame->error)))
		goto done;
	if (!store_databases_logged(frame) ||
		!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-logged", frame->error))
		goto done;
	if (!chunks_packs_open_moving_from(io, frame->old.bytes.ptr, frame->old.bytes.len, &failed))
		goto raised;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-marked", frame->error))
		goto done;
	if (!chunks_packs_open_forgot(io, frame->repository, frame->from, &failed))
		goto raised;
	if (!chunks_packs_open_moved_in(io, frame->copy.bytes.ptr, frame->copy.bytes.len, frame->new.bytes.ptr,
									frame->new.bytes.len, &failed))
		goto raised;
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-renamed", frame->error) ||
		!txn_state_rehomed(io, frame->repository, frame->from, frame->to, frame->error) ||
		!txn_memtable_branch_rehomed(io, frame->repository, frame->from, frame->to, frame->error))
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
	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-rekeyed", frame->error))
		goto done;
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
 * each answering one another process did meanwhile: the copy made from the
 * old directory where neither the repository nor its copy stands, as a
 * standby's replay finds it, under the quiet's hold of the database's lock,
 * whose conflict ended every session of it and keeps a fold worker from
 * connecting until the finish ends, so nothing writes the old directory
 * after the copy (bugs row 449), the move's log in the copy with the end of
 * its record, moved, then the mark in the old
 * directory, the pack record of the old directory freed, the copy renamed to
 * the repository's own name in its new tablespace, the database's states made
 * to name it there with its working state, its manifest's storages of the old
 * tablespace put under the new one, and the old directory removed, the mark
 * last; a finish that ran to its end before, which a replay of its record
 * after a crash meets, only removes each tablespace's directory of
 * repositories that holds none, so never a later table's files.
 */
static bool
store_databases_finish(HostIoIo *io, uint32 repository, uint32 from, uint32 to, HostLsn moved, HostError *error)
{
	store_databases_finish_frame_0_t frame = {.io = io, .repository = repository, .from = from, .to = to,
	.moved = moved, .error = error};

	std_landing(&frame, store_databases_finish_body_0, store_databases_finish_pad_0);
	return frame.ok;
}

/*
 * store_databases_moved_finish makes a moved database's copy its repository
 * in movedb after Postgres' drop record of the old directory and before the
 * database's lock is released: it rekeys the database's states first, then
 * appends the record and flushes it, so a crash after the finish's rename
 * leaves it for a standby to replay (bugs row 448), finishes the move, and
 * checkpoints.
 */
bool
store_databases_moved_finish(HostIoIo *io, uint32 database, uint32 from, uint32 to, HostError *error)
{
	StoreDatabasesMoved record = {database, from, to};
	StdVec		body;
	HostLsn		end;

	if (!store_databases_id(database, error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-finishing", error) ||
		!txn_state_rekeyed(io, database, from, to, error))
		return false;
	body = store_databases_Moved_encode(&record);
	return store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_MOVED, &body, true, &end, error) &&
		store_databases_finish(io, database, from, to, end, error) &&
		host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-store-move-finished", error) &&
		host_checkpoint_now(HOST_IO_IO_IO_DOORS(io), error);
}

/* store_databases_unfinished_body_0 is unfinished's body: the repository's own directory not standing. */
static void
store_databases_unfinished_body_0(void *frame_)
{
	store_databases_frame_t *frame = frame_;
	bool		own;

	frame->ok = false;
	store_databases_held_second(&frame->held, host_tablespace_dir(frame->database, frame->to));
	if (!store_databases_there(frame->read, &frame->held.second, &own, frame->error))
		goto done;
	*frame->result = !own;
	frame->ok = true;
done:
	store_databases_pad(frame);
}

/*
 * store_databases_unfinished reports whether a repository's own directory
 * doesn't stand in the tablespace its database's row names, as when a move
 * committed and its finish never ran, whether or not its copy stands there
 * (bugs row 449).
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
	HostLsn		end;

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
	frame->ok = store_databases_inserted(io, HOST_WAL_KIND_REPOSITORY_MOVED, &body, true, &end, frame->error) &&
		store_databases_finish(io, frame->database, record.from, frame->to, end, frame->error);
done:
	store_databases_pad(frame);
}

/*
 * store_databases_committed returns the tablespace a session opens its
 * database's repository in: the one its row names, after finishing a move
 * that committed and whose finish never ran, when the repository doesn't
 * stand there, the finish making its copy where none stands; on a hot
 * standby, which can't log, the repository's old directory.
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

/* store_databases_redo_moved_t is redo_moved's replay's captures: the record and its end. */
typedef struct store_databases_redo_moved_t
{
	StoreDatabasesMoved record;
	HostLsn		end;
} store_databases_redo_moved_t;

/* store_databases_redo_moved_closure_0 is redo_moved's replay: the move's finish. */
static bool
store_databases_redo_moved_closure_0(void *captures, HostIoIo *io, HostError *error)
{
	const store_databases_redo_moved_t *moved = captures;

	return store_databases_finish(io, moved->record.database, moved->record.from, moved->record.to, moved->end,
								  error);
}

/*
 * store_databases_redo_moved replays a move's finish under the database's
 * lock, doing whichever of its steps isn't done. It first flushes the WAL
 * through the record, which in recovery moves the minimum recovery point past
 * it, so a standby restarted after the finish changed the old directory takes
 * no connections before it replays the record again (bugs row 443): movedb's
 * update of pg_database commits before the record, and the quiet ends the
 * database's sessions before the finish's mark.
 */
bool
store_databases_redo_moved(HostIoIo *io, const uint8 *body, size_t body_len, HostLsn end, HostError *error)
{
	store_databases_redo_moved_t moved = {.end = end};

	return store_databases_Moved_decode(body, body_len, &moved.record, error) &&
		host_wal_flush(HOST_IO_IO_IO_DOORS(io), end, error) &&
		store_databases_quieted(io, moved.record.database, store_databases_redo_moved_closure_0, &moved, error);
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
	StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0), STD_FMT_LIT(" in tablespace "),
	STD_FMT_ARG(1), STD_FMT_LIT(" from template "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&record.database, std_u32_Display_fmt}, {&record.tablespace, std_u32_Display_fmt},
	{&record.template, std_u32_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!store_databases_Created_decode(body, body_len, &record, error))
		return false;
	std_vec_free_Checked(&record.kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
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
		return store_databases_finish(io, repository, from, to, 0, frame->error);
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
 * repositories, which it makes when absent, and leaves the template's pack
 * table as it was but for the records its pack record owed, which the copy
 * writes first; its id then conflicts with a new database's.
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

	store_databases_tests_ok(store_databases_made(io, 931, HOST_PG_DEFAULT, &error), &error, 588, 47);
	template = store_databases_tests_table(931, HOST_PG_DEFAULT, 589, 103);
	tablespace = store_databases_tests_space(70001);
	store_databases_tests_ok(store_databases_created(io, 931, 932, tablespace, &error), &error, 591, 43);
	STD_ASSERT(592, 9, store_databases_tests_opens(io, 932, tablespace), "opens(io, 932, tablespace)");
	after = store_databases_tests_table(931, HOST_PG_DEFAULT, 593, 99);
	STD_ASSERT(__LINE__, 0, after.len >= template.len && memcmp(after.ptr, template.ptr, template.len) == 0,
			   "table(931, host::PG_DEFAULT).starts_with(&template)");
	std_vec_free_Checked(&after, 1, 1);
	std_vec_free_Checked(&template, 1, 1);
	STD_ASSERT(594, 9, store_databases_tests_conflicts(io, 932, 594, 36) && !store_databases_tests_conflicts(io, 933, 594, 68),
			   "conflicts(io, 932).unwrap() && !conflicts(io, 933).unwrap()");
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_databases_tests_a_failed_creation_removes_what_it_made: a
 * creation leaves its directory marked until its transaction ends, which no
 * listing of repositories names and the id's conflict sees, and its
 * transaction's abort removes it (bugs row 404).
 */
void
test_store_databases_tests_a_failed_creation_removes_what_it_made(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdVec		found;
	ChunksErrorError failed;
	HostError	error;
	uint32		own;
	bool		settled;

	store_databases_tests_ok(store_databases_made(io, 933, HOST_PG_DEFAULT, &error), &error, __LINE__, 0);
	store_databases_tests_ok(store_databases_created(io, 933, 934, HOST_PG_DEFAULT, &error), &error, __LINE__, 0);
	if (!chunks_packs_open_repositories(HOST_IO_IO_READ_DOORS(io), &found, &failed))
		store_databases_tests_chunks_unwrapped(&failed, __LINE__, 0);
	STD_ASSERT(__LINE__, 0, !store_databases_found(&found, 934, &own) &&
			   store_databases_tests_conflicts(io, 934, __LINE__, 0),
			   "!chunks::repositories(io).unwrap().contains(934) && conflicts(io, 934).unwrap()");
	std_vec_free_Checked(&found, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	store_databases_tests_ok(store_databases_creation_ended(io, false, &settled, &error), &error, __LINE__, 0);
	STD_ASSERT(__LINE__, 0, settled && !store_databases_tests_there(934, HOST_PG_DEFAULT) &&
			   !store_databases_tests_conflicts(io, 934, __LINE__, 0),
			   "!there(934, host::PG_DEFAULT) && !conflicts(io, 934).unwrap()");
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

	store_databases_tests_ok(store_databases_made(io, 935, tablespace, &error), &error, 618, 41);
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

/*
 * store_databases_tests_redo_created replays a creation of a database in a
 * tablespace from template1, which it makes first if it has no repository,
 * with template1's digest as its primary logs it, and unwraps at a line and
 * column.
 */
static void
store_databases_tests_redo_created(HostIoIo *io, uint32 database, uint32 tablespace, uint32 line, uint32 col)
{
	StoreDatabasesCreated record = {.database = database, .tablespace = tablespace, .template = 1};
	StdVec		body;
	HostError	error;
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failed;
	bool		disagreed;
	TxnFoldDigested at;

	if (!store_databases_tests_there(STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT))
		store_databases_tests_ok(store_databases_made(io, STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT, &error), &error,
								 line, col);
	if (!repo_Repo_open(io, STORE_DATABASES_TEMPLATE1, HOST_PG_DEFAULT, &repo, &failed))
		store_databases_tests_chunks_unwrapped(&failed, line, col);
	store_databases_tests_ok(host_shared_read(io, &reading, &error), &error, line, col);
	std_vec_new(&record.kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	store_databases_tests_ok(txn_fold_digest(io, &repo, &reading, STORE_DATABASES_TEMPLATE1, NULL, &record.kinds,
											 record.digest, &disagreed, &at, &error), &error, line, col);
	if (!repo_Repo_close(&repo, io, &reading, &failed))
		store_databases_tests_chunks_unwrapped(&failed, line, col);
	host_shared_Reader_drop(&reading);
	body = store_databases_Created_encode(&record);
	std_vec_free_Checked(&record.kinds, sizeof(TxnFoldDigested), _Alignof(TxnFoldDigested));
	store_databases_tests_ok(store_databases_redo_created(io, body.ptr, body.len, 1, 1, &error), &error, line, col);
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
	store_databases_tests_ok(store_databases_made(io, 938, moved, &error), &error, 671, 36);
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
 * with a field no repository record has, one missing a field it requires,
 * and a body cut short are data corruption.
 */
void
test_store_databases_tests_a_record_that_doesnt_read_is_corrupt(void)
{
	ChunksRecordRecordWriter w;
	StdVec		bodies[3];
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
	std_slice_to_vec_Checked(&bodies[2], creation.ptr, 3, 1, 1);
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

	store_databases_tests_ok(store_databases_made(io, 951, from, &error), &error, 745, 35);
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

/* store_databases_tests_redo_moving replays a move's finish, which unwraps at a line and column. */
static void
store_databases_tests_redo_moving(HostIoIo *io, uint32 database, uint32 from, uint32 to, uint32 line, uint32 col)
{
	StoreDatabasesMoved record = {database, from, to};
	StdVec		body = store_databases_Moved_encode(&record);
	HostError	error;

	store_databases_tests_ok(store_databases_redo_moved(io, body.ptr, body.len, 0, &error), &error, line, col);
	std_vec_free_Checked(&body, 1, 1);
}

/*
 * test_store_databases_tests_a_moves_records_replay_twice_as_once_and_after_it_finished:
 * a move's finish replayed twice makes its copy and leaves what one run
 * leaves; replayed after the move finished and its old tablespace's
 * directory went, it makes nothing and fails nothing; and a finish replayed
 * after a finish renamed the copy, its old directory still standing marked,
 * makes no second copy.
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

	store_databases_tests_ok(store_databases_made(io, 952, from, &error), &error, __LINE__, 35);
	store_databases_tests_redo_moving(io, 952, from, to, __LINE__, 33);
	store_databases_tests_redo_moving(io, 952, from, to, __LINE__, 33);
	STD_ASSERT(__LINE__, 9, !store_databases_tests_moving_there(952, to) && !store_databases_tests_there(952, from) &&
			   store_databases_tests_opens(io, 952, to),
			   "!moving(952, to).exists() && !there(952, from) && opens(io, 952, to)");
	store_databases_tests_redo_moving(io, 952, from, to, __LINE__, 33);
	STD_ASSERT(__LINE__, 9, !store_databases_tests_moving_there(952, to) && store_databases_tests_opens(io, 952, to),
			   "!moving(952, to).exists() && opens(io, 952, to)");
	store_databases_tests_ok(store_databases_made(io, 956, from, &error), &error, __LINE__, 35);
	store_databases_tests_ok(store_databases_moved_copy(io, 956, from, to, &error), &error, __LINE__, 39);
	old = host_tablespace_dir(956, from);
	store_databases_tests_written(&old, CHUNKS_PACKS_OPEN_MOVED_FROM, "", __LINE__, 87);
	copy = store_databases_moving(956, to);
	own = host_tablespace_dir(956, to);
	if (!std_fs_rename(copy.bytes.ptr, copy.bytes.len, own.bytes.ptr, own.bytes.len, &io_error))
		store_databases_tests_io_unwrapped(&io_error, __LINE__, 73);
	store_databases_tests_redo_moving(io, 956, from, to, __LINE__, 82);
	STD_ASSERT(__LINE__, 9, !store_databases_tests_moving_there(956, to) && store_databases_tests_opens(io, 956, to),
			   "!moving(956, to).exists() && opens(io, 956, to)");
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

	store_databases_tests_ok(store_databases_made(io, 953, from, &error), &error, 792, 35);
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

	store_databases_tests_ok(store_databases_made(io, 954, from, &error), &error, 808, 35);
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

	store_databases_tests_ok(store_databases_made(io, 955, from, &error), &error, 822, 35);
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

	store_databases_tests_ok(store_databases_finish(io, database, from, to, 0, &error), &error, line, 47);
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

	store_databases_tests_ok(store_databases_made(io, 957, from, &error), &error, 838, 35);
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

	store_databases_tests_ok(store_databases_made(io, 958, from, &error), &error, 857, 35);
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

	store_databases_tests_ok(store_databases_made(io, 963, spaces.from, &error), &error, 887, 35);
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
