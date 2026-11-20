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
 * The identity pairs of a database's relations, columns, and schemas
 * (doltgres' identity.rs, design item 10): each recorded in the transaction
 * that makes the object and removed in the one that drops it, through txn's
 * identity tree, by the catalog's names and numbers.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_namespace_d.h"
#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/identity.h"
#include "doltgres/doltgres/relation.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/identity.h"
#include "pg_calls.h"

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "identity.rs"

/* The element layouts of identity's vectors. */
#define DOLTGRES_IDENTITY_BYTES 1, 1
#define DOLTGRES_IDENTITY_VEC sizeof(StdVec), _Alignof(StdVec)

/* DOLTGRES_IDENTITY_TOAST is TOAST: the name a table's TOAST relation derives its id from under the table's id. */
static const uint8 DOLTGRES_IDENTITY_TOAST[] = {'\0', 't', 'o', 'a', 's', 't'};

/* The initial schemas' OIDs identity's INITIAL names are Postgres' own, checked as the crate compiles. */
StaticAssertDecl(TXN_IDENTITY_INITIAL_0 == PG_CATALOG_NAMESPACE && TXN_IDENTITY_INITIAL_1 == PG_TOAST_NAMESPACE &&
				 TXN_IDENTITY_INITIAL_2 == PG_PUBLIC_NAMESPACE,
				 "the identity tree's initial schemas are Postgres' own");

/*
 * doltgres_identity_Pairing_t is Pairing: what a relation's pairs are
 * written with, the database's identity tree and the running transaction's
 * writer with its entry's room taken.
 */
typedef struct doltgres_identity_Pairing_t
{
	TxnIdentityDatabase tree;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
} doltgres_identity_Pairing_t;

/*
 * doltgres_identity_held_t is what an identity frame holds across its pad,
 * as the frame's first member: the keys it made, the first len of them
 * live, and a removal's keys while keys_live.
 */
typedef struct doltgres_identity_held_t
{
	StdVec		bytes[2];
	size_t		len;
	StdVec		keys;
	bool		keys_live;
} doltgres_identity_held_t;

/* doltgres_identity_bytes_drop frees a vector of bytes held as a vector's element. */
static void
doltgres_identity_bytes_drop(void *bytes)
{
	std_vec_free_Checked(bytes, DOLTGRES_IDENTITY_BYTES);
}

/* doltgres_identity_held_pad frees what a frame holds live, its first member's. */
static void
doltgres_identity_held_pad(void *frame_)
{
	doltgres_identity_held_t *held = frame_;

	while (held->len > 0)
	{
		held->len--;
		std_vec_free_Checked(&held->bytes[held->len], DOLTGRES_IDENTITY_BYTES);
	}
	if (held->keys_live)
		std_vec_drop_Checked(&held->keys, DOLTGRES_IDENTITY_VEC, doltgres_identity_bytes_drop);
	held->keys_live = false;
}

/* doltgres_identity_internal returns an internal error of the identity pairs. */
static PgPgError
doltgres_identity_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return pg_PgError_fork((const uint8 *) "XX000", &text);
}

/* doltgres_identity_named answers the name a call copied into a buffer, by the length it answered. */
static bool
doltgres_identity_named(const uint8 *buffer, int32 len, StdBytes *result, PgPgError *error)
{
	if (len < 0 || len >= NAMEDATALEN)
	{
		*error = doltgres_identity_internal("a name's length is past a name's buffer");
		return false;
	}
	result->ptr = buffer;
	result->len = (size_t) len;
	return true;
}

/*
 * doltgres_identity_users reports whether a relation is a user's, whose OID
 * initdb didn't give, the only relations whose pairs the identity tree can
 * hold.
 */
bool
doltgres_identity_users(const PgRel *rel)
{
	return pg_calls_rel_id(rel) >= FirstNormalObjectId;
}

/* doltgres_identity_indexed answers the table an index is on, or false for a relation that isn't an index. */
static bool
doltgres_identity_indexed(const PgRel *rel, uint32 *result)
{
	uint8		kind = pg_calls_rel_kind(rel);

	if (kind != RELKIND_INDEX && kind != RELKIND_PARTITIONED_INDEX)
		return false;
	*result = pg_calls_index_table(rel);
	return true;
}

