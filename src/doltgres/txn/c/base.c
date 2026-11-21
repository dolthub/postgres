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
 * A storage's base record: the value of its entry in its repository's base
 * tree, which the save's gathering writes from a storage's state and a make's
 * fill reads back; a chain tree's and the tuple id and pending trees' entries;
 * and the error of a failure of the storage a reader reads.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/record.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/base.h"
#ifdef DOLTGRES_TEST_TXN
#include "doltgres/std/test.h"
#include "doltgres/std/tuple.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "base.rs"

/*
 * The record's field numbers, in the format's order, TABLE, MAX_ENDED, CHAIN,
 * and READ always present, READ the highest-numbered and last, so a record
 * cut short anywhere is refused.
 */
#define TXN_BASE_ROOT 1
#define TXN_BASE_FOLDED 2
#define TXN_BASE_MAX_FOLDED 3
#define TXN_BASE_EPOCH 4
#define TXN_BASE_TUPLE_IDS 5
#define TXN_BASE_COUNTER 6
#define TXN_BASE_FRESH_END 7
#define TXN_BASE_FREE_SPACE 8
#define TXN_BASE_SIZES 9
#define TXN_BASE_INDEX 10
#define TXN_BASE_KEY 11
#define TXN_BASE_TABLE 12
#define TXN_BASE_PENDING 13
#define TXN_BASE_MAX_ENDED 14
#define TXN_BASE_CHAIN 16
#define TXN_BASE_READ 18

/* TXN_BASE_ATTNUM_MAX is Postgres' MaxHeapAttributeNumber, the largest attribute number a key's column has. */
#define TXN_BASE_ATTNUM_MAX UINT64CONST(1600)

/* TXN_BASE_TUPLE_ID_MAX is the largest tuple id, 48 bits. */
#define TXN_BASE_TUPLE_ID_MAX ((UINT64CONST(1) << 48) - 1)

/* TXN_BASE_U32_MAX is the largest u32, as a u64. */
#define TXN_BASE_U32_MAX ((uint64) PG_UINT32_MAX)

/* TXN_BASE_COLUMN is the size and alignment of a KeyColumn. */
#define TXN_BASE_COLUMN sizeof(TxnBaseKeyColumn), _Alignof(TxnBaseKeyColumn)

/* TXN_BASE_PENDING_SIZE is the size and alignment of a Pending. */
#define TXN_BASE_PENDING_SIZE sizeof(TxnBasePending), _Alignof(TxnBasePending)

/* TXN_BASE_FIELD is the size and alignment of a record's Field. */
#define TXN_BASE_FIELD sizeof(ChunksRecordField), _Alignof(ChunksRecordField)

/* TXN_BASE_PLACED is the size and alignment of an address's offset and locator. */
#define TXN_BASE_PLACED sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/* TXN_BASE_NO_BYTES is an empty slice's bytes. */
static const uint8 TXN_BASE_NO_BYTES[1];

/* TXN_BASE_NO_ADDRESSES is an empty slice's addresses. */
static const chunks_TupleU32Locator_t TXN_BASE_NO_ADDRESSES[1];

/* txn_base_fork returns the fork's error of a SQLSTATE and a message. */
static HostError
txn_base_fork(const char *sqlstate, const char *message, size_t message_len)
{
	StdString	string;

	std_string_from_Checked(&string, message, message_len);
	return host_Error_fork((const uint8 *) sqlstate, &string);
}

/* TXN_BASE_FORK is fork of a literal message. */
#define TXN_BASE_FORK(sqlstate, literal) txn_base_fork((sqlstate), (literal), sizeof(literal) - 1)

/* txn_base_corrupt returns the error of a base record that doesn't decode. */
static HostError
txn_base_corrupt(void)
{
	return TXN_BASE_FORK("XX001", "a storage's base record is corrupt");
}

/* txn_base_Sizes_Default_default returns sizes all zero. */
TxnBaseSizes
txn_base_Sizes_Default_default(void)
{
	TxnBaseSizes sizes = {0};

	return sizes;
}

/* txn_base_Sizes_PartialEq_eq reports whether two sizes are equal field by field. */
bool
txn_base_Sizes_PartialEq_eq(const TxnBaseSizes *self, const TxnBaseSizes *other)
{
	return self->entries == other->entries && self->leaf_bytes == other->leaf_bytes &&
		self->key_bytes == other->key_bytes && self->heap_bytes == other->heap_bytes && self->rows == other->rows;
}

/* txn_base_Sizes_Debug_fmt is Sizes' derived Debug. */
bool
txn_base_Sizes_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseSizes *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Sizes", 5);

	std_DebugStruct_field(&s, "entries", 7, &self->entries, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "leaf_bytes", 10, &self->leaf_bytes, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "key_bytes", 9, &self->key_bytes, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "heap_bytes", 10, &self->heap_bytes, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "rows", 4, &self->rows, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_base_BlockCounts_Default_default returns counts all zero. */
TxnBaseBlockCounts
txn_base_BlockCounts_Default_default(void)
{
	TxnBaseBlockCounts counts = {0};

	return counts;
}

/* txn_base_BlockCounts_PartialEq_eq reports whether two counts are equal field by field. */
bool
txn_base_BlockCounts_PartialEq_eq(const TxnBaseBlockCounts *self, const TxnBaseBlockCounts *other)
{
	return self->leaves == other->leaves && self->internal == other->internal && self->height == other->height;
}

/* txn_base_BlockCounts_Debug_fmt is BlockCounts' derived Debug. */
bool
txn_base_BlockCounts_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseBlockCounts *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "BlockCounts", 11);

	std_DebugStruct_field(&s, "leaves", 6, &self->leaves, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "internal", 8, &self->internal, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "height", 6, &self->height, std_u8_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_base_KeyColumn_PartialEq_eq reports whether two key columns are equal field by field. */
bool
txn_base_KeyColumn_PartialEq_eq(const TxnBaseKeyColumn *self, const TxnBaseKeyColumn *other)
{
	return self->attnum == other->attnum && self->class == other->class && self->collation == other->collation &&
		self->typid == other->typid && self->compared == other->compared;
}

/* txn_base_KeyColumn_Debug_fmt is KeyColumn's derived Debug. */
bool
txn_base_KeyColumn_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseKeyColumn *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "KeyColumn", 9);

	std_DebugStruct_field(&s, "attnum", 6, &self->attnum, std_u16_Debug_fmt);
	std_DebugStruct_field(&s, "class", 5, &self->class, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "collation", 9, &self->collation, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "typid", 5, &self->typid, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "compared", 8, &self->compared, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_base_key_columns_Debug_fmt is a Vec<KeyColumn>'s Debug, a list of its columns. */
static bool
txn_base_key_columns_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *columns = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t i = 0; i < columns->len; i++)
		std_DebugList_entry(&l, &STD_VEC_AT(columns, TxnBaseKeyColumn, i), txn_base_KeyColumn_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* txn_base_Key_of returns the key of a table whose primary key has some columns, in key order: none for no columns. */
TxnBaseKey
txn_base_Key_of(StdVec *columns)
{
	TxnBaseKey	key = {.tag = TXN_BASE_KEY_NONE};

	if (columns->len == 0)
		std_vec_free_Checked(columns, TXN_BASE_COLUMN);
	else
	{
		key.tag = TXN_BASE_KEY_COLUMNS;
		key.u.columns = *columns;
	}
	return key;
}

/* txn_base_Key_Clone_clone copies a key into another. */
void
txn_base_Key_Clone_clone(const TxnBaseKey *self, TxnBaseKey *result)
{
	result->tag = self->tag;
	if (self->tag == TXN_BASE_KEY_COLUMNS)
		std_slice_to_vec_Checked(&result->u.columns, self->u.columns.ptr, self->u.columns.len, TXN_BASE_COLUMN);
}

/* txn_base_Key_PartialEq_eq reports whether two keys are the same variant with equal columns. */
bool
txn_base_Key_PartialEq_eq(const TxnBaseKey *self, const TxnBaseKey *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag != TXN_BASE_KEY_COLUMNS)
		return true;
	if (self->u.columns.len != other->u.columns.len)
		return false;
	for (size_t i = 0; i < self->u.columns.len; i++)
		if (!txn_base_KeyColumn_PartialEq_eq(&STD_VEC_AT(&self->u.columns, TxnBaseKeyColumn, i),
											 &STD_VEC_AT(&other->u.columns, TxnBaseKeyColumn, i)))
			return false;
	return true;
}

/* txn_base_Key_Debug_fmt is Key's derived Debug. */
bool
txn_base_Key_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseKey *self = value;
	StdDebugTuple t;

	switch (self->tag)
	{
		case TXN_BASE_KEY_NONE:
			return std_fmt_write_str(f, "None", 4);
		case TXN_BASE_KEY_UNKNOWN:
			return std_fmt_write_str(f, "Unknown", 7);
		case TXN_BASE_KEY_COLUMNS:
			t = std_fmt_debug_tuple(f, "Columns", 7);
			std_DebugTuple_field(&t, &self->u.columns, txn_base_key_columns_Debug_fmt);
			return std_DebugTuple_finish(&t);
	}
	pg_unreachable();
}

/* txn_base_Key_drop frees a key's columns. */
void
txn_base_Key_drop(TxnBaseKey *self)
{
	if (self->tag == TXN_BASE_KEY_COLUMNS)
		std_vec_free_Checked(&self->u.columns, TXN_BASE_COLUMN);
}

/*
 * txn_base_Base_empty returns the base record of a storage no save has
 * written yet, of a table's object id, and an index's for an index's storage,
 * under a key it takes: every tree absent, every size and point zero, and the
 * counter and the fresh end at the first tuple id.
 */
TxnBaseBase
txn_base_Base_empty(TxnBaseKey *key, uint32 table, bool has_index, uint32 index)
{
	TxnBaseBase base = {.counter = 1, .fresh_end = 1, .table = table, .has_index = has_index, .key = *key};

	if (has_index)
		base.index = index;
	return base;
}

/* txn_base_tree_eq reports whether two optional trees are both absent or name the same tree. */
static bool
txn_base_tree_eq(bool has_a, const TreesStoreTree *a, bool has_b, const TreesStoreTree *b)
{
	return has_a == has_b && (!has_a || trees_store_Tree_PartialEq_eq(a, b));
}

/* txn_base_Base_PartialEq_eq reports whether two base records are equal field by field. */
bool
txn_base_Base_PartialEq_eq(const TxnBaseBase *self, const TxnBaseBase *other)
{
	return txn_base_tree_eq(self->has_root, &self->root, other->has_root, &other->root) &&
		self->folded == other->folded && self->max_folded == other->max_folded && self->epoch == other->epoch &&
		txn_base_tree_eq(self->has_tuple_ids, &self->tuple_ids, other->has_tuple_ids, &other->tuple_ids) &&
		self->counter == other->counter && self->fresh_end == other->fresh_end &&
		txn_base_tree_eq(self->has_free_space, &self->free_space, other->has_free_space, &other->free_space) &&
		txn_base_Sizes_PartialEq_eq(&self->sizes, &other->sizes) && self->table == other->table &&
		self->has_index == other->has_index && (!self->has_index || self->index == other->index) &&
		txn_base_Key_PartialEq_eq(&self->key, &other->key) &&
		txn_base_tree_eq(self->has_pending, &self->pending, other->has_pending, &other->pending) &&
		self->max_ended == other->max_ended &&
		txn_base_tree_eq(self->has_chain, &self->chain, other->has_chain, &other->chain) && self->read == other->read;
}

