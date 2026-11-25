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
 * Object ids and the identity tree (txn's identity.rs): a schema's, a
 * relation's, and a column's id derived from its kind, its parent's id, and
 * its name; the identity tree's two key forms, which pair each id with the
 * number Postgres gives the object; and the tree's changes, versions in a
 * database's working state's delta written in the DDL's transaction.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/host/delta.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/identity.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/visible.h"
#include "doltgres/txn/written.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/chunks/record.h"
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"

/* RUST_FILE is the file the tests' asserts name, identity.rs. */
#define RUST_FILE "txn" STD_SEP "src" STD_SEP "identity.rs"
#endif

/* The prepare whose closure pushes an identity change's versions, or answers false to start over. */
#define TXN_MEMTABLE_BRANCH_RS Bool
#define TXN_MEMTABLE_BRANCH_WS Striping
#define TXN_MEMTABLE_BRANCH_RETURNS bool
#define TXN_MEMTABLE_BRANCH_RESULT_PARAMS , bool *result, HostError *error
#define TXN_MEMTABLE_BRANCH_WITH_PARAMS , HostIoStriping striping
#define TXN_MEMTABLE_BRANCH_DECLARE
#define TXN_MEMTABLE_BRANCH_DEFINE
#define TXN_MEMTABLE_BRANCH_WITH_FIELDS HostIoStriping striping;
#define TXN_MEMTABLE_BRANCH_WITH_INIT , striping
#define TXN_MEMTABLE_BRANCH_WITH_ARGS(c) , (c)->striping
#define TXN_MEMTABLE_BRANCH_RESULT_FIELDS bool *result;
#define TXN_MEMTABLE_BRANCH_RESULT_INIT , result
#define TXN_MEMTABLE_BRANCH_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_BRANCH_RESULT_ERROR error
#include "doltgres/txn/memtable/branch_template.h"

/* The prepare whose closure pushes a replayed identity change's version. */
#define TXN_MEMTABLE_BRANCH_RS Handle
#define TXN_MEMTABLE_BRANCH_WS Unit
#define TXN_MEMTABLE_BRANCH_RETURNS bool
#define TXN_MEMTABLE_BRANCH_RESULT_PARAMS , TxnMemtableVersionHandle *result, HostError *error
#define TXN_MEMTABLE_BRANCH_WITH_PARAMS
#define TXN_MEMTABLE_BRANCH_DECLARE
#define TXN_MEMTABLE_BRANCH_DEFINE
#define TXN_MEMTABLE_BRANCH_WITH_FIELDS
#define TXN_MEMTABLE_BRANCH_WITH_INIT
#define TXN_MEMTABLE_BRANCH_WITH_ARGS(c)
#define TXN_MEMTABLE_BRANCH_RESULT_FIELDS TxnMemtableVersionHandle *result;
#define TXN_MEMTABLE_BRANCH_RESULT_INIT , result
#define TXN_MEMTABLE_BRANCH_RESULT_ARGS(c) , (c)->result
#define TXN_MEMTABLE_BRANCH_RESULT_ERROR error
#include "doltgres/txn/memtable/branch_template.h"

/* TXN_IDENTITY_BY_ID starts a key that finds an object's number from its id. */
#define TXN_IDENTITY_BY_ID 1

/* TXN_IDENTITY_BY_NUMBER starts a key that finds an object's id from its number. */
#define TXN_IDENTITY_BY_NUMBER 2

/* TXN_IDENTITY_PROBED is the pause after a derivation's probe, before its pairs' push. */
#define TXN_IDENTITY_PROBED "doltgres-identity-probed"

/* The element layouts of the vectors identity's functions build. */
#define TXN_IDENTITY_BYTES 1, 1
#define TXN_IDENTITY_VEC sizeof(StdVec), _Alignof(StdVec)
#define TXN_IDENTITY_PAIR sizeof(TxnIdentityPair), _Alignof(TxnIdentityPair)
#define TXN_IDENTITY_PAIRED sizeof(TxnIdentityPaired), _Alignof(TxnIdentityPaired)
#define TXN_IDENTITY_OP sizeof(TxnMemtableOp), _Alignof(TxnMemtableOp)
#define TXN_IDENTITY_NEED sizeof(txn_TupleRefSliceU8Usize_t), _Alignof(txn_TupleRefSliceU8Usize_t)
#define TXN_IDENTITY_HANDLE sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle)
#define TXN_IDENTITY_FOLDED sizeof(txn_TupleVecU8VecHandle_t), _Alignof(txn_TupleVecU8VecHandle_t)

/* txn_TupleVecU8VecHandle_t is dropped's (Vec<u8>, Vec<Handle>): a key and its folded versions. */
typedef struct txn_TupleVecU8VecHandle_t
{
	StdVec		f0;
	StdVec		f1;
} txn_TupleVecU8VecHandle_t;

/* TxnIdentityFoundTag is Found's variants. */
typedef enum TxnIdentityFoundTag
{
	/* The value of the key's live pair. */
	TXN_IDENTITY_FOUND_VALUE,
	/* Another transaction's running change of the key, which must end first. */
	TXN_IDENTITY_FOUND_WAIT,
	/* Nothing. */
	TXN_IDENTITY_FOUND_FREE,
} TxnIdentityFoundTag;

/* TxnIdentityFound is Found: what a key of the identity tree holds for the running transaction. */
typedef struct TxnIdentityFound
{
	TxnIdentityFoundTag tag;
	union
	{
		/* value is a Vec<u8>. */
		StdVec		value;
		HostXid		wait;
	}			u;
} TxnIdentityFound;

const TxnIdentityInitial TXN_IDENTITY_INITIAL[3] = {
	{TXN_IDENTITY_INITIAL_0, "pg_catalog", 10},
	{TXN_IDENTITY_INITIAL_1, "pg_toast", 8},
	{TXN_IDENTITY_INITIAL_2, "public", 6},
};

/* txn_identity_fork returns an error of a code and a message. */
static HostError
txn_identity_fork(const char *code, const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) code, &message);
}

/* txn_identity_bytes copies bytes into a new vector, as to_vec does. */
static void
txn_identity_bytes(const uint8 *bytes, size_t bytes_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, bytes_len, TXN_IDENTITY_BYTES);
	std_vec_extend_from_slice_Checked(result, bytes, bytes_len, TXN_IDENTITY_BYTES);
}

/* txn_identity_Pair_drop frees a pair's key and its value. */
static void
txn_identity_Pair_drop(void *pair_)
{
	TxnIdentityPair *pair = pair_;

	std_vec_free_Checked(&pair->f0, TXN_IDENTITY_BYTES);
	if (pair->has_f1)
		std_vec_free_Checked(&pair->f1, TXN_IDENTITY_BYTES);
}

/* txn_identity_Paired_drop frees a paired key and its value. */
static void
txn_identity_Paired_drop(void *paired_)
{
	TxnIdentityPaired *paired = paired_;

	std_vec_free_Checked(&paired->f0, TXN_IDENTITY_BYTES);
	std_vec_free_Checked(&paired->f1, TXN_IDENTITY_BYTES);
}

/* txn_identity_bytes_drop frees a vector of bytes held as a vector's element. */
static void
txn_identity_bytes_drop(void *bytes)
{
	std_vec_free_Checked(bytes, TXN_IDENTITY_BYTES);
}

/*
 * txn_identity_derive_object_id returns the first candidate of a new
 * object's id from its kind, its parent's id, and its name at creation, so
 * the same creation gives the same id in every database; a taken candidate
 * moves to next_id's.
 */
bool
txn_identity_derive_object_id(TxnIdentityKind kind, uint64 parent, const uint8 *name, size_t name_len, uint64 *result,
							  HostError *error)
{
	StdVec		input = STD_VEC_NEW;
	uint8		be[8];
	ChunksAddressAddress hash;

	if (!std_vec_try_reserve_exact(&input, 9 + name_len, TXN_IDENTITY_BYTES))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_u8_push_Checked(&input, (uint8) kind);
	std_u64_to_be_bytes(parent, be);
	std_vec_extend_from_slice_Checked(&input, be, sizeof(be), TXN_IDENTITY_BYTES);
	std_vec_extend_from_slice_Checked(&input, name, name_len, TXN_IDENTITY_BYTES);
	hash = chunks_address_Address_of(input.ptr, input.len);
	std_vec_free_Checked(&input, TXN_IDENTITY_BYTES);
	*result = std_u64_from_be_bytes(hash.f0) | TXN_IDENTITY_OBJECT_ID_BIT;
	return true;
}

/*
 * txn_identity_paired_frame_t is paired's state across its pad: the two keys
 * it takes and the varint it makes, each while live.
 */
typedef struct txn_identity_paired_frame_t
{
	StdVec		numbered;
	StdVec		keyed;
	bool		keys_live;
	uint64		id;
	uint32		number;
	StdVec		varint;
	bool		varint_live;
	TxnIdentityPaired *result;
} txn_identity_paired_frame_t;

/* txn_identity_paired_body makes the number's varint and the id's bytes, then the two pairs. */
static void
txn_identity_paired_body(void *frame_)
{
	txn_identity_paired_frame_t *frame = frame_;
	uint8		be[8];
	StdVec		id;

	std_vec_new(&frame->varint, TXN_IDENTITY_BYTES);
	frame->varint_live = true;
	chunks_varint_put_varint(&frame->varint, (uint64) frame->number);
	std_u64_to_be_bytes(frame->id, be);
	txn_identity_bytes(be, sizeof(be), &id);
	frame->result[0].f0 = frame->numbered;
	frame->result[0].f1 = id;
	frame->result[1].f0 = frame->keyed;
	frame->result[1].f1 = frame->varint;
	frame->keys_live = frame->varint_live = false;
}

/* txn_identity_paired_pad frees what paired holds live: its keys and its varint. */
static void
txn_identity_paired_pad(void *frame_)
{
	txn_identity_paired_frame_t *frame = frame_;

	if (frame->varint_live)
		std_vec_free_Checked(&frame->varint, TXN_IDENTITY_BYTES);
	if (frame->keys_live)
	{
		std_vec_free_Checked(&frame->keyed, TXN_IDENTITY_BYTES);
		std_vec_free_Checked(&frame->numbered, TXN_IDENTITY_BYTES);
	}
	frame->varint_live = frame->keys_live = false;
}

/*
 * txn_identity_paired returns an object's two pairs, taking both keys: its
 * pair by number holding its id, and its pair by id holding its number as a
 * varint.
 */
static void
txn_identity_paired(StdVec numbered, StdVec keyed, uint64 id, uint32 number, TxnIdentityPaired *result)
{
	txn_identity_paired_frame_t frame = {.numbered = numbered, .keyed = keyed, .keys_live = true, .id = id,
	.number = number, .result = result};

	std_landing(&frame, txn_identity_paired_body, txn_identity_paired_pad);
	txn_identity_paired_pad(&frame);
}

/*
 * txn_identity_initial_frame_t is initial's state across its pad: its pairs,
 * a schema's key by OID before it is paired, and a schema's two pairs before
 * they are added, each while live.
 */
typedef struct txn_identity_initial_frame_t
{
	StdVec		pairs;
	bool		pairs_live;
	StdVec		numbered;
	bool		numbered_live;
	TxnIdentityPaired two[2];
	bool		two_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_initial_frame_t;

/* txn_identity_initial_body derives each initial schema's id and adds its two pairs. */
static void
txn_identity_initial_body(void *frame_)
{
	txn_identity_initial_frame_t *frame = frame_;

	std_vec_new(&frame->pairs, TXN_IDENTITY_PAIRED);
	frame->pairs_live = true;
	for (int i = 0; i < lengthof(TXN_IDENTITY_INITIAL); i++)
	{
		const TxnIdentityInitial *schema = &TXN_IDENTITY_INITIAL[i];
		uint64		id;
		StdVec		keyed;

		if (!txn_identity_derive_object_id(TXN_IDENTITY_KIND_SCHEMA, 0, (const uint8 *) schema->name,
										   schema->name_len, &id, frame->error))
			return;
		txn_identity_by_oid(TXN_IDENTITY_KIND_SCHEMA, schema->oid, &frame->numbered);
		frame->numbered_live = true;
		txn_identity_by_id(TXN_IDENTITY_KIND_SCHEMA, id, &keyed);
		frame->numbered_live = false;
		txn_identity_paired(frame->numbered, keyed, id, schema->oid, frame->two);
		frame->two_live = true;
		std_vec_reserve_Checked(&frame->pairs, lengthof(frame->two), TXN_IDENTITY_PAIRED);
		for (int j = 0; j < lengthof(frame->two); j++)
			std_vec_push_Checked(&frame->pairs, &frame->two[j], TXN_IDENTITY_PAIRED);
		frame->two_live = false;
	}
	*frame->result = frame->pairs;
	frame->pairs_live = false;
	frame->ok = true;
}

/* txn_identity_initial_pad frees what initial holds live: a schema's pairs and key, and its pairs. */
static void
txn_identity_initial_pad(void *frame_)
{
	txn_identity_initial_frame_t *frame = frame_;

	if (frame->two_live)
		for (int j = 0; j < lengthof(frame->two); j++)
			txn_identity_Paired_drop(&frame->two[j]);
	if (frame->numbered_live)
		std_vec_free_Checked(&frame->numbered, TXN_IDENTITY_BYTES);
	if (frame->pairs_live)
		std_vec_drop_Checked(&frame->pairs, TXN_IDENTITY_PAIRED, txn_identity_Paired_drop);
	frame->two_live = frame->numbered_live = frame->pairs_live = false;
}

/*
 * txn_identity_initial returns the pairs of a database's initial schemas,
 * each its pair by OID and its pair by id, which a new repository's identity
 * tree starts with, so that no writer derives a schema's pair.
 */
bool
txn_identity_initial(StdVec *result, HostError *error)
{
	txn_identity_initial_frame_t frame = {.result = result, .error = error};

	std_landing(&frame, txn_identity_initial_body, txn_identity_initial_pad);
	txn_identity_initial_pad(&frame);
	return frame.ok;
}

/* txn_identity_next_id returns the candidate after a taken one, which keeps the user bit. */
uint64
txn_identity_next_id(uint64 id)
{
	return (id + 1) | TXN_IDENTITY_OBJECT_ID_BIT;
}

/*
 * txn_identity_column_name returns the name a column's id derives from under
 * its table: its name and its base type's category, so a column made again
 * under its name with a type of another category is another column.
 */
void
txn_identity_column_name(const uint8 *name, size_t name_len, uint8 category, StdVec *result)
{
	std_vec_with_capacity_Checked(result, name_len + 2, TXN_IDENTITY_BYTES);
	std_vec_extend_from_slice_Checked(result, name, name_len, TXN_IDENTITY_BYTES);
	std_vec_u8_push_Checked(result, 0);
	std_vec_u8_push_Checked(result, category);
}

/* txn_identity_keyed returns a key of a form and a kind followed by a big-endian number of a width. */
static void
txn_identity_keyed(uint8 form, uint8 kind, const uint8 *number, size_t number_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, 2 + number_len, TXN_IDENTITY_BYTES);
	std_vec_u8_push_Checked(result, form);
	std_vec_u8_push_Checked(result, kind);
	std_vec_extend_from_slice_Checked(result, number, number_len, TXN_IDENTITY_BYTES);
}