/* doltgres_identity_tree answers the backend's database's identity tree as its readers reach it. */
static bool
doltgres_identity_tree(const RepoRepo *repo, const HostSharedReader *reading, TxnIdentityDatabase *result,
					   PgPgError *error)
{
	uint32		database = pg_calls_my_database_id();

	if (database == 0)
	{
		*error = doltgres_identity_internal("a backend has no database");
		return false;
	}
	*result = (TxnIdentityDatabase)
	{
		repo, reading, database
	};
	return true;
}

/*
 * doltgres_identity_looked_up answers the id a key of a database's identity
 * tree pairs with a number, or false for none.
 */
static bool
doltgres_identity_looked_up(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase tree, const StdVec *key,
							bool *has_result, uint64 *result, PgPgError *error)
{
	StdVec		value;
	HostError	failed;
	bool		ok;

	if (!txn_identity_looked_up(io, waits, tree, key->ptr, key->len, has_result, &value, &failed))
	{
		*error = pghost_raised(&failed);
		return false;
	}
	if (!*has_result)
		return true;
	ok = txn_identity_id(value.ptr, value.len, result, &failed);
	std_vec_free_Checked(&value, DOLTGRES_IDENTITY_BYTES);
	if (!ok)
		*error = pghost_raised(&failed);
	return ok;
}