/* txn_base_Base_Debug_fmt is Base's derived Debug. */
bool
txn_base_Base_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseBase *self = value;
	StdOptionRef root = {self->has_root, {&self->root, trees_store_Tree_Debug_fmt}};
	StdOptionRef tuple_ids = {self->has_tuple_ids, {&self->tuple_ids, trees_store_Tree_Debug_fmt}};
	StdOptionRef free_space = {self->has_free_space, {&self->free_space, trees_store_Tree_Debug_fmt}};
	StdOptionRef index = {self->has_index, {&self->index, std_u32_Debug_fmt}};
	StdOptionRef pending = {self->has_pending, {&self->pending, trees_store_Tree_Debug_fmt}};
	StdOptionRef chain = {self->has_chain, {&self->chain, trees_store_Tree_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Base", 4);

	std_DebugStruct_field(&s, "root", 4, &root, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "folded", 6, &self->folded, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "max_folded", 10, &self->max_folded, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "epoch", 5, &self->epoch, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "tuple_ids", 9, &tuple_ids, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "counter", 7, &self->counter, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "fresh_end", 9, &self->fresh_end, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "free_space", 10, &free_space, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "sizes", 5, &self->sizes, txn_base_Sizes_Debug_fmt);
	std_DebugStruct_field(&s, "table", 5, &self->table, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "index", 5, &index, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "key", 3, &self->key, txn_base_Key_Debug_fmt);
	std_DebugStruct_field(&s, "pending", 7, &pending, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "max_ended", 9, &self->max_ended, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "chain", 5, &chain, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "read", 4, &self->read, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_base_Base_drop frees a base record's key. */
void
txn_base_Base_drop(TxnBaseBase *self)
{
	txn_base_Key_drop(&self->key);
}

/*
 * txn_base_Pending_same reports whether two entries are one DDL's transform,
 * whatever each knows of its commit's end.
 */
bool
txn_base_Pending_same(const TxnBasePending *self, const TxnBasePending *other)
{
	return self->xid == other->xid && self->command == other->command && self->sub == other->sub &&
		trees_store_VecU8_PartialEq_eq(&self->transform, &other->transform);
}

/* txn_base_Pending_Clone_clone copies a transform's entry into another. */
void
txn_base_Pending_Clone_clone(const TxnBasePending *self, TxnBasePending *result)
{
	*result = *self;
	std_slice_to_vec_Checked(&result->transform, self->transform.ptr, self->transform.len, 1, 1);
}

/* txn_base_Pending_PartialEq_eq reports whether two transforms' entries are equal field by field. */
bool
txn_base_Pending_PartialEq_eq(const TxnBasePending *self, const TxnBasePending *other)
{
	return txn_base_Pending_same(self, other) && self->has_end == other->has_end &&
		(!self->has_end || self->end == other->end);
}

/* txn_base_Pending_Debug_fmt is Pending's derived Debug. */
bool
txn_base_Pending_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBasePending *self = value;
	StdOptionRef end = {self->has_end, {&self->end, std_u64_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Pending", 7);

	std_DebugStruct_field(&s, "xid", 3, &self->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "command", 7, &self->command, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "sub", 3, &self->sub, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "transform", 9, &self->transform, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "end", 3, &end, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_base_Pending_drop frees a transform's bytes. */
void
txn_base_Pending_drop(TxnBasePending *self)
{
	std_vec_free_Checked(&self->transform, 1, 1);
}

/*
 * txn_base_chain_key writes the key of a transform's entry in a chain tree:
 * its place in the chain, from 1, big-endian.
 */
void
txn_base_chain_key(uint32 place, uint8 result[4])
{
	result[0] = (uint8) (place >> 24);
	result[1] = (uint8) (place >> 16);
	result[2] = (uint8) (place >> 8);
	result[3] = (uint8) place;
}

/* txn_base_chain_value_frame_t is chain_value's state across its pad: its writer, while live. */
typedef struct txn_base_chain_value_frame_t
{
	const TxnBasePending *pending;
	StdVec	   *result;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
} txn_base_chain_value_frame_t;

/*
 * txn_base_chain_value_body is chain_value's body: the record written, its
 * bytes kept and its empty address table freed.
 */
static void
txn_base_chain_value_body(void *frame_)
{
	txn_base_chain_value_frame_t *frame = frame_;
	const TxnBasePending *pending = frame->pending;
	StdVec		addresses;

	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	chunks_record_RecordWriter_uint(&frame->writer, 1, pending->xid);
	chunks_record_RecordWriter_uint(&frame->writer, 2, (uint64) pending->command);
	chunks_record_RecordWriter_field(&frame->writer, 3, pending->transform.ptr, pending->transform.len);
	chunks_record_RecordWriter_uint(&frame->writer, 4, pending->sub);
	if (pending->has_end)
		chunks_record_RecordWriter_uint(&frame->writer, 5, pending->end);
	frame->writer_live = false;
	chunks_record_RecordWriter_finish_value(&frame->writer, frame->result, &addresses);
	std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
}

/* txn_base_chain_value_pad is chain_value's pad: its writer, while live. */
static void
txn_base_chain_value_pad(void *frame_)
{
	txn_base_chain_value_frame_t *frame = frame_;

	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
}

/*
 * txn_base_chain_value returns a chain tree entry's value, a record of 1, its
 * DDL's top-level xid, 2, its command, 3, the transform's bytes, 4, the xid of
 * the subtransaction that ran it, and 5, its commit's end, absent until a
 * save writes it.
 */
void
txn_base_chain_value(const TxnBasePending *pending, StdVec *result)
{
	txn_base_chain_value_frame_t frame = {.pending = pending, .result = result};

	std_landing(&frame, txn_base_chain_value_body, txn_base_chain_value_pad);
}

/* txn_base_external_frame_t is storage's state across its pad for an error it formats: the error, while live. */
typedef struct txn_base_external_frame_t
{
	ChunksErrorError *error;
	StdString	message;
} txn_base_external_frame_t;

/* txn_base_external_body formats the error's message and frees the error. */
static void
txn_base_external_body(void *frame_)
{
	txn_base_external_frame_t *frame = frame_;

	std_to_string(frame->error, chunks_error_Error_Display_fmt, &frame->message);
	chunks_error_Error_drop(frame->error);
}

/* txn_base_external_pad frees the error, which the body hadn't freed. */
static void
txn_base_external_pad(void *frame_)
{
	txn_base_external_frame_t *frame = frame_;

	chunks_error_Error_drop(frame->error);
}

/*
 * txn_base_storage returns the error of a failure of the storage a reader
 * reads, taking it: a host's own as it is, stored data that doesn't read as
 * Postgres' data corruption, and any other an internal error.
 */
HostError
txn_base_storage(ChunksErrorError *error)
{
	txn_base_external_frame_t frame = {.error = error};

	switch (error->tag)
	{
		case CHUNKS_ERROR_ERROR_HOST:
			return error->u.host;
		case CHUNKS_ERROR_ERROR_CORRUPT:
			return host_Error_fork((const uint8 *) "XX001", &error->u.corrupt);
		case CHUNKS_ERROR_ERROR_DAMAGED:
			std_vec_free_Checked(&error->u.damaged.path.bytes, 1, 1);
			return host_Error_fork((const uint8 *) "XX001", &error->u.damaged.message);
		case CHUNKS_ERROR_ERROR_MISSING:
			std_landing(&frame, txn_base_external_body, txn_base_external_pad);
			return host_Error_fork((const uint8 *) "XX001", &frame.message);
		case CHUNKS_ERROR_ERROR_REFUSED:
			return host_Error_fork((const uint8 *) "XX000", &error->u.refused);
		case CHUNKS_ERROR_ERROR_INVARIANT:
			return host_Error_fork((const uint8 *) "XX000", &error->u.invariant);
		case CHUNKS_ERROR_ERROR_EXTERNAL:
			std_landing(&frame, txn_base_external_body, txn_base_external_pad);
			return host_Error_fork((const uint8 *) "XX000", &frame.message);
	}
	pg_unreachable();
}

/* txn_base_record_uint reads a record's integer field, refusing one absent or outside a range as data corruption. */
static bool
txn_base_record_uint(const ChunksRecordRecord *record, uint32 number, uint64 least, uint64 most, uint64 *result,
					 HostError *error)
{
	const ChunksRecordField *field = chunks_record_Record_field(record, number);
	ChunksErrorError failed;

	if (field == NULL)
	{
		*error = txn_base_corrupt();
		return false;
	}
	if (!chunks_record_uint(field, result, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (*result < least || *result > most)
	{
		*error = txn_base_corrupt();
		return false;
	}
	return true;
}

/*
 * txn_base_read_fields fills a vector with the fields of a record's bytes, giving the
 * storage's error of bytes that aren't a record.
 */
static bool
txn_base_read_fields(const uint8 *bytes, size_t bytes_len, StdVec *fields, HostError *error)
{
	ChunksErrorError failed;

	if (chunks_record_fields(bytes, bytes_len, fields, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/* txn_base_numbered reports whether fields are numbered from 1 to a last number, one each. */
static bool
txn_base_numbered(const StdVec *fields, uint32 last)
{
	if (fields->len != last)
		return false;
	for (uint32 i = 0; i < last; i++)
		if (STD_VEC_AT(fields, ChunksRecordField, i).number != i + 1)
			return false;
	return true;
}


/*
 * txn_base_chain_read_frame_t is chain_read's state across its pad: its
 * fields, its record, and the transform it copied, while live.
 */
typedef struct txn_base_chain_read_frame_t
{
	const uint8 *value;
	size_t		value_len;
	TxnBasePending *result;
	HostError  *error;
	bool		ok;
	StdVec		fields;
	bool		fields_live;
	ChunksRecordRecord record;
	bool		record_live;
	bool		transform_live;
} txn_base_chain_read_frame_t;

/* txn_base_chain_read_body is chain_read's body: the fields' numbers checked, then the record read. */
static void
txn_base_chain_read_body(void *frame_)
{
	txn_base_chain_read_frame_t *frame = frame_;
	TxnBasePending *result = frame->result;
	ChunksErrorError failed;
	uint64		command;
	bool		ended;
	const ChunksRecordField *transform;

	if (!txn_base_read_fields(frame->value, frame->value_len, &frame->fields, frame->error))
		return;
	frame->fields_live = true;
	ended = txn_base_numbered(&frame->fields, 5);
	if (!txn_base_numbered(&frame->fields, 4) && !ended)
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	frame->fields_live = false;
	std_vec_free_Checked(&frame->fields, TXN_BASE_FIELD);
	if (!chunks_record_Record_value(frame->value, frame->value_len, TXN_BASE_NO_ADDRESSES, 0, &frame->record, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->record_live = true;
	if (!txn_base_record_uint(&frame->record, 1, 0, PG_UINT64_MAX, &result->xid, frame->error) ||
		!txn_base_record_uint(&frame->record, 2, 0, PG_UINT64_MAX, &command, frame->error))
		return;
	if (command > TXN_BASE_U32_MAX)
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	result->command = (uint32) command;
	if (!txn_base_record_uint(&frame->record, 4, 0, PG_UINT64_MAX, &result->sub, frame->error))
		return;
	transform = chunks_record_Record_field(&frame->record, 3);
	std_slice_to_vec_Checked(&result->transform, transform->bytes, transform->bytes_len, 1, 1);
	frame->transform_live = true;
	result->has_end = ended;
	if (ended && !txn_base_record_uint(&frame->record, 5, 0, PG_UINT64_MAX, &result->end, frame->error))
		return;
	frame->transform_live = false;
	frame->record_live = false;
	chunks_record_Record_drop(&frame->record);
	frame->ok = true;
}

/* txn_base_chain_read_pad is chain_read's pad, and its cleanup on an error: what it holds live. */
static void
txn_base_chain_read_pad(void *frame_)
{
	txn_base_chain_read_frame_t *frame = frame_;

	if (frame->transform_live)
		std_vec_free_Checked(&frame->result->transform, 1, 1);
	frame->transform_live = false;
	if (frame->record_live)
		chunks_record_Record_drop(&frame->record);
	frame->record_live = false;
	if (frame->fields_live)
		std_vec_free_Checked(&frame->fields, TXN_BASE_FIELD);
	frame->fields_live = false;
}

/*
 * txn_base_chain_read reads a chain tree entry's value as chain_value writes
 * it, refusing as data corruption a field absent, out of sequence, or that it
 * doesn't have, and a command of 2^32 or more.
 */
bool
txn_base_chain_read(const uint8 *value, size_t value_len, TxnBasePending *result, HostError *error)
{
	txn_base_chain_read_frame_t frame = {.value = value, .value_len = value_len, .result = result, .error = error};

	std_landing(&frame, txn_base_chain_read_body, txn_base_chain_read_pad);
	txn_base_chain_read_pad(&frame);
	return frame.ok;
}

/*
 * txn_base_chain_of_frame_t is chain_of's state across its pads: its arguments, the
 * chain read so far, and in its scope the scope's error, its cursor, and the
 * entry it read, while live.
 */
typedef struct txn_base_chain_of_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreTree tree;
	StdVec	   *chain;
	HostError  *error;
	bool		ok;
	TreesPinPin *pin;
	ChunksErrorError *failed;
	bool		scope_ok;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TxnBasePending pending;
	bool		pending_live;
} txn_base_chain_of_frame_t;

/* txn_base_host_failed sets a storage error that holds a host's error. */
static void
txn_base_host_failed(ChunksErrorError *error, HostError host)
{
	error->tag = CHUNKS_ERROR_ERROR_HOST;
	error->u.host = host;
}

/*
 * txn_base_chain_of_scope_body is chain_of's scope's body: each entry read in
 * order, its key checked as the next place.
 */
static void
txn_base_chain_of_scope_body(void *frame_)
{
	txn_base_chain_of_frame_t *frame = frame_;
	HostError	failed;

	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->tree), &frame->cursor,
								   frame->failed))
		return;
	frame->cursor_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);
		uint8		expected[4];

		if (frame->chain->len >= PG_UINT32_MAX)
		{
			txn_base_host_failed(frame->failed, txn_base_corrupt());
			return;
		}
		txn_base_chain_key((uint32) (frame->chain->len + 1), expected);
		if (key.len != 4 || memcmp(key.ptr, expected, 4) != 0)
		{
			txn_base_host_failed(frame->failed, txn_base_corrupt());
			return;
		}
		if (!txn_base_chain_read(value.ptr, value.len, &frame->pending, &failed))
		{
			txn_base_host_failed(frame->failed, failed);
			return;
		}
		frame->pending_live = true;
		std_vec_push_Checked(frame->chain, &frame->pending, TXN_BASE_PENDING_SIZE);
		frame->pending_live = false;
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->failed))
			return;
	}
	frame->scope_ok = true;
}

/*
 * txn_base_chain_of_scope_pad is chain_of's scope's pad, and its cleanup: the
 * entry it read, then its cursor, while live.
 */
static void
txn_base_chain_of_scope_pad(void *frame_)
{
	txn_base_chain_of_frame_t *frame = frame_;

	if (frame->pending_live)
		txn_base_Pending_drop(&frame->pending);
	frame->pending_live = false;
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* txn_base_chain_of_scope is chain_of's function in its pin's scope. */
static bool
txn_base_chain_of_scope(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	txn_base_chain_of_frame_t *frame = captures;

	frame->io = io;
	frame->pin = pin;
	frame->failed = error;
	std_landing(frame, txn_base_chain_of_scope_body, txn_base_chain_of_scope_pad);
	txn_base_chain_of_scope_pad(frame);
	return frame->scope_ok;
}

/* txn_base_chain_drop frees a chain of transforms' entries. */
static void
txn_base_chain_drop(StdVec *chain)
{
	for (size_t i = 0; i < chain->len; i++)
		txn_base_Pending_drop(&STD_VEC_AT(chain, TxnBasePending, i));
	std_vec_free_Checked(chain, TXN_BASE_PENDING_SIZE);
}

/*
 * txn_base_chain_of_body is chain_of's body: its scope run over the chain's
 * tree, a failure made the storage's error.
 */
static void
txn_base_chain_of_body(void *frame_)
{
	txn_base_chain_of_frame_t *frame = frame_;
	ChunksErrorError failed;

	frame->ok = trees_pin_pin_ResultUnit(frame->io, frame->store, txn_base_chain_of_scope, frame, NULL, &failed);
	if (!frame->ok)
		*frame->error = txn_base_storage(&failed);
}

/* txn_base_chain_of_pad is chain_of's pad: the chain read so far. */
static void
txn_base_chain_of_pad(void *frame_)
{
	txn_base_chain_of_frame_t *frame = frame_;

	txn_base_chain_drop(frame->chain);
}

/*
 * txn_base_chain_of reads a base's chain tree whole, oldest first, none for a
 * base with no chain, refusing as data corruption a key that isn't the next
 * place in the chain.
 */
bool
txn_base_chain_of(HostIoIo *io, TreesStoreNodeStore store, bool has_chain, TreesStoreTree chain, StdVec *result,
				  HostError *error)
{
	txn_base_chain_of_frame_t frame = {.io = io, .store = store, .tree = chain, .chain = result, .error = error};

	std_vec_new(result, TXN_BASE_PENDING_SIZE);
	if (!has_chain)
		return true;
	std_landing(&frame, txn_base_chain_of_body, txn_base_chain_of_pad);
	if (!frame.ok)
		txn_base_chain_drop(result);
	return frame.ok;
}

/*
 * txn_base_tuple_id_key writes a tuple id's key in a base's tuple id tree,
 * its block's four bytes and then its offset's two, big-endian, so that the
 * tree orders its entries as their ctids order.
 */
void
txn_base_tuple_id_key(uint64 tuple_id, uint8 result[6])
{
	for (int i = 0; i < 6; i++)
		result[i] = (uint8) (tuple_id >> (8 * (5 - i)));
}

/*
 * txn_base_tuple_id_of returns the tuple id a key of the tuple id tree, or of
 * the pending tree, holds, its six bytes big-endian; any other key is data
 * corruption.
 */
bool
txn_base_tuple_id_of(const uint8 *key, size_t key_len, uint64 *result, HostError *error)
{
	if (key_len != 6)
	{
		*error = TXN_BASE_FORK("XX001", "a tuple id tree's key is corrupt");
		return false;
	}
	*result = 0;
	for (int i = 0; i < 6; i++)
		*result = (*result << 8) | key[i];
	return true;
}

/* txn_base_put_u32_le appends a u32's four bytes, little-endian. */
static void
txn_base_put_u32_le(StdVec *out, uint32 value)
{
	uint8		bytes[4] = {(uint8) value, (uint8) (value >> 8), (uint8) (value >> 16), (uint8) (value >> 24)};

	std_vec_extend_from_slice_Checked(out, bytes, 4, 1, 1);
}

/*
 * txn_base_pair_value returns a tuple id tree pair's value: the row's key,
 * then its heap bytes, four bytes little-endian.
 */
void
txn_base_pair_value(const uint8 *key, size_t key_len, uint32 heap_bytes, StdVec *result)
{
	std_vec_with_capacity_Checked(result, key_len + 4, 1, 1);
	std_vec_extend_from_slice_Checked(result, key, key_len, 1, 1);
	txn_base_put_u32_le(result, heap_bytes);
}

/*
 * txn_base_pending_value returns a pending tuple id's entry's value in the
 * base's pending tree: its row's ender's full xid, 0 when the ender aborted,
 * with TXN_BASE_OWN set when the ender is the row's own writer, then its heap
 * bytes, 0 when a prune already gave them back, then the value ids its row
 * names, each little-endian.
 */
void
txn_base_pending_value(uint64 ender, uint32 heap_bytes, const uint32 *ids, size_t ids_len, StdVec *result)
{
	uint8		bytes[8];

	std_vec_with_capacity_Checked(result, 12 + 4 * ids_len, 1, 1);
	for (int i = 0; i < 8; i++)
		bytes[i] = (uint8) (ender >> (8 * i));
	std_vec_extend_from_slice_Checked(result, bytes, 8, 1, 1);
	txn_base_put_u32_le(result, heap_bytes);
	for (size_t i = 0; i < ids_len; i++)
		txn_base_put_u32_le(result, ids[i]);
}

/*
 * txn_base_pending_read reads a pending tuple id's entry's value as
 * pending_value writes it, refusing a length that isn't 12 bytes and a whole
 * number of value ids as data corruption.
 */
bool
txn_base_pending_read(const uint8 *value, size_t value_len, uint64 *result_0, uint32 *result_1, StdVec *result_2,
					  HostError *error)
{
	size_t		ids;

	if (value_len < 12 || (value_len - 12) % 4 != 0)
	{
		*error = TXN_BASE_FORK("XX001", "a pending tuple id's entry is corrupt");
		return false;
	}
	*result_0 = std_u64_from_le_bytes(value);
	*result_1 = std_u32_from_le_bytes(value + 8);
	ids = (value_len - 12) / 4;
	std_vec_with_capacity_Checked(result_2, ids, sizeof(uint32), _Alignof(uint32));
	for (size_t i = 0; i < ids; i++)
	{
		uint32		id = std_u32_from_le_bytes(value + 12 + 4 * i);

		std_vec_push_Checked(result_2, &id, sizeof(uint32), _Alignof(uint32));
	}
	return true;
}

/*
 * txn_base_pair_parts returns the row's key and its heap bytes from a tuple id
 * tree pair's value, refusing a value shorter than five bytes or with heap
 * bytes of 0 as data corruption.
 */
bool
txn_base_pair_parts(const uint8 *value, size_t value_len, StdBytes *result_0, uint32 *result_1, HostError *error)
{
	if (value_len < 5 || std_u32_from_le_bytes(value + value_len - 4) == 0)
	{
		*error = TXN_BASE_FORK("XX001", "a tuple id tree's pair is corrupt");
		return false;
	}
	result_0->ptr = value;
	result_0->len = value_len - 4;
	*result_1 = std_u32_from_le_bytes(value + value_len - 4);
	return true;
}

/* txn_base_Sizes_summed returns two sizes added field by field. */
TxnBaseSizes
txn_base_Sizes_summed(TxnBaseSizes self, TxnBaseSizes other)
{
	TxnBaseSizes sizes;

	sizes.entries = STD_ADD(u64, self.entries, other.entries, 380, 22);
	sizes.leaf_bytes = STD_ADD(u64, self.leaf_bytes, other.leaf_bytes, 381, 25);
	sizes.key_bytes = STD_ADD(u64, self.key_bytes, other.key_bytes, 382, 24);
	sizes.heap_bytes = STD_ADD(u64, self.heap_bytes, other.heap_bytes, 383, 25);
	sizes.rows = STD_ADD(u64, self.rows, other.rows, 384, 19);
	return sizes;
}

/*
 * txn_base_ceil_div_wide returns the 128-bit product of two u64s divided by a divisor
 * of at least 1, rounded up, or u64's largest when that doesn't fit a u64.
 */
static uint64
txn_base_ceil_div_wide(uint64 a, uint64 b, uint64 divisor)
{
	StdU128		product = std_mul_u64_wide(a, b);
	uint64		quotient = 0;
	uint64		remainder = product.hi;

	if (product.hi >= divisor)
		return PG_UINT64_MAX;
	for (int bit = 63; bit >= 0; bit--)
	{
		bool		carry = (remainder >> 63) != 0;

		remainder = (remainder << 1) | ((product.lo >> bit) & 1);
		quotient <<= 1;
		if (carry || remainder >= divisor)
		{
			remainder -= divisor;
			quotient |= 1;
		}
	}
	if (remainder == 0)
		return quotient;
	return quotient == PG_UINT64_MAX ? PG_UINT64_MAX : quotient + 1;
}

/*
 * txn_base_Sizes_counts returns the blocks a tree of entries of these sizes
 * takes, its leaves of the splitter's mean node size and the levels of nodes
 * above them, each entry above the leaves counting for a key of the mean
 * size, as a function of the sizes alone. A leaf holds at least one entry and
 * a node above the leaves at least two, as the splitter makes them.
 */
TxnBaseBlockCounts
txn_base_Sizes_counts(const TxnBaseSizes *self, uint32 target)
{
	TxnBaseBlockCounts counts = {0};
	uint64		fill;
	uint64		entry;
	uint64		level;

	if (self->entries == 0)
		return counts;
	fill = Max(trees_chunker_mean_node_size(target), 1);
	entry = STD_ADD(u64, trees_chunker_internal_entry_size(TXN_BASE_NO_BYTES, 0), self->key_bytes / self->entries, 397, 32);
	counts.leaves = Min(Max(txn_base_ceil_div_wide(self->leaf_bytes, 1, fill), 1), self->entries);
	level = counts.leaves;
	while (level > 1)
	{
		uint64		filled = txn_base_ceil_div_wide(level, entry, fill);

		level = Min(Max(filled, 1), level / 2 + level % 2);
		counts.internal = STD_ADD(u64, counts.internal, level, 404, 13);
		counts.height = STD_ADD(u8, counts.height, 1, 405, 13);
	}
	return counts;
}

/* txn_base_address writes a tree's field, or none for no tree. */
static void
txn_base_address(ChunksRecordRecordWriter *writer, uint32 number, bool has_tree, const TreesStoreTree *tree)
{
	chunks_TupleAddressLocator_t parts;

	if (!has_tree)
		return;
	parts.f0 = tree->address;
	parts.f1 = tree->locator;
	chunks_record_RecordWriter_addresses(writer, number, &parts, 1);
}

/* txn_base_encode_frame_t is encode's state across its pad: its writers, while live. */
typedef struct txn_base_encode_frame_t
{
	const TxnBaseBase *self;
	StdVec	   *result_0;
	StdVec	   *result_1;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	ChunksRecordRecordWriter nested;
	bool		nested_live;
	ChunksRecordRecordWriter written;
	bool		written_live;
} txn_base_encode_frame_t;

/* txn_base_encode_key writes the key's field, none for no key, from writers the frame holds. */
static void
txn_base_encode_key(txn_base_encode_frame_t *frame)
{
	const TxnBaseKey *key = &frame->self->key;

	if (key->tag == TXN_BASE_KEY_NONE)
		return;
	frame->nested = chunks_record_RecordWriter_new();
	frame->nested_live = true;
	if (key->tag == TXN_BASE_KEY_COLUMNS)
		for (size_t i = 0; i < key->u.columns.len; i++)
		{
			const TxnBaseKeyColumn *column = &STD_VEC_AT(&key->u.columns, TxnBaseKeyColumn, i);

			frame->written = chunks_record_RecordWriter_new();
			frame->written_live = true;
			chunks_record_RecordWriter_uint(&frame->written, 1, (uint64) column->attnum);
			chunks_record_RecordWriter_uint(&frame->written, 2, (uint64) column->class);
			chunks_record_RecordWriter_uint(&frame->written, 3, (uint64) column->collation);
			chunks_record_RecordWriter_uint(&frame->written, 4, (uint64) column->typid);
			if (column->compared)
				chunks_record_RecordWriter_uint(&frame->written, 5, 1);
			frame->written_live = false;
			chunks_record_RecordWriter_record(&frame->nested, (uint32) (i + 1), &frame->written);
		}
	frame->nested_live = false;
	chunks_record_RecordWriter_record(&frame->writer, TXN_BASE_KEY, &frame->nested);
}

/* txn_base_Sizes_each writes the sizes in their fields' order. */
static void
txn_base_Sizes_each(const TxnBaseSizes *self, uint64 result[5])
{
	result[0] = self->entries;
	result[1] = self->leaf_bytes;
	result[2] = self->key_bytes;
	result[3] = self->heap_bytes;
	result[4] = self->rows;
}

/* txn_base_encode_body is encode's body: the record's fields written in order. */
static void
txn_base_encode_body(void *frame_)
{
	txn_base_encode_frame_t *frame = frame_;
	const TxnBaseBase *self = frame->self;
	uint64		sizes[5];

	txn_base_Sizes_each(&self->sizes, sizes);
	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	txn_base_address(&frame->writer, TXN_BASE_ROOT, self->has_root, &self->root);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_FOLDED, self->folded);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_MAX_FOLDED, self->max_folded);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_EPOCH, (uint64) self->epoch);
	txn_base_address(&frame->writer, TXN_BASE_TUPLE_IDS, self->has_tuple_ids, &self->tuple_ids);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_COUNTER, self->counter);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_FRESH_END, self->fresh_end);
	txn_base_address(&frame->writer, TXN_BASE_FREE_SPACE, self->has_free_space, &self->free_space);
	frame->nested = chunks_record_RecordWriter_new();
	frame->nested_live = true;
	for (uint32 number = 1; number <= 5; number++)
		chunks_record_RecordWriter_uint(&frame->nested, number, sizes[number - 1]);
	frame->nested_live = false;
	chunks_record_RecordWriter_record(&frame->writer, TXN_BASE_SIZES, &frame->nested);
	if (self->has_index)
		chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_INDEX, (uint64) self->index);
	txn_base_encode_key(frame);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_TABLE, (uint64) self->table);
	txn_base_address(&frame->writer, TXN_BASE_PENDING, self->has_pending, &self->pending);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_MAX_ENDED, self->max_ended);
	if (self->has_chain)
		txn_base_address(&frame->writer, TXN_BASE_CHAIN, true, &self->chain);
	else
		chunks_record_RecordWriter_field(&frame->writer, TXN_BASE_CHAIN, TXN_BASE_NO_BYTES, 0);
	chunks_record_RecordWriter_uint(&frame->writer, TXN_BASE_READ, self->read);
	frame->writer_live = false;
	chunks_record_RecordWriter_finish_value(&frame->writer, frame->result_0, frame->result_1);
}