/* txn_identity_by_id returns the key that finds a schema's or a relation's OID from its id. */
void
txn_identity_by_id(TxnIdentityKind kind, uint64 id, StdVec *result)
{
	uint8		be[8];

	std_u64_to_be_bytes(id, be);
	txn_identity_keyed(TXN_IDENTITY_BY_ID, (uint8) kind, be, sizeof(be), result);
}

/* txn_identity_by_oid returns the key that finds a schema's or a relation's id from its OID. */
void
txn_identity_by_oid(TxnIdentityKind kind, uint32 oid, StdVec *result)
{
	uint8		be[4];

	std_u32_to_be_bytes(oid, be);
	txn_identity_keyed(TXN_IDENTITY_BY_NUMBER, (uint8) kind, be, sizeof(be), result);
}

/* txn_identity_column_by_id returns the key that finds a column's attribute number from its id under its table's id. */
void
txn_identity_column_by_id(uint64 table, uint64 column, StdVec *result)
{
	uint8		be[16];

	std_u64_to_be_bytes(table, be);
	std_u64_to_be_bytes(column, be + 8);
	txn_identity_keyed(TXN_IDENTITY_BY_ID, TXN_IDENTITY_KIND_COLUMN, be, sizeof(be), result);
}

/*
 * txn_identity_column_by_number returns the key that finds a column's id from
 * its attribute number under its table's id.
 */
void
txn_identity_column_by_number(uint64 table, uint16 attnum, StdVec *result)
{
	uint8		be[10];

	std_u64_to_be_bytes(table, be);
	std_u16_to_be_bytes(attnum, be + 8);
	txn_identity_keyed(TXN_IDENTITY_BY_NUMBER, TXN_IDENTITY_KIND_COLUMN, be, sizeof(be), result);
}

/*
 * txn_identity_columns returns the key prefix of a table's columns' pairs in
 * one of the two forms, by id when ids says so.
 */
void
txn_identity_columns(uint64 table, bool ids, StdVec *result)
{
	uint8		be[8];

	std_u64_to_be_bytes(table, be);
	txn_identity_keyed(ids ? TXN_IDENTITY_BY_ID : TXN_IDENTITY_BY_NUMBER, TXN_IDENTITY_KIND_COLUMN, be, sizeof(be),
					   result);
}

/* txn_identity_corrupt returns the error of an identity tree's value of the wrong form. */
static HostError
txn_identity_corrupt(void)
{
	return txn_identity_fork("XX001", "an identity tree's value has the wrong form");
}

/*
 * txn_identity_stripe returns the key lock's stripe of a database's identity
 * tree, which every change of its pairs takes, so a derivation's probe and
 * its pairs' insert are one step to every other writer of the tree.
 */
static uint32
txn_identity_stripe(uint32 repository)
{
	HostStorage storage = txn_spill_identity_storage(repository);

	return txn_change_stripe(&storage, NULL, 0, false);
}

/*
 * txn_identity_found_frame_t is found's state across its pad: the key's
 * versions, its spilled versions before they join them, the folded tree's
 * value, and the resolve, each while live.
 */
typedef struct txn_identity_found_frame_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	HostSharedRecord deltas;
	const uint8 *key;
	size_t		key_len;
	StdVec		versions;
	bool		versions_live;
	StdVec		spilled;
	bool		spilled_live;
	bool		has_based;
	StdVec		based;
	bool		based_live;
	TxnVisibleResolved resolved;
	bool		resolved_live;
	TxnIdentityFound *result;
	HostError  *error;
	bool		ok;
} txn_identity_found_frame_t;

/* txn_identity_found_dropped drops the key's versions a read of found gave. */
static void
txn_identity_found_dropped(void *captures)
{
	txn_identity_found_frame_t *frame = captures;

	if (frame->spilled_live)
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->spilled_live = frame->versions_live = false;
}

/* txn_identity_found_read is found's read: the key's versions in the memtable, then in its spills (bugs row 291). */
static bool
txn_identity_found_read(void *captures, uint64 *listed, HostError *error)
{
	txn_identity_found_frame_t *frame = captures;

	if (!txn_memtable_branch_identity_versions(frame->io, frame->hold, frame->deltas, frame->key, frame->key_len,
											   &frame->versions, error))
		return false;
	frame->versions_live = true;
	if (!txn_spill_versions(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, frame->hold, frame->deltas,
							TXN_RUNS_KIND_SPILLED_IDENTITY, frame->key, frame->key_len, listed, &frame->spilled, error))
		return false;
	frame->spilled_live = true;
	return true;
}

/*
 * txn_identity_found_body reads the key's versions, the memtable's with its
 * spills' placed among them in commit order (bugs rows 291 and 397), and
 * then the folded tree's value, resolves the
 * key, and for a key that exists answers the newest put whose writer is the
 * caller's transaction or committed, or the folded tree's value.
 */
static void
txn_identity_found_body(void *frame_)
{
	txn_identity_found_frame_t *frame = frame_;
	ChunksErrorError storage;
	StdBytes	based;

	if (!txn_spill_taken(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, frame->deltas, txn_identity_found_read,
						 txn_identity_found_dropped, frame, frame->error) ||
		!txn_spill_ordered(frame->io, &frame->versions, &frame->spilled, frame->error))
		return;
	frame->spilled_live = false;
	std_vec_free_Checked(&frame->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (!repo_Repo_identity(frame->repo, frame->io, frame->reading, frame->key, frame->key_len, &frame->has_based,
							&frame->based, &storage))
	{
		*frame->error = txn_base_storage(&storage);
		return;
	}
	frame->based_live = frame->has_based;
	based.ptr = frame->has_based ? frame->based.ptr : NULL;
	based.len = frame->has_based ? frame->based.len : 0;
	if (!txn_visible_resolved(frame->io, frame->versions.ptr, frame->versions.len, based, &frame->resolved,
							  frame->error))
		return;
	frame->resolved_live = true;
	switch (frame->resolved.tag)
	{
		case TXN_VISIBLE_RESOLVED_FREE:
			frame->result->tag = TXN_IDENTITY_FOUND_FREE;
			frame->ok = true;
			return;
		case TXN_VISIBLE_RESOLVED_WAIT:
			frame->result->tag = TXN_IDENTITY_FOUND_WAIT;
			frame->result->u.wait = frame->resolved.u.wait.f0;
			frame->ok = true;
			return;
		case TXN_VISIBLE_RESOLVED_EXISTS:
			break;
	}
	frame->resolved_live = false;
	txn_visible_Resolved_drop(&frame->resolved);
	for (size_t i = 0; i < frame->versions.len; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
		HostXid		writer = {txn_memtable_version_Record_writer(&record).xid};
		HostStatus	status;

		if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_PUT)
			continue;
		if (!host_is_current(HOST_IO_IO_IO_DOORS(frame->io), writer))
		{
			if (!txn_outcome_outcome(frame->io, version, &status, frame->error))
				return;
			if (status != HOST_STATUS_COMMITTED)
				continue;
		}
		frame->result->tag = TXN_IDENTITY_FOUND_VALUE;
		txn_identity_bytes(op.u.put.value.ptr, op.u.put.value.len, &frame->result->u.value);
		frame->ok = true;
		return;
	}
	if (!frame->based_live)
	{
		*frame->error = txn_identity_fork("XX000", "an identity tree key that exists has no value");
		return;
	}
	frame->result->tag = TXN_IDENTITY_FOUND_VALUE;
	frame->result->u.value = frame->based;
	frame->based_live = false;
	frame->ok = true;
}

/* txn_identity_found_pad frees what found holds live: its resolve, the folded tree's value, and the versions. */
static void
txn_identity_found_pad(void *frame_)
{
	txn_identity_found_frame_t *frame = frame_;

	if (frame->resolved_live)
		txn_visible_Resolved_drop(&frame->resolved);
	if (frame->based_live)
		std_vec_free_Checked(&frame->based, TXN_IDENTITY_BYTES);
	if (frame->spilled_live)
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->resolved_live = frame->based_live = frame->spilled_live = frame->versions_live = false;
}

/*
 * txn_identity_found reads a key of the identity tree as a unique check
 * reads its key, the caller's own changes and every committed one counted,
 * with the value of the version that decides it: the newest put whose writer
 * is the caller's transaction or committed, or the folded tree's value with
 * no such version. It reads the delta's versions, its spills' included,
 * before the folded tree, so a save between the reads only folds versions it
 * has already read.
 */
static bool
txn_identity_found(HostIoIo *io, const HostDeltaHold *hold, const RepoRepo *repo, const HostSharedReader *reading,
				   HostSharedRecord deltas, const uint8 *key, size_t key_len, TxnIdentityFound *result,
				   HostError *error)
{
	txn_identity_found_frame_t frame = {.io = io, .hold = hold, .repo = repo, .reading = reading, .deltas = deltas,
	.key = key, .key_len = key_len, .result = result, .error = error};

	std_landing(&frame, txn_identity_found_body, txn_identity_found_pad);
	txn_identity_found_pad(&frame);
	return frame.ok;
}

/* txn_identity_looked_up_frame_t is looked_up's state across its pad: its deltas and a read's hold, each while live. */
typedef struct txn_identity_looked_up_frame_t
{
	HostIoIo   *io;
	HostIoWaits *waits;
	TxnIdentityDatabase database;
	const uint8 *key;
	size_t		key_len;
	HostSharedHeld deltas;
	bool		deltas_live;
	HostDeltaHold hold;
	bool		hold_live;
	bool	   *has_result;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_looked_up_frame_t;

/* txn_identity_looked_up_body reads the key under a hold until it finds no running change of it. */
static void
txn_identity_looked_up_body(void *frame_)
{
	txn_identity_looked_up_frame_t *frame = frame_;
	HostOperation none = {0};

	if (!txn_memtable_branch_deltas(frame->io, frame->database.f2, repo_Repo_own(frame->database.f0), &frame->deltas,
									frame->error))
		return;
	frame->deltas_live = true;
	for (;;)
	{
		TxnIdentityFound read;
		bool		waited;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_identity_found(frame->io, &frame->hold, frame->database.f0, frame->database.f1,
								host_shared_Held_record(&frame->deltas), frame->key, frame->key_len, &read,
								frame->error))
			return;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
		switch (read.tag)
		{
			case TXN_IDENTITY_FOUND_VALUE:
				*frame->has_result = true;
				*frame->result = read.u.value;
				frame->ok = true;
				return;
			case TXN_IDENTITY_FOUND_FREE:
				*frame->has_result = false;
				frame->ok = true;
				return;
			case TXN_IDENTITY_FOUND_WAIT:
				if (!host_xact_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), read.u.wait, false, none, true,
									&waited, frame->error))
					return;
				break;
		}
	}
}

/* txn_identity_looked_up_pad frees what looked_up holds live: a read's hold and its deltas. */
static void
txn_identity_looked_up_pad(void *frame_)
{
	txn_identity_looked_up_frame_t *frame = frame_;

	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	frame->hold_live = frame->deltas_live = false;
}

/*
 * txn_identity_looked_up returns the value a key of a database's identity
 * tree holds for the running transaction, or none, waiting for another
 * transaction's running change of it to end first.
 */
bool
txn_identity_looked_up(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase database, const uint8 *key,
					   size_t key_len, bool *has_result, StdVec *result, HostError *error)
{
	txn_identity_looked_up_frame_t frame = {.io = io, .waits = waits, .database = database, .key = key,
	.key_len = key_len, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, txn_identity_looked_up_body, txn_identity_looked_up_pad);
	txn_identity_looked_up_pad(&frame);
	return frame.ok;
}

/* txn_identity_id reads an object's id from the value of its pair by number, eight bytes big-endian. */
bool
txn_identity_id(const uint8 *value, size_t value_len, uint64 *result, HostError *error)
{
	if (value_len != 8)
	{
		*error = txn_identity_corrupt();
		return false;
	}
	*result = std_u64_from_be_bytes(value);
	return true;
}

/*
 * txn_identity_number reads an object's number, an OID or an attribute number,
 * from the value of its pair by id, a varint.
 */
bool
txn_identity_number(const uint8 *value, size_t value_len, uint32 *result, HostError *error)
{
	size_t		at = 0;
	uint64		number;
	ChunksErrorError storage;

	if (!chunks_varint_read_varint(value, value_len, &at, &number, &storage))
	{
		*error = txn_base_storage(&storage);
		return false;
	}
	if (at != value_len || number > PG_UINT32_MAX)
	{
		*error = txn_identity_corrupt();
		return false;
	}
	*result = (uint32) number;
	return true;
}

/* txn_identity_Op_drop frees an op held as a vector's element. */
static void
txn_identity_Op_drop(void *op)
{
	txn_memtable_Op_drop(op);
}

/*
 * txn_identity_op returns the version's change of an identity tree key's
 * value, taking it: a put of the value, or a delete for none.
 */
static void
txn_identity_op(bool has_value, StdVec value, TxnMemtableOp *result)
{
	if (has_value)
	{
		TxnMemtableOp put = {.tag = TXN_MEMTABLE_OP_PUT,
		.u.put = {.value = value, .addresses = STD_VEC_NEW, .values = STD_VEC_NEW}};

		*result = put;
	}
	else
	{
		TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};

		*result = deleted;
	}
}