/* doltgres_identity_numbered_frame_t is numbered's state across its pad: the key it made, and what it answers. */
typedef struct doltgres_identity_numbered_frame_t
{
	doltgres_identity_held_t held;
	HostIoIo   *io;
	HostIoWaits *waits;
	TxnIdentityDatabase tree;
	bool	   *has_result;
	uint64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_identity_numbered_frame_t;

/* doltgres_identity_numbered_body looks the object's pair by number up. */
static void
doltgres_identity_numbered_body(void *frame_)
{
	doltgres_identity_numbered_frame_t *frame = frame_;

	frame->ok = doltgres_identity_looked_up(frame->io, frame->waits, frame->tree, &frame->held.bytes[0],
											frame->has_result, frame->result, frame->error);
}

/*
 * doltgres_identity_numbered answers the id a schema's or a relation's pair
 * by number holds, by its kind and OID, or false for none.
 */
static bool
doltgres_identity_numbered(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase tree, TxnIdentityKind kind,
						   uint32 oid, bool *has_result, uint64 *result, PgPgError *error)
{
	doltgres_identity_numbered_frame_t frame = {.io = io, .waits = waits, .tree = tree, .has_result = has_result,
	.result = result, .error = error};

	txn_identity_by_oid(kind, oid, &frame.held.bytes[0]);
	frame.held.len = 1;
	std_landing(&frame, doltgres_identity_numbered_body, doltgres_identity_held_pad);
	doltgres_identity_held_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_identity_deriving_frame_t is the state of recording an object's
 * pairs across its pad: the column's name it made, if any, and its pair by
 * number, and what the pairs are recorded with.
 */
typedef struct doltgres_identity_deriving_frame_t
{
	doltgres_identity_held_t held;
	HostIoIo   *io;
	HostIoWaits *waits;
	const doltgres_identity_Pairing_t *pairing;
	TxnIdentityKind kind;
	uint64		parent;
	StdBytes	name;
	uint32		number;
	TxnIdentityKeyed keyed;
	const void *captures;
	uint64	   *result;
	PgPgError  *error;
	bool		ok;
} doltgres_identity_deriving_frame_t;

/*
 * doltgres_identity_deriving_body makes the object's pair by number, a
 * column's by its attribute number under its table and any other's by its
 * OID, and records its pairs.
 */
static void
doltgres_identity_deriving_body(void *frame_)
{
	doltgres_identity_deriving_frame_t *frame = frame_;
	StdVec	   *numbered = &frame->held.bytes[frame->held.len];
	HostError	failed;

	if (frame->kind == TXN_IDENTITY_KIND_COLUMN)
		txn_identity_column_by_number(frame->parent, (uint16) frame->number, numbered);
	else
		txn_identity_by_oid(frame->kind, frame->number, numbered);
	frame->held.len++;
	frame->ok = txn_identity_derived(frame->io, frame->waits, frame->pairing->tree, frame->pairing->writer,
									 frame->pairing->roomed, frame->kind, frame->parent, frame->name.ptr,
									 frame->name.len, numbered->ptr, numbered->len, frame->number, frame->keyed,
									 frame->captures, NULL, frame->result, &failed);
	if (!frame->ok)
		*frame->error = pghost_raised(&failed);
}

/* doltgres_identity_derived_closure_0 is derived's keyed: an object's pair by id under its kind. */
static void
doltgres_identity_derived_closure_0(const void *captures, uint64 id, StdVec *result)
{
	txn_identity_by_id(*(const TxnIdentityKind *) captures, id, result);
}

/*
 * doltgres_identity_derived records an object's pair by number and its pair
 * by id, a schema's or a relation's under its kind, and answers its id, or
 * the id its pair by number already holds.
 */
static bool
doltgres_identity_derived(HostIoIo *io, HostIoWaits *waits, const doltgres_identity_Pairing_t *pairing,
						  TxnIdentityKind kind, uint64 parent, StdBytes name, uint32 oid, uint64 *result,
						  PgPgError *error)
{
	doltgres_identity_deriving_frame_t frame = {.io = io, .waits = waits, .pairing = pairing, .kind = kind,
		.parent = parent, .name = name, .number = oid, .keyed = doltgres_identity_derived_closure_0,
	.captures = &kind, .result = result, .error = error};

	std_landing(&frame, doltgres_identity_deriving_body, doltgres_identity_held_pad);
	doltgres_identity_held_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_identity_index records an index's pair under its table's id, by
 * the name it is made with, which the caller copied into a buffer, and its
 * OID.
 */
static bool
doltgres_identity_index(HostIoIo *io, HostIoWaits *waits, const doltgres_identity_Pairing_t *pairing,
						const uint8 *buffer, int32 len, uint32 oid, uint64 table, uint64 *result, PgPgError *error)
{
	StdBytes	name;

	if (!doltgres_identity_named(buffer, len, &name, error))
		return false;
	return doltgres_identity_derived(io, waits, pairing, TXN_IDENTITY_KIND_RELATION, table, name, oid, result,
									 error);
}

/*
 * doltgres_identity_column_of records a column's pairs under a table's id
 * by an attribute number, its id derived from its name and its type's
 * category, its pair by id made by keyed, and answers its id, or the id its
 * pair by number already holds.
 */
static bool
doltgres_identity_column_of(HostIoIo *io, HostIoWaits *waits, const doltgres_identity_Pairing_t *pairing,
							uint64 table, StdBytes name, uint8 category, uint16 number, TxnIdentityKeyed keyed,
							uint64 *result, PgPgError *error)
{
	doltgres_identity_deriving_frame_t frame = {.io = io, .waits = waits, .pairing = pairing,
		.kind = TXN_IDENTITY_KIND_COLUMN, .parent = table, .number = number, .keyed = keyed, .captures = &table,
	.result = result, .error = error};

	txn_identity_column_name(name.ptr, name.len, category, &frame.held.bytes[0]);
	frame.held.len = 1;
	frame.name.ptr = frame.held.bytes[0].ptr;
	frame.name.len = frame.held.bytes[0].len;
	std_landing(&frame, doltgres_identity_deriving_body, doltgres_identity_held_pad);
	doltgres_identity_held_pad(&frame);
	return frame.ok;
}

/* doltgres_identity_number answers an attribute number as a pair's u16, or an internal error past its width. */
static bool
doltgres_identity_number(int32 attnum, const char *message, uint16 *result, PgPgError *error)
{
	if (attnum < 0 || attnum > UINT16_MAX)
	{
		*error = doltgres_identity_internal(message);
		return false;
	}
	*result = (uint16) attnum;
	return true;
}

/* doltgres_identity_column_closure_1 is column's keyed: a column's pair by id under its table's. */
static void
doltgres_identity_column_closure_1(const void *captures, uint64 id, StdVec *result)
{
	txn_identity_column_by_id(*(const uint64 *) captures, id, result);
}

/*
 * doltgres_identity_column records a table's column's pairs under the
 * table's id by its attribute number, and answers its id.
 */
static bool
doltgres_identity_column(const PgCatalogCaps *caps, HostIoIo *io, HostIoWaits *waits,
						 const doltgres_identity_Pairing_t *pairing, const PgRel *rel, uint64 table, int32 attnum,
						 uint64 *result, PgPgError *error)
{
	uint8		category;
	uint8		buffer[NAMEDATALEN] = {0};
	StdBytes	name;
	uint16		number;

	if (!pg_calls_type_category(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps),
								pg_calls_rel_att_type(rel, attnum), &category, error) ||
		!doltgres_identity_named(buffer, pg_calls_rel_att_name(rel, attnum, buffer, sizeof(buffer)), &name, error) ||
		!doltgres_identity_number(attnum, "a column's attribute number is past its width", &number, error))
		return false;
	return doltgres_identity_column_of(io, waits, pairing, table, name, category, number,
									   doltgres_identity_column_closure_1, result, error);
}

/*
 * doltgres_identity_table records a table's pairs, its own under its
 * schema's id and its columns', and answers its id.
 */
static bool
doltgres_identity_table(const PgCatalogCaps *caps, HostIoIo *io, HostIoWaits *waits,
						const doltgres_identity_Pairing_t *pairing, const PgRel *rel, uint64 *result,
						PgPgError *error)
{
	bool		has;
	uint64		schema;
	uint8		buffer[NAMEDATALEN] = {0};
	StdBytes	name;

	if (!doltgres_identity_numbered(io, waits, pairing->tree, TXN_IDENTITY_KIND_SCHEMA, pg_calls_rel_namespace(rel),
									&has, &schema, error))
		return false;
	if (!has)
	{
		*error = doltgres_identity_internal("a table's first write found its schema with no identity pair");
		return false;
	}
	if (!doltgres_identity_named(buffer, pg_calls_rel_name(rel, buffer, sizeof(buffer)), &name, error) ||
		!doltgres_identity_derived(io, waits, pairing, TXN_IDENTITY_KIND_RELATION, schema, name,
								   pg_calls_rel_id(rel), result, error))
		return false;
	for (int32 attnum = 1; attnum <= pg_calls_rel_natts(rel); attnum++)
	{
		uint64		column;

		if (!pg_calls_rel_att_dropped(rel, attnum) &&
			!doltgres_identity_column(caps, io, waits, pairing, rel, *result, attnum, &column, error))
			return false;
	}
	return true;
}

/*
 * doltgres_identity_pairing answers the pairing of the database's tree and the
 * running transaction's writer at a command.
 */
static bool
doltgres_identity_pairing(HostIoIo *io, TxnIdentityDatabase tree, uint32 command,
						  doltgres_identity_Pairing_t *result, PgPgError *error)
{
	result->tree = tree;
	return doltgres_entries_current(io, command, &result->writer, &result->roomed, error);
}

/* doltgres_identity_current_pairing answers the pairing at the current command, which the call uses. */
static bool
doltgres_identity_current_pairing(const PgCatalogCaps *caps, HostIoIo *io, TxnIdentityDatabase tree,
								  doltgres_identity_Pairing_t *result, PgPgError *error)
{
	uint32		command;

	if (!pg_calls_current_command(pg_CatalogCaps_may_raise(caps), true, &command, error))
		return false;
	return doltgres_identity_pairing(io, tree, command, result, error);
}

/*
 * doltgres_identity_removing_frame_t is removing's state across its pad: the
 * key it looks up and the keys it removes, and what it removes them with.
 */
typedef struct doltgres_identity_removing_frame_t
{
	doltgres_identity_held_t held;
	const PgCatalogCaps *caps;
	HostIoIo   *io;
	HostIoWaits *waits;
	TxnIdentityDatabase tree;
	TxnIdentityKind kind;
	uint32		oid;
	const PgRel *columns;
	const doltgres_identity_Pairing_t *written;
	PgPgError  *error;
	bool		ok;
} doltgres_identity_removing_frame_t;

/*
 * doltgres_identity_removing_body gathers the object's pairs, and its
 * columns' that have pairs, and removes them, or nothing for an object with
 * no pairs.
 */
static void
doltgres_identity_removing_body(void *frame_)
{
	doltgres_identity_removing_frame_t *frame = frame_;
	doltgres_identity_held_t *held = &frame->held;
	bool		has;
	uint64		id;
	doltgres_identity_Pairing_t pairing;
	HostError	failed;

	txn_identity_by_oid(frame->kind, frame->oid, &held->bytes[0]);
	held->len = 1;
	if (!doltgres_identity_looked_up(frame->io, frame->waits, frame->tree, &held->bytes[0], &has, &id, frame->error))
		return;
	if (!has)
	{
		frame->ok = true;
		return;
	}
	std_vec_with_capacity_Checked(&held->keys, 2, DOLTGRES_IDENTITY_VEC);
	held->keys_live = true;
	held->len = 0;
	std_vec_push_Checked(&held->keys, &held->bytes[0], DOLTGRES_IDENTITY_VEC);
	txn_identity_by_id(frame->kind, id, &held->bytes[0]);
	std_vec_push_Checked(&held->keys, &held->bytes[0], DOLTGRES_IDENTITY_VEC);
	for (int32 attnum = 1; frame->columns != NULL && attnum <= pg_calls_rel_natts(frame->columns); attnum++)
	{
		uint16		number;
		bool		found;
		uint64		column;

		if (!doltgres_identity_number(attnum, "a column's attribute number is past its width", &number,
									  frame->error))
			return;
		txn_identity_column_by_number(id, number, &held->bytes[0]);
		held->len = 1;
		if (!doltgres_identity_looked_up(frame->io, frame->waits, frame->tree, &held->bytes[0], &found, &column,
										 frame->error))
			return;
		if (!found)
		{
			held->len = 0;
			std_vec_free_Checked(&held->bytes[0], DOLTGRES_IDENTITY_BYTES);
			continue;
		}
		std_vec_reserve_Checked(&held->keys, 2, DOLTGRES_IDENTITY_VEC);
		held->len = 0;
		std_vec_push_Checked(&held->keys, &held->bytes[0], DOLTGRES_IDENTITY_VEC);
		txn_identity_column_by_id(id, column, &held->bytes[0]);
		std_vec_push_Checked(&held->keys, &held->bytes[0], DOLTGRES_IDENTITY_VEC);
	}
	if (frame->written != NULL)
		pairing = *frame->written;
	else if (!doltgres_identity_current_pairing(frame->caps, frame->io, frame->tree, &pairing, frame->error))
		return;
	held->keys_live = false;
	frame->ok = txn_identity_removed(frame->io, frame->waits, frame->tree.f2, repo_Repo_own(frame->tree.f0),
									 pairing.writer, pairing.roomed, &held->keys, &failed);
	if (!frame->ok)
		*frame->error = pghost_raised(&failed);
}

/*
 * doltgres_identity_removing removes a schema's or a relation's pairs by its
 * kind and OID, and a table's columns' when columns names the table, with
 * written, or the current command's writer when it is NULL, which it takes
 * only after finding the object's pairs; an object with none has nothing to
 * remove.
 */
static bool
doltgres_identity_removing(const PgCatalogCaps *caps, HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase tree,
						   TxnIdentityKind kind, uint32 oid, const PgRel *columns,
						   const doltgres_identity_Pairing_t *written, PgPgError *error)
{
	doltgres_identity_removing_frame_t frame = {.caps = caps, .io = io, .waits = waits, .tree = tree, .kind = kind,
	.oid = oid, .columns = columns, .written = written, .error = error};

	std_landing(&frame, doltgres_identity_removing_body, doltgres_identity_held_pad);
	doltgres_identity_held_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_identity_recorded records a relation's pairs in its creating
 * transaction when its pair by OID is missing: a table's own, under its
 * schema's, and its columns', or an index's own under its table's id. A
 * temporary relation records nothing, and a TOAST relation's pairs are
 * toast_created's.
 */
bool
doltgres_identity_recorded(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
						   const HostSharedReader *stored_1, const PgRel *rel, uint32 command, PgPgError *error)
{
	TxnIdentityDatabase tree;
	HostIoWaits waits;
	bool		has;
	uint64		id;
	doltgres_identity_Pairing_t pairing;
	uint32		table;
	uint8		buffer[NAMEDATALEN] = {0};

	if (pg_calls_rel_namespace(rel) == PG_TOAST_NAMESPACE || pg_calls_rel_temporary(rel))
		return true;
	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error))
		return false;
	waits = pghost_waits();
	if (!doltgres_identity_numbered(io, &waits, tree, TXN_IDENTITY_KIND_RELATION, pg_calls_rel_id(rel), &has, &id,
									error))
		return false;
	if (has)
		return true;
	if (!doltgres_identity_pairing(io, tree, command, &pairing, error))
		return false;
	if (!doltgres_identity_indexed(rel, &table))
		return doltgres_identity_table(caps, io, &waits, &pairing, rel, &id, error);
	if (!doltgres_identity_numbered(io, &waits, tree, TXN_IDENTITY_KIND_RELATION, table, &has, &id, error))
		return false;
	if (!has)
	{
		*error = doltgres_identity_internal("an index's build found its table with no identity pair");
		return false;
	}
	return doltgres_identity_index(io, &waits, &pairing, buffer, pg_calls_rel_name(rel, buffer, sizeof(buffer)),
								   pg_calls_rel_id(rel), id, &id, error);
}

/*
 * doltgres_identity_toast_created records a table's TOAST relation's pair
 * under the table's id, and the TOAST relation's index's under its, in the
 * transaction that made the TOAST relation. A rewrite's new heap records it
 * for the table it rewrites, in place of that table's old TOAST relation's
 * pair; a temporary table records nothing.
 */
bool
doltgres_identity_toast_created(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
								const HostSharedReader *stored_1, const PgRel *rel, uint32 toast, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(caps);
	TxnIdentityDatabase tree;
	HostIoWaits waits;
	uint32		rewritten;
	bool		has;
	uint64		id;
	doltgres_identity_Pairing_t written;
	StdBytes	name = {DOLTGRES_IDENTITY_TOAST, sizeof(DOLTGRES_IDENTITY_TOAST)};
	uint64		toast_id;
	uint32		index;
	PgRefRel	opened;
	uint8		buffer[NAMEDATALEN] = {0};
	int32		len;
	uint32		oid;

	if (pg_calls_rel_temporary(rel))
		return true;
	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error))
		return false;
	waits = pghost_waits();
	rewritten = doltgres_relation_rewrites(pg_calls_rel_id(rel), pg_calls_rel_rewrite(rel));
	if (!doltgres_identity_numbered(io, &waits, tree, TXN_IDENTITY_KIND_RELATION,
									rewritten == 0 ? pg_calls_rel_id(rel) : rewritten, &has, &id, error))
		return false;
	if (!has)
	{
		*error = doltgres_identity_internal("a table's new TOAST relation found its table with no identity pair");
		return false;
	}
	if (!doltgres_identity_current_pairing(caps, io, tree, &written, error))
		return false;
	if (rewritten != 0)
	{
		uint32		old;

		if (!pg_RefRel_opened(raise, catalog, rewritten, &has, &opened, error))
			return false;
		if (!has)
		{
			*error = doltgres_identity_internal("a rewrite's new TOAST relation found no table it rewrites");
			return false;
		}
		old = pg_calls_rel_toast_id(pg_RefRel_rel(&opened));
		pg_RefRel_drop(&opened);
		if (!doltgres_identity_removing(caps, io, &waits, tree, TXN_IDENTITY_KIND_RELATION, old, NULL, &written,
										error))
			return false;
	}
	if (!doltgres_identity_derived(io, &waits, &written, TXN_IDENTITY_KIND_RELATION, id,
								   name, toast,
								   &toast_id, error) ||
		!pg_calls_toast_index(raise, catalog, toast, &index, error) ||
		!pg_RefRel_opened(raise, catalog, index, &has, &opened, error))
		return false;
	if (!has)
	{
		*error = doltgres_identity_internal("a new TOAST relation has no index");
		return false;
	}
	len = pg_calls_rel_name(pg_RefRel_rel(&opened), buffer, sizeof(buffer));
	oid = pg_calls_rel_id(pg_RefRel_rel(&opened));
	pg_RefRel_drop(&opened);
	return doltgres_identity_index(io, &waits, &written, buffer, len, oid, toast_id, &id, error);
}