/* txn_base_encode_pad is encode's pad: its writers, while live. */
static void
txn_base_encode_pad(void *frame_)
{
	txn_base_encode_frame_t *frame = frame_;

	if (frame->written_live)
		chunks_record_RecordWriter_drop(&frame->written);
	if (frame->nested_live)
		chunks_record_RecordWriter_drop(&frame->nested);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
}

/*
 * txn_base_Base_encode returns the record's bytes and the offsets of its
 * addresses with their locators, as the base tree's entry holds them.
 */
void
txn_base_Base_encode(const TxnBaseBase *self, StdVec *result_0, StdVec *result_1)
{
	txn_base_encode_frame_t frame = {.self = self, .result_0 = result_0, .result_1 = result_1};

	std_landing(&frame, txn_base_encode_body, txn_base_encode_pad);
}

/*
 * txn_base_decode_frame_t is decode's state across its pad: the record, its nested
 * records, the fields it checks, and the key's columns, while live.
 */
typedef struct txn_base_decode_frame_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	const chunks_TupleU32Locator_t *addresses;
	size_t		addresses_len;
	TxnBaseBase *result;
	HostError  *error;
	bool		ok;
	ChunksRecordRecord record;
	bool		record_live;
	ChunksRecordRecord held;
	bool		held_live;
	ChunksRecordRecord key;
	bool		key_live;
	ChunksRecordRecord column;
	bool		column_live;
	StdVec		fields;
	bool		fields_live;
	StdVec		key_fields;
	bool		key_fields_live;
	StdVec		columns;
	bool		columns_live;
} txn_base_decode_frame_t;