/*
 * txn_identity_changed_frame_t is changed's state across its pad and its
 * prepare: the pairs from the first it hasn't taken, and the records, keys,
 * ops, and needs it makes of them, each while live.
 */
typedef struct txn_identity_changed_frame_t
{
	HostIoIo   *io;
	HostIoWaits *waits;
	const HostSharedHeld *deltas;
	uint32		repository;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	StdVec		pairs;
	size_t		at;
	bool		has_count;
	uint64		count;
	uint32		stripe;
	StdVec		bodies;
	bool		bodies_live;
	StdVec		keys;
	bool		keys_live;
	StdVec		ops;
	bool		ops_live;
	StdVec		needs;
	bool		needs_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_changed_frame_t;

/*
 * txn_identity_changed_closure_4_frame_t is the push's state across its pad:
 * its stripe's guard and its checkpoint window, each while live.
 */
typedef struct txn_identity_changed_closure_4_frame_t
{
	txn_identity_changed_frame_t *changed;
	HostIoIo   *io;
	TxnMemtableTablePreparedBranchDeltas *prepared;
	HostIoStriping striping;
	HostKeyGuard guard;
	bool		guard_live;
	HostDelayCheckpoint window;
	bool		window_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_changed_closure_4_frame_t;

/*
 * txn_identity_changed_closure_4_body takes the tree's stripe, answers false
 * when another writer pushed under it since the caller's count, and else in
 * one checkpoint window appends each of the prepare's group's changes' records
 * and pushes its version with its LSN.
 */
static void
txn_identity_changed_closure_4_body(void *frame_)
{
	txn_identity_changed_closure_4_frame_t *frame = frame_;
	txn_identity_changed_frame_t *changed = frame->changed;
	HostIoKeyIo key_io;
	HostIoTakesShort io;

	if (!host_key_lock(&frame->striping, frame->io, &changed->stripe, 1, &frame->guard, frame->error))
		return;
	frame->guard_live = true;
	key_io = host_KeyGuard_io(&frame->guard);
	io.ops = &HOST_IO_KEY_IO_TAKES_SHORT_OPS;
	io.self = &key_io;
	if (changed->has_count && host_key_count(changed->stripe) != changed->count)
	{
		*frame->result = false;
		frame->ok = true;
		return;
	}
	if (!host_delay_checkpoint(HOST_IO_TAKES_SHORT_IO_DOORS(io), NULL, 0, &frame->window, frame->error))
		return;
	frame->window_live = true;
	for (size_t i = 0; i < frame->prepared->aims_len; i++)
	{
		size_t		at = frame->prepared->first + i;
		const StdVec *body = &STD_VEC_AT(&changed->bodies, StdVec, at);
		TxnMemtableVersionChange change = {.writer = changed->writer,
			.op = &STD_VEC_AT(&changed->ops, TxnMemtableOp, at), .lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}, .has_lsn = true};
		TxnMemtableVersionHandle handle;

		if (!host_wal_insert(HOST_IO_TAKES_SHORT_IO_DOORS(io), HOST_WAL_KIND_DATABASE_CHANGE, body->ptr, body->len,
							 &change.lsn, frame->error) ||
			!txn_memtable_branch_identity_finish(io, frame->prepared, i, &change, true, changed->roomed, &handle,
												 frame->error))
			return;
	}
	frame->window_live = false;
	host_DelayCheckpoint_finish(&frame->window);
	*frame->result = true;
	frame->ok = true;
}

/* txn_identity_changed_closure_4_pad frees what the push holds live: its window and its guard. */
static void
txn_identity_changed_closure_4_pad(void *frame_)
{
	txn_identity_changed_closure_4_frame_t *frame = frame_;

	if (frame->window_live)
		host_DelayCheckpoint_drop(&frame->window);
	if (frame->guard_live)
		host_KeyGuard_drop(&frame->guard);
	frame->window_live = frame->guard_live = false;
}

/* txn_identity_changed_closure_4 is the prepare's closure of a change: its push, or false to start over. */
static bool
txn_identity_changed_closure_4(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
							   HostIoStriping striping, bool *result, HostError *error)
{
	txn_identity_changed_closure_4_frame_t frame = {.changed = captures, .io = io, .prepared = prepared,
	.striping = striping, .result = result, .error = error};

	std_landing(&frame, txn_identity_changed_closure_4_body, txn_identity_changed_closure_4_pad);
	if (frame.window_live && !frame.ok)
		host_DelayCheckpoint_failed(&frame.window, frame.error);
	txn_identity_changed_closure_4_pad(&frame);
	return frame.ok;
}

/*
 * txn_identity_changed_body makes each pair's record, key, and op, then
 * records each key in the transaction's list with its version's need,
 * spilling the transaction's own identity versions past the spill threshold
 * (bugs row 291), and prepares the push.
 */
static void
txn_identity_changed_body(void *frame_)
{
	txn_identity_changed_frame_t *frame = frame_;
	HostStorage storage = txn_spill_identity_storage(frame->repository);
	uint64		top = frame->writer.has_top ? frame->writer.top : frame->writer.xid;
	TxnMemtableVersionShown command = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	size_t		len = frame->pairs.len;

	frame->stripe = txn_identity_stripe(frame->repository);
	std_vec_new(&frame->bodies, TXN_IDENTITY_VEC);
	frame->bodies_live = true;
	if (!std_vec_try_reserve_exact(&frame->bodies, len, TXN_IDENTITY_VEC))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	std_vec_new(&frame->keys, TXN_IDENTITY_VEC);
	frame->keys_live = true;
	std_vec_new(&frame->ops, TXN_IDENTITY_OP);
	frame->ops_live = true;
	if (!std_vec_try_reserve_exact(&frame->keys, len, TXN_IDENTITY_VEC) ||
		!std_vec_try_reserve_exact(&frame->ops, len, TXN_IDENTITY_OP))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	for (; frame->at < len; frame->at++)
	{
		TxnIdentityPair *pair = &STD_VEC_AT(&frame->pairs, TxnIdentityPair, frame->at);
		TxnRecordDatabaseChange change = {.tree = TXN_RECORD_TREE_IDENTITY, .writer = frame->writer,
			.repository = frame->repository, .key = pair->f0, .has_value = pair->has_f1,
		.value = pair->f1};
		StdVec		body;
		TxnMemtableOp op;

		txn_record_DatabaseChange_encode(&change, &body);
		std_vec_push_Checked(&frame->bodies, &body, TXN_IDENTITY_VEC);
		std_vec_push_Checked(&frame->keys, &change.key, TXN_IDENTITY_VEC);
		txn_identity_op(change.has_value, change.value, &op);
		std_vec_push_Checked(&frame->ops, &op, TXN_IDENTITY_OP);
	}
	std_vec_with_capacity_Checked(&frame->needs, len, TXN_IDENTITY_NEED);
	frame->needs_live = true;
	for (size_t i = 0; i < len; i++)
	{
		const StdVec *key = &STD_VEC_AT(&frame->keys, StdVec, i);
		txn_TupleRefSliceU8Usize_t need = {{key->ptr, key->len},
		txn_memtable_version_encoded(&frame->writer, &STD_VEC_AT(&frame->ops, TxnMemtableOp, i), command, true, 0)};
		uint64		touched;

		if (!txn_written_touched(frame->io, top, &storage, TXN_WRITTEN_OF_IDENTITY, key->ptr, key->len, need.f1,
								 &touched, frame->error))
			return;
		if (touched >= txn_spill_threshold())
		{
			bool		spilled;

			if (!txn_spill_spilled(frame->io, &storage, TXN_WRITTEN_OF_IDENTITY, top, &spilled, frame->error))
				return;
		}
		std_vec_push_Checked(&frame->needs, &need, TXN_IDENTITY_NEED);
	}
	frame->ok = txn_memtable_branch_identity_prepare_Bool_Striping(frame->io, host_shared_Held_record(frame->deltas),
																   frame->needs.ptr, frame->needs.len,
																   host_io_Waits_striping(frame->waits),
																   txn_identity_changed_closure_4, frame, NULL,
																   frame->result, frame->error);
}

/* txn_identity_changed_pad frees what changed holds live: its needs, ops, keys, and records, and the untaken pairs. */
static void
txn_identity_changed_pad(void *frame_)
{
	txn_identity_changed_frame_t *frame = frame_;

	if (frame->needs_live)
		std_vec_free_Checked(&frame->needs, TXN_IDENTITY_NEED);
	if (frame->ops_live)
		std_vec_drop_Checked(&frame->ops, TXN_IDENTITY_OP, txn_identity_Op_drop);
	if (frame->keys_live)
		std_vec_drop_Checked(&frame->keys, TXN_IDENTITY_VEC, txn_identity_bytes_drop);
	if (frame->bodies_live)
		std_vec_drop_Checked(&frame->bodies, TXN_IDENTITY_VEC, txn_identity_bytes_drop);
	for (; frame->at < frame->pairs.len; frame->at++)
		txn_identity_Pair_drop(&STD_VEC_AT(&frame->pairs, TxnIdentityPair, frame->at));
	std_vec_free_Checked(&frame->pairs, TXN_IDENTITY_PAIR);
	frame->needs_live = frame->ops_live = frame->keys_live = frame->bodies_live = false;
}

/*
 * txn_identity_changed gives keys of a database's identity tree their
 * values, a removal for none, in the running transaction, taking the pairs:
 * it records each key in the transaction's list, which an abort prunes, then
 * in the window of each of the prepare's groups under the tree's stripe
 * appends each change's record and pushes its version with its LSN (bugs row
 * 338), every pair of a database being
 * logged. With the push count the caller read before its probe, it pushes
 * nothing and answers false when another writer pushed under the stripe
 * since. A change with a count is at most TXN_MEMTABLE_TABLE_WHOLE pairs, one
 * group, since a later group's check would meet the earlier groups' own
 * pushes and start them over.
 */
static bool
txn_identity_changed(HostIoIo *io, HostIoWaits *waits, const HostSharedHeld *deltas, uint32 repository,
					 TxnMemtableVersionWriter writer, TxnStatusRoomed roomed, StdVec *pairs, bool has_count,
					 uint64 count, bool *result, HostError *error)
{
	txn_identity_changed_frame_t frame = {.io = io, .waits = waits, .deltas = deltas, .repository = repository,
		.writer = writer, .roomed = roomed, .pairs = *pairs, .has_count = has_count, .count = count,
	.result = result, .error = error};

	Assert(!has_count || pairs->len <= TXN_MEMTABLE_TABLE_WHOLE);
	std_landing(&frame, txn_identity_changed_body, txn_identity_changed_pad);
	txn_identity_changed_pad(&frame);
	return frame.ok;
}

/*
 * txn_identity_derived_frame_t is derived's state across its pad: its
 * deltas, a probe's hold, a candidate's key, the pairs' keys before they are
 * paired, the pairs before they are listed, and the keyed closure, each
 * while live.
 */
typedef struct txn_identity_derived_frame_t
{
	HostIoIo   *io;
	HostIoWaits *waits;
	TxnIdentityDatabase database;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	TxnIdentityKind kind;
	uint64		parent;
	const uint8 *name;
	size_t		name_len;
	const uint8 *numbered;
	size_t		numbered_len;
	uint32		number;
	TxnIdentityKeyed keyed;
	const void *keyed_captures;
	void		(*keyed_drop) (void *);
	bool		keyed_live;
	HostSharedHeld deltas;
	bool		deltas_live;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		key;
	bool		key_live;
	StdVec		numbered_key;
	bool		numbered_key_live;
	TxnIdentityPaired two[2];
	bool		two_live;
	uint64	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_derived_frame_t;

/*
 * txn_identity_derived_probe reads the object's pair by number under the
 * hold, then for a free one derives the id, a candidate taken when its pair
 * by id is live: it answers the found value, a running writer to wait for, or
 * the derived id.
 */
static bool
txn_identity_derived_probe(txn_identity_derived_frame_t *frame, TxnIdentityFound *found, bool *has_derivation,
						   uint64 *derivation)
{
	HostSharedRecord deltas = host_shared_Held_record(&frame->deltas);
	uint64		candidate;

	*has_derivation = false;
	if (!txn_identity_found(frame->io, &frame->hold, frame->database.f0, frame->database.f1, deltas, frame->numbered,
							frame->numbered_len, found, frame->error))
		return false;
	if (found->tag != TXN_IDENTITY_FOUND_FREE)
		return true;
	if (!txn_identity_derive_object_id(frame->kind, frame->parent, frame->name, frame->name_len, &candidate,
									   frame->error))
		return false;
	for (;;)
	{
		frame->keyed(frame->keyed_captures, candidate, &frame->key);
		frame->key_live = true;
		if (!txn_identity_found(frame->io, &frame->hold, frame->database.f0, frame->database.f1, deltas,
								frame->key.ptr, frame->key.len, found, frame->error))
			return false;
		frame->key_live = false;
		std_vec_free_Checked(&frame->key, TXN_IDENTITY_BYTES);
		switch (found->tag)
		{
			case TXN_IDENTITY_FOUND_VALUE:
				std_vec_free_Checked(&found->u.value, TXN_IDENTITY_BYTES);
				candidate = txn_identity_next_id(candidate);
				break;
			case TXN_IDENTITY_FOUND_FREE:
				*has_derivation = true;
				*derivation = candidate;
				return true;
			case TXN_IDENTITY_FOUND_WAIT:
				return true;
		}
	}
}

/*
 * txn_identity_derived_body probes under a push count of the tree's stripe
 * and a hold, waits for a running writer with no stripe held and starts
 * over, or pushes the derived id's pairs, starting over when another writer
 * pushed under the stripe since the count.
 */
static void
txn_identity_derived_body(void *frame_)
{
	txn_identity_derived_frame_t *frame = frame_;
	uint32		stripe = txn_identity_stripe(frame->database.f2);
	HostOperation none = {0};

	if (!txn_memtable_branch_deltas(frame->io, frame->database.f2, repo_Repo_own(frame->database.f0), &frame->deltas,
									frame->error))
		return;
	frame->deltas_live = true;
	for (;;)
	{
		uint64		count;
		TxnIdentityFound found;
		bool		has_derivation;
		uint64		derivation;
		bool		waited;
		StdVec		keyed;
		StdVec		pairs;
		bool		pushed;

		if (!host_key_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), stripe, frame->error))
			return;
		count = host_key_count(stripe);
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_identity_derived_probe(frame, &found, &has_derivation, &derivation))
			return;
		if (!has_derivation && found.tag == TXN_IDENTITY_FOUND_VALUE)
		{
			frame->ok = txn_identity_id(found.u.value.ptr, found.u.value.len, frame->result, frame->error);
			std_vec_free_Checked(&found.u.value, TXN_IDENTITY_BYTES);
			return;
		}
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
		if (found.tag == TXN_IDENTITY_FOUND_WAIT)
		{
			if (!host_xact_wait(frame->waits, HOST_IO_IO_IO_DOORS(frame->io), found.u.wait, false, none, true,
								&waited, frame->error))
				return;
			continue;
		}
		if (!has_derivation)
			continue;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), TXN_IDENTITY_PROBED, frame->error))
			return;
		txn_identity_bytes(frame->numbered, frame->numbered_len, &frame->numbered_key);
		frame->numbered_key_live = true;
		frame->keyed(frame->keyed_captures, derivation, &keyed);
		frame->numbered_key_live = false;
		txn_identity_paired(frame->numbered_key, keyed, derivation, frame->number, frame->two);
		frame->two_live = true;
		std_vec_with_capacity_Checked(&pairs, lengthof(frame->two), TXN_IDENTITY_PAIR);
		for (int i = 0; i < lengthof(frame->two); i++)
		{
			TxnIdentityPair pair = {.f0 = frame->two[i].f0, .has_f1 = true, .f1 = frame->two[i].f1};

			std_vec_push_Checked(&pairs, &pair, TXN_IDENTITY_PAIR);
		}
		frame->two_live = false;
		if (!txn_identity_changed(frame->io, frame->waits, &frame->deltas, frame->database.f2, frame->writer,
								  frame->roomed, &pairs, true, count, &pushed, frame->error))
			return;
		if (pushed)
		{
			*frame->result = derivation;
			frame->ok = true;
			return;
		}
	}
}