/*
 * doltgres_identity_dropped removes a dropped relation's pairs in the
 * dropping transaction: an index's own, or a table's own and its columns',
 * a dropped column's among them; a relation with none has nothing to
 * remove.
 */
bool
doltgres_identity_dropped(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
						  const HostSharedReader *stored_1, const PgRel *rel, PgPgError *error)
{
	TxnIdentityDatabase tree;
	HostIoWaits waits;
	uint32		table;

	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error))
		return false;
	waits = pghost_waits();
	return doltgres_identity_removing(caps, io, &waits, tree, TXN_IDENTITY_KIND_RELATION, pg_calls_rel_id(rel),
									  doltgres_identity_indexed(rel, &table) ? NULL : rel, NULL, error);
}

/* doltgres_identity_schema_created records a new schema's pairs in its creating transaction, by its name as made. */
bool
doltgres_identity_schema_created(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
								 const HostSharedReader *stored_1, uint32 namespace, const uint8 *name,
								 size_t name_len, PgPgError *error)
{
	HostIoWaits waits = pghost_waits();
	TxnIdentityDatabase tree;
	doltgres_identity_Pairing_t pairing;
	StdBytes	named = {name, name_len};
	uint64		id;

	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error) ||
		!doltgres_identity_current_pairing(caps, io, tree, &pairing, error))
		return false;
	return doltgres_identity_derived(io, &waits, &pairing, TXN_IDENTITY_KIND_SCHEMA, 0,
									 named, namespace, &id, error);
}