/* txn_base_decode_fields reads the fields of bytes into the frame, live until decode_fields_free frees them. */
static bool
txn_base_decode_fields(txn_base_decode_frame_t *frame, const uint8 *bytes, size_t bytes_len)
{
	if (!txn_base_read_fields(bytes, bytes_len, &frame->fields, frame->error))
		return false;
	frame->fields_live = true;
	return true;
}

/* txn_base_decode_fields_free frees the fields the frame read. */
static void
txn_base_decode_fields_free(txn_base_decode_frame_t *frame)
{
	frame->fields_live = false;
	std_vec_free_Checked(&frame->fields, TXN_BASE_FIELD);
}

/*
 * txn_base_decode_outside reports whether a field the frame read has a number
 * outside a range and isn't another number.
 */
static bool
txn_base_decode_outside(const txn_base_decode_frame_t *frame, uint32 least, uint32 most, uint32 other, uint32 last)
{
	for (size_t i = 0; i < frame->fields.len; i++)
	{
		uint32		number = STD_VEC_AT(&frame->fields, ChunksRecordField, i).number;

		if ((number < least || number > most) && number != other && number != last)
			return true;
	}
	return false;
}

/* txn_base_decode_nested reads a field's nested record into one of the frame's records, which it marks live. */
static bool
txn_base_decode_nested(txn_base_decode_frame_t *frame, const ChunksRecordRecord *record, const ChunksRecordField *field,
					   ChunksRecordRecord *nested, bool *live)
{
	ChunksErrorError failed;

	if (!chunks_record_Record_nested(record, field, nested, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	*live = true;
	return true;
}

/*
 * txn_base_decode_tree reads a tree's field, none when it is absent, refusing as data
 * corruption a field with no entry in the address table or that holds no
 * address.
 */
static bool
txn_base_decode_tree(txn_base_decode_frame_t *frame, uint32 number, bool *has_tree, TreesStoreTree *tree)
{
	const ChunksRecordField *field = chunks_record_Record_field(&frame->record, number);
	ChunksErrorError failed;
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	bool		located = false;
	bool		has_address;

	*has_tree = false;
	if (field == NULL)
		return true;
	for (size_t i = 0; i < frame->addresses_len && !located; i++)
		located = (size_t) frame->addresses[i].f0 == field->at;
	if (!located)
	{
		*frame->error = txn_base_corrupt();
		return false;
	}
	if (!chunks_record_Record_address(&frame->record, number, &has_address, &address, &locator, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	if (!has_address)
	{
		*frame->error = txn_base_corrupt();
		return false;
	}
	*has_tree = true;
	*tree = trees_store_Tree_decoded(address, locator);
	return true;
}

/*
 * txn_base_decode_column reads a key's column from its nested record, refusing as data
 * corruption a field missing or out of sequence, an attribute number out of
 * range or repeated, an object id of 2^32 or more, and a field it doesn't
 * have.
 */
static bool
txn_base_decode_column(txn_base_decode_frame_t *frame, const ChunksRecordField *nested, TxnBaseKeyColumn *column)
{
	uint64		value;
	bool		compared;

	if (!txn_base_decode_fields(frame, nested->bytes, nested->bytes_len))
		return false;
	compared = txn_base_numbered(&frame->fields, 5);
	if (!txn_base_numbered(&frame->fields, 4) && !compared)
	{
		*frame->error = txn_base_corrupt();
		return false;
	}
	txn_base_decode_fields_free(frame);
	if (!txn_base_decode_nested(frame, &frame->key, nested, &frame->column, &frame->column_live) ||
		!txn_base_record_uint(&frame->column, 1, 1, TXN_BASE_ATTNUM_MAX, &value, frame->error))
		return false;
	column->attnum = (uint16) value;
	for (size_t i = 0; i < frame->columns.len; i++)
		if (STD_VEC_AT(&frame->columns, TxnBaseKeyColumn, i).attnum == column->attnum)
		{
			*frame->error = txn_base_corrupt();
			return false;
		}
	if (!txn_base_record_uint(&frame->column, 2, 0, TXN_BASE_U32_MAX, &value, frame->error))
		return false;
	column->class = (uint32) value;
	if (!txn_base_record_uint(&frame->column, 3, 0, TXN_BASE_U32_MAX, &value, frame->error))
		return false;
	column->collation = (uint32) value;
	if (!txn_base_record_uint(&frame->column, 4, 0, TXN_BASE_U32_MAX, &value, frame->error))
		return false;
	column->typid = (uint32) value;
	if (compared && !txn_base_record_uint(&frame->column, 5, 1, 1, &value, frame->error))
		return false;
	column->compared = compared;
	frame->column_live = false;
	chunks_record_Record_drop(&frame->column);
	return true;
}

/*
 * txn_base_key reads field 11's nested record, its bytes and its record held in
 * the frame: unknown when empty, and otherwise its columns, each field's
 * number checked as the next before its column is read.
 */
static bool
txn_base_key(txn_base_decode_frame_t *frame, const ChunksRecordField *field, TxnBaseKey *key)
{
	ChunksErrorError failed;

	if (!chunks_record_fields(field->bytes, field->bytes_len, &frame->key_fields, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->key_fields_live = true;
	if (frame->key_fields.len == 0)
	{
		key->tag = TXN_BASE_KEY_UNKNOWN;
		return true;
	}
	std_vec_new(&frame->columns, TXN_BASE_COLUMN);
	frame->columns_live = true;
	for (size_t i = 0; i < frame->key_fields.len; i++)
	{
		uint32		number = STD_VEC_AT(&frame->key_fields, ChunksRecordField, i).number;
		const ChunksRecordField *nested;
		TxnBaseKeyColumn column;

		if (number != i + 1)
		{
			*frame->error = txn_base_corrupt();
			return false;
		}
		nested = chunks_record_Record_field(&frame->key, number);
		if (nested == NULL)
		{
			*frame->error = txn_base_corrupt();
			return false;
		}
		if (!txn_base_decode_column(frame, nested, &column))
			return false;
		std_vec_push_Checked(&frame->columns, &column, TXN_BASE_COLUMN);
	}
	key->tag = TXN_BASE_KEY_COLUMNS;
	key->u.columns = frame->columns;
	return true;
}

/* txn_base_decode_body is decode's body: the record read, every field checked in the format's order. */
static void
txn_base_decode_body(void *frame_)
{
	txn_base_decode_frame_t *frame = frame_;
	TxnBaseBase base = {.key = {.tag = TXN_BASE_KEY_NONE}};
	ChunksErrorError failed;
	const ChunksRecordField *sizes;
	const ChunksRecordField *key;
	const ChunksRecordField *chain;
	uint64		value;

	if (!chunks_record_Record_value(frame->bytes, frame->bytes_len, frame->addresses, frame->addresses_len,
									&frame->record, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->record_live = true;
	if (!txn_base_decode_fields(frame, frame->bytes, frame->bytes_len))
		return;
	if (txn_base_decode_outside(frame, TXN_BASE_ROOT, TXN_BASE_MAX_ENDED, TXN_BASE_CHAIN, TXN_BASE_READ))
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	txn_base_decode_fields_free(frame);
	sizes = chunks_record_Record_field(&frame->record, TXN_BASE_SIZES);
	if (sizes == NULL)
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	if (!txn_base_decode_nested(frame, &frame->record, sizes, &frame->held, &frame->held_live) ||
		!txn_base_decode_fields(frame, sizes->bytes, sizes->bytes_len))
		return;
	if (txn_base_decode_outside(frame, 1, 5, 1, 1))
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	txn_base_decode_fields_free(frame);
	base.has_index = chunks_record_Record_field(&frame->record, TXN_BASE_INDEX) != NULL;
	if (base.has_index)
	{
		if (!txn_base_record_uint(&frame->record, TXN_BASE_INDEX, 0, TXN_BASE_U32_MAX, &value, frame->error))
			return;
		base.index = (uint32) value;
	}
	if (!txn_base_decode_tree(frame, TXN_BASE_ROOT, &base.has_root, &base.root) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_FOLDED, 0, PG_UINT64_MAX, &base.folded, frame->error) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_MAX_FOLDED, 0, PG_UINT64_MAX, &base.max_folded, frame->error) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_EPOCH, 0, TXN_BASE_U32_MAX, &value, frame->error))
		return;
	base.epoch = (uint32) value;
	if (!txn_base_decode_tree(frame, TXN_BASE_TUPLE_IDS, &base.has_tuple_ids, &base.tuple_ids) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_COUNTER, 0, TXN_BASE_TUPLE_ID_MAX, &base.counter, frame->error) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_FRESH_END, 0, TXN_BASE_TUPLE_ID_MAX, &base.fresh_end, frame->error) ||
		!txn_base_decode_tree(frame, TXN_BASE_FREE_SPACE, &base.has_free_space, &base.free_space) ||
		!txn_base_record_uint(&frame->held, 1, 0, PG_UINT64_MAX, &base.sizes.entries, frame->error) ||
		!txn_base_record_uint(&frame->held, 2, 0, PG_UINT64_MAX, &base.sizes.leaf_bytes, frame->error) ||
		!txn_base_record_uint(&frame->held, 3, 0, PG_UINT64_MAX, &base.sizes.key_bytes, frame->error) ||
		!txn_base_record_uint(&frame->held, 4, 0, PG_UINT64_MAX, &base.sizes.heap_bytes, frame->error) ||
		!txn_base_record_uint(&frame->held, 5, 0, PG_UINT64_MAX, &base.sizes.rows, frame->error) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_TABLE, 0, TXN_BASE_U32_MAX, &value, frame->error))
		return;
	base.table = (uint32) value;
	key = chunks_record_Record_field(&frame->record, TXN_BASE_KEY);
	if (key != NULL && base.has_index)
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	if (key != NULL &&
		(!txn_base_decode_nested(frame, &frame->record, key, &frame->key, &frame->key_live) ||
		 !txn_base_key(frame, key, &base.key)))
		return;
	if (!txn_base_decode_tree(frame, TXN_BASE_PENDING, &base.has_pending, &base.pending) ||
		!txn_base_record_uint(&frame->record, TXN_BASE_MAX_ENDED, 0, PG_UINT64_MAX, &base.max_ended, frame->error))
		return;
	chain = chunks_record_Record_field(&frame->record, TXN_BASE_CHAIN);
	if (chain == NULL)
	{
		*frame->error = txn_base_corrupt();
		return;
	}
	if (chain->bytes_len != 0 && !txn_base_decode_tree(frame, TXN_BASE_CHAIN, &base.has_chain, &base.chain))
		return;
	if (!txn_base_record_uint(&frame->record, TXN_BASE_READ, 0, base.folded, &base.read, frame->error))
		return;
	frame->columns_live = false;
	*frame->result = base;
	frame->ok = true;
}

/* txn_base_decode_pad is decode's pad, and its cleanup: what it holds live. */
static void
txn_base_decode_pad(void *frame_)
{
	txn_base_decode_frame_t *frame = frame_;

	if (frame->columns_live)
		std_vec_free_Checked(&frame->columns, TXN_BASE_COLUMN);
	frame->columns_live = false;
	if (frame->fields_live)
		std_vec_free_Checked(&frame->fields, TXN_BASE_FIELD);
	frame->fields_live = false;
	if (frame->key_fields_live)
		std_vec_free_Checked(&frame->key_fields, TXN_BASE_FIELD);
	frame->key_fields_live = false;
	if (frame->column_live)
		chunks_record_Record_drop(&frame->column);
	frame->column_live = false;
	if (frame->key_live)
		chunks_record_Record_drop(&frame->key);
	frame->key_live = false;
	if (frame->held_live)
		chunks_record_Record_drop(&frame->held);
	frame->held_live = false;
	if (frame->record_live)
		chunks_record_Record_drop(&frame->record);
	frame->record_live = false;
}

/*
 * txn_base_Base_decode reads a record from its bytes and the offsets of its
 * addresses with their locators, refusing every form but the one encode
 * writes as data corruption: a field it doesn't know, fields out of order or
 * repeated, an integer absent or past its width, an address present and empty
 * or with no entry in the address table, and bytes cut short.
 */
bool
txn_base_Base_decode(const uint8 *bytes, size_t bytes_len, const chunks_TupleU32Locator_t *addresses,
					 size_t addresses_len, TxnBaseBase *result, HostError *error)
{
	txn_base_decode_frame_t frame = {.bytes = bytes, .bytes_len = bytes_len, .addresses = addresses,
	.addresses_len = addresses_len, .result = result, .error = error};

	std_landing(&frame, txn_base_decode_body, txn_base_decode_pad);
	txn_base_decode_pad(&frame);
	return frame.ok;
}

#ifdef DOLTGRES_TEST_TXN

/* txn_base_Base_Clone_clone copies a base record into another, as the tests' derived Clone does. */
void
txn_base_Base_Clone_clone(const TxnBaseBase *self, TxnBaseBase *result)
{
	*result = *self;
	txn_base_Key_Clone_clone(&self->key, &result->key);
}

/* txn_base_tests_tree returns a made-up tree name, its address and its locator from a number. */
static TreesStoreTree
txn_base_tests_tree(uint8 n)
{
	ChunksAddressAddress address;

	memset(address.f0, n, sizeof(address.f0));
	return trees_store_Tree_decoded(address, chunks_trailer_Locator_at((uint64) n, 64 * (uint64) n, 64));
}

/* txn_base_tests_full returns a record with every field present, each integer at its width's largest. */
TxnBaseBase
txn_base_tests_full(void)
{
	TxnBaseBase base = {
		.has_root = true, .root = txn_base_tests_tree(1),
		.folded = PG_UINT64_MAX, .max_folded = PG_UINT64_MAX, .epoch = PG_UINT32_MAX,
		.has_tuple_ids = true, .tuple_ids = txn_base_tests_tree(2),
		.counter = TXN_BASE_TUPLE_ID_MAX, .fresh_end = TXN_BASE_TUPLE_ID_MAX,
		.has_free_space = true, .free_space = txn_base_tests_tree(3),
		.sizes = {.entries = PG_UINT64_MAX, .leaf_bytes = 0, .key_bytes = 1, .heap_bytes = PG_UINT64_MAX, .rows = 7},
		.table = PG_UINT32_MAX, .has_index = true, .index = PG_UINT32_MAX,
		.key = {.tag = TXN_BASE_KEY_NONE},
		.has_pending = true, .pending = txn_base_tests_tree(4),
		.max_ended = PG_UINT64_MAX,
		.has_chain = true, .chain = txn_base_tests_tree(5),
		.read = PG_UINT64_MAX,
	};

	return base;
}

/* txn_base_tests_empty returns a table's record with every field that can be absent absent and every integer zero. */
TxnBaseBase
txn_base_tests_empty(void)
{
	TxnBaseBase base = {.key = {.tag = TXN_BASE_KEY_NONE}};

	return base;
}

/* txn_base_tests_refused reports whether a record's bytes and addresses decode as data corruption. */
static bool
txn_base_tests_refused(const uint8 *bytes, size_t bytes_len, const chunks_TupleU32Locator_t *addresses, size_t addresses_len)
{
	TxnBaseBase base;
	HostError	error;
	bool		corrupt;

	if (txn_base_Base_decode(bytes, bytes_len, addresses, addresses_len, &base, &error))
	{
		txn_base_Base_drop(&base);
		return false;
	}
	corrupt = memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0;
	host_Error_drop(&error);
	return corrupt;
}

/* txn_base_tests_refused_vecs is refused of a record's bytes and addresses held in vectors, which it frees. */
static bool
txn_base_tests_refused_vecs(StdVec *bytes, StdVec *addresses)
{
	bool		corrupt = txn_base_tests_refused(bytes->ptr, bytes->len, addresses->ptr, addresses->len);

	std_vec_free_Checked(bytes, 1, 1);
	std_vec_free_Checked(addresses, TXN_BASE_PLACED);
	return corrupt;
}

/*
 * txn_base_tests_assert_refused panics with a message of a number between two
 * texts when a record's vectors aren't refused.
 */
static void
txn_base_tests_assert_refused(uint32 line, uint32 col, StdVec *bytes, StdVec *addresses, const char *before, const char *after,
							  uint32 number)
{
	StdStr		texts[2] = {{before, strlen(before)}, {after, strlen(after)}};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&texts[0], std_str_Display_fmt}, {&number, std_u32_Display_fmt},
	{&texts[1], std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	if (!txn_base_tests_refused_vecs(bytes, addresses))
		std_panic_fmt(RUST_FILE, line, col, &arguments);
}

/* txn_base_tests_decoded returns a record's decoding, panicking as unwrap does on an error. */
static TxnBaseBase
txn_base_tests_decoded(uint32 line, uint32 col, const StdVec *bytes, const StdVec *addresses)
{
	TxnBaseBase base;
	HostError	error;

	if (!txn_base_Base_decode(bytes->ptr, bytes->len, addresses->ptr, addresses->len, &base, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return base;
}

/* txn_base_tests_round_trips asserts that a base record decodes from its encoding as itself. */
static void
txn_base_tests_round_trips(uint32 line, const TxnBaseBase *base)
{
	StdVec		bytes;
	StdVec		addresses;
	TxnBaseBase back;

	txn_base_Base_encode(base, &bytes, &addresses);
	back = txn_base_tests_decoded(line, 31, &bytes, &addresses);
	STD_ASSERT_EQ(line, 13, txn_base_Base_PartialEq_eq(&back, base), &back, txn_base_Base_Debug_fmt, base,
				  txn_base_Base_Debug_fmt);
	txn_base_Base_drop(&back);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
}

/*
 * test_txn_base_tests_a_base_record_round_trips_at_its_limits: every field
 * round-trips at its limits, each integer at zero and at its width's largest,
 * each tree present and absent, each size known and unknown, and a table's
 * record and an index's.
 */
void
test_txn_base_tests_a_base_record_round_trips_at_its_limits(void)
{
	TxnBaseBase bases[5] = {txn_base_tests_full(), txn_base_tests_empty(), txn_base_tests_full(),
	txn_base_tests_empty(), txn_base_tests_empty()};

	bases[2].has_index = false;
	bases[2].index = 0;
	bases[3].has_index = true;
	bases[3].index = 0;
	bases[3].has_root = true;
	bases[3].root = txn_base_tests_tree(9);
	bases[4].folded = 200;
	bases[4].read = 149;
	for (int i = 0; i < lengthof(bases); i++)
		txn_base_tests_round_trips(605, &bases[i]);
}

/*
 * test_txn_base_tests_a_cut_short_or_unlocated_record_is_corrupt: bytes cut
 * short at every length, and a record with an address that has no entry in
 * the address table, decode as data corruption.
 */
void
test_txn_base_tests_a_cut_short_or_unlocated_record_is_corrupt(void)
{
	TxnBaseBase full = txn_base_tests_full();
	StdVec		bytes;
	StdVec		addresses;
	bool		unlocated;

	txn_base_Base_encode(&full, &bytes, &addresses);
	for (size_t len = 0; len < bytes.len; len++)
		if (!txn_base_tests_refused(bytes.ptr, len, addresses.ptr, addresses.len))
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("a record cut at "), STD_FMT_ARG(0), STD_FMT_LIT(" bytes decoded")};
			StdFmtArg	args[] = {{&len, std_usize_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};

			std_panic_fmt(RUST_FILE, 615, 13, &arguments);
		}
	unlocated = txn_base_tests_refused(bytes.ptr, bytes.len, (const chunks_TupleU32Locator_t *) addresses.ptr + 1,
									   addresses.len - 1);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
	STD_ASSERT(617, 9, unlocated, "refused(&bytes, &addresses[1..])");
}

/* TxnBaseTestsFormKind is how a write of every other form changes a field from its plain form. */
typedef enum TxnBaseTestsFormKind
{
	TXN_BASE_TESTS_FORM_PLAIN,
	TXN_BASE_TESTS_FORM_PAST,
	TXN_BASE_TESTS_FORM_EMPTY_ROOT,
	TXN_BASE_TESTS_FORM_WITHOUT,
	TXN_BASE_TESTS_FORM_ODD_SIZE,
	TXN_BASE_TESTS_FORM_UNKNOWN,
	TXN_BASE_TESTS_FORM_EMPTY_PENDING,
	TXN_BASE_TESTS_FORM_UNLOCATED_CHAIN,
} TxnBaseTestsFormKind;

/* TxnBaseTestsForm is a change to a record's plain form: its kind, and the field and value or the size it names. */
typedef struct TxnBaseTestsForm
{
	TxnBaseTestsFormKind kind;
	uint32		number;
	uint64		most;
	uint32		absent;
} TxnBaseTestsForm;

/* txn_base_tests_write_sizes writes the sizes' record of sizes from 1 to a last, each of a value, but an absent one. */
static void
txn_base_tests_write_sizes(ChunksRecordRecordWriter *writer, uint32 last, uint64 value, uint32 absent)
{
	ChunksRecordRecordWriter sizes = chunks_record_RecordWriter_new();

	for (uint32 size = 1; size <= last; size++)
		if (size != absent)
			chunks_record_RecordWriter_uint(&sizes, size, value);
	chunks_record_RecordWriter_record(writer, TXN_BASE_SIZES, &sizes);
}

/*
 * txn_base_tests_write_plain writes a field in its plain form: trees, the
 * index, and the key absent, and every integer zero.
 */
static void
txn_base_tests_write_plain(ChunksRecordRecordWriter *writer, uint32 number)
{
	switch (number)
	{
		case TXN_BASE_ROOT:
		case TXN_BASE_TUPLE_IDS:
		case TXN_BASE_FREE_SPACE:
		case TXN_BASE_PENDING:
		case TXN_BASE_INDEX:
		case TXN_BASE_KEY:
		case 15:
		case 17:
			break;
		case TXN_BASE_CHAIN:
			chunks_record_RecordWriter_field(writer, TXN_BASE_CHAIN, TXN_BASE_NO_BYTES, 0);
			break;
		case TXN_BASE_SIZES:
			txn_base_tests_write_sizes(writer, 5, 0, 0);
			break;
		default:
			chunks_record_RecordWriter_uint(writer, number, 0);
			break;
	}
}

/* txn_base_tests_write_changed writes a field in a form's change of it, or in its plain form. */
static void
txn_base_tests_write_changed(ChunksRecordRecordWriter *writer, const TxnBaseTestsForm *form, uint32 number)
{
	if (form->kind == TXN_BASE_TESTS_FORM_PAST && number == form->number)
		chunks_record_RecordWriter_uint(writer, number, form->most + 1);
	else if (form->kind == TXN_BASE_TESTS_FORM_EMPTY_ROOT && number == TXN_BASE_ROOT)
		chunks_record_RecordWriter_field(writer, TXN_BASE_ROOT, TXN_BASE_NO_BYTES, 0);
	else if (form->kind == TXN_BASE_TESTS_FORM_WITHOUT && number == TXN_BASE_SIZES)
		txn_base_tests_write_sizes(writer, 5, 0, form->absent);
	else if (form->kind == TXN_BASE_TESTS_FORM_ODD_SIZE && number == TXN_BASE_SIZES)
		txn_base_tests_write_sizes(writer, 6, 1, 0);
	else if (form->kind == TXN_BASE_TESTS_FORM_UNKNOWN && number == 15)
		chunks_record_RecordWriter_uint(writer, 15, 1);
	else if (form->kind == TXN_BASE_TESTS_FORM_EMPTY_PENDING && number == TXN_BASE_PENDING)
		chunks_record_RecordWriter_field(writer, TXN_BASE_PENDING, TXN_BASE_NO_BYTES, 0);
	else if (form->kind == TXN_BASE_TESTS_FORM_UNLOCATED_CHAIN && number == TXN_BASE_CHAIN)
		chunks_record_RecordWriter_uint(writer, TXN_BASE_CHAIN, 1);
	else
		txn_base_tests_write_plain(writer, number);
}

/* txn_base_tests_write_form writes every field from ROOT to READ in a form, but a skipped one, as a value. */
static void
txn_base_tests_write_form(uint32 skip, const TxnBaseTestsForm *form, StdVec *bytes, StdVec *addresses)
{
	ChunksRecordRecordWriter writer = chunks_record_RecordWriter_new();

	for (uint32 number = TXN_BASE_ROOT; number <= TXN_BASE_READ; number++)
		if (number != skip)
			txn_base_tests_write_changed(&writer, form, number);
	chunks_record_RecordWriter_finish_value(&writer, bytes, addresses);
}

/*
 * test_txn_base_tests_every_other_form_of_a_base_record_is_corrupt: each form
 * but the one the codec writes decodes as data corruption: each integer field
 * absent or past its width, an address field present and empty, the sizes
 * absent, each size absent, a size field it doesn't know, and a field it
 * doesn't know.
 */
void
test_txn_base_tests_every_other_form_of_a_base_record_is_corrupt(void)
{
	static const uint32 required[] = {TXN_BASE_FOLDED, TXN_BASE_MAX_FOLDED, TXN_BASE_EPOCH, TXN_BASE_COUNTER, TXN_BASE_FRESH_END, TXN_BASE_SIZES, TXN_BASE_TABLE, TXN_BASE_MAX_ENDED, TXN_BASE_CHAIN, TXN_BASE_READ};
	static const TxnBaseTestsForm past[] = {{TXN_BASE_TESTS_FORM_PAST, TXN_BASE_EPOCH, TXN_BASE_U32_MAX}, {TXN_BASE_TESTS_FORM_PAST, TXN_BASE_COUNTER, TXN_BASE_TUPLE_ID_MAX},
	{TXN_BASE_TESTS_FORM_PAST, TXN_BASE_FRESH_END, TXN_BASE_TUPLE_ID_MAX}, {TXN_BASE_TESTS_FORM_PAST, TXN_BASE_TABLE, TXN_BASE_U32_MAX}, {TXN_BASE_TESTS_FORM_PAST, TXN_BASE_INDEX, TXN_BASE_U32_MAX}, {TXN_BASE_TESTS_FORM_PAST, TXN_BASE_READ, 0}};
	static const TxnBaseTestsForm plain = {TXN_BASE_TESTS_FORM_PLAIN};
	StdVec		bytes;
	StdVec		addresses;
	TxnBaseBase base;
	HostError	error;
	bool		decodes;
	TxnBaseTestsForm form = {TXN_BASE_TESTS_FORM_PLAIN};

	txn_base_tests_write_form(0, &plain, &bytes, &addresses);
	decodes = txn_base_Base_decode(bytes.ptr, bytes.len, addresses.ptr, addresses.len, &base, &error);
	if (decodes)
		txn_base_Base_drop(&base);
	else
		host_Error_drop(&error);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
	STD_ASSERT(652, 9, decodes, "Base::decode(&bytes, &addresses).is_ok()");
	for (size_t i = 0; i < lengthof(required); i++)
	{
		txn_base_tests_write_form(required[i], &plain, &bytes, &addresses);
		txn_base_tests_assert_refused(655, 13, &bytes, &addresses, "field ", " absent decoded", required[i]);
	}
	for (size_t i = 0; i < lengthof(past); i++)
	{
		txn_base_tests_write_form(0, &past[i], &bytes, &addresses);
		txn_base_tests_assert_refused(671, 13, &bytes, &addresses, "field ", " past its width decoded", past[i].number);
	}
	form.kind = TXN_BASE_TESTS_FORM_EMPTY_ROOT;
	txn_base_tests_write_form(0, &form, &bytes, &addresses);
	STD_ASSERT_MSG(680, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "an empty address decoded");
	for (uint32 absent = 1; absent <= 5; absent++)
	{
		form.kind = TXN_BASE_TESTS_FORM_WITHOUT;
		form.absent = absent;
		txn_base_tests_write_form(0, &form, &bytes, &addresses);
		txn_base_tests_assert_refused(693, 13, &bytes, &addresses, "size ", " absent decoded", absent);
	}
	form.kind = TXN_BASE_TESTS_FORM_ODD_SIZE;
	txn_base_tests_write_form(0, &form, &bytes, &addresses);
	STD_ASSERT_MSG(706, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "an unknown size field decoded");
	form.kind = TXN_BASE_TESTS_FORM_UNKNOWN;
	txn_base_tests_write_form(0, &form, &bytes, &addresses);
	STD_ASSERT_MSG(714, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "an unknown field decoded");
	form.kind = TXN_BASE_TESTS_FORM_EMPTY_PENDING;
	txn_base_tests_write_form(0, &form, &bytes, &addresses);
	STD_ASSERT_MSG(722, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "an empty pending tree's address decoded");
	form.kind = TXN_BASE_TESTS_FORM_UNLOCATED_CHAIN;
	txn_base_tests_write_form(0, &form, &bytes, &addresses);
	STD_ASSERT_MSG(730, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "a chain field that isn't an address decoded");
}

/* TxnBaseTestsKeyedColumn is a column's fields as the key case writes them, each a number and a value. */
typedef struct TxnBaseTestsKeyedColumn
{
	int			len;
	std_TupleU32U64_t fields[6];
} TxnBaseTestsKeyedColumn;

/* TxnBaseTestsKeyedCase is a key case's columns and the field numbers its key record gives them. */
typedef struct TxnBaseTestsKeyedCase
{
	int			len;
	TxnBaseTestsKeyedColumn columns[2];
	int			numbers_len;
	uint32		numbers[2];
} TxnBaseTestsKeyedCase;

/* TXN_BASE_TESTS_WHOLE is a column's whole fields at an attribute number. */
#define TXN_BASE_TESTS_WHOLE(attnum) {4, {{1, (attnum)}, {2, 0}, {3, 0}, {4, 0}}}

/*
 * txn_base_tests_keyed writes a record of a table's base whose key record
 * gives columns of fields the case's numbers.
 */
static void
txn_base_tests_keyed(const TxnBaseTestsKeyedCase *keyed_case, StdVec *bytes, StdVec *addresses)
{
	ChunksRecordRecordWriter writer = chunks_record_RecordWriter_new();
	ChunksRecordRecordWriter key = chunks_record_RecordWriter_new();
	int			pairs = Min(keyed_case->len, keyed_case->numbers_len);

	chunks_record_RecordWriter_uint(&writer, TXN_BASE_FOLDED, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_MAX_FOLDED, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_EPOCH, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_COUNTER, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_FRESH_END, 0);
	txn_base_tests_write_sizes(&writer, 5, 0, 0);
	for (int i = 0; i < pairs; i++)
	{
		ChunksRecordRecordWriter written = chunks_record_RecordWriter_new();

		for (int f = 0; f < keyed_case->columns[i].len; f++)
			chunks_record_RecordWriter_uint(&written, keyed_case->columns[i].fields[f].f0,
											keyed_case->columns[i].fields[f].f1);
		chunks_record_RecordWriter_record(&key, keyed_case->numbers[i], &written);
	}
	chunks_record_RecordWriter_record(&writer, TXN_BASE_KEY, &key);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_TABLE, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_MAX_ENDED, 0);
	chunks_record_RecordWriter_field(&writer, TXN_BASE_CHAIN, TXN_BASE_NO_BYTES, 0);
	chunks_record_RecordWriter_uint(&writer, TXN_BASE_READ, 0);
	chunks_record_RecordWriter_finish_value(&writer, bytes, addresses);
}

/* txn_base_tests_keyed_column_Debug_fmt is a key case's column's Debug, a Vec<(u32, u64)>'s. */
static bool
txn_base_tests_keyed_column_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseTestsKeyedColumn *column = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (int i = 0; i < column->len; i++)
	{
		StdTupleRef pair = STD_TUPLE(2, STD_ITEM(&column->fields[i].f0, std_u32_Debug_fmt),
									 STD_ITEM(&column->fields[i].f1, std_u64_Debug_fmt));

		std_DebugList_entry(&l, &pair, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&l);
}

/* txn_base_tests_keyed_columns_Debug_fmt is a key case's columns' Debug, a Vec<Vec<(u32, u64)>>'s. */
static bool
txn_base_tests_keyed_columns_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseTestsKeyedCase *keyed_case = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (int i = 0; i < keyed_case->len; i++)
		std_DebugList_entry(&l, &keyed_case->columns[i], txn_base_tests_keyed_column_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* txn_base_tests_keyed_numbers_Debug_fmt is a key case's numbers' Debug, a Vec<u32>'s. */
static bool
txn_base_tests_keyed_numbers_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnBaseTestsKeyedCase *keyed_case = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (int i = 0; i < keyed_case->numbers_len; i++)
		std_DebugList_entry(&l, &keyed_case->numbers[i], std_u32_Debug_fmt);
	return std_DebugList_finish(&l);
}

/*
 * txn_base_tests_key_column returns the key case's column at an attribute
 * number, its class and type at u32's largest.
 */
static TxnBaseKeyColumn
txn_base_tests_key_column(uint16 attnum)
{
	TxnBaseKeyColumn column = {.attnum = attnum, .class = PG_UINT32_MAX, .typid = PG_UINT32_MAX};

	return column;
}

/* txn_base_tests_keyed_base returns a table's empty record under a key of columns. */
static TxnBaseBase
txn_base_tests_keyed_base(TxnBaseKeyTag tag, const TxnBaseKeyColumn *columns, size_t columns_len)
{
	TxnBaseBase base = txn_base_tests_empty();

	base.key.tag = tag;
	if (tag == TXN_BASE_KEY_COLUMNS)
		std_slice_to_vec_Checked(&base.key.u.columns, columns, columns_len, TXN_BASE_COLUMN);
	return base;
}

/*
 * test_txn_base_tests_a_storage_key_round_trips_and_every_other_form_is_corrupt:
 * a key round-trips unknown and with columns at their limits, a compared one
 * among them, and each other form decodes as data corruption: a column's field
 * missing or out of sequence, an attribute number of 0 or past 1,600 or
 * repeated, an object id of 2^32, a compared mark other than 1, a field a
 * column doesn't have, and the field on an index's storage.
 */
void
test_txn_base_tests_a_storage_key_round_trips_and_every_other_form_is_corrupt(void)
{
	static const TxnBaseTestsKeyedCase accepted = {2, {TXN_BASE_TESTS_WHOLE(1), TXN_BASE_TESTS_WHOLE(2)}, 2, {1, 2}};
	static const TxnBaseTestsKeyedCase cases[] = {
		{2, {TXN_BASE_TESTS_WHOLE(1), TXN_BASE_TESTS_WHOLE(2)}, 2, {1, 3}},
		{1, {TXN_BASE_TESTS_WHOLE(1)}, 1, {2}},
		{1, {TXN_BASE_TESTS_WHOLE(0)}, 1, {1}},
		{1, {TXN_BASE_TESTS_WHOLE(1601)}, 1, {1}},
		{2, {TXN_BASE_TESTS_WHOLE(1), TXN_BASE_TESTS_WHOLE(1)}, 2, {1, 2}},
		{1, {{4, {{1, 1}, {2, UINT64CONST(1) << 32}, {3, 0}, {4, 0}}}}, 1, {1}},
		{1, {{4, {{1, 1}, {2, 0}, {3, UINT64CONST(1) << 32}, {4, 0}}}}, 1, {1}},
		{1, {{4, {{1, 1}, {2, 0}, {3, 0}, {4, UINT64CONST(1) << 32}}}}, 1, {1}},
		{1, {{3, {{1, 1}, {3, 0}, {4, 0}}}}, 1, {1}},
		{1, {{3, {{1, 1}, {2, 0}, {3, 0}}}}, 1, {1}},
		{1, {{5, {{1, 1}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}}}, 1, {1}},
		{1, {{5, {{1, 1}, {2, 0}, {3, 0}, {4, 0}, {5, 2}}}}, 1, {1}},
		{1, {{5, {{1, 1}, {2, 0}, {3, 0}, {4, 0}, {6, 1}}}}, 1, {1}},
		{1, {{6, {{1, 1}, {2, 0}, {3, 0}, {4, 0}, {5, 1}, {6, 1}}}}, 1, {1}},
	};
	TxnBaseKeyColumn wide[2] = {txn_base_tests_key_column(1600), txn_base_tests_key_column(1)};
	TxnBaseKeyColumn compared[2] = {txn_base_tests_key_column(2), txn_base_tests_key_column(1)};
	TxnBaseBase bases[3];
	TxnBaseBase index = txn_base_tests_empty();
	StdVec		bytes;
	StdVec		addresses;
	TxnBaseBase base;

	compared[0].compared = true;
	bases[0] = txn_base_tests_keyed_base(TXN_BASE_KEY_UNKNOWN, NULL, 0);
	bases[1] = txn_base_tests_keyed_base(TXN_BASE_KEY_COLUMNS, wide, 2);
	bases[2] = txn_base_tests_keyed_base(TXN_BASE_KEY_COLUMNS, compared, 2);
	for (int i = 0; i < 3; i++)
	{
		txn_base_tests_round_trips(747, &bases[i]);
		txn_base_Base_drop(&bases[i]);
	}
	txn_base_tests_keyed(&accepted, &bytes, &addresses);
	base = txn_base_tests_decoded(770, 9, &bytes, &addresses);
	txn_base_Base_drop(&base);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
	for (size_t i = 0; i < lengthof(cases); i++)
	{
		txn_base_tests_keyed(&cases[i], &bytes, &addresses);
		if (!txn_base_tests_refused_vecs(&bytes, &addresses))
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" at "), STD_FMT_ARG(1), STD_FMT_LIT(" decoded")};
			StdFmtArg	args[] = {{&cases[i], txn_base_tests_keyed_columns_Debug_fmt}, {&cases[i], txn_base_tests_keyed_numbers_Debug_fmt}};
			StdFmtArguments arguments = {pieces, 4, args};

			std_panic_fmt(RUST_FILE, 788, 13, &arguments);
		}
	}
	index.has_index = true;
	index.index = 1;
	index.key.tag = TXN_BASE_KEY_UNKNOWN;
	txn_base_Base_encode(&index, &bytes, &addresses);
	STD_ASSERT_MSG(792, 9, txn_base_tests_refused_vecs(&bytes, &addresses), "an index's storage with a key decoded");
}