/*
 * txn_identity_derived_pad frees what derived holds live: a push's pairs and
 * keys, a probe's key and hold, its deltas, and keyed.
 */
static void
txn_identity_derived_pad(void *frame_)
{
	txn_identity_derived_frame_t *frame = frame_;

	if (frame->two_live)
		for (int i = 0; i < lengthof(frame->two); i++)
			txn_identity_Paired_drop(&frame->two[i]);
	if (frame->numbered_key_live)
		std_vec_free_Checked(&frame->numbered_key, TXN_IDENTITY_BYTES);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, TXN_IDENTITY_BYTES);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	if (frame->keyed_live && frame->keyed_drop != NULL)
		frame->keyed_drop((void *) frame->keyed_captures);
	frame->two_live = frame->numbered_key_live = frame->key_live = frame->hold_live = frame->deltas_live = false;
	frame->keyed_live = false;
}

/*
 * txn_identity_derived records a new object's pairs in the running
 * transaction and returns its id, or returns the id its pair by number
 * already holds, its pair by id holding the number as a varint: under a push
 * count of the tree's stripe read first, it reads the pair by number, then
 * derives the id, a candidate taken when its pair by id is live, and pushes
 * both pairs under the stripe, starting over when another writer pushed
 * under it since the count. A running change of a key it reads is waited
 * for, with no stripe held, and the derivation starts over. keyed gives the
 * key of an id's pair by id; derived takes it.
 */
bool
txn_identity_derived(HostIoIo *io, HostIoWaits *waits, TxnIdentityDatabase database, TxnMemtableVersionWriter writer,
					 TxnStatusRoomed roomed, TxnIdentityKind kind, uint64 parent, const uint8 *name, size_t name_len,
					 const uint8 *numbered, size_t numbered_len, uint32 number, TxnIdentityKeyed keyed,
					 const void *keyed_captures, void (*keyed_drop) (void *), uint64 *result, HostError *error)
{
	txn_identity_derived_frame_t frame = {.io = io, .waits = waits, .database = database, .writer = writer,
		.roomed = roomed, .kind = kind, .parent = parent, .name = name, .name_len = name_len, .numbered = numbered,
		.numbered_len = numbered_len, .number = number, .keyed = keyed, .keyed_captures = keyed_captures,
	.keyed_drop = keyed_drop, .keyed_live = true, .result = result, .error = error};

	std_landing(&frame, txn_identity_derived_body, txn_identity_derived_pad);
	txn_identity_derived_pad(&frame);
	return frame.ok;
}

/* txn_identity_removed_frame_t is removed's state across its pad: the keys it takes and its deltas, each while live. */
typedef struct txn_identity_removed_frame_t
{
	HostIoIo   *io;
	HostIoWaits *waits;
	uint32		repository;
	uint32		own;
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
	StdVec		keys;
	bool		keys_live;
	HostSharedHeld deltas;
	bool		deltas_live;
	HostError  *error;
	bool		ok;
} txn_identity_removed_frame_t;

/* txn_identity_removed_body makes each key a removal and changes the tree. */
static void
txn_identity_removed_body(void *frame_)
{
	txn_identity_removed_frame_t *frame = frame_;
	StdVec		pairs;
	bool		pushed;

	if (!txn_memtable_branch_deltas(frame->io, frame->repository, frame->own, &frame->deltas, frame->error))
		return;
	frame->deltas_live = true;
	std_vec_with_capacity_Checked(&pairs, frame->keys.len, TXN_IDENTITY_PAIR);
	for (size_t i = 0; i < frame->keys.len; i++)
	{
		TxnIdentityPair pair = {.f0 = STD_VEC_AT(&frame->keys, StdVec, i)};

		std_vec_push_Checked(&pairs, &pair, TXN_IDENTITY_PAIR);
	}
	std_vec_free_Checked(&frame->keys, TXN_IDENTITY_VEC);
	frame->keys_live = false;
	frame->ok = txn_identity_changed(frame->io, frame->waits, &frame->deltas, frame->repository, frame->writer,
									 frame->roomed, &pairs, false, 0, &pushed, frame->error);
}

/* txn_identity_removed_pad frees what removed holds live: its deltas and the keys. */
static void
txn_identity_removed_pad(void *frame_)
{
	txn_identity_removed_frame_t *frame = frame_;

	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	if (frame->keys_live)
		std_vec_drop_Checked(&frame->keys, TXN_IDENTITY_VEC, txn_identity_bytes_drop);
	frame->deltas_live = frame->keys_live = false;
}

/*
 * txn_identity_removed removes keys of a database's identity tree in the
 * running transaction, a dropped object's pairs, under the tree's stripe,
 * taking the keys.
 */
bool
txn_identity_removed(HostIoIo *io, HostIoWaits *waits, uint32 repository, uint32 own, TxnMemtableVersionWriter writer,
					 TxnStatusRoomed roomed, StdVec *keys, HostError *error)
{
	txn_identity_removed_frame_t frame = {.io = io, .waits = waits, .repository = repository, .own = own,
	.writer = writer, .roomed = roomed, .keys = *keys, .keys_live = true, .error = error};

	std_landing(&frame, txn_identity_removed_body, txn_identity_removed_pad);
	txn_identity_removed_pad(&frame);
	return frame.ok;
}

/* txn_identity_folded_closure_0_captures_t is folded's walk's captures: the deltas, the fold point, the pairs, and the keys the walk read, in order. */
typedef struct txn_identity_folded_closure_0_captures_t
{
	HostSharedRecord deltas;
	HostLsn		point;
	StdVec	   *pairs;
	StdVec	   *walked;
} txn_identity_folded_closure_0_captures_t;

/*
 * txn_identity_folded_key adds a key's change of its newest version, from
 * at, that the fold applies, if any, dropping each version it passes over.
 */
static bool
txn_identity_folded_key(HostIoIo *io, HostLsn point, const uint8 *key, size_t key_len, StdVec *versions, size_t *at,
						StdVec *pairs, HostError *error)
{
	for (; *at < versions->len; (*at)++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(versions, TxnMemtableVersionVersion, *at);
		TxnFoldApplied applied;
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		TxnIdentityPair pair = {0};

		if (!txn_fold_applied(io, version, point, &applied, error))
			return false;
		if (applied.tag != TXN_FOLD_APPLIED_YES)
		{
			txn_memtable_version_Version_drop(version);
			continue;
		}
		record = txn_memtable_version_Version_record(version);
		op = txn_memtable_version_Record_op(&record);
		if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_PUT && op.tag != TXN_MEMTABLE_VERSION_OP_REF_DELETE)
		{
			*error = txn_identity_fork("XX001", "an identity tree's version neither puts nor deletes");
			return false;
		}
		if (!std_vec_try_reserve(pairs, 1, TXN_IDENTITY_PAIR))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		pair.has_f1 = op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT;
		if (pair.has_f1)
			txn_identity_bytes(op.u.put.value.ptr, op.u.put.value.len, &pair.f1);
		txn_identity_bytes(key, key_len, &pair.f0);
		std_vec_push_Checked(pairs, &pair, TXN_IDENTITY_PAIR);
		return true;
	}
	return true;
}

/*
 * txn_identity_spilled_ordered puts a key's versions only spills hold, as
 * the spills' next key gives them, in commit order (bugs row 397).
 */
static bool
txn_identity_spilled_ordered(HostIoIo *io, StdVec *versions, HostError *error)
{
	StdVec		ordered;

	std_vec_new(&ordered, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (!txn_spill_ordered(io, &ordered, versions, error))
	{
		std_vec_extend_from_slice_Checked(versions, ordered.ptr, ordered.len, sizeof(TxnMemtableVersionVersion),
										  _Alignof(TxnMemtableVersionVersion));
		std_vec_free_Checked(&ordered, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		return false;
	}
	std_vec_free_Checked(versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	*versions = ordered;
	return true;
}

/*
 * txn_identity_folded_closure_0_frame_t is the walk's state across its pad:
 * a key's hold, its versions from the first it hasn't dropped, and its
 * spilled versions before they join them, each while live.
 */
typedef struct txn_identity_folded_closure_0_frame_t
{
	txn_identity_folded_closure_0_captures_t *walk;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	size_t		at;
	bool		versions_live;
	StdVec		spilled;
	bool		spilled_live;
	HostError  *error;
	bool		ok;
} txn_identity_folded_closure_0_frame_t;

/* txn_identity_folded_closure_0_dropped drops the key's versions a read gave. */
static void
txn_identity_folded_closure_0_dropped(void *captures)
{
	txn_identity_folded_closure_0_frame_t *frame = captures;

	if (frame->spilled_live)
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->spilled_live = frame->versions_live = false;
}

/* txn_identity_folded_closure_0_read reads the key's memtable versions, then its spills'. */
static bool
txn_identity_folded_closure_0_read(void *captures, uint64 *listed, HostError *error)
{
	txn_identity_folded_closure_0_frame_t *frame = captures;

	if (!txn_memtable_branch_identity_versions(frame->io, &frame->hold, frame->walk->deltas, frame->key,
											   frame->key_len, &frame->versions, error))
		return false;
	frame->versions_live = true;
	if (!txn_spill_versions(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, &frame->hold, frame->walk->deltas,
							TXN_RUNS_KIND_SPILLED_IDENTITY, frame->key, frame->key_len, listed, &frame->spilled,
							error))
		return false;
	frame->spilled_live = true;
	return true;
}

/* txn_identity_folded_closure_0_body reads the key's memtable versions with its spills' placed among them in commit order (bugs row 397), keeps the key as walked, and adds its change. */
static void
txn_identity_folded_closure_0_body(void *frame_)
{
	txn_identity_folded_closure_0_frame_t *frame = frame_;
	txn_identity_folded_closure_0_captures_t *walk = frame->walk;
	StdVec		key;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_spill_taken(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, walk->deltas, txn_identity_folded_closure_0_read,
						 txn_identity_folded_closure_0_dropped, frame, frame->error) ||
		!txn_spill_ordered(frame->io, &frame->versions, &frame->spilled, frame->error))
		return;
	frame->spilled_live = false;
	std_vec_free_Checked(&frame->spilled, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	if (!std_vec_try_reserve(walk->walked, 1, TXN_IDENTITY_VEC))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	txn_identity_bytes(frame->key, frame->key_len, &key);
	std_vec_push_Checked(walk->walked, &key, TXN_IDENTITY_VEC);
	frame->ok = txn_identity_folded_key(frame->io, walk->point, frame->key, frame->key_len, &frame->versions,
										&frame->at, walk->pairs, frame->error);
}

/* txn_identity_folded_closure_0_pad frees what the walk holds live: its versions and its hold. */
static void
txn_identity_folded_closure_0_pad(void *frame_)
{
	txn_identity_folded_closure_0_frame_t *frame = frame_;

	if (frame->spilled_live)
		txn_memtable_version_versions_drop(&frame->spilled, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, frame->at);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->spilled_live = frame->versions_live = frame->hold_live = false;
}

/* txn_identity_folded_closure_0 is folded's walk's closure, of one key of the delta. */
static bool
txn_identity_folded_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							  size_t values_len, HostError *error)
{
	txn_identity_folded_closure_0_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_identity_folded_closure_0_body, txn_identity_folded_closure_0_pad);
	txn_identity_folded_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * txn_identity_folded_frame_t is folded's state across its pad: its deltas,
 * its pairs, the keys its walk read, and the spills' next key, its versions,
 * and its hold, each while live.
 */
typedef struct txn_identity_folded_frame_t
{
	HostIoIo   *io;
	uint32		repository;
	HostLsn		point;
	HostSharedHeld deltas;
	bool		deltas_live;
	StdVec		pairs;
	bool		pairs_live;
	StdVec		walked;
	bool		walked_live;
	HostDeltaHold hold;
	bool		hold_live;
	bool		has_after;
	StdVec		after;
	txn_TupleVecU8VecVersion_t next;
	size_t		at;
	bool		next_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_identity_folded_frame_t;

/* txn_identity_folded_walked_cmp orders a key among the walked keys by its bytes. */
static int
txn_identity_folded_walked_cmp(const void *a, const void *b)
{
	const StdVec *x = a;
	const StdVec *y = b;

	return std_bytes_Ord_cmp(x->ptr, x->len, y->ptr, y->len);
}

/*
 * txn_identity_folded_body walks the tree's delta, if the database has one,
 * for each key's folded change, and then, when a listed identity spill's
 * transaction is one the point applies, the keys its identity spills alone
 * hold (bugs row 291).
 */
static void
txn_identity_folded_body(void *frame_)
{
	txn_identity_folded_frame_t *frame = frame_;
	txn_identity_folded_closure_0_captures_t walk = {.point = frame->point, .pairs = &frame->pairs,
	.walked = &frame->walked};
	bool		some;
	bool		applies;

	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &some, &frame->deltas,
								  frame->error))
		return;
	std_vec_new(&frame->pairs, TXN_IDENTITY_PAIR);
	if (!some)
	{
		*frame->result = frame->pairs;
		frame->ok = true;
		return;
	}
	frame->deltas_live = true;
	frame->pairs_live = true;
	std_vec_new(&frame->walked, TXN_IDENTITY_VEC);
	frame->walked_live = true;
	walk.deltas = host_shared_Held_record(&frame->deltas);
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, walk.deltas,
								   txn_identity_folded_closure_0, &walk, NULL, frame->error))
		return;
	if (!txn_spill_identity_applies(frame->io, walk.deltas, frame->point, &applies, frame->error))
		return;
	qsort(frame->walked.ptr, frame->walked.len, sizeof(StdVec), txn_identity_folded_walked_cmp);
	while (applies)
	{
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_spill_next(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, &frame->hold, walk.deltas,
							TXN_RUNS_KIND_SPILLED_IDENTITY, frame->has_after, frame->after.ptr, frame->after.len,
							TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, NULL, &some, &frame->next, frame->error))
			return;
		if (!some)
			break;
		frame->next_live = true;
		frame->at = 0;
		if (!txn_identity_spilled_ordered(frame->io, &frame->next.f1, frame->error))
			return;
		if (bsearch(&frame->next.f0, frame->walked.ptr, frame->walked.len, sizeof(StdVec),
					txn_identity_folded_walked_cmp) == NULL &&
			!txn_identity_folded_key(frame->io, frame->point, frame->next.f0.ptr, frame->next.f0.len,
									 &frame->next.f1, &frame->at, &frame->pairs, frame->error))
			return;
		if (frame->has_after)
			std_vec_free_Checked(&frame->after, TXN_IDENTITY_BYTES);
		frame->after = frame->next.f0;
		frame->has_after = true;
		txn_memtable_version_versions_drop(&frame->next.f1, frame->at);
		frame->next_live = false;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	*frame->result = frame->pairs;
	frame->pairs_live = false;
	frame->ok = true;
}