/*
 * doltgres_identity_schema_dropped removes a dropped schema's pairs in the
 * dropping transaction; a schema with none has nothing to remove.
 */
bool
doltgres_identity_schema_dropped(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
								 const HostSharedReader *stored_1, uint32 namespace, PgPgError *error)
{
	TxnIdentityDatabase tree;
	HostIoWaits waits;

	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error))
		return false;
	waits = pghost_waits();
	return doltgres_identity_removing(caps, io, &waits, tree, TXN_IDENTITY_KIND_SCHEMA, namespace, NULL, NULL,
									  error);
}

/* doltgres_identity_added_closure_2 is added's keyed: a column's pair by id under its inheritor's. */
static void
doltgres_identity_added_closure_2(const void *captures, uint64 id, StdVec *result)
{
	txn_identity_column_by_id(*(const uint64 *) captures, id, result);
}

/*
 * doltgres_identity_added records an ADD COLUMN's new column's pairs: every
 * column without a pair under a written table's id, and, for each written
 * inheritor, each of the table's columns without a pair under the
 * inheritor's id by its attribute number there.
 */
static bool
doltgres_identity_added(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
						const HostSharedReader *stored_1, const PgRel *rel, PgPgError *error)
{
	const PgMayRaise *raise = pg_CatalogCaps_may_raise(caps);
	const PgCatalog *catalog = pg_CatalogCaps_catalog(caps);
	TxnIdentityDatabase tree;
	HostIoWaits waits;
	uint32		oid = pg_calls_rel_id(rel);
	bool		has;
	uint64		id;
	doltgres_identity_Pairing_t pairing;
	int32		next = 1;

	if (!doltgres_identity_tree(stored_0, stored_1, &tree, error))
		return false;
	waits = pghost_waits();
	if (!doltgres_identity_numbered(io, &waits, tree, TXN_IDENTITY_KIND_RELATION, oid, &has, &id, error) ||
		!doltgres_identity_current_pairing(caps, io, tree, &pairing, error))
		return false;
	for (int32 attnum = 1; has && attnum <= pg_calls_rel_natts(rel); attnum++)
	{
		uint64		column;

		if (!pg_calls_rel_att_dropped(rel, attnum) &&
			!doltgres_identity_column(caps, io, &waits, &pairing, rel, id, attnum, &column, error))
			return false;
	}
	for (;;)
	{
		int32		at = next;
		uint32		child;
		bool		found;
		uint64		child_id;

		next = std_forward_i32_Checked(next, 1);
		if (!pg_calls_rel_inheritor(raise, catalog, oid, at, &child, error))
			return false;
		if (child == 0)
			break;
		if (!doltgres_identity_numbered(io, &waits, tree, TXN_IDENTITY_KIND_RELATION, child, &found, &child_id,
										error))
			return false;
		for (int32 attnum = 1; found && attnum <= pg_calls_rel_natts(rel); attnum++)
		{
			uint8		buffer[NAMEDATALEN] = {0};
			StdBytes	name;
			int32		inherited;
			uint16		number;
			uint8		category;
			uint64		column;

			if (pg_calls_rel_att_dropped(rel, attnum))
				continue;
			if (!doltgres_identity_named(buffer, pg_calls_rel_att_name(rel, attnum, buffer, sizeof(buffer)), &name,
										 error) ||
				!pg_calls_att_number(raise, catalog, child, name.ptr, name.len, &inherited, error) ||
				!doltgres_identity_number(inherited, "an inherited column has no attribute number", &number, error) ||
				!pg_calls_type_category(raise, catalog, pg_calls_rel_att_type(rel, attnum), &category, error) ||
				!doltgres_identity_column_of(io, &waits, &pairing, child_id, name, category, number,
											 doltgres_identity_added_closure_2, &column, error))
				return false;
		}
	}
	return true;
}