/* txn_base_tests_assert_counts asserts that sizes plan counts at a target. */
static void
txn_base_tests_assert_counts(uint32 line, uint32 col, const TxnBaseSizes *sizes, uint32 target, const TxnBaseBlockCounts *expected)
{
	TxnBaseBlockCounts counts = txn_base_Sizes_counts(sizes, target);

	STD_ASSERT_EQ(line, col, txn_base_BlockCounts_PartialEq_eq(&counts, expected), &counts,
				  txn_base_BlockCounts_Debug_fmt, expected, txn_base_BlockCounts_Debug_fmt);
}

/* test_txn_base_tests_planned_counts_follow_the_sizes_alone: a tree's planned counts follow its sizes alone. */
void
test_txn_base_tests_planned_counts_follow_the_sizes_alone(void)
{
	const uint32 target = TREES_CHUNKER_DEFAULT_TARGET;
	TxnBaseSizes none = txn_base_Sizes_Default_default();
	TxnBaseBlockCounts zero = txn_base_BlockCounts_Default_default();
	TxnBaseSizes small = {.entries = 1000, .leaf_bytes = 100000, .key_bytes = 8000};
	TxnBaseBlockCounts small_counts = {.leaves = 14, .internal = 1, .height = 1};
	TxnBaseSizes large = {.entries = 10000000, .leaf_bytes = 1000000000, .key_bytes = 80000000};
	TxnBaseBlockCounts large_counts = {.leaves = 134681, .internal = 878, .height = 3};

	txn_base_tests_assert_counts(799, 5, &none, target, &zero);
	txn_base_tests_assert_counts(801, 5, &small, target, &small_counts);
	txn_base_tests_assert_counts(804, 5, &large, target, &large_counts);
}