/* txn_identity_folded_pad frees what folded holds live: the spills' next key and its hold, the walked keys, its pairs, and its deltas. */
static void
txn_identity_folded_pad(void *frame_)
{
	txn_identity_folded_frame_t *frame = frame_;

	if (frame->next_live)
	{
		txn_memtable_version_versions_drop(&frame->next.f1, frame->at);
		std_vec_free_Checked(&frame->next.f0, TXN_IDENTITY_BYTES);
	}
	if (frame->has_after)
		std_vec_free_Checked(&frame->after, TXN_IDENTITY_BYTES);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->walked_live)
		std_vec_drop_Checked(&frame->walked, TXN_IDENTITY_VEC, txn_identity_bytes_drop);
	if (frame->pairs_live)
		std_vec_drop_Checked(&frame->pairs, TXN_IDENTITY_PAIR, txn_identity_Pair_drop);
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	frame->next_live = frame->has_after = frame->hold_live = frame->walked_live = frame->pairs_live =
		frame->deltas_live = false;
}

/*
 * txn_identity_folded returns what a save at a fold point folds of a
 * database's identity tree: for each key of the tree's delta and its spills,
 * the change of its newest version the fold applies, as the key's value or
 * none for its removal, which the save makes only where the folded tree holds
 * another.
 */
bool
txn_identity_folded(HostIoIo *io, uint32 repository, HostLsn point, StdVec *result, HostError *error)
{
	txn_identity_folded_frame_t frame = {.io = io, .repository = repository, .point = point, .result = result,
	.error = error};

	std_landing(&frame, txn_identity_folded_body, txn_identity_folded_pad);
	txn_identity_folded_pad(&frame);
	return frame.ok;
}

/* txn_identity_folded_drop frees a key's folded versions held as a vector's element. */
static void
txn_identity_folded_drop(void *folded_)
{
	txn_TupleVecU8VecHandle_t *folded = folded_;

	std_vec_free_Checked(&folded->f0, TXN_IDENTITY_BYTES);
	std_vec_free_Checked(&folded->f1, TXN_IDENTITY_HANDLE);
}

/*
 * txn_identity_dropped_closure_0_captures_t is dropped's walk's captures: the
 * deltas, the fold point, and the keys it folded.
 */
typedef struct txn_identity_dropped_closure_0_captures_t
{
	HostSharedRecord deltas;
	HostLsn		point;
	StdVec	   *folded;
} txn_identity_dropped_closure_0_captures_t;

/*
 * txn_identity_dropped_closure_0_frame_t is the walk's state across its pad:
 * a key's hold, its versions from the first it hasn't dropped, and its
 * folded ones, each while live.
 */
typedef struct txn_identity_dropped_closure_0_frame_t
{
	txn_identity_dropped_closure_0_captures_t *walk;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	size_t		at;
	bool		versions_live;
	StdVec		applied;
	bool		applied_live;
	HostError  *error;
	bool		ok;
} txn_identity_dropped_closure_0_frame_t;

/*
 * txn_identity_dropped_closure_0_body lists the key's versions the fold
 * applied at the point, with their top-level xids.
 */
static void
txn_identity_dropped_closure_0_body(void *frame_)
{
	txn_identity_dropped_closure_0_frame_t *frame = frame_;
	txn_identity_dropped_closure_0_captures_t *walk = frame->walk;
	txn_TupleVecU8VecHandle_t folded;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	std_vec_new(&frame->applied, TXN_IDENTITY_HANDLE);
	frame->applied_live = true;
	if (!txn_memtable_branch_identity_versions(frame->io, &frame->hold, walk->deltas, frame->key, frame->key_len,
											   &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	for (; frame->at < frame->versions.len; frame->at++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, frame->at);
		TxnFoldApplied applied;

		if (!txn_fold_applied(frame->io, version, walk->point, &applied, frame->error))
			return;
		if (applied.tag == TXN_FOLD_APPLIED_YES)
		{
			if (!std_vec_try_reserve(&frame->applied, 1, TXN_IDENTITY_HANDLE))
			{
				*frame->error = host_Error_out_of_memory();
				return;
			}
			std_vec_push_Checked(&frame->applied, &version->handle, TXN_IDENTITY_HANDLE);
		}
		txn_memtable_version_Version_drop(version);
	}
	if (frame->applied.len > 0)
	{
		if (!std_vec_try_reserve(walk->folded, 1, TXN_IDENTITY_FOLDED))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		txn_identity_bytes(frame->key, frame->key_len, &folded.f0);
		folded.f1 = frame->applied;
		std_vec_push_Checked(walk->folded, &folded, TXN_IDENTITY_FOLDED);
		frame->applied_live = false;
	}
	frame->ok = true;
}

/* txn_identity_dropped_closure_0_pad frees what the walk holds live: its versions, its folded ones, and its hold. */
static void
txn_identity_dropped_closure_0_pad(void *frame_)
{
	txn_identity_dropped_closure_0_frame_t *frame = frame_;

	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, frame->at);
	if (frame->applied_live)
		std_vec_free_Checked(&frame->applied, TXN_IDENTITY_HANDLE);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->versions_live = frame->applied_live = frame->hold_live = false;
}

/* txn_identity_dropped_closure_0 is dropped's walk's closure, of one key of the delta. */
static bool
txn_identity_dropped_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							   size_t values_len, HostError *error)
{
	txn_identity_dropped_closure_0_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_identity_dropped_closure_0_body, txn_identity_dropped_closure_0_pad);
	txn_identity_dropped_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * txn_identity_dropped_frame_t is dropped's state across its pad: its
 * deltas, the folded keys from the first it hasn't taken out, and a key's
 * handles and what their removal took, each while live.
 */
typedef struct txn_identity_dropped_frame_t
{
	HostIoIo   *io;
	uint32		repository;
	HostLsn		point;
	HostSharedHeld deltas;
	bool		deltas_live;
	StdVec		folded;
	size_t		at;
	bool		folded_live;
	bool	   *took;
	HostError  *error;
	bool		ok;
} txn_identity_dropped_frame_t;

/*
 * txn_identity_dropped_body lists each key's versions the fold applied, then
 * takes each key's out of its chain, the remover giving back each counted
 * one's count at its top-level transaction's entry.
 */
static void
txn_identity_dropped_body(void *frame_)
{
	txn_identity_dropped_frame_t *frame = frame_;
	txn_identity_dropped_closure_0_captures_t walk = {.point = frame->point, .folded = &frame->folded};
	bool		some;
	bool		files;

	*frame->took = false;
	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(frame->io), frame->repository, &some, &frame->deltas,
								  frame->error))
		return;
	if (!some)
	{
		frame->ok = true;
		return;
	}
	frame->deltas_live = true;
	std_vec_new(&frame->folded, TXN_IDENTITY_FOLDED);
	frame->folded_live = true;
	walk.deltas = host_shared_Held_record(&frame->deltas);
	if (!txn_memtable_table_walked(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, walk.deltas,
								   txn_identity_dropped_closure_0, &walk, NULL, frame->error))
		return;
	*frame->took = frame->folded.len > 0;
	for (; frame->at < frame->folded.len; frame->at++)
	{
		txn_TupleVecU8VecHandle_t *key = &STD_VEC_AT(&frame->folded, txn_TupleVecU8VecHandle_t, frame->at);
		StdVec		taken;

		if (!txn_memtable_table_removed(&TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS, frame->io, walk.deltas, key->f0.ptr,
										key->f0.len, key->f1.ptr, key->f1.len, &taken, frame->error))
			return;
		std_vec_free_Checked(&taken, sizeof(bool), _Alignof(bool));
		txn_identity_folded_drop(key);
	}
	frame->ok = txn_spill_identity_folded(frame->io, walk.deltas, frame->point, &files, frame->error);
	*frame->took = *frame->took || files;
}

/* txn_identity_dropped_pad frees what dropped holds live: the folded keys left and its deltas. */
static void
txn_identity_dropped_pad(void *frame_)
{
	txn_identity_dropped_frame_t *frame = frame_;

	if (frame->folded_live)
	{
		for (; frame->at < frame->folded.len; frame->at++)
			txn_identity_folded_drop(&STD_VEC_AT(&frame->folded, txn_TupleVecU8VecHandle_t, frame->at));
		std_vec_free_Checked(&frame->folded, TXN_IDENTITY_FOLDED);
	}
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	frame->folded_live = frame->deltas_live = false;
}

/*
 * txn_identity_dropped takes out of a database's identity tree's delta, once
 * a save at a fold point has made its manifest durable, every version that
 * save folded, each one folded finds applied at the point, and gives each
 * one's count back at its top-level transaction's entry, so the folded pairs
 * live in the manifest's tree alone. A reader reads the delta and then the
 * newest manifest (found), so no reader tells the two apart once the save is
 * durable, and no horizon is waited for. It answers whether it took a
 * version or an identity spill out (bugs row 340).
 */
bool
txn_identity_dropped(HostIoIo *io, uint32 repository, HostLsn point, bool *took, HostError *error)
{
	txn_identity_dropped_frame_t frame = {.io = io, .repository = repository, .point = point, .took = took,
	.error = error};

	std_landing(&frame, txn_identity_dropped_body, txn_identity_dropped_pad);
	txn_identity_dropped_pad(&frame);
	return frame.ok;
}

/*
 * txn_identity_redo_closure_0_captures_t is a redo's prepare's closure's
 * captures: the version it pushes and its slot.
 */
typedef struct txn_identity_redo_closure_0_captures_t
{
	const TxnMemtableVersionChange *pushed;
	bool		has_roomed;
	TxnStatusRoomed roomed;
} txn_identity_redo_closure_0_captures_t;

/* txn_identity_redo_closure_0 is a redo's prepare's closure: it pushes the replayed version. */
static bool
txn_identity_redo_closure_0(void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared,
							TxnMemtableVersionHandle *result, HostError *error)
{
	txn_identity_redo_closure_0_captures_t *inside = captures;

	return txn_memtable_branch_identity_finish(HOST_IO_IO_TAKES_SHORT(io), prepared, 0, inside->pushed,
											   inside->has_roomed, inside->roomed, result, error);
}

/*
 * txn_identity_redo_frame_t is redo's state across its pad: the change it
 * takes, its deltas, and the op made of the change's value, each while live.
 */
typedef struct txn_identity_redo_frame_t
{
	HostIoIo   *io;
	TxnRecordDatabaseChange change;
	bool		value_live;
	uint32		own;
	uint64		lsn;
	HostSharedHeld deltas;
	bool		deltas_live;
	TxnMemtableOp op;
	bool		op_live;
	HostError  *error;
	bool		ok;
} txn_identity_redo_frame_t;

/*
 * txn_identity_redo_body pushes the replayed change's version unless the
 * delta, its spills included, holds it already, counting it toward its
 * transaction's replayed bytes, which spill past the threshold (bugs row
 * 291).
 */