/*
 * doltgres_identity_altered records what an executed ALTER TABLE subcommand
 * changes of a relation's pairs, by every subcommand, with no default, so an
 * upstream subcommand stops the build until it is placed here: only ADD
 * COLUMN writes.
 */
bool
doltgres_identity_altered(const PgCatalogCaps *caps, HostIoIo *io, const RepoRepo *stored_0,
						  const HostSharedReader *stored_1, const PgRel *rel, AlterTableType subtype,
						  PgPgError *error)
{
	switch (subtype)
	{
		case AT_AddColumn:
			return doltgres_identity_added(caps, io, stored_0, stored_1, rel, error);
		case AT_AddColumnToView:
		case AT_ColumnDefault:
		case AT_CookedColumnDefault:
		case AT_DropNotNull:
		case AT_SetNotNull:
		case AT_SetExpression:
		case AT_DropExpression:
		case AT_SetStatistics:
		case AT_SetOptions:
		case AT_ResetOptions:
		case AT_SetStorage:
		case AT_SetCompression:
		case AT_DropColumn:
		case AT_AddIndex:
		case AT_ReAddIndex:
		case AT_AddConstraint:
		case AT_ReAddConstraint:
		case AT_ReAddDomainConstraint:
		case AT_AlterConstraint:
		case AT_ValidateConstraint:
		case AT_AddIndexConstraint:
		case AT_DropConstraint:
		case AT_ReAddComment:
		case AT_AlterColumnType:
		case AT_AlterColumnGenericOptions:
		case AT_ChangeOwner:
		case AT_ClusterOn:
		case AT_DropCluster:
		case AT_SetLogged:
		case AT_SetUnLogged:
		case AT_DropOids:
		case AT_SetAccessMethod:
		case AT_SetTableSpace:
		case AT_SetRelOptions:
		case AT_ResetRelOptions:
		case AT_ReplaceRelOptions:
		case AT_EnableTrig:
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_DisableTrig:
		case AT_EnableTrigAll:
		case AT_DisableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrigUser:
		case AT_EnableRule:
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddInherit:
		case AT_DropInherit:
		case AT_AddOf:
		case AT_DropOf:
		case AT_ReplicaIdentity:
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
		case AT_GenericOptions:
		case AT_AttachPartition:
		case AT_DetachPartition:
		case AT_DetachPartitionFinalize:
		case AT_AddIdentity:
		case AT_SetIdentity:
		case AT_DropIdentity:
		case AT_ReAddStatistics:
			return true;
	}
	pg_unreachable();
}
#endif