/* TXN_BASE_TESTS_KEY_LEN is the key length of the case of entries larger than a node. */
#define TXN_BASE_TESTS_KEY_LEN 10016

/* txn_base_tests_key_order orders two of the built tree's keys, each KEY_LEN bytes, as their bytes order. */
static int
txn_base_tests_key_order(const void *a, const void *b)
{
	return memcmp(*(const uint8 *const *) a, *(const uint8 *const *) b, TXN_BASE_TESTS_KEY_LEN);
}

/*
 * txn_base_tests_built returns the blocks the splitter makes of a tree of a number of
 * entries with random keys of KEY_LEN bytes and values of a length, each node
 * above the leaves holding an entry for each node below it.
 */
static TxnBaseBlockCounts
txn_base_tests_built(size_t entries, size_t value_len, uint32 target)
{
	TxnBaseBlockCounts counts = {0};
	uint64		seed = UINT64CONST(0x9e3779b97f4a7c15);
	uint8	   *keys = palloc_extended(entries * TXN_BASE_TESTS_KEY_LEN, MCXT_ALLOC_HUGE);
	const uint8 **level_keys = palloc(entries * sizeof(uint8 *));
	const uint8 **ends = palloc(entries * sizeof(uint8 *));
	uint8	   *value = palloc(value_len);
	size_t		count = entries;
	uint8		level = 0;

	for (size_t i = 0; i < entries * TXN_BASE_TESTS_KEY_LEN; i++)
	{
		seed = seed * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
		keys[i] = (uint8) (seed >> 56);
	}
	for (size_t i = 0; i < entries; i++)
		level_keys[i] = keys + i * TXN_BASE_TESTS_KEY_LEN;
	qsort(level_keys, entries, sizeof(uint8 *), txn_base_tests_key_order);
	memset(value, 7, value_len);
	for (;;)
	{
		TreesChunkerSplitter splitter = trees_chunker_Splitter_new(level, target);
		size_t		ended = 0;
		const uint8 **swap;

		for (size_t at = 0; at < count; at++)
		{
			uint64		size = level == 0 ?
				trees_chunker_leaf_entry_size(level_keys[at], TXN_BASE_TESTS_KEY_LEN, value, value_len) :
				trees_chunker_internal_entry_size(level_keys[at], TXN_BASE_TESTS_KEY_LEN);

			if (trees_chunker_Splitter_add(&splitter, level_keys[at], TXN_BASE_TESTS_KEY_LEN, size) || at == count - 1)
				ends[ended++] = level_keys[at];
		}
		trees_chunker_Splitter_drop(&splitter);
		if (level == 0)
			counts.leaves = ended;
		else
		{
			counts.internal += ended;
			counts.height++;
		}
		if (ended == 1)
			break;
		swap = level_keys;
		level_keys = ends;
		ends = swap;
		count = ended;
		level++;
	}
	pfree(value);
	pfree(ends);
	pfree(level_keys);
	pfree(keys);
	return counts;
}