static void
txn_identity_redo_body(void *frame_)
{
	txn_identity_redo_frame_t *frame = frame_;
	TxnRecordDatabaseChange *change = &frame->change;
	TxnMemtableVersionShown replayed = {.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED};
	HostSharedRecord deltas;
	bool		holds;
	txn_identity_redo_closure_0_captures_t inside;
	TxnMemtableVersionChange pushed;
	txn_TupleRefSliceU8Usize_t needs;
	TxnMemtableVersionHandle handle;
	HostStorage storage = txn_spill_identity_storage(change->repository);
	uint64		top = change->writer.has_top ? change->writer.top : change->writer.xid;
	uint64		counted;
	bool		spilled;

	if (!txn_memtable_branch_deltas(frame->io, change->repository, frame->own, &frame->deltas, frame->error))
		return;
	frame->deltas_live = true;
	deltas = host_shared_Held_record(&frame->deltas);
	if (!txn_memtable_branch_identity_holds(frame->io, deltas, change->key.ptr, change->key.len, frame->lsn, &holds,
											frame->error) ||
		(!holds && !txn_spill_holds(&TXN_SPILL_IDENTITY_HOME_OPS, frame->io, deltas, TXN_RUNS_KIND_SPILLED_IDENTITY,
									change->key.ptr, change->key.len, frame->lsn, &holds, frame->error)))
		return;
	if (holds)
	{
		frame->ok = true;
		return;
	}
	if (!txn_status_redo_roomed(frame->io, &change->writer, &inside.has_roomed, &inside.roomed, frame->error))
		return;
	frame->value_live = false;
	txn_identity_op(change->has_value, change->value, &frame->op);
	frame->op_live = true;
	needs.f0.ptr = change->key.ptr;
	needs.f0.len = change->key.len;
	needs.f1 = txn_memtable_version_encoded(&change->writer, &frame->op, replayed, true, frame->lsn);
	if (!txn_spill_replayed(frame->io, &storage, TXN_WRITTEN_OF_IDENTITY, top, needs.f1, &counted, frame->error) ||
		(counted >= txn_spill_threshold() &&
		 !txn_spill_spilled(frame->io, &storage, TXN_WRITTEN_OF_IDENTITY, top, &spilled, frame->error)))
		return;
	pushed.writer = change->writer;
	pushed.op = &frame->op;
	pushed.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE;
	pushed.shown = replayed;
	pushed.has_lsn = true;
	pushed.lsn = frame->lsn;
	inside.pushed = &pushed;
	frame->ok = txn_memtable_branch_identity_prepare_Handle_Unit(frame->io, deltas, &needs, 1,
																 txn_identity_redo_closure_0, &inside, NULL, &handle,
																 frame->error);
}

/* txn_identity_redo_pad frees what redo holds live: its op, its deltas, and the change. */
static void
txn_identity_redo_pad(void *frame_)
{
	txn_identity_redo_frame_t *frame = frame_;

	if (frame->op_live)
		txn_memtable_Op_drop(&frame->op);
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	if (frame->value_live)
		std_vec_free_Checked(&frame->change.value, TXN_IDENTITY_BYTES);
	std_vec_free_Checked(&frame->change.key, TXN_IDENTITY_BYTES);
	frame->op_live = frame->deltas_live = frame->value_live = false;
}

/*
 * txn_identity_redo pushes a replayed identity change into its database's
 * working state's delta, taking it, in the startup process or on a standby,
 * with no catalog access, unless the checkpoint's runs brought it back,
 * which the delta or its spills hold by its key and the record's LSN.
 */
bool
txn_identity_redo(HostIoIo *io, TxnRecordDatabaseChange *change, uint32 own, uint64 lsn, HostError *error)
{
	txn_identity_redo_frame_t frame = {.io = io, .change = *change, .value_live = change->has_value, .own = own,
	.lsn = lsn, .error = error};

	std_landing(&frame, txn_identity_redo_body, txn_identity_redo_pad);
	txn_identity_redo_pad(&frame);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_TXN
/* TXN_IDENTITY_TESTS_UNWRAP is Result::unwrap's panic text. */
#define TXN_IDENTITY_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* TXN_IDENTITY_TESTS_SCHEMA is the parent id the tests' tables derive their ids under. */
#define TXN_IDENTITY_TESTS_SCHEMA (TXN_IDENTITY_OBJECT_ID_BIT | 5)

/* txn_identity_tests_ok unwraps a host result at a line. */
static void
txn_identity_tests_ok(bool ok, HostError *error, uint32 line)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, 0, TXN_IDENTITY_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* txn_identity_tests_derived returns an object's derived id, which unwraps at a line. */
static uint64
txn_identity_tests_derived(TxnIdentityKind kind, uint64 parent, const char *name, size_t name_len, uint32 line)
{
	uint64		id;
	HostError	error;

	txn_identity_tests_ok(txn_identity_derive_object_id(kind, parent, (const uint8 *) name, name_len, &id, &error),
						  &error, line);
	return id;
}

/* txn_identity_tests_eq reports whether a key is the concatenation of a prefix and a big-endian number's bytes. */
static bool
txn_identity_tests_eq(const StdVec *key, const uint8 *prefix, size_t prefix_len, uint64 number, size_t number_len)
{
	if (key->len != prefix_len + number_len || memcmp(key->ptr, prefix, prefix_len) != 0)
		return false;
	for (size_t k = 0; k < number_len; k++)
		if (((const uint8 *) key->ptr)[prefix_len + k] != (uint8) (number >> (8 * (number_len - 1 - k))))
			return false;
	return true;
}

/* txn_identity_tests_starts reports whether a key starts with a prefix's bytes. */
static bool
txn_identity_tests_starts(const StdVec *key, const StdVec *prefix)
{
	return key->len >= prefix->len && memcmp(key->ptr, prefix->ptr, prefix->len) == 0;
}

/*
 * test_txn_identity_tests_an_objects_id_derives_from_its_kind_parent_and_name:
 * §II.12's derivation, the same kind, parent, and name give the same id, with
 * the user bit set; another name or parent gives another; a taken id moves
 * to the next free one.
 */
void
test_txn_identity_tests_an_objects_id_derives_from_its_kind_parent_and_name(void)
{
	uint64		schema = txn_identity_tests_derived(TXN_IDENTITY_KIND_SCHEMA, 0, "public", 6, __LINE__);
	uint64		table = txn_identity_tests_derived(TXN_IDENTITY_KIND_RELATION, schema, "t", 1, __LINE__);

	STD_ASSERT_MSG(__LINE__, 9, (schema & TXN_IDENTITY_OBJECT_ID_BIT) == TXN_IDENTITY_OBJECT_ID_BIT,
				   "schema & OBJECT_ID_BIT == OBJECT_ID_BIT");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_derived(TXN_IDENTITY_KIND_SCHEMA, 0, "public", 6, __LINE__) == schema,
				   "the same derivation gives the same id");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_derived(TXN_IDENTITY_KIND_RELATION, schema, "u", 1, __LINE__) !=
				   table, "another name gives another id");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_derived(TXN_IDENTITY_KIND_RELATION, table, "t", 1, __LINE__) !=
				   table, "another parent gives another id");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_next_id(table) == ((table + 1) | TXN_IDENTITY_OBJECT_ID_BIT),
				   "next_id(table) == (table + 1) | OBJECT_ID_BIT");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_next_id(PG_UINT64_MAX) == TXN_IDENTITY_OBJECT_ID_BIT,
				   "next_id(u64::MAX) == OBJECT_ID_BIT");
}

/* txn_identity_tests_column returns a column's derived id under parent 7, by its name and category. */
static uint64
txn_identity_tests_column(uint8 category)
{
	StdVec		name;
	uint64		id;

	txn_identity_column_name((const uint8 *) "a", 1, category, &name);
	id = txn_identity_tests_derived(TXN_IDENTITY_KIND_COLUMN, 7, name.ptr, name.len, __LINE__);
	std_vec_free_Checked(&name, 1, 1);
	return id;
}

/*
 * test_txn_identity_tests_a_columns_id_holds_its_types_category: a column's
 * name for its id holds its type's category, so the same name with another
 * category is another column, and the same name and category the same
 * column.
 */
void
test_txn_identity_tests_a_columns_id_holds_its_types_category(void)
{
	uint64 numeric = txn_identity_tests_column('N');
	uint64 text = txn_identity_tests_column('S');

	STD_ASSERT_MSG(__LINE__, 9, numeric !=text, "int != text");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_column('N') == numeric, "the same name and category, the same id");
}

/*
 * test_txn_identity_tests_a_pairs_keys_find_it_from_either_side: the two key
 * forms of a pair, and a table's columns' prefix, which every column key of
 * the table starts with.
 */
void
test_txn_identity_tests_a_pairs_keys_find_it_from_either_side(void)
{
	static const uint8 by_id_prefix[] = {1, 2};
	static const uint8 by_oid_prefix[] = {2, 2};
	static const uint8 column_prefix[] = {2, 3, 0, 0, 0, 0, 0, 0, 0, 9};
	StdVec		key;
	StdVec		column;
	StdVec		prefix;

	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, 5, &key);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_eq(&key, by_id_prefix, 2, 5, 8),
				   "by_id(Kind::Relation, 5) == [1, 2] ++ 5u64.to_be_bytes()");
	std_vec_free_Checked(&key, 1, 1);
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_eq(&key, by_oid_prefix, 2, 16384, 4),
				   "by_oid(Kind::Relation, 16384) == [2, 2] ++ 16384u32.to_be_bytes()");
	std_vec_free_Checked(&key, 1, 1);
	txn_identity_column_by_number(9, 3, &column);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_eq(&column, column_prefix, sizeof(column_prefix), 3, 2),
				   "column_by_number(9, 3) == [2, 3] ++ 9u64.to_be_bytes() ++ 3u16.to_be_bytes()");
	txn_identity_columns(9, false, &prefix);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_starts(&column, &prefix), "column.starts_with(&columns(9, false))");
	std_vec_free_Checked(&prefix, 1, 1);
	std_vec_free_Checked(&column, 1, 1);
	txn_identity_column_by_id(9, 11, &column);
	txn_identity_columns(9, true, &prefix);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_starts(&column, &prefix),
				   "column_by_id(9, 11).starts_with(&columns(9, true))");
	std_vec_free_Checked(&prefix, 1, 1);
	std_vec_free_Checked(&column, 1, 1);
}

/* txn_identity_tests_writing_t is writing's (Writer, Roomed): the running transaction's writer and its entry's room. */
typedef struct txn_identity_tests_writing_t
{
	TxnMemtableVersionWriter writer;
	TxnStatusRoomed roomed;
} txn_identity_tests_writing_t;

/* txn_identity_tests_writing returns the running transaction's writer at its current command, with its entry's room taken. */
static txn_identity_tests_writing_t
txn_identity_tests_writing(HostIoIo *io)
{
	txn_identity_tests_writing_t writing = {0};
	HostXid		sub;
	HostXid		top;
	HostError	error;

	txn_identity_tests_ok(host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &error) &&
						  txn_status_roomed(io, top, &writing.roomed, &error), &error, __LINE__);
	writing.writer.xid = sub.f0;
	writing.writer.has_top = sub.f0 != top.f0;
	writing.writer.top = top.f0;
	return writing;
}

/* txn_identity_tests_committed commits the running transaction as Postgres' commit leaves its entry once the commit's slot is settled: committed at the WAL's end. */
static void
txn_identity_tests_committed(HostIoIo *io)
{
	HostXid		sub;
	HostXid		top;
	TxnStatusEntry entry;
	TxnStatusOutcome to = {.tag = TXN_STATUS_OUTCOME_COMMITTED};
	bool		has;
	bool		settled = false;
	HostError	error;

	txn_identity_tests_ok(host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &error), &error, __LINE__);
	simc_end(true);
	txn_identity_tests_ok(txn_status_lookup(io, top, &has, &entry, &error), &error, __LINE__);
	STD_ASSERT_MSG(__LINE__, 0, has, "status::lookup(io, top).unwrap().unwrap()");
	txn_identity_tests_ok(host_fold_point(HOST_IO_IO_IO_DOORS(io), &to.u.committed, &error) &&
						  txn_status_settled(io, top, entry.outcome, to, &settled, &error), &error, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, settled, "status::settled(io, top, entry.outcome, Committed(end)).unwrap()");
}

/* txn_identity_tests_repository makes a test's database's repository and returns its number. */
static uint32
txn_identity_tests_repository(HostIoIo *io, uint32 number)
{
	RepoRepo	repo;
	ChunksErrorError failed;

	if (!repo_Repo_create(io, number, HOST_PG_DEFAULT, &repo, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, TXN_IDENTITY_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	repo_Repo_drop(&repo);
	return number;
}

/* txn_identity_tests_opened_t is opened's database: the repository opened, a reader, and its number. */
typedef struct txn_identity_tests_opened_t
{
	RepoRepo	repo;
	HostSharedReader reading;
	uint32		repository;
} txn_identity_tests_opened_t;

/* txn_identity_tests_opened opens a test's database's repository and a reader, the identity tree as its readers reach it. */
static txn_identity_tests_opened_t
txn_identity_tests_opened(HostIoIo *io, uint32 repository)
{
	txn_identity_tests_opened_t opened = {.repository = repository};
	ChunksErrorError failed;
	HostError	error;

	if (!repo_Repo_open(io, repository, HOST_PG_DEFAULT, &opened.repo, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, TXN_IDENTITY_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	txn_identity_tests_ok(host_shared_read(io, &opened.reading, &error), &error, __LINE__);
	return opened;
}

/* txn_identity_tests_opened_database returns an opened repository as the identity tree's database. */
static TxnIdentityDatabase
txn_identity_tests_opened_database(const txn_identity_tests_opened_t *opened)
{
	TxnIdentityDatabase database = {&opened->repo, &opened->reading, opened->repository};

	return database;
}

/* txn_identity_tests_opened_drop frees what opened returned. */
static void
txn_identity_tests_opened_drop(txn_identity_tests_opened_t *opened)
{
	host_shared_Reader_drop(&opened->reading);
	repo_Repo_drop(&opened->repo);
}

/* txn_identity_tests_keyed is a table's keyed closure: an id's pair by id, as a relation. */
static void
txn_identity_tests_keyed(const void *captures, uint64 id, StdVec *result)
{
	(void) captures;
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, result);
}

/* txn_identity_tests_table records a table's pairs under SCHEMA in the running transaction and returns its id. */
static uint64
txn_identity_tests_table(HostIoIo *io, uint32 repository, const char *name, uint32 oid)
{
	txn_identity_tests_writing_t written = txn_identity_tests_writing(io);
	txn_identity_tests_opened_t opened = txn_identity_tests_opened(io, repository);
	HostIoWaits waits = host_io_Waits_testing();
	StdVec		numbered;
	uint64		id;
	HostError	error;

	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, oid, &numbered);
	txn_identity_tests_ok(txn_identity_derived(io, &waits, txn_identity_tests_opened_database(&opened), written.writer,
											   written.roomed, TXN_IDENTITY_KIND_RELATION, TXN_IDENTITY_TESTS_SCHEMA,
											   (const uint8 *) name, strlen(name), numbered.ptr, numbered.len, oid,
											   txn_identity_tests_keyed, NULL, NULL, &id, &error), &error, __LINE__);
	std_vec_free_Checked(&numbered, 1, 1);
	txn_identity_tests_opened_drop(&opened);
	return id;
}

/* txn_identity_tests_read reads a key of a database's identity tree for the running transaction, answering whether it holds one, into value when given. */
static bool
txn_identity_tests_read(HostIoIo *io, uint32 repository, const StdVec *key, StdVec *value)
{
	txn_identity_tests_opened_t opened = txn_identity_tests_opened(io, repository);
	HostIoWaits waits = host_io_Waits_testing();
	StdVec		read;
	bool		has;
	HostError	error;

	txn_identity_tests_ok(txn_identity_looked_up(io, &waits, txn_identity_tests_opened_database(&opened), key->ptr,
												 key->len, &has, &read, &error), &error, __LINE__);
	txn_identity_tests_opened_drop(&opened);
	if (has && value != NULL)
		*value = read;
	else if (has)
		std_vec_free_Checked(&read, 1, 1);
	return has;
}

/* txn_identity_tests_read_is reports whether a key of a database's identity tree reads as some bytes. */
static bool
txn_identity_tests_read_is(HostIoIo *io, uint32 repository, const StdVec *key, const uint8 *want, size_t want_len)
{
	StdVec		value;
	bool		is;

	if (!txn_identity_tests_read(io, repository, key, &value))
		return false;
	is = value.len == want_len && memcmp(value.ptr, want, want_len) == 0;
	std_vec_free_Checked(&value, 1, 1);
	return is;
}

/* txn_identity_tests_id_bytes returns an id's big-endian bytes. */
static void
txn_identity_tests_id_bytes(uint64 id, uint8 *bytes)
{
	for (int k = 0; k < 8; k++)
		bytes[k] = (uint8) (id >> (8 * (7 - k)));
}

/* TXN_IDENTITY_TESTS_NUMBER is the pair by id's value of OID 16384, its varint. */
static const uint8 TXN_IDENTITY_TESTS_NUMBER[] = {0x80, 0x80, 0x01};

/* txn_identity_tests_pairs_read reports whether a table's two pairs read as its OID's and its id's. */
static bool
txn_identity_tests_pairs_read(HostIoIo *io, uint32 repository, uint32 oid, uint64 id)
{
	StdVec		by_oid;
	StdVec		by_id;
	uint8		bytes[8];
	bool		read;

	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, oid, &by_oid);
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &by_id);
	txn_identity_tests_id_bytes(id, bytes);
	read = txn_identity_tests_read_is(io, repository, &by_oid, bytes, sizeof(bytes)) &&
		txn_identity_tests_read_is(io, repository, &by_id, TXN_IDENTITY_TESTS_NUMBER,
								   sizeof(TXN_IDENTITY_TESTS_NUMBER));
	std_vec_free_Checked(&by_id, 1, 1);
	std_vec_free_Checked(&by_oid, 1, 1);
	return read;
}

/* txn_identity_tests_pairs_gone reports whether neither of a table's two pairs reads. */
static bool
txn_identity_tests_pairs_gone(HostIoIo *io, uint32 repository, uint32 oid, uint64 id)
{
	StdVec		by_oid;
	StdVec		by_id;
	bool		gone;

	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, oid, &by_oid);
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &by_id);
	gone = !txn_identity_tests_read(io, repository, &by_oid, NULL) &&
		!txn_identity_tests_read(io, repository, &by_id, NULL);
	std_vec_free_Checked(&by_id, 1, 1);
	std_vec_free_Checked(&by_oid, 1, 1);
	return gone;
}

/* txn_identity_tests_logged_count returns how many database change records the simulation logged. */
static size_t
txn_identity_tests_logged_count(void)
{
	StdVec		logged = simc_logged();
	size_t		count = 0;

	for (size_t k = 0; k < logged.len; k++)
	{
		simc_TupleU8VecU8Lsn_t *one = &STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, k);
		HostWalKind kind;

		count += host_WalKind_of(one->f0, &kind) && kind == HOST_WAL_KIND_DATABASE_CHANGE;
		std_vec_free_Checked(&one->f1, 1, 1);
	}
	std_vec_free_Checked(&logged, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	return count;
}

/* txn_identity_tests_versions returns how many identity versions the delta holds at a key. */
static size_t
txn_identity_tests_versions(HostIoIo *io, uint32 repository, const StdVec *key)
{
	HostSharedHeld deltas;
	HostDeltaHold hold;
	StdVec		versions;
	size_t		count;
	HostError	error;

	txn_identity_tests_ok(txn_memtable_branch_deltas(io, repository, HOST_PG_DEFAULT, &deltas, &error) &&
						  host_delta_hold(io, &hold, &error) &&
						  txn_memtable_branch_identity_versions(io, &hold, host_shared_Held_record(&deltas), key->ptr,
																key->len, &versions, &error), &error, __LINE__);
	count = versions.len;
	for (size_t k = 0; k < versions.len; k++)
		txn_memtable_version_Version_drop(&STD_VEC_AT(&versions, TxnMemtableVersionVersion, k));
	std_vec_free_Checked(&versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&deltas);
	return count;
}

/* txn_identity_tests_dropped drops a repository's branch state, which unwraps at a line. */
static void
txn_identity_tests_dropped(HostIoIo *io, uint32 repository, uint32 line)
{
	HostError	error;

	txn_identity_tests_ok(txn_memtable_branch_dropped(io, repository, &error), &error, line);
}

/*
 * test_txn_identity_tests_a_tables_pairs_read_back_by_its_oid_and_its_id:
 * PLAN item 10's pairs, a table's derivation records its pair by OID and its
 * pair by id, which read back in its transaction and, once it commits, in
 * the next; the id is the derivation's, and recording the table again finds
 * its id and pushes nothing.
 */
void
test_txn_identity_tests_a_tables_pairs_read_back_by_its_oid_and_its_id(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		id;
	size_t		logged;
	StdVec		by_id;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9470);
	id = txn_identity_tests_table(io, repository, "t", 16384);
	STD_ASSERT_MSG(__LINE__, 9, id == txn_identity_tests_derived(TXN_IDENTITY_KIND_RELATION, TXN_IDENTITY_TESTS_SCHEMA,
																 "t", 1, __LINE__),
				   "id == derive_object_id(Kind::Relation, SCHEMA, b\"t\").unwrap()");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_read(io, repository, 16384, id),
				   "the table's pairs read back in its transaction");
	logged = txn_identity_tests_logged_count();
	simc_end(true);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_table(io, repository, "t", 16384) == id,
				   "recording the table again finds its id");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_logged_count() == logged, "recording it again pushed nothing");
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &by_id);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_read_is(io, repository, &by_id, TXN_IDENTITY_TESTS_NUMBER,
														   sizeof(TXN_IDENTITY_TESTS_NUMBER)),
				   "read(io, repository, &by_id(Kind::Relation, id)) == Some(vec![0x80, 0x80, 0x01])");
	std_vec_free_Checked(&by_id, 1, 1);
	simc_end(true);
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_identity_tests_an_aborted_transactions_pairs_go_at_the_take_back:
 * 2g's take-back for the identity tree, an aborted transaction's pairs go at
 * its abort, so its table has none.
 */
void
test_txn_identity_tests_an_aborted_transactions_pairs_go_at_the_take_back(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		id;
	StdVec		by_id;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9471);
	id = txn_identity_tests_table(io, repository, "t", 16384);
	simc_end(false);
	txn_identity_tests_ok(txn_written_aborted(io, 0, &error), &error, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_gone(io, repository, 16384, id),
				   "an aborted transaction's pairs went at its abort");
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &by_id);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_versions(io, repository, &by_id) == 0,
				   "branch::identity_versions(...).unwrap().is_empty()");
	std_vec_free_Checked(&by_id, 1, 1);
	txn_identity_tests_dropped(io, repository, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_identity_tests_a_taken_id_moves_on_and_a_removed_pair_frees_it:
 * I14 in one process, a derivation whose id another object's live pair holds
 * takes the next free id, and once that object's pairs are removed, an
 * object made again under its name derives its id again.
 */
void
test_txn_identity_tests_a_taken_id_moves_on_and_a_removed_pair_frees_it(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		first;
	txn_identity_tests_writing_t written;
	HostIoWaits waits = host_io_Waits_testing();
	StdVec		keys;
	StdVec		key;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9472);
	first = txn_identity_tests_table(io, repository, "t", 16384);
	simc_end(true);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_table(io, repository, "t", 16390) == txn_identity_next_id(first),
				   "a taken id moves on to the next free one");
	simc_end(true);
	written = txn_identity_tests_writing(io);
	std_vec_new(&keys, sizeof(StdVec), _Alignof(StdVec));
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, first, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_tests_ok(txn_identity_removed(io, &waits, repository, HOST_PG_DEFAULT, written.writer, written.roomed,
											   &keys, &error), &error, __LINE__);
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	STD_ASSERT_MSG(__LINE__, 9, !txn_identity_tests_read(io, repository, &key, NULL),
				   "read(io, repository, &by_oid(Kind::Relation, 16384)) == None");
	std_vec_free_Checked(&key, 1, 1);
	simc_end(true);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_table(io, repository, "t", 16400) == first,
				   "a removed pair frees its id");
	simc_end(true);
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_identity_tests_redone_identity_changes_give_the_same_pairs_once:
 * I12's case for the identity tree, a database's logged identity changes,
 * redone into another database's delta with no catalog, give it the same
 * pairs, and a change redone again is held by its LSN and pushes nothing.
 */
void
test_txn_identity_tests_redone_identity_changes_give_the_same_pairs_once(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		primary;
	uint32		replica;
	uint64		id;
	StdVec		logged;
	StdVec		records;
	StdVec		by_id;
	HostError	error;

	simc_end(false);
	primary = txn_identity_tests_repository(io, 9473);
	replica = txn_identity_tests_repository(io, 9474);
	id = txn_identity_tests_table(io, primary, "t", 16384);
	simc_end(true);
	logged = simc_logged();
	std_vec_new(&records, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	for (size_t k = 0; k < logged.len; k++)
	{
		simc_TupleU8VecU8Lsn_t *one = &STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, k);
		TxnRecordDatabaseChange change;
		HostWalKind kind;
		bool		kept = false;

		if (host_WalKind_of(one->f0, &kind) && kind == HOST_WAL_KIND_DATABASE_CHANGE)
		{
			if (!txn_record_DatabaseChange_decode(one->f1.ptr, one->f1.len, &change, &error))
				host_Error_drop(&error);
			else
			{
				kept = change.repository == primary;
				txn_record_DatabaseChange_drop(&change);
			}
		}
		if (kept)
			std_vec_push_Checked(&records, one, sizeof(*one), _Alignof(simc_TupleU8VecU8Lsn_t));
		else
			std_vec_free_Checked(&one->f1, 1, 1);
	}
	std_vec_free_Checked(&logged, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	STD_ASSERT_MSG(__LINE__, 9, records.len == 2, "records.len() == 2");
	for (int pass = 0; pass < 2; pass++)
		for (size_t k = 0; k < records.len; k++)
		{
			simc_TupleU8VecU8Lsn_t *one = &STD_VEC_AT(&records, simc_TupleU8VecU8Lsn_t, k);
			TxnRecordDatabaseChange change;

			txn_identity_tests_ok(txn_record_DatabaseChange_decode(one->f1.ptr, one->f1.len, &change, &error), &error,
								  __LINE__);
			change.repository = replica;
			txn_identity_tests_ok(txn_identity_redo(io, &change, HOST_PG_DEFAULT, one->f2, &error), &error,
								  __LINE__);
		}
	for (size_t k = 0; k < records.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&records, simc_TupleU8VecU8Lsn_t, k).f1, 1, 1);
	std_vec_free_Checked(&records, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_read(io, replica, 16384, id),
				   "the replica's pairs read as the primary's");
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &by_id);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_versions(io, replica, &by_id) == 1,
				   "a change redone again pushes nothing");
	std_vec_free_Checked(&by_id, 1, 1);
	txn_identity_tests_dropped(io, primary, __LINE__);
	txn_identity_tests_dropped(io, replica, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_identity_tests_refused reports whether a database change record's body is refused. */
static bool
txn_identity_tests_refused(ChunksRecordRecordWriter *w)
{
	StdVec		body = chunks_record_RecordWriter_finish(w);
	TxnRecordDatabaseChange change;
	HostError	error;
	bool		refused = !txn_record_DatabaseChange_decode(body.ptr, body.len, &change, &error);

	if (refused)
		host_Error_drop(&error);
	else
		txn_record_DatabaseChange_drop(&change);
	std_vec_free_Checked(&body, 1, 1);
	return refused;
}

/*
 * test_txn_identity_tests_an_identity_change_reads_back_as_written: the
 * database change record's form, a subtransaction's writer, a put's value,
 * and a removal's lack of one read back as written, and an unknown field
 * and a tree none is are refused.
 */
void
test_txn_identity_tests_an_identity_change_reads_back_as_written(void)
{
	TxnMemtableVersionWriter writer = {.xid = 41, .has_top = true, .top = 40, .command = 3};
	ChunksRecordRecordWriter w;

	for (int put = 0; put < 2; put++)
	{
		TxnRecordDatabaseChange change = {.tree = TXN_RECORD_TREE_IDENTITY, .writer = writer, .repository = 9475,
		.has_value = put == 0};
		TxnRecordDatabaseChange read;
		StdVec		body;
		HostError	error;

		txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &change.key);
		if (put == 0)
			std_slice_to_vec_Checked(&change.value, "v", 1, 1, 1);
		txn_record_DatabaseChange_encode(&change, &body);
		txn_identity_tests_ok(txn_record_DatabaseChange_decode(body.ptr, body.len, &read, &error), &error, __LINE__);
		STD_ASSERT_MSG(__LINE__, 13, txn_record_DatabaseChange_PartialEq_eq(&read, &change),
					   "DatabaseChange::decode(&change.encode()).unwrap() == change");
		txn_record_DatabaseChange_drop(&read);
		txn_record_DatabaseChange_drop(&change);
		std_vec_free_Checked(&body, 1, 1);
	}
	w = chunks_record_RecordWriter_with_capacity(32);
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(
																									chunks_record_RecordWriter_uint(&w, 1, 1), 2, 9475), 3, 40), 5, 0);
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_field(&w, 6, (const uint8 *) "k", 1), 8, 1);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_refused(&w), "an unknown field read back");
	w = chunks_record_RecordWriter_with_capacity(32);
	chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(chunks_record_RecordWriter_uint(
																									chunks_record_RecordWriter_uint(&w, 1, 2), 2, 9475), 3, 40), 5, 0);
	chunks_record_RecordWriter_field(&w, 6, (const uint8 *) "k", 1);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_refused(&w), "a tree none is read back");
}