/*
 * test_txn_base_tests_planned_counts_of_entries_larger_than_a_node_are_those_of_the_tree_the_splitter_makes:
 * entries larger than a node plan the tree the splitter makes of them.
 */
void
test_txn_base_tests_planned_counts_of_entries_larger_than_a_node_are_those_of_the_tree_the_splitter_makes(void)
{
	static const size_t counts[] = {1, 3, 1000};
	const uint32 target = TREES_CHUNKER_DEFAULT_TARGET;
	const size_t value_len = 12;
	uint8	   *zeros = palloc0(TXN_BASE_TESTS_KEY_LEN);

	for (size_t i = 0; i < lengthof(counts); i++)
	{
		size_t		entries = counts[i];
		TxnBaseSizes sizes = {
			.entries = entries,
			.leaf_bytes = entries * trees_chunker_leaf_entry_size(zeros, TXN_BASE_TESTS_KEY_LEN, zeros, value_len),
			.key_bytes = entries * TXN_BASE_TESTS_KEY_LEN,
		};
		TxnBaseBlockCounts planned = txn_base_Sizes_counts(&sizes, target);
		TxnBaseBlockCounts made = txn_base_tests_built(entries, value_len, target);

		if (!txn_base_BlockCounts_PartialEq_eq(&planned, &made))
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" entries")};
			StdFmtArg	args[] = {{&entries, std_usize_Display_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			std_assert_failed(RUST_FILE, 859, 13, "==", &planned, txn_base_BlockCounts_Debug_fmt, &made,
							  txn_base_BlockCounts_Debug_fmt, &arguments);
		}
	}
	pfree(zeros);
}

/* txn_base_tests_chain_round_trips asserts that a transform's entry reads back from its value as itself. */
static void
txn_base_tests_chain_round_trips(uint32 line, const TxnBasePending *pending)
{
	StdVec		value;
	TxnBasePending back;
	HostError	error;

	txn_base_chain_value(pending, &value);
	if (!txn_base_chain_read(value.ptr, value.len, &back, &error))
		STD_RESULT_EXPECT_FAILED(line, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&value, 1, 1);
	STD_ASSERT_EQ(line, 5, txn_base_Pending_PartialEq_eq(&back, pending), &back, txn_base_Pending_Debug_fmt,
				  pending, txn_base_Pending_Debug_fmt);
	txn_base_Pending_drop(&back);
}

/* TxnBaseTestsChainCase is a chain entry case: its fields, each a number and a value, and what it names. */
typedef struct TxnBaseTestsChainCase
{
	int			len;
	std_TupleU32U64_t fields[6];
	const char *what;
} TxnBaseTestsChainCase;

/*
 * test_txn_base_tests_a_chain_entry_round_trips_and_every_other_form_is_corrupt:
 * a value round-trips at its integers' largest, with and without its
 * commit's end, and a value with a field absent or one it doesn't have, or a
 * command past 32 bits, is data corruption.
 */
void
test_txn_base_tests_a_chain_entry_round_trips_and_every_other_form_is_corrupt(void)
{
	static const uint8 ones[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
	static const uint8 seven[1] = {7};
	static const TxnBaseTestsChainCase cases[] = {
		{3, {{1, 5}, {2, 6}, {4, 8}}, "a transform's bytes absent"},
		{3, {{1, 5}, {3, 0}, {4, 8}}, "a transform's command absent"},
		{3, {{1, 5}, {2, 6}, {3, 0}}, "a transform's subtransaction absent"},
		{4, {{1, 5}, {2, 6}, {3, 0}, {5, 9}}, "a commit's end with no subtransaction"},
		{6, {{1, 5}, {2, 6}, {3, 0}, {4, 8}, {5, 0}, {6, 0}}, "a transform's unknown field"},
		{4, {{1, 5}, {2, UINT64CONST(1) << 32}, {3, 0}, {4, 8}}, "a transform's command past 32 bits"},
	};
	TxnBasePending pending = {.xid = PG_UINT64_MAX, .command = PG_UINT32_MAX, .sub = PG_UINT64_MAX};
	TxnBasePending ended;

	std_slice_to_vec_Checked(&pending.transform, ones, 9, 1, 1);
	txn_base_tests_chain_round_trips(869, &pending);
	txn_base_Pending_Clone_clone(&pending, &ended);
	ended.has_end = true;
	ended.end = PG_UINT64_MAX;
	txn_base_tests_chain_round_trips(871, &ended);
	txn_base_Pending_drop(&ended);
	txn_base_Pending_drop(&pending);
	for (size_t i = 0; i < lengthof(cases); i++)
	{
		ChunksRecordRecordWriter writer = chunks_record_RecordWriter_new();
		StdVec		bytes;
		StdVec		addresses;
		TxnBasePending read;
		HostError	error;
		bool		corrupt = false;

		for (int f = 0; f < cases[i].len; f++)
			if (cases[i].fields[f].f0 == 3)
				chunks_record_RecordWriter_field(&writer, 3, seven, 1);
			else
				chunks_record_RecordWriter_uint(&writer, cases[i].fields[f].f0, cases[i].fields[f].f1);
		chunks_record_RecordWriter_finish_value(&writer, &bytes, &addresses);
		std_vec_free_Checked(&addresses, TXN_BASE_PLACED);
		if (txn_base_chain_read(bytes.ptr, bytes.len, &read, &error))
			txn_base_Pending_drop(&read);
		else
		{
			corrupt = memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0;
			host_Error_drop(&error);
		}
		std_vec_free_Checked(&bytes, 1, 1);
		if (!corrupt)
		{
			StdStr		what = {cases[i].what, strlen(cases[i].what)};
			StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" decoded")};
			StdFmtArg	args[] = {{&what, std_str_Display_fmt}};
			StdFmtArguments arguments = {pieces, 2, args};

			std_panic_fmt(RUST_FILE, 888, 13, &arguments);
		}
	}
}

#endif							/* DOLTGRES_TEST_TXN */