/* txn_identity_tests_gather is a save's gather of the folded pairs, each as the identity tree's pair. */
static bool
txn_identity_tests_gather(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	const StdVec *pairs = captures;

	(void) io;
	(void) read;
	(void) error;
	std_vec_new(result, sizeof(RepoGathered), _Alignof(RepoGathered));
	for (size_t k = 0; k < pairs->len; k++)
	{
		const TxnIdentityPair *pair = &STD_VEC_AT(pairs, TxnIdentityPair, k);
		RepoGathered gathered = {.tag = REPO_GATHERED_PAIR, .u.pair = {.has_value = pair->has_f1}};

		std_slice_to_vec_Checked(&gathered.u.pair.key, pair->f0.ptr, pair->f0.len, 1, 1);
		if (pair->has_f1)
			std_slice_to_vec_Checked(&gathered.u.pair.value, pair->f1.ptr, pair->f1.len, 1, 1);
		std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	}
	return true;
}

/* txn_identity_tests_saved is a save's after-write call, which does nothing. */
static bool
txn_identity_tests_saved(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/* txn_identity_tests_folded folds a repository's committed pairs at a point and saves them into the tree the manifest names, returning the pairs. */
static StdVec
txn_identity_tests_folded(HostIoIo *io, uint32 repository, HostLsn point)
{
	StdVec		pairs;
	txn_identity_tests_opened_t opened;
	ChunksErrorError failed;
	HostError	error;

	txn_identity_tests_ok(txn_identity_folded(io, repository, point, &pairs, &error), &error, __LINE__);
	opened = txn_identity_tests_opened(io, repository);
	if (!repo_Repo_save(&opened.repo, io, &opened.reading, false, NULL, txn_identity_tests_gather, &pairs,
						txn_identity_tests_saved, NULL, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, TXN_IDENTITY_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	txn_identity_tests_opened_drop(&opened);
	return pairs;
}

/* txn_identity_tests_pairs_drop drops folded's pairs. */
static void
txn_identity_tests_pairs_drop(StdVec *pairs)
{
	for (size_t k = 0; k < pairs->len; k++)
	{
		TxnIdentityPair *pair = &STD_VEC_AT(pairs, TxnIdentityPair, k);

		std_vec_free_Checked(&pair->f0, 1, 1);
		if (pair->has_f1)
			std_vec_free_Checked(&pair->f1, 1, 1);
	}
	std_vec_free_Checked(pairs, sizeof(TxnIdentityPair), _Alignof(TxnIdentityPair));
}

/* txn_identity_tests_point returns the fold point the running fold position lowers to. */
static HostLsn
txn_identity_tests_point(HostIoIo *io)
{
	HostLsn		position;
	HostLsn		point = 0;
	HostError	error;

	txn_identity_tests_ok(host_fold_point(HOST_IO_IO_IO_DOORS(io), &position, &error) &&
						  txn_status_lowered(io, position, &point, &error), &error, __LINE__);
	return point;
}

/*
 * test_txn_identity_tests_a_saves_fold_keeps_its_committed_pairs_and_leaves_running_ones:
 * §III.6's fold of the identity tree at a save, a save folds the pairs whose
 * writers committed at or below its point into the tree the manifest names,
 * a running writer's pair staying in the delta; with the delta's versions
 * gone, the folded pairs read from the tree alone, and the running writer's
 * pair is gone with them.
 */
void
test_txn_identity_tests_a_saves_fold_keeps_its_committed_pairs_and_leaves_running_ones(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		id;
	uint64		running;
	HostLsn		point;
	StdVec		pairs;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9477);
	id = txn_identity_tests_table(io, repository, "t", 16384);
	txn_identity_tests_committed(io);
	point = txn_identity_tests_point(io);
	running = txn_identity_tests_table(io, repository, "u", 16385);
	pairs = txn_identity_tests_folded(io, repository, point);
	STD_ASSERT_MSG(__LINE__, 9, pairs.len == 2, "pairs.len() == 2");
	txn_identity_tests_pairs_drop(&pairs);
	simc_end(false);
	txn_identity_tests_ok(txn_written_aborted(io, 0, &error), &error, __LINE__);
	txn_identity_tests_dropped(io, repository, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_read(io, repository, 16384, id),
				   "the folded pairs read from the tree alone");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_gone(io, repository, 16385, running),
				   "the running writer's pair is gone");
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_identity_tests_kept returns how many identity versions the delta holds at a table's two pairs. */
static size_t
txn_identity_tests_kept(HostIoIo *io, uint32 repository, uint32 oid, uint64 id)
{
	StdVec		key;
	size_t		kept;

	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, oid, &key);
	kept = txn_identity_tests_versions(io, repository, &key);
	std_vec_free_Checked(&key, 1, 1);
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, id, &key);
	kept += txn_identity_tests_versions(io, repository, &key);
	std_vec_free_Checked(&key, 1, 1);
	return kept;
}

/* txn_identity_tests_counted returns an xid's entry's count, 0 for none. */
static uint64
txn_identity_tests_counted(HostIoIo *io, HostXid top)
{
	TxnStatusEntry entry;
	bool		has;
	HostError	error;

	txn_identity_tests_ok(txn_status_lookup(io, top, &has, &entry, &error), &error, __LINE__);
	return has ? entry.count : 0;
}

/*
 * test_txn_identity_tests_a_saves_drop_takes_its_folded_pairs_out: bugs row
 * 164, a save's drop takes the identity versions it folded out of the delta
 * and gives their counts back at their writer's entry, the pairs then
 * reading from the manifest's tree alone.
 */
void
test_txn_identity_tests_a_saves_drop_takes_its_folded_pairs_out(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		id;
	HostXid		sub;
	HostXid		top;
	HostLsn		point;
	StdVec		pairs;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9479);
	id = txn_identity_tests_table(io, repository, "t", 16384);
	txn_identity_tests_ok(host_current_writer(HOST_IO_IO_IO_DOORS(io), &sub, &top, &error), &error, __LINE__);
	txn_identity_tests_committed(io);
	point = txn_identity_tests_point(io);
	pairs = txn_identity_tests_folded(io, repository, point);
	txn_identity_tests_pairs_drop(&pairs);
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_kept(io, repository, 16384, id) == 2 &&
				   txn_identity_tests_counted(io, top) == 2, "(kept(io), counted(io)) == (2, 2)");
	{
		bool		took;

		txn_identity_tests_ok(txn_identity_dropped(io, repository, point, &took, &error), &error, __LINE__);
	}
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_kept(io, repository, 16384, id) == 0 &&
				   txn_identity_tests_counted(io, top) == 0, "(kept(io), counted(io)) == (0, 0)");
	STD_ASSERT_MSG(__LINE__, 9, txn_identity_tests_pairs_read(io, repository, 16384, id),
				   "the pairs read from the manifest's tree alone");
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_identity_tests_an_identity_put_carries_no_locker: an identity
 * tree's put, made over a change whose bytes held anything, carries no row
 * locker, so its record holds only what the put names and reads back as
 * stored (bugs row 369).
 */
void
test_txn_identity_tests_an_identity_put_carries_no_locker(void)
{
	static const uint8 bytes[] = {1, 2, 3};
	TxnMemtableVersionWriter writer = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	TxnMemtableOp op;
	StdVec		value;
	StdVec		record;
	TxnMemtableVersionRecord read;
	HostError	error;

	memset(&op, 0xaa, sizeof(op));
	std_slice_to_vec_Checked(&value, bytes, sizeof(bytes), 1, 1);
	txn_identity_op(true, value, &op);
	std_vec_new(&record, 1, 1);
	txn_memtable_version_encode(&writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, true, 9, &record);
	STD_ASSERT_MSG(__LINE__, 9, record.len == TXN_MEMTABLE_VERSION_HEADER + 8 + sizeof(bytes),
				   "the record holds its header, its WAL end, and its value alone");
	STD_ASSERT_MSG(__LINE__, 9, ((const uint8 *) record.ptr)[27] == TXN_MEMTABLE_LOCK_MODE_UPDATE,
				   "the lock byte holds its lock and no carried locker");
	txn_identity_tests_ok(txn_memtable_version_stored(record.ptr, record.len, &read, &error), &error, __LINE__);
	std_vec_free_Checked(&record, 1, 1);
	txn_memtable_Op_drop(&op);
}

/* txn_identity_tests_identity_spilled spills a writer's identity pairs in a repository, asserting at its line that it spilled. */
static void
txn_identity_tests_identity_spilled(HostIoIo *io, uint32 repository, TxnMemtableVersionWriter writer, uint32 line)
{
	HostStorage storage = txn_spill_identity_storage(repository);
	HostError	error;
	bool		spilled;

	txn_identity_tests_ok(txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_IDENTITY,
											writer.has_top ? writer.top : writer.xid, &spilled, &error),
						  &error, line);
	STD_ASSERT_MSG(line, 9, spilled, "the writer's identity pairs spilled");
}

/*
 * Bugs row 397's identity tree: a key's spilled versions read among its
 * memtable versions in commit order. A running transaction's removal of a
 * committed table's pairs spills though the keys hold the committed pairs,
 * which stay in the memtable above the removal's file: the running
 * transaction reads no pair, and once it commits neither the next
 * transaction nor a save's fold finds one.
 */
void
test_txn_identity_tests_a_spilled_removal_reads_above_the_pair_it_removes_whatever_the_listing(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		first;
	txn_identity_tests_writing_t removing;
	HostIoWaits waits = host_io_Waits_testing();
	StdVec		keys;
	StdVec		key;
	StdVec		pairs;
	bool		removed = false;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9496);
	first = txn_identity_tests_table(io, repository, "t", 16384);
	txn_identity_tests_committed(io);
	removing = txn_identity_tests_writing(io);
	std_vec_new(&keys, sizeof(StdVec), _Alignof(StdVec));
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, first, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_tests_ok(txn_identity_removed(io, &waits, repository, HOST_PG_DEFAULT, removing.writer,
											   removing.roomed, &keys, &error), &error, __LINE__);
	txn_identity_tests_identity_spilled(io, repository, removing.writer, __LINE__);
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	STD_ASSERT_MSG(__LINE__, 9, !txn_identity_tests_read(io, repository, &key, NULL),
				   "the removing transaction reads no pair");
	txn_identity_tests_committed(io);
	STD_ASSERT_MSG(__LINE__, 9, !txn_identity_tests_read(io, repository, &key, NULL),
				   "after its commit the next transaction reads no pair");
	pairs = txn_identity_tests_folded(io, repository, txn_identity_tests_point(io));
	for (size_t k = 0; k < pairs.len; k++)
	{
		const TxnIdentityPair *pair = &STD_VEC_AT(&pairs, TxnIdentityPair, k);

		if (pair->f0.len == key.len && memcmp(pair->f0.ptr, key.ptr, key.len) == 0)
			removed = !pair->has_f1;
	}
	STD_ASSERT_MSG(__LINE__, 9, removed, "a save's fold removes the pair");
	txn_identity_tests_pairs_drop(&pairs);
	std_vec_free_Checked(&key, 1, 1);
	simc_end(true);
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * Bugs row 397's own runs for the identity tree: a transaction's table pairs
 * spill, then its removal of them spills to a second file no merge joins
 * (the older the larger): it reads no pair, and after its commit neither does
 * the next transaction.
 */
void
test_txn_identity_tests_a_transactions_spilled_removal_reads_above_its_spilled_pair(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	uint32		repository;
	uint64		first;
	txn_identity_tests_writing_t writing;
	HostIoWaits waits = host_io_Waits_testing();
	StdVec		keys;
	StdVec		key;
	HostError	error;

	simc_end(false);
	repository = txn_identity_tests_repository(io, 9501);
	first = txn_identity_tests_table(io, repository, "t", 16384);
	writing = txn_identity_tests_writing(io);
	txn_identity_tests_identity_spilled(io, repository, writing.writer, __LINE__);
	std_vec_new(&keys, sizeof(StdVec), _Alignof(StdVec));
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_by_id(TXN_IDENTITY_KIND_RELATION, first, &key);
	std_vec_push_Checked(&keys, &key, sizeof(key), _Alignof(StdVec));
	txn_identity_tests_ok(txn_identity_removed(io, &waits, repository, HOST_PG_DEFAULT, writing.writer,
											   writing.roomed, &keys, &error), &error, __LINE__);
	txn_identity_tests_identity_spilled(io, repository, writing.writer, __LINE__);
	txn_identity_by_oid(TXN_IDENTITY_KIND_RELATION, 16384, &key);
	STD_ASSERT_MSG(__LINE__, 9, !txn_identity_tests_read(io, repository, &key, NULL),
				   "the transaction reads no pair");
	txn_identity_tests_committed(io);
	STD_ASSERT_MSG(__LINE__, 9, !txn_identity_tests_read(io, repository, &key, NULL),
				   "after its commit the next transaction reads no pair");
	std_vec_free_Checked(&key, 1, 1);
	simc_end(true);
	txn_identity_tests_dropped(io, repository, __LINE__);
	txn_identity_tests_ok(txn_written_ended(io, &error), &error, __LINE__);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

#endif							/* DOLTGRES_TEST_TXN */
