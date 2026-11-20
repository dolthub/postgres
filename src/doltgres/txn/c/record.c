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
 * The WAL records of a storage's writes: a row change, which redo pushes
 * into the storage's delta with no catalog access; beside it the fork's own
 * leave of a fresh page for the next, which no row change names, a blob's
 * chunk, logged before the row that names it, and the records of a
 * completion, a spent tuple id, a database tree's change, a base's advance, a
 * map's carried versions, an emptied storage, and a free of tuple ids.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/record.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/chunks/varint.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/store.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/record.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "record.rs"

/* A row change's marks of a counted key's added and removed copies, and of a row added, updated, and deleted. */
#define TXN_RECORD_ADD 1
#define TXN_RECORD_REMOVE 2
#define TXN_RECORD_ADD_ROW 3
#define TXN_RECORD_UPDATE_ROW 4
#define TXN_RECORD_DELETE_ROW 5

/* TXN_RECORD_INDEX is the field of a change of an index's storage, its index's object id. */
#define TXN_RECORD_INDEX 31

/* TXN_RECORD_CARRIED is the field of a put's carried locker, the full xid of the lock its row's update carried. */
#define TXN_RECORD_CARRIED 32

/* TXN_RECORD_SPECULATIVE and TXN_RECORD_KILLED are field 29's marks of a speculative insert and of its kill. */
#define TXN_RECORD_SPECULATIVE 1
#define TXN_RECORD_KILLED 2

/* TXN_RECORD_ADDRESS is the width of a put's address in field 8: its offset and its locator. */
#define TXN_RECORD_ADDRESS (4 + CHUNKS_TRAILER_LOCATOR_LEN)

/* TXN_RECORD_PLACED is the size and alignment of an address's offset and locator. */
#define TXN_RECORD_PLACED sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)

/* TXN_RECORD_ID is the size and alignment of a value id. */
#define TXN_RECORD_ID sizeof(uint32), _Alignof(uint32)

/* TXN_RECORD_NO_ADDRESSES is an empty slice's addresses, for a record read with none. */
static const chunks_TupleU32Locator_t TXN_RECORD_NO_ADDRESSES[1];

/* txn_record_corrupt returns the error of a row change that doesn't decode. */
static HostError
txn_record_corrupt(void)
{
	static const char message[] = "a WAL row change is corrupt";
	StdString text;

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return host_Error_fork((const uint8 *) "XX001", &text);
}

/* txn_record_uint reads a record's integer field, zero when it is absent, as the storage's error of one that isn't. */
static bool
txn_record_uint(const ChunksRecordRecord *r, uint32 number, uint64 *result, HostError *error)
{
	ChunksErrorError failed;

	if (chunks_record_Record_uint(r, number, result, &failed))
		return true;
	*error = txn_base_storage(&failed);
	return false;
}

/* txn_record_narrow reads a record's integer field as a u32, refusing one of 2^32 or more as data corruption. */
static bool
txn_record_narrow(const ChunksRecordRecord *r, uint32 number, uint32 *result, HostError *error)
{
	uint64		value;

	if (!txn_record_uint(r, number, &value, error))
		return false;
	if (value > PG_UINT32_MAX)
	{
		*error = txn_record_corrupt();
		return false;
	}
	*result = (uint32) value;
	return true;
}

/* txn_record_optional reads an integer field when it is present, reporting whether it is. */
static bool
txn_record_optional(const ChunksRecordRecord *r, uint32 number, bool *has, uint64 *result, HostError *error)
{
	*has = chunks_record_Record_field(r, number) != NULL;
	return !*has || txn_record_uint(r, number, result, error);
}

/* txn_record_successor_bytes writes where an update moved a row: 0 for another table, 1 and the key for a key. */
static void
txn_record_successor_bytes(const TxnMemtableSuccessor *successor, StdVec *out)
{
	uint8		tag = successor->tag == TXN_MEMTABLE_SUCCESSOR_KEY ? 1 : 0;

	std_vec_u8_push_Checked(out, tag);
	if (tag == 1)
		std_vec_extend_from_slice_Checked(out, successor->u.key.ptr, successor->u.key.len, 1, 1);
}

/* txn_record_read_successor reads what successor_bytes wrote, refusing any other form as data corruption. */
static bool
txn_record_read_successor(const uint8 *bytes, size_t len, TxnMemtableSuccessor *result, HostError *error)
{
	if (len == 1 && bytes[0] == 0)
	{
		result->tag = TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE;
		return true;
	}
	if (len >= 1 && bytes[0] == 1)
	{
		result->tag = TXN_MEMTABLE_SUCCESSOR_KEY;
		std_slice_to_vec_Checked(&result->u.key, bytes + 1, len - 1, 1, 1);
		return true;
	}
	*error = txn_record_corrupt();
	return false;
}

/* txn_record_successor_drop frees where an update moved a row. */
static void
txn_record_successor_drop(TxnMemtableSuccessor *successor)
{
	if (successor->tag == TXN_MEMTABLE_SUCCESSOR_KEY)
		std_vec_free_Checked(&successor->u.key, 1, 1);
}

/* txn_record_put_drop frees a put's vectors. */
static void
txn_record_put_drop(TxnMemtablePut *put)
{
	std_vec_free_Checked(&put->value, 1, 1);
	std_vec_free_Checked(&put->addresses, TXN_RECORD_PLACED);
	std_vec_free_Checked(&put->values, TXN_RECORD_ID);
}

/*
 * txn_record_RowChange_put returns the put a change carries, of a put, an
 * added copy, an added row, or an updated row, false for none.
 */
bool
txn_record_RowChange_put(const TxnRecordRowChange *self, const TxnMemtablePut **result)
{
	switch (self->op.tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			*result = &self->op.u.put;
			return true;
		case TXN_MEMTABLE_OP_ADD:
			*result = &self->op.u.add;
			return true;
		case TXN_MEMTABLE_OP_ADD_ROW:
			*result = &self->op.u.add_row;
			return true;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			*result = &self->op.u.update_row.f1;
			return true;
		default:
			return false;
	}
}

/* txn_record_op_put returns the put an op being built carries, NULL for none. */
TxnMemtablePut *
txn_record_op_put(TxnMemtableOp *op)
{
	switch (op->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			return &op->u.put;
		case TXN_MEMTABLE_OP_ADD:
			return &op->u.add;
		case TXN_MEMTABLE_OP_ADD_ROW:
			return &op->u.add_row;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			return &op->u.update_row.f1;
		default:
			return NULL;
	}
}

/* txn_record_moved returns where an op that moves a row moved it, NULL for none. */
static const TxnMemtableSuccessor *
txn_record_moved(const TxnMemtableOp *op)
{
	if (op->tag == TXN_MEMTABLE_OP_MOVE)
		return &op->u.move;
	if (op->tag == TXN_MEMTABLE_OP_REMOVE && op->u.remove.has_f1)
		return &op->u.remove.f1;
	if (op->tag == TXN_MEMTABLE_OP_DELETE_ROW && op->u.delete_row.has_f1)
		return &op->u.delete_row.f1;
	return NULL;
}

/* txn_record_contains reports whether a vector of places holds a place. */
static bool
txn_record_contains(const StdVec *places, size_t place)
{
	for (size_t i = 0; i < places->len; i++)
		if (STD_VEC_AT(places, size_t, i) == place)
			return true;
	return false;
}

/* txn_record_encoding_frame_t is encoding's state across its pad: the encoding it makes, its vectors live. */
typedef struct txn_record_encoding_frame_t
{
	const TxnRecordRowChange *self;
	TxnRecordEncoding *result;
	int			live;
} txn_record_encoding_frame_t;

/*
 * txn_record_encoding_body makes the addresses', the successor's, the values',
 * and the kept values' bytes, then the writer.
 */
static void
txn_record_encoding_body(void *frame_)
{
	txn_record_encoding_frame_t *frame = frame_;
	const TxnRecordRowChange *self = frame->self;
	TxnRecordEncoding *encoding = frame->result;
	const TxnMemtablePut *put = NULL;
	const TxnMemtableSuccessor *successor = txn_record_moved(&self->op);
	bool		has_put = txn_record_RowChange_put(self, &put);
	size_t		blob = 0;
	size_t		value;
	size_t		most;

	std_vec_new(&encoding->addresses, 1, 1);
	std_vec_new(&encoding->successor, 1, 1);
	std_vec_new(&encoding->values, 1, 1);
	std_vec_new(&encoding->kept, 1, 1);
	frame->live = 1;
	if (has_put)
		for (size_t i = 0; i < put->addresses.len; i++)
		{
			const chunks_TupleU32Locator_t *address = &STD_VEC_AT(&put->addresses, chunks_TupleU32Locator_t, i);
			uint8		offset[4] = {(uint8) address->f0, (uint8) (address->f0 >> 8), (uint8) (address->f0 >> 16),
			(uint8) (address->f0 >> 24)};

			std_vec_extend_from_slice_Checked(&encoding->addresses, offset, 4, 1, 1);
			chunks_trailer_Locator_write(&address->f1, &encoding->addresses);
		}
	if (successor != NULL)
		txn_record_successor_bytes(successor, &encoding->successor);
	for (size_t place = 0; has_put && place < put->values.len; place++)
	{
		uint32		id = STD_VEC_AT(&put->values, uint32, place);

		if (txn_record_contains(&self->kept, place))
		{
			chunks_varint_put_varint(&encoding->kept, (uint64) place);
			chunks_varint_put_varint(&encoding->kept, (uint64) id);
		}
		else if (blob < self->blobs.len)
		{
			const chunks_TupleAddressU64_t *stored = &STD_VEC_AT(&self->blobs, chunks_TupleAddressU64_t, blob++);

			chunks_varint_put_varint(&encoding->values, (uint64) id);
			std_vec_extend_from_slice_Checked(&encoding->values, stored->f0.f0, 32, 1, 1);
			chunks_varint_put_varint(&encoding->values, stored->f1);
		}
	}
	value = has_put ? put->value.len : 0;
	most = 18 * 20 + self->key.len + value + encoding->addresses.len + encoding->successor.len +
		encoding->values.len + encoding->kept.len;
	encoding->writer = chunks_record_RecordWriter_with_capacity(most);
	frame->live = 0;
}

/* txn_record_Encoding_drop frees an encoding's writer and its vectors. */
void
txn_record_Encoding_drop(TxnRecordEncoding *self)
{
	chunks_record_RecordWriter_drop(&self->writer);
	std_vec_free_Checked(&self->addresses, 1, 1);
	std_vec_free_Checked(&self->successor, 1, 1);
	std_vec_free_Checked(&self->values, 1, 1);
	std_vec_free_Checked(&self->kept, 1, 1);
}

/* txn_record_encoding_pad frees the vectors encoding made before it unwound. */
static void
txn_record_encoding_pad(void *frame_)
{
	txn_record_encoding_frame_t *frame = frame_;

	if (frame->live)
	{
		std_vec_free_Checked(&frame->result->addresses, 1, 1);
		std_vec_free_Checked(&frame->result->successor, 1, 1);
		std_vec_free_Checked(&frame->result->values, 1, 1);
		std_vec_free_Checked(&frame->result->kept, 1, 1);
	}
}

/*
 * txn_record_RowChange_encoding makes what encode_with needs, outside any
 * window: the addresses' and the successor's bytes, and a writer with room
 * for every field at its widest.
 */
void
txn_record_RowChange_encoding(const TxnRecordRowChange *self, TxnRecordEncoding *result)
{
	txn_record_encoding_frame_t frame = {.self = self, .result = result};

	std_landing(&frame, txn_record_encoding_body, txn_record_encoding_pad);
}

/*
 * txn_record_Encoding_name sets the tuple id that ends a successor's key, the
 * new row of a keyless or deferrable key's update, which the update's take
 * answers inside its window, allocating nothing.
 */
void
txn_record_Encoding_name(TxnRecordEncoding *self, HostRow tid)
{
	size_t		at = std_saturating_sub_usize(self->successor.len, 8);
	uint8		be[8];

	for (int i = 0; i < 8; i++)
		be[i] = (uint8) (tid.f0 >> (8 * (7 - i)));
	std_slice_copy_from_slice_Checked((uint8 *) self->successor.ptr + at, self->successor.len - at, be, 8, 1, 1,
									  RUST_FILE, 116, 29);
}

/* txn_record_encode_frame_t is encode_with's state across its pad: the encoding it takes, while live. */
typedef struct txn_record_encode_frame_t
{
	const TxnRecordRowChange *self;
	TxnRecordEncoding encoding;
	bool		encoding_live;
	StdVec	   *result;
} txn_record_encode_frame_t;

/* txn_record_encode_body writes the change's fields through the encoding's writer, in the format's order. */
static void
txn_record_encode_body(void *frame_)
{
	txn_record_encode_frame_t *frame = frame_;
	const TxnRecordRowChange *self = frame->self;
	TxnRecordEncoding *e = &frame->encoding;
	ChunksRecordRecordWriter *w = &e->writer;
	const TxnMemtablePut *put = NULL;
	bool		has_put = txn_record_RowChange_put(self, &put);
	uint64		mark = 0;
	bool		has_row = false;
	HostRow		row = {0};

	chunks_record_RecordWriter_uint(w, 1, self->writer.has_top ? self->writer.top : self->writer.xid);
	chunks_record_RecordWriter_uint(w, 2, self->storage.tablespace);
	chunks_record_RecordWriter_uint(w, 3, self->storage.database);
	chunks_record_RecordWriter_uint(w, 4, self->storage.relation);
	if (self->writer.has_top)
		chunks_record_RecordWriter_uint(w, 5, self->writer.xid);
	chunks_record_RecordWriter_field(w, 6, self->key.ptr, self->key.len);
	if (has_put)
	{
		chunks_record_RecordWriter_field(w, 7, put->value.ptr, put->value.len);
		if (e->addresses.len != 0)
			chunks_record_RecordWriter_field(w, 8, e->addresses.ptr, e->addresses.len);
		if (!self->has_index || self->op.tag == TXN_MEMTABLE_OP_ADD)
			chunks_record_RecordWriter_uint(w, 9, put->tuple.f0);
	}
	switch (self->op.tag)
	{
		case TXN_MEMTABLE_OP_ADD:
			mark = TXN_RECORD_ADD;
			break;
		case TXN_MEMTABLE_OP_REMOVE:
			mark = TXN_RECORD_REMOVE;
			has_row = true;
			row = self->op.u.remove.f0;
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			mark = TXN_RECORD_ADD_ROW;
			break;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			mark = TXN_RECORD_UPDATE_ROW;
			has_row = true;
			row = self->op.u.update_row.f0;
			break;
		case TXN_MEMTABLE_OP_DELETE_ROW:
			mark = TXN_RECORD_DELETE_ROW;
			has_row = true;
			row = self->op.u.delete_row.f0;
			break;
		default:
			break;
	}
	if (mark != 0)
		chunks_record_RecordWriter_uint(w, 11, mark);
	if (has_row)
		chunks_record_RecordWriter_uint(w, 12, row.f0);
	if (e->successor.len != 0)
		chunks_record_RecordWriter_field(w, 13, e->successor.ptr, e->successor.len);
	if (self->no_key)
		chunks_record_RecordWriter_uint(w, 14, 1);
	chunks_record_RecordWriter_uint(w, 15, self->table);
	chunks_record_RecordWriter_uint(w, 16, self->writer.command);
	if (self->frozen)
		chunks_record_RecordWriter_uint(w, 21, 1);
	if (has_put && !self->has_index)
		chunks_record_RecordWriter_uint(w, 22, self->heap_bytes);
	if (e->values.len != 0)
		chunks_record_RecordWriter_field(w, 24, e->values.ptr, e->values.len);
	if (self->has_toast)
	{
		chunks_record_RecordWriter_uint(w, 25, self->toast.tablespace);
		chunks_record_RecordWriter_uint(w, 26, self->toast.relation);
		chunks_record_RecordWriter_uint(w, 27, self->toast.table);
	}
	if (e->kept.len != 0)
		chunks_record_RecordWriter_field(w, 28, e->kept.ptr, e->kept.len);
	if (self->speculative)
		chunks_record_RecordWriter_uint(w, 29, TXN_RECORD_SPECULATIVE);
	if (self->has_replaced)
		chunks_record_RecordWriter_uint(w, 30, self->replaced.f0);
	if (self->has_index)
		chunks_record_RecordWriter_uint(w, TXN_RECORD_INDEX, self->index);
	if (has_put && put->has_carried)
		chunks_record_RecordWriter_uint(w, TXN_RECORD_CARRIED, put->carried);
	*frame->result = chunks_record_RecordWriter_finish(w);
	frame->encoding_live = false;
	txn_record_Encoding_drop(e);
}

/* txn_record_encode_pad frees the encoding encode_with took, while live. */
static void
txn_record_encode_pad(void *frame_)
{
	txn_record_encode_frame_t *frame = frame_;

	if (frame->encoding_live)
		txn_record_Encoding_drop(&frame->encoding);
}

/*
 * txn_record_RowChange_encode_with writes the record's body through an
 * encoding of the change, which it takes, allocating nothing, so a write
 * calls it inside its checkpoint window once a take answers the tuple id.
 */
void
txn_record_RowChange_encode_with(const TxnRecordRowChange *self, TxnRecordEncoding *encoding, StdVec *result)
{
	txn_record_encode_frame_t frame = {.self = self, .encoding = *encoding, .encoding_live = true,
	.result = result};

	std_landing(&frame, txn_record_encode_body, txn_record_encode_pad);
}

/* txn_record_RowChange_encode returns the record's body, as encode_with writes it. */
void
txn_record_RowChange_encode(const TxnRecordRowChange *self, StdVec *result)
{
	TxnRecordEncoding encoding;

	txn_record_RowChange_encoding(self, &encoding);
	txn_record_RowChange_encode_with(self, &encoding, result);
}

/*
 * txn_record_read_put reads a change's put from its value (field 7) and
 * addresses (8), naming a tuple id, with no value ids and no heap bytes,
 * which the caller reads from their own fields.
 */
static bool
txn_record_read_put(const ChunksRecordRecord *r, HostRow tuple, TxnMemtablePut *result, HostError *error)
{
	size_t		raw_len = 0;
	const uint8 *raw = chunks_record_Record_bytes(r, 8, &raw_len);
	size_t		value_len;
	const uint8 *value;

	if (raw == NULL)
		raw_len = 0;
	if (raw_len % TXN_RECORD_ADDRESS != 0)
	{
		*error = txn_record_corrupt();
		return false;
	}
	value = chunks_record_Record_bytes(r, 7, &value_len);
	if (value == NULL)
	{
		*error = txn_record_corrupt();
		return false;
	}
	std_vec_with_capacity_Checked(&result->addresses, raw_len / TXN_RECORD_ADDRESS, TXN_RECORD_PLACED);
	for (size_t at = 0; at < raw_len; at += TXN_RECORD_ADDRESS)
	{
		chunks_TupleU32Locator_t address;

		address.f0 = std_u32_from_le_bytes(raw + at);
		address.f1 = chunks_trailer_Locator_read(raw + at + 4, CHUNKS_TRAILER_LOCATOR_LEN);
		std_vec_push_Checked(&result->addresses, &address, TXN_RECORD_PLACED);
	}
	std_slice_to_vec_Checked(&result->value, value, value_len, 1, 1);
	std_vec_new(&result->values, TXN_RECORD_ID);
	result->tuple = tuple;
	result->heap_bytes = 0;
	return true;
}

/*
 * txn_record_decode_frame_t is a row change's decode's state across its pad:
 * the fields and the record it reads, and every vector it has made, each
 * while live, the op once it holds them.
 */
typedef struct txn_record_decode_frame_t
{
	const uint8 *body;
	size_t		body_len;
	TxnRecordRowChange *result;
	HostError  *error;
	bool		ok;
	StdVec		known;
	bool		known_live;
	ChunksRecordRecord record;
	bool		record_live;
	TxnMemtableSuccessor successor;
	bool		successor_live;
	TxnMemtablePut put;
	bool		put_live;
	bool		op_live;
	bool		key_live;
	StdVec		blobs;
	bool		blobs_live;
	StdVec		stored;
	bool		stored_live;
	StdVec		kept;
	bool		kept_live;
	StdVec		kept_ids;
	bool		kept_ids_live;
	StdVec		ids;
	bool		ids_live;
} txn_record_decode_frame_t;

/* txn_record_decode_pad frees what a decode holds live, its op whole once it holds its parts. */
static void
txn_record_decode_pad(void *frame_)
{
	txn_record_decode_frame_t *frame = frame_;

	if (frame->ids_live)
		std_vec_free_Checked(&frame->ids, TXN_RECORD_ID);
	if (frame->kept_ids_live)
		std_vec_free_Checked(&frame->kept_ids, TXN_RECORD_ID);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, sizeof(size_t), _Alignof(size_t));
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, TXN_RECORD_ID);
	if (frame->blobs_live)
		std_vec_free_Checked(&frame->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	if (frame->key_live)
		std_vec_free_Checked(&frame->result->key, 1, 1);
	if (frame->op_live)
		txn_memtable_Op_drop(&frame->result->op);
	if (frame->put_live)
		txn_record_put_drop(&frame->put);
	if (frame->successor_live)
		txn_record_successor_drop(&frame->successor);
	if (frame->record_live)
		chunks_record_Record_drop(&frame->record);
	if (frame->known_live)
		std_vec_free_Checked(&frame->known, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	frame->ids_live = frame->kept_ids_live = frame->kept_live = frame->stored_live = false;
	frame->blobs_live = frame->key_live = frame->op_live = frame->put_live = false;
	frame->successor_live = frame->record_live = frame->known_live = false;
}

/* txn_record_fail sets a row change's corruption error and answers false. */
static bool
txn_record_fail(txn_record_decode_frame_t *frame)
{
	*frame->error = txn_record_corrupt();
	return false;
}

/* txn_record_take_put moves the frame's put into an op's place for it. */
static void
txn_record_take_put(txn_record_decode_frame_t *frame, TxnMemtablePut *into)
{
	*into = frame->put;
	frame->put_live = false;
}

/* txn_record_take_successor moves the frame's successor, if any, into an op's place for it. */
static void
txn_record_take_successor(txn_record_decode_frame_t *frame, bool *has, TxnMemtableSuccessor *into)
{
	*has = frame->successor_live;
	if (frame->successor_live)
		*into = frame->successor;
	frame->successor_live = false;
}

/* txn_record_row reads the tuple id a counted or row op names (field 12). */
static bool
txn_record_row(txn_record_decode_frame_t *frame, HostRow *row)
{
	return txn_record_uint(&frame->record, 12, &row->f0, frame->error);
}

/* txn_record_writer reads a change's writer: its top-level xid (1), its subtransaction's (5), and its command (16). */
static bool
txn_record_writer(const ChunksRecordRecord *r, TxnMemtableVersionWriter *writer, HostError *error)
{
	uint64		top;
	uint64		sub;
	bool		has_sub;

	if (!txn_record_uint(r, 1, &top, error) || !txn_record_optional(r, 5, &has_sub, &sub, error))
		return false;
	writer->has_top = has_sub;
	writer->xid = has_sub ? sub : top;
	writer->top = has_sub ? top : 0;
	return txn_record_narrow(r, 16, &writer->command, error);
}

/* txn_record_storage reads a change's storage, its tablespace, database, and relation (fields 2 to 4). */
static bool
txn_record_storage(const ChunksRecordRecord *r, HostStorage *storage, HostError *error)
{
	return txn_record_narrow(r, 2, &storage->tablespace, error) &&
		txn_record_narrow(r, 3, &storage->database, error) && txn_record_narrow(r, 4, &storage->relation, error);
}

/* txn_record_key copies a change's key (field 6) into the result, refusing a change with none. */
static bool
txn_record_key(txn_record_decode_frame_t *frame)
{
	size_t		len;
	const uint8 *key = chunks_record_Record_bytes(&frame->record, 6, &len);

	if (key == NULL)
		return txn_record_fail(frame);
	std_slice_to_vec_Checked(&frame->result->key, key, len, 1, 1);
	frame->key_live = true;
	return true;
}

/*
 * txn_record_RowChange_entry reads a change of an index's storage, refusing as data
 * corruption any field its op doesn't carry: every change carries the writer,
 * the storage, the entry's key, the table, the command, and the index; a put
 * its value and any addresses; a delete nothing more; a counted entry's added
 * copy its value, any addresses, the copy's tuple id, and its mark; and a
 * removed copy its mark and the copy's tuple id.
 */
static bool
txn_record_RowChange_entry(txn_record_decode_frame_t *frame)
{
	static const uint32 every[] = {1, 2, 3, 4, 5, 6, 15, 16, TXN_RECORD_INDEX};
	static const uint32 put[] = {7, 8};
	static const uint32 add[] = {7, 8, 9, 11};
	static const uint32 removed[] = {11, 12};
	TxnRecordRowChange *result = frame->result;
	ChunksErrorError failed;
	bool		has_mark;
	uint64		mark;
	bool		valued;
	const uint32 *carried = NULL;
	size_t		carried_len = 0;

	if (!chunks_record_Record_value(frame->body, frame->body_len, TXN_RECORD_NO_ADDRESSES, 0, &frame->record, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->record_live = true;
	if (!txn_record_optional(&frame->record, 11, &has_mark, &mark, frame->error))
		return false;
	valued = chunks_record_Record_field(&frame->record, 7) != NULL;
	if (!has_mark && valued)
	{
		HostRow		none = {0};

		carried = put;
		carried_len = lengthof(put);
		if (!txn_record_read_put(&frame->record, none, &result->op.u.put, frame->error))
			return false;
		result->op.tag = TXN_MEMTABLE_OP_PUT;
		frame->op_live = true;
	}
	else if (!has_mark && !valued)
		result->op.tag = TXN_MEMTABLE_OP_DELETE;
	else if (mark == TXN_RECORD_ADD && valued)
	{
		HostRow		tuple;

		carried = add;
		carried_len = lengthof(add);
		if (!txn_record_uint(&frame->record, 9, &tuple.f0, frame->error) ||
			!txn_record_read_put(&frame->record, tuple, &result->op.u.add, frame->error))
			return false;
		result->op.tag = TXN_MEMTABLE_OP_ADD;
		frame->op_live = true;
	}
	else if (mark == TXN_RECORD_REMOVE && !valued)
	{
		carried = removed;
		carried_len = lengthof(removed);
		if (!txn_record_row(frame, &result->op.u.remove.f0))
			return false;
		result->op.tag = TXN_MEMTABLE_OP_REMOVE;
		result->op.u.remove.has_f1 = false;
	}
	else
		return txn_record_fail(frame);
	for (size_t i = 0; i < frame->known.len; i++)
	{
		uint32		number = STD_VEC_AT(&frame->known, ChunksRecordField, i).number;
		bool		allowed = false;

		for (size_t j = 0; j < lengthof(every) && !allowed; j++)
			allowed = every[j] == number;
		for (size_t j = 0; j < carried_len && !allowed; j++)
			allowed = carried[j] == number;
		if (!allowed)
			return txn_record_fail(frame);
	}
	if (!txn_record_writer(&frame->record, &result->writer, frame->error) ||
		!txn_record_storage(&frame->record, &result->storage, frame->error) ||
		!txn_record_narrow(&frame->record, 15, &result->table, frame->error) || !txn_record_key(frame))
		return false;
	result->no_key = false;
	result->frozen = false;
	result->heap_bytes = 0;
	result->has_toast = false;
	std_vec_new(&result->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_new(&result->kept, sizeof(size_t), _Alignof(size_t));
	result->speculative = false;
	result->has_replaced = false;
	result->has_index = true;
	return txn_record_narrow(&frame->record, TXN_RECORD_INDEX, &result->index, frame->error);
}

/* txn_record_blobs reads field 24, each stored value's id, its blob's address, and its length. */
static bool
txn_record_blobs(txn_record_decode_frame_t *frame)
{
	size_t		len;
	const uint8 *bytes = chunks_record_Record_bytes(&frame->record, 24, &len);
	size_t		at = 0;
	ChunksVarintMalformed malformed;

	std_vec_new(&frame->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	frame->blobs_live = true;
	std_vec_new(&frame->stored, TXN_RECORD_ID);
	frame->stored_live = true;
	if (bytes == NULL)
		return true;
	while (at < len)
	{
		uint64		id;
		uint32		narrowed;
		chunks_TupleAddressU64_t blob;

		if (!chunks_varint_decode(bytes, len, &at, &id, &malformed) || id > PG_UINT32_MAX)
			return txn_record_fail(frame);
		narrowed = (uint32) id;
		std_vec_push_Checked(&frame->stored, &narrowed, TXN_RECORD_ID);
		if (len - at < 32)
			return txn_record_fail(frame);
		memcpy(blob.f0.f0, bytes + at, 32);
		at += 32;
		if (!chunks_varint_decode(bytes, len, &at, &blob.f1, &malformed))
			return txn_record_fail(frame);
		std_vec_push_Checked(&frame->blobs, &blob, sizeof(blob), _Alignof(chunks_TupleAddressU64_t));
	}
	if (frame->stored.len == 0)
		return txn_record_fail(frame);
	return true;
}

/* txn_record_holds reports whether a vector of value ids holds an id. */
static bool
txn_record_holds(const StdVec *ids, uint32 id)
{
	for (size_t i = 0; i < ids->len; i++)
		if (STD_VEC_AT(ids, uint32, i) == id)
			return true;
	return false;
}

/* txn_record_kept reads field 28, an update's kept values' places, ascending, and their ids, none of a stored id. */
static bool
txn_record_kept(txn_record_decode_frame_t *frame)
{
	size_t		len;
	const uint8 *bytes = chunks_record_Record_bytes(&frame->record, 28, &len);
	size_t		at = 0;
	ChunksVarintMalformed malformed;

	std_vec_new(&frame->kept, sizeof(size_t), _Alignof(size_t));
	frame->kept_live = true;
	std_vec_new(&frame->kept_ids, TXN_RECORD_ID);
	frame->kept_ids_live = true;
	if (bytes == NULL)
		return true;
	while (at < len)
	{
		uint64		place;
		uint64		id;
		size_t		narrowed;
		uint32		kept_id;

		if (!chunks_varint_decode(bytes, len, &at, &place, &malformed) ||
			!chunks_varint_decode(bytes, len, &at, &id, &malformed) || id > PG_UINT32_MAX)
			return txn_record_fail(frame);
		narrowed = (size_t) place;
		kept_id = (uint32) id;
		if ((frame->kept.len != 0 && narrowed <= STD_VEC_AT(&frame->kept, size_t, frame->kept.len - 1)) ||
			txn_record_holds(&frame->stored, kept_id))
			return txn_record_fail(frame);
		std_vec_push_Checked(&frame->kept, &narrowed, sizeof(size_t), _Alignof(size_t));
		std_vec_push_Checked(&frame->kept_ids, &kept_id, TXN_RECORD_ID);
	}
	if (frame->kept.len == 0)
		return txn_record_fail(frame);
	return true;
}

/*
 * txn_record_ids gives the op's put its value ids, the stored and the kept
 * ones at their places, refusing a kept place past them all, too few ids for
 * the places, and an op that carries no put.
 */
static bool
txn_record_ids(txn_record_decode_frame_t *frame)
{
	size_t		count = frame->stored.len + frame->kept.len;
	size_t		stored = 0;
	size_t		kept = 0;
	TxnMemtablePut *put;

	if (frame->stored.len == 0 && frame->kept.len == 0)
		return true;
	if (frame->kept.len != 0 && STD_VEC_AT(&frame->kept, size_t, frame->kept.len - 1) >= count)
		return txn_record_fail(frame);
	std_vec_with_capacity_Checked(&frame->ids, count, TXN_RECORD_ID);
	frame->ids_live = true;
	for (size_t place = 0; place < count; place++)
	{
		uint32		id;

		if (txn_record_contains(&frame->kept, place))
		{
			if (kept == frame->kept_ids.len)
				return txn_record_fail(frame);
			id = STD_VEC_AT(&frame->kept_ids, uint32, kept++);
		}
		else
		{
			if (stored == frame->stored.len)
				return txn_record_fail(frame);
			id = STD_VEC_AT(&frame->stored, uint32, stored++);
		}
		std_vec_push_Checked(&frame->ids, &id, TXN_RECORD_ID);
	}
	put = txn_record_op_put(&frame->result->op);
	if (put == NULL)
		return txn_record_fail(frame);
	std_vec_free_Checked(&put->values, TXN_RECORD_ID);
	put->values = frame->ids;
	frame->ids_live = false;
	return true;
}

/* txn_record_op assembles the change's op from its copy mark, its put, its row, and its successor. */
static bool
txn_record_op(txn_record_decode_frame_t *frame, bool has_copy, uint64 copy)
{
	TxnMemtableOp *op = &frame->result->op;
	bool		has_put = frame->put_live;

	if (!has_copy && has_put)
	{
		op->tag = TXN_MEMTABLE_OP_PUT;
		txn_record_take_put(frame, &op->u.put);
	}
	else if (!has_copy && !has_put && frame->successor_live)
	{
		bool		has;

		op->tag = TXN_MEMTABLE_OP_MOVE;
		txn_record_take_successor(frame, &has, &op->u.move);
	}
	else if (!has_copy && !has_put)
		op->tag = TXN_MEMTABLE_OP_DELETE;
	else if (copy == TXN_RECORD_ADD && has_put)
	{
		op->tag = TXN_MEMTABLE_OP_ADD;
		txn_record_take_put(frame, &op->u.add);
	}
	else if (copy == TXN_RECORD_REMOVE && !has_put)
	{
		if (!txn_record_row(frame, &op->u.remove.f0))
			return false;
		op->tag = TXN_MEMTABLE_OP_REMOVE;
		txn_record_take_successor(frame, &op->u.remove.has_f1, &op->u.remove.f1);
	}
	else if (copy == TXN_RECORD_ADD_ROW && has_put)
	{
		op->tag = TXN_MEMTABLE_OP_ADD_ROW;
		txn_record_take_put(frame, &op->u.add_row);
	}
	else if (copy == TXN_RECORD_UPDATE_ROW && has_put)
	{
		if (!txn_record_row(frame, &op->u.update_row.f0))
			return false;
		op->tag = TXN_MEMTABLE_OP_UPDATE_ROW;
		txn_record_take_put(frame, &op->u.update_row.f1);
	}
	else if (copy == TXN_RECORD_DELETE_ROW && !has_put)
	{
		if (!txn_record_row(frame, &op->u.delete_row.f0))
			return false;
		op->tag = TXN_MEMTABLE_OP_DELETE_ROW;
		txn_record_take_successor(frame, &op->u.delete_row.has_f1, &op->u.delete_row.f1);
	}
	else
		return txn_record_fail(frame);
	frame->op_live = true;
	if (frame->successor_live)
		txn_record_successor_drop(&frame->successor);
	frame->successor_live = false;
	return true;
}

/* txn_record_decode_body reads a row change's body in the Rust's order, its index's form for a change of an index. */
static void
txn_record_decode_body(void *frame_)
{
	txn_record_decode_frame_t *frame = frame_;
	TxnRecordRowChange *result = frame->result;
	ChunksRecordRecord *r = &frame->record;
	ChunksErrorError failed;
	bool		has_copy;
	uint64		copy;
	const uint8 *successor;
	size_t		successor_len;
	bool		has_heap;
	uint64		heap_bytes;
	bool		has_carried;
	uint64		carried;
	TxnMemtablePut *put;
	bool		fields[3];
	bool		has_speculative;
	uint64		speculative;

	if (!chunks_record_fields(frame->body, frame->body_len, &frame->known, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->known_live = true;
	for (size_t i = 0; i < frame->known.len; i++)
		if (STD_VEC_AT(&frame->known, ChunksRecordField, i).number == TXN_RECORD_INDEX)
		{
			frame->ok = txn_record_RowChange_entry(frame);
			return;
		}
	if (!chunks_record_Record_value(frame->body, frame->body_len, TXN_RECORD_NO_ADDRESSES, 0, r, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->record_live = true;
	if (!txn_record_optional(r, 11, &has_copy, &copy, frame->error))
		return;
	successor = chunks_record_Record_bytes(r, 13, &successor_len);
	if (successor != NULL)
	{
		if (!txn_record_read_successor(successor, successor_len, &frame->successor, frame->error))
			return;
		frame->successor_live = true;
	}
	if (chunks_record_Record_field(r, 7) != NULL)
	{
		HostRow		tuple;

		if (!txn_record_uint(r, 9, &tuple.f0, frame->error) ||
			!txn_record_read_put(r, tuple, &frame->put, frame->error))
			return;
		frame->put_live = true;
	}
	if (!txn_record_op(frame, has_copy, copy) || !txn_record_optional(r, 22, &has_heap, &heap_bytes, frame->error) ||
		!txn_record_optional(r, 30, &result->has_replaced, &result->replaced.f0, frame->error) ||
		!txn_record_optional(r, TXN_RECORD_CARRIED, &has_carried, &carried, frame->error))
		return;
	if (result->has_replaced && !has_heap)
	{
		txn_record_fail(frame);
		return;
	}
	put = txn_record_op_put(&result->op);
	if ((put != NULL) != has_heap || (put != NULL && (heap_bytes == 0 || heap_bytes > PG_UINT32_MAX)) ||
		(has_carried && (put == NULL || carried < TXN_STATUS_FIRST_NORMAL)))
	{
		txn_record_fail(frame);
		return;
	}
	if (put != NULL)
	{
		put->heap_bytes = (uint32) heap_bytes;
		put->has_carried = has_carried;
		put->carried = has_carried ? carried : 0;
	}
	if (!txn_record_blobs(frame) || !txn_record_kept(frame) || !txn_record_ids(frame))
		return;
	for (int i = 0; i < 3; i++)
		fields[i] = chunks_record_Record_field(r, 25 + i) != NULL;
	result->has_toast = fields[0] && fields[1] && fields[2];
	if (!result->has_toast && (fields[0] || fields[1] || fields[2]))
	{
		txn_record_fail(frame);
		return;
	}
	if (result->has_toast &&
		(!txn_record_narrow(r, 25, &result->toast.tablespace, frame->error) ||
		 !txn_record_narrow(r, 26, &result->toast.relation, frame->error) ||
		 !txn_record_narrow(r, 27, &result->toast.table, frame->error)))
		return;
	if (result->has_toast != (put != NULL && put->values.len != 0))
	{
		txn_record_fail(frame);
		return;
	}
	if (!txn_record_writer(r, &result->writer, frame->error) || !txn_record_storage(r, &result->storage, frame->error) ||
		!txn_record_narrow(r, 15, &result->table, frame->error) || !txn_record_key(frame) ||
		!txn_record_optional(r, 29, &has_speculative, &speculative, frame->error))
		return;
	if (has_speculative && speculative != TXN_RECORD_SPECULATIVE)
	{
		txn_record_fail(frame);
		return;
	}
	result->no_key = chunks_record_Record_field(r, 14) != NULL;
	result->frozen = chunks_record_Record_field(r, 21) != NULL;
	result->heap_bytes = has_heap ? heap_bytes : 0;
	result->blobs = frame->blobs;
	frame->blobs_live = false;
	result->kept = frame->kept;
	frame->kept_live = false;
	result->speculative = has_speculative;
	result->has_index = false;
	frame->ok = true;
}

/* txn_record_RowChange_decode reads a row change's body, refusing any other form as data corruption. */
bool
txn_record_RowChange_decode(const uint8 *body, size_t body_len, TxnRecordRowChange *result, HostError *error)
{
	txn_record_decode_frame_t frame = {.body = body, .body_len = body_len, .result = result, .error = error};

	std_landing(&frame, txn_record_decode_body, txn_record_decode_pad);
	if (frame.ok)
	{
		frame.op_live = false;
		frame.key_live = false;
	}
	txn_record_decode_pad(&frame);
	return frame.ok;
}

/* txn_record_RowChange_drop frees a row change's key, op, blobs, and kept places. */
void
txn_record_RowChange_drop(TxnRecordRowChange *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	txn_memtable_Op_drop(&self->op);
	std_vec_free_Checked(&self->blobs, sizeof(chunks_TupleAddressU64_t), _Alignof(chunks_TupleAddressU64_t));
	std_vec_free_Checked(&self->kept, sizeof(size_t), _Alignof(size_t));
}

/* TXN_RECORD_NO_LOCATORS is an empty slice's locators, for a chunk stored with none. */
static const ChunksTrailerLocator TXN_RECORD_NO_LOCATORS[1];

/* TxnRecordFill writes a record's fields through a writer. */
typedef void (*TxnRecordFill) (ChunksRecordRecordWriter *w, const void *self);

/* txn_record_written_frame_t is a record's encode's state across its pad: its writer, while live. */
typedef struct txn_record_written_frame_t
{
	TxnRecordFill fill;
	const void *self;
	size_t		capacity;
	bool		has_writer;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	StdVec	   *result;
} txn_record_written_frame_t;

/* txn_record_written_body writes the record's fields and finishes its body. */
static void
txn_record_written_body(void *frame_)
{
	txn_record_written_frame_t *frame = frame_;

	if (!frame->has_writer)
		frame->writer = chunks_record_RecordWriter_with_capacity(frame->capacity);
	frame->writer_live = true;
	frame->fill(&frame->writer, frame->self);
	*frame->result = chunks_record_RecordWriter_finish(&frame->writer);
	frame->writer_live = false;
	chunks_record_RecordWriter_drop(&frame->writer);
}

/* txn_record_written_pad drops the writer, while live. */
static void
txn_record_written_pad(void *frame_)
{
	txn_record_written_frame_t *frame = frame_;

	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
}

/* txn_record_written returns a record's body, its fields written by a fill through a writer given or made with room. */
static void
txn_record_written(TxnRecordFill fill, const void *self, ChunksRecordRecordWriter *writer, size_t capacity,
				   StdVec *result)
{
	txn_record_written_frame_t frame = {.fill = fill, .self = self, .capacity = capacity, .result = result};

	if (writer != NULL)
	{
		frame.writer = *writer;
		frame.has_writer = true;
	}
	std_landing(&frame, txn_record_written_body, txn_record_written_pad);
}

/* txn_record_head writes a record's top-level xid (1), its storage (2 to 4), and a subtransaction's xid (5). */
static void
txn_record_head(ChunksRecordRecordWriter *w, const TxnMemtableVersionWriter *writer, const HostStorage *storage)
{
	chunks_record_RecordWriter_uint(w, 1, writer->has_top ? writer->top : writer->xid);
	chunks_record_RecordWriter_uint(w, 2, storage->tablespace);
	chunks_record_RecordWriter_uint(w, 3, storage->database);
	chunks_record_RecordWriter_uint(w, 4, storage->relation);
	if (writer->has_top)
		chunks_record_RecordWriter_uint(w, 5, writer->xid);
}

/*
 * TxnRecordRead is a record's decode's state across its pad: its fields, its
 * record, the result it builds, and a decoder's own parts, each while live.
 */
typedef struct TxnRecordRead
{
	const uint8 *body;
	size_t		body_len;
	HostError  *error;
	bool		ok;
	StdVec		known;
	bool		known_live;
	ChunksRecordRecord record;
	bool		record_live;
	void	   *result;
	void		(*drop) (void *result);
	bool		result_live;
	bool		(*read) (struct TxnRecordRead *read);
	StdVec		nested;
	bool		nested_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	TreesNodeFlat flat;
	bool		flat_live;
	StdVec		records;
	bool		records_live;
	StdVec		blocks;
	bool		blocks_live;
} TxnRecordRead;

static void txn_record_records_drop(StdVec *records);

/* txn_record_read_free frees what a decode holds live: a decoder's parts, the record, the fields, then the result. */
static void
txn_record_read_free(void *read_)
{
	TxnRecordRead *read = read_;

	if (read->blocks_live)
		std_vec_free_Checked(&read->blocks, sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t));
	if (read->records_live)
		txn_record_records_drop(&read->records);
	if (read->flat_live)
		trees_node_Flat_drop(&read->flat);
	if (read->chunk_live)
		chunks_chunk_Chunk_drop(&read->chunk);
	if (read->stamps_live)
		chunks_trailer_Stamps_drop(&read->stamps);
	if (read->nested_live)
		std_vec_free_Checked(&read->nested, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	if (read->record_live)
		chunks_record_Record_drop(&read->record);
	if (read->known_live)
		std_vec_free_Checked(&read->known, sizeof(ChunksRecordField), _Alignof(ChunksRecordField));
	if (read->result_live && read->drop != NULL)
		read->drop(read->result);
	read->blocks_live = read->records_live = false;
	read->flat_live = read->chunk_live = read->stamps_live = read->nested_live = false;
	read->record_live = read->known_live = read->result_live = false;
}

/* txn_record_read_body runs the decoder, its result kept when it reads the body. */
static void
txn_record_read_body(void *read_)
{
	TxnRecordRead *read = read_;

	read->ok = read->read(read);
	if (read->ok)
		read->result_live = false;
}

/* txn_record_read runs a decoder over a body, freeing what it holds after it, and its result on a failure. */
static bool
txn_record_read(TxnRecordRead *read)
{
	std_landing(read, txn_record_read_body, txn_record_read_free);
	txn_record_read_free(read);
	return read->ok;
}

/* txn_record_known reads a body's fields, refusing as data corruption one whose number isn't among those allowed. */
static bool
txn_record_known(TxnRecordRead *read, const uint32 *allowed, size_t allowed_len)
{
	ChunksErrorError failed;

	if (!chunks_record_fields(read->body, read->body_len, &read->known, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->known_live = true;
	for (size_t i = 0; i < read->known.len; i++)
	{
		uint32		number = STD_VEC_AT(&read->known, ChunksRecordField, i).number;
		bool		listed = false;

		for (size_t j = 0; j < allowed_len && !listed; j++)
			listed = allowed[j] == number;
		if (!listed)
		{
			*read->error = txn_record_corrupt();
			return false;
		}
	}
	return true;
}

/* txn_record_has reports whether the body's fields hold a number. */
static bool
txn_record_has(const TxnRecordRead *read, uint32 number)
{
	for (size_t i = 0; i < read->known.len; i++)
		if (STD_VEC_AT(&read->known, ChunksRecordField, i).number == number)
			return true;
	return false;
}

/* txn_record_value reads the body as a record with no addresses. */
static bool
txn_record_value(TxnRecordRead *read)
{
	ChunksErrorError failed;

	if (!chunks_record_Record_value(read->body, read->body_len, TXN_RECORD_NO_ADDRESSES, 0, &read->record, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->record_live = true;
	return true;
}

/* txn_record_refuse sets the read's corruption error and answers false. */
static bool
txn_record_refuse(TxnRecordRead *read)
{
	*read->error = txn_record_corrupt();
	return false;
}

/* txn_record_copy copies a field's bytes into a vector, refusing a body without the field. */
static bool
txn_record_copy(TxnRecordRead *read, uint32 number, StdVec *out)
{
	size_t		len;
	const uint8 *bytes = chunks_record_Record_bytes(&read->record, number, &len);

	if (bytes == NULL)
		return txn_record_refuse(read);
	std_slice_to_vec_Checked(out, bytes, len, 1, 1);
	return true;
}

/* txn_record_top reads a writer's top-level xid (1) and a subtransaction's (5), with a command of 0. */
static bool
txn_record_top(const ChunksRecordRecord *r, TxnMemtableVersionWriter *writer, HostError *error)
{
	uint64		top;
	uint64		sub;
	bool		has_sub;

	if (!txn_record_uint(r, 1, &top, error) || !txn_record_optional(r, 5, &has_sub, &sub, error))
		return false;
	writer->has_top = has_sub;
	writer->xid = has_sub ? sub : top;
	writer->top = has_sub ? top : 0;
	writer->command = 0;
	return true;
}

/* txn_record_completion_fill writes a completion's fields, its kill's mark when the self is a killed one's. */
static void
txn_record_completion_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordCompletion *self = self_;

	txn_record_head(w, &self->writer, &self->storage);
	chunks_record_RecordWriter_field(w, 6, self->key.ptr, self->key.len);
	chunks_record_RecordWriter_uint(w, 9, self->tuple.f0);
}

/* txn_record_killed_fill writes a killed completion's fields and its mark. */
static void
txn_record_killed_fill(ChunksRecordRecordWriter *w, const void *self)
{
	txn_record_completion_fill(w, self);
	chunks_record_RecordWriter_uint(w, 29, TXN_RECORD_KILLED);
}

/* txn_record_Completion_encode returns the record's body, a kill's with its mark. */
void
txn_record_Completion_encode(const TxnRecordCompletion *self, bool killed, StdVec *result)
{
	txn_record_written(killed ? txn_record_killed_fill : txn_record_completion_fill, self, NULL,
					   8 * 21 + self->key.len, result);
}

/* txn_record_killed_read reads whether a row change's body carries a kill's mark. */
static bool
txn_record_killed_read(TxnRecordRead *read)
{
	bool		has;
	uint64		mark;

	if (!txn_record_value(read) || !txn_record_optional(&read->record, 29, &has, &mark, read->error))
		return false;
	*(bool *) read->result = has && mark == TXN_RECORD_KILLED;
	return true;
}

/* txn_record_Completion_killed reports whether a row change record's body is a speculative insert's kill. */
bool
txn_record_Completion_killed(const uint8 *body, size_t body_len, bool *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.read = txn_record_killed_read};

	return txn_record_read(&read);
}

/* txn_record_Completion_drop_void frees a completion's key. */
static void
txn_record_Completion_drop_void(void *self)
{
	txn_record_Completion_drop(self);
}

/* txn_record_Completion_drop frees a completion's key. */
void
txn_record_Completion_drop(TxnRecordCompletion *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
}

/* txn_record_completion_read reads a completion's body in the Rust's order. */
static bool
txn_record_completion_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {1, 2, 3, 4, 5, 6, 9, 29};
	TxnRecordCompletion *result = read->result;
	bool		has;
	uint64		mark;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read) ||
		!txn_record_optional(&read->record, 29, &has, &mark, read->error))
		return false;
	if (has && mark != TXN_RECORD_KILLED)
		return txn_record_refuse(read);
	return txn_record_top(&read->record, &result->writer, read->error) &&
		txn_record_storage(&read->record, &result->storage, read->error) &&
		txn_record_copy(read, 6, &result->key) && txn_record_uint(&read->record, 9, &result->tuple.f0, read->error);
}

/*
 * txn_record_Completion_decode reads a completion's body, refusing any other
 * form as data corruption: a field it doesn't know, a mark other than a
 * kill's, or an integer past its width.
 */
bool
txn_record_Completion_decode(const uint8 *body, size_t body_len, TxnRecordCompletion *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.drop = txn_record_Completion_drop_void, .result_live = true, .read = txn_record_completion_read};

	std_vec_new(&result->key, 1, 1);
	return txn_record_read(&read);
}

/* txn_record_page_left_fill writes a leave's fields: its head, the counter it moved to (6), and its command (16). */
static void
txn_record_page_left_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordPageLeft *self = self_;

	txn_record_head(w, &self->writer, &self->storage);
	chunks_record_RecordWriter_uint(w, 6, self->to);
	chunks_record_RecordWriter_uint(w, 16, self->writer.command);
}

/*
 * txn_record_PageLeft_encode_with writes the record's body through a writer
 * with MOST's room, which it takes, allocating nothing, so a leave calls it
 * inside its checkpoint window.
 */
void
txn_record_PageLeft_encode_with(const TxnRecordPageLeft *self, ChunksRecordRecordWriter *w, StdVec *result)
{
	txn_record_written(txn_record_page_left_fill, self, w, 0, result);
}

/* txn_record_PageLeft_encode returns the record's body, as encode_with writes it. */
void
txn_record_PageLeft_encode(const TxnRecordPageLeft *self, StdVec *result)
{
	txn_record_written(txn_record_page_left_fill, self, NULL, TXN_RECORD_PAGE_LEFT_MOST, result);
}

/* txn_record_page_left_read reads a leave's body, refusing a counter that names no page's first tuple id. */
static bool
txn_record_page_left_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {1, 2, 3, 4, 5, 6, 16};
	TxnRecordPageLeft *result = read->result;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read) ||
		!txn_record_uint(&read->record, 6, &result->to, read->error))
		return false;
	if ((result->to & 0xffff) != 1 || result->to >> 16 == 0)
		return txn_record_refuse(read);
	return txn_record_writer(&read->record, &result->writer, read->error) &&
		txn_record_storage(&read->record, &result->storage, read->error);
}

/*
 * txn_record_PageLeft_decode reads a leave's body, refusing any other form
 * as data corruption: a field it doesn't know, a counter that names no page's
 * first tuple id, or an integer past its width.
 */
bool
txn_record_PageLeft_decode(const uint8 *body, size_t body_len, TxnRecordPageLeft *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.read = txn_record_page_left_read};

	return txn_record_read(&read);
}

/* txn_record_spent_fill writes a spent place's fields: its head, its tuple id (9), and its heap bytes (22). */
static void
txn_record_spent_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordSpent *self = self_;

	txn_record_head(w, &self->writer, &self->storage);
	chunks_record_RecordWriter_uint(w, 9, self->tuple.f0);
	chunks_record_RecordWriter_uint(w, 22, self->heap_bytes);
}

/*
 * txn_record_Spent_encode_with writes the record's body through a writer
 * with MOST's room, which it takes, allocating nothing, so a refused write
 * calls it inside its checkpoint window.
 */
void
txn_record_Spent_encode_with(const TxnRecordSpent *self, ChunksRecordRecordWriter *w, StdVec *result)
{
	txn_record_written(txn_record_spent_fill, self, w, 0, result);
}

/* txn_record_spent_is_read reports whether a body has no key (field 6), which every other row change has. */
static bool
txn_record_spent_is_read(TxnRecordRead *read)
{
	ChunksErrorError failed;

	if (!chunks_record_fields(read->body, read->body_len, &read->known, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->known_live = true;
	*(bool *) read->result = !txn_record_has(read, 6);
	return true;
}

/* txn_record_Spent_is reports whether a row change record's body is a spent place's. */
bool
txn_record_Spent_is(const uint8 *body, size_t body_len, bool *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.read = txn_record_spent_is_read};

	return txn_record_read(&read);
}

/* txn_record_spent_read reads a spent place's body in the Rust's order. */
static bool
txn_record_spent_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {1, 2, 3, 4, 5, 9, 22};
	TxnRecordSpent *result = read->result;

	return txn_record_known(read, allowed, lengthof(allowed)) && txn_record_value(read) &&
		txn_record_top(&read->record, &result->writer, read->error) &&
		txn_record_storage(&read->record, &result->storage, read->error) &&
		txn_record_uint(&read->record, 9, &result->tuple.f0, read->error) &&
		txn_record_uint(&read->record, 22, &result->heap_bytes, read->error);
}

/*
 * txn_record_Spent_decode reads a spent place's body, refusing any other
 * form as data corruption: a field it doesn't know, or an integer past its
 * width.
 */
bool
txn_record_Spent_decode(const uint8 *body, size_t body_len, TxnRecordSpent *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.read = txn_record_spent_read};

	return txn_record_read(&read);
}

/* txn_record_chunk_fill writes a chunk's fields, each numbered on its own. */
static void
txn_record_chunk_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordChunkWrite *self = self_;

	chunks_record_RecordWriter_uint(w, 1, self->repository);
	chunks_record_RecordWriter_field(w, 2, self->address.f0, sizeof(self->address.f0));
	chunks_record_RecordWriter_field(w, 3, self->bytes.ptr, self->bytes.len);
	if (self->has_node)
	{
		chunks_record_RecordWriter_field(w, 5, self->node.stamps.ptr, self->node.stamps.len);
		chunks_record_RecordWriter_uint(w, 6, self->node.tablespace);
		chunks_record_RecordWriter_uint(w, 7, self->node.relation);
		chunks_record_RecordWriter_uint(w, 8, self->node.point);
		chunks_record_RecordWriter_uint(w, 9, self->node.writer);
	}
	if (self->has_writer)
	{
		chunks_record_RecordWriter_uint(w, 10, self->writer.xid);
		if (self->writer.has_top)
			chunks_record_RecordWriter_uint(w, 11, self->writer.top);
	}
}

/*
 * txn_record_ChunkWrite_encode returns the record's body: the repository's
 * number, the chunk's address, its bytes, a built base's node's stamps,
 * storage, fold point, and writer, and a blob's chunk's writer's xid and,
 * for a subtransaction, its top-level xid.
 */
void
txn_record_ChunkWrite_encode(const TxnRecordChunkWrite *self, StdVec *result)
{
	txn_record_written(txn_record_chunk_fill, self, NULL,
					   80 + self->bytes.len + (self->has_node ? self->node.stamps.len : 0), result);
}

/* txn_record_ChunkWrite_drop frees a chunk's bytes and a node's stamps. */
void
txn_record_ChunkWrite_drop(TxnRecordChunkWrite *self)
{
	std_vec_free_Checked(&self->bytes, 1, 1);
	if (self->has_node)
		std_vec_free_Checked(&self->node.stamps, 1, 1);
}

/* txn_record_ChunkWrite_drop_void is ChunkWrite's drop of a decode's result. */
static void
txn_record_ChunkWrite_drop_void(void *self)
{
	txn_record_ChunkWrite_drop(self);
}

/* txn_record_pieces_frame_t is pieces' state across its pad: the chunk it takes, its piece, and its value. */
typedef struct txn_record_pieces_frame_t
{
	TxnRecordChunkWrite *self;
	bool		self_live;
	txn_TupleVecU8VecU8_t piece;
	bool		piece_live;
	StdVec		value;
	bool		value_live;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_record_pieces_frame_t;

/* txn_record_pieces_body makes the chunk's pieces, taking what it holds as it moves into them. */
static void
txn_record_pieces_body(void *frame_)
{
	txn_record_pieces_frame_t *frame = frame_;
	TxnRecordChunkWrite *self = frame->self;
	uint8		key[TXN_MEMTABLE_BRANCH_NODE_KEY];

	if (!self->has_node)
	{
		std_slice_to_vec_Checked(&frame->piece.f0, self->address.f0, sizeof(self->address.f0), 1, 1);
		frame->piece.f1 = self->bytes;
		frame->piece_live = true;
		frame->self_live = false;
		std_vec_with_capacity_Checked(frame->result, 1, sizeof(frame->piece), _Alignof(txn_TupleVecU8VecU8_t));
		std_vec_push_Checked(frame->result, &frame->piece, sizeof(frame->piece), _Alignof(txn_TupleVecU8VecU8_t));
		frame->piece_live = false;
		frame->ok = true;
		return;
	}
	txn_memtable_branch_node_key(self->address.f0, self->node.tablespace, self->node.relation, self->node.point, key);
	std_vec_with_capacity_Checked(&frame->value, self->bytes.len + self->node.stamps.len, 1, 1);
	frame->value_live = true;
	std_vec_extend_from_slice_Checked(&frame->value, self->bytes.ptr, self->bytes.len, 1, 1);
	std_vec_extend_from_slice_Checked(&frame->value, self->node.stamps.ptr, self->node.stamps.len, 1, 1);
	frame->ok = txn_memtable_branch_pieces(key, self->node.writer, frame->value.ptr, frame->value.len, frame->result,
										   frame->error);
}

/* txn_record_pieces_pad frees what pieces holds live: its value, its piece, and what is left of the chunk. */
static void
txn_record_pieces_pad(void *frame_)
{
	txn_record_pieces_frame_t *frame = frame_;

	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	if (frame->piece_live)
	{
		std_vec_free_Checked(&frame->piece.f0, 1, 1);
		std_vec_free_Checked(&frame->piece.f1, 1, 1);
	}
	if (frame->self_live)
		txn_record_ChunkWrite_drop(frame->self);
	frame->value_live = frame->piece_live = frame->self_live = false;
}

/*
 * txn_record_ChunkWrite_pieces returns the chunk as the working state's delta
 * keeps it, taking it: a blob's chunk its bytes under its address, and a built
 * base's node its bytes with the trailer of its stamps in pieces under its key.
 */
bool
txn_record_ChunkWrite_pieces(TxnRecordChunkWrite *self, StdVec *result, HostError *error)
{
	txn_record_pieces_frame_t frame = {.self = self, .self_live = true, .result = result, .error = error};

	std_landing(&frame, txn_record_pieces_body, txn_record_pieces_pad);
	txn_record_pieces_pad(&frame);
	return frame.ok;
}

/*
 * txn_record_chunk_stamps checks a node's stamps: a tree's node's, counting
 * none or the node's entries, a leaf's entries or an internal node's
 * subtrees.
 */
static bool
txn_record_chunk_stamps(TxnRecordRead *read, const TxnRecordChunkWrite *result)
{
	const uint8 *bytes = result->bytes.ptr;
	StdBytes	hashed;
	ChunksErrorError failed;
	TreesNodeNode node;
	size_t		count;
	size_t		counted = 0;

	if (result->bytes.len == 0 || (bytes[0] != CHUNKS_HEADER_TREE_LEAF && bytes[0] != CHUNKS_HEADER_TREE_INTERNAL))
		return txn_record_refuse(read);
	if (!chunks_trailer_unlocated(result->node.stamps.ptr, result->node.stamps.len, &hashed, &read->stamps, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->stamps_live = true;
	chunks_chunk_Chunk_stored(result->bytes.ptr, result->bytes.len, TXN_RECORD_NO_LOCATORS, 0, &read->stamps,
							  &read->chunk);
	read->chunk_live = true;
	if (!trees_node_Flat_of(&read->chunk, &read->flat, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->flat_live = true;
	node = trees_node_Flat_node(&read->flat);
	count = trees_node_Node_len(&node);
	if (trees_node_Node_is_leaf(&node))
		counted = chunks_trailer_Stamps_entries(&read->stamps);
	else
	{
		uint64		max;

		while (counted <= count && chunks_trailer_Stamps_subtree_max(&read->stamps, counted, &max))
			counted++;
	}
	if (counted != 0 && counted != count)
		return txn_record_refuse(read);
	return true;
}

/* txn_record_chunk_read reads a chunk's body in the Rust's order. */
static bool
txn_record_chunk_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11};
	TxnRecordChunkWrite *result = read->result;
	ChunksRecordRecord *r = &read->record;
	bool		present[5];
	bool		some = false;
	bool		all = true;
	uint64		repository;
	size_t		address_len;
	const uint8 *address;
	ChunksAddressAddress hashed;

	if (!txn_record_known(read, allowed, lengthof(allowed)))
		return false;
	for (int i = 0; i < 5; i++)
	{
		present[i] = txn_record_has(read, (uint32) (5 + i));
		some |= present[i];
		all &= present[i];
	}
	if (some && !all)
		return txn_record_refuse(read);
	if (txn_record_has(read, 10) == present[0] || (txn_record_has(read, 11) && !txn_record_has(read, 10)))
		return txn_record_refuse(read);
	if (!txn_record_value(read) || !txn_record_uint(r, 1, &repository, read->error))
		return false;
	if (repository == 0 || repository > PG_UINT32_MAX)
		return txn_record_refuse(read);
	result->repository = (uint32) repository;
	address = chunks_record_Record_bytes(r, 2, &address_len);
	if (address == NULL || address_len != 32)
		return txn_record_refuse(read);
	memcpy(result->address.f0, address, 32);
	if (!txn_record_copy(read, 3, &result->bytes))
		return false;
	hashed = chunks_address_Address_of(result->bytes.ptr, result->bytes.len);
	if (!chunks_address_Address_PartialEq_eq(&hashed, &result->address))
		return txn_record_refuse(read);
	if (present[0])
	{
		if (!txn_record_copy(read, 5, &result->node.stamps))
			return false;
		result->has_node = true;
		if (!txn_record_chunk_stamps(read, result) ||
			!txn_record_narrow(r, 6, &result->node.tablespace, read->error) ||
			!txn_record_narrow(r, 7, &result->node.relation, read->error) ||
			!txn_record_uint(r, 8, &result->node.point, read->error) ||
			!txn_record_uint(r, 9, &result->node.writer, read->error))
			return false;
	}
	result->has_writer = txn_record_has(read, 10);
	if (result->has_writer)
	{
		result->writer.has_top = txn_record_has(read, 11);
		result->writer.command = 0;
		if ((result->writer.has_top && !txn_record_uint(r, 11, &result->writer.top, read->error)) ||
			!txn_record_uint(r, 10, &result->writer.xid, read->error))
			return false;
	}
	return true;
}

/*
 * txn_record_ChunkWrite_decode reads a chunk's body, refusing any other form
 * as data corruption: a field it doesn't know, a repository number past 32
 * bits or zero, an address of another length, bytes whose hash isn't the
 * address, stamps on a chunk that isn't a tree's node or whose count isn't
 * the node's entries', a node's part incomplete, and a writer where the
 * chunk's kind doesn't take one.
 */
bool
txn_record_ChunkWrite_decode(const uint8 *body, size_t body_len, TxnRecordChunkWrite *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.drop = txn_record_ChunkWrite_drop_void, .result_live = true, .read = txn_record_chunk_read};

	std_vec_new(&result->bytes, 1, 1);
	result->has_node = false;
	result->has_writer = false;
	return txn_record_read(&read);
}

/* txn_record_database_fill writes a database change's fields. */
static void
txn_record_database_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordDatabaseChange *self = self_;

	chunks_record_RecordWriter_uint(w, 1, (uint64) self->tree);
	chunks_record_RecordWriter_uint(w, 2, self->repository);
	chunks_record_RecordWriter_uint(w, 3, self->writer.xid);
	if (self->writer.has_top)
		chunks_record_RecordWriter_uint(w, 4, self->writer.top);
	chunks_record_RecordWriter_uint(w, 5, self->writer.command);
	chunks_record_RecordWriter_field(w, 6, self->key.ptr, self->key.len);
	if (self->has_value)
		chunks_record_RecordWriter_field(w, 7, self->value.ptr, self->value.len);
}

/*
 * txn_record_DatabaseChange_encode returns the record's body: the tree, the
 * repository's number, the writer's xid, its top-level xid when it is a
 * subtransaction, its command, the key, and the value when it has one.
 */
void
txn_record_DatabaseChange_encode(const TxnRecordDatabaseChange *self, StdVec *result)
{
	txn_record_written(txn_record_database_fill, self, NULL,
					   8 * 21 + self->key.len + (self->has_value ? self->value.len : 0), result);
}

/* txn_record_DatabaseChange_drop frees a database change's key and value. */
void
txn_record_DatabaseChange_drop(TxnRecordDatabaseChange *self)
{
	std_vec_free_Checked(&self->key, 1, 1);
	if (self->has_value)
		std_vec_free_Checked(&self->value, 1, 1);
}

/* txn_record_DatabaseChange_drop_void is DatabaseChange's drop of a decode's result. */
static void
txn_record_DatabaseChange_drop_void(void *self)
{
	txn_record_DatabaseChange_drop(self);
}

/* txn_record_database_read reads a database change's body in the Rust's order. */
static bool
txn_record_database_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {1, 2, 3, 4, 5, 6, 7};
	TxnRecordDatabaseChange *result = read->result;
	ChunksRecordRecord *r = &read->record;
	uint64		tree;
	uint32		repository;
	size_t		value_len;
	const uint8 *value;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read) ||
		!txn_record_uint(r, 1, &tree, read->error))
		return false;
	if (tree != TXN_RECORD_TREE_IDENTITY)
		return txn_record_refuse(read);
	result->tree = TXN_RECORD_TREE_IDENTITY;
	if (!txn_record_optional(r, 4, &result->writer.has_top, &result->writer.top, read->error) ||
		!txn_record_uint(r, 3, &result->writer.xid, read->error) ||
		!txn_record_narrow(r, 5, &result->writer.command, read->error) ||
		!txn_record_narrow(r, 2, &repository, read->error))
		return false;
	if (repository == 0)
		return txn_record_refuse(read);
	result->repository = repository;
	if (!txn_record_copy(read, 6, &result->key))
		return false;
	value = chunks_record_Record_bytes(r, 7, &value_len);
	result->has_value = value != NULL;
	if (value != NULL)
		std_slice_to_vec_Checked(&result->value, value, value_len, 1, 1);
	return true;
}

/*
 * txn_record_DatabaseChange_decode reads a database change's body, refusing
 * any other form as data corruption: a field it doesn't know, a tree none is,
 * a repository number past 32 bits or zero, or a command past 32 bits.
 */
bool
txn_record_DatabaseChange_decode(const uint8 *body, size_t body_len, TxnRecordDatabaseChange *result,
								 HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.drop = txn_record_DatabaseChange_drop_void, .result_live = true, .read = txn_record_database_read};

	std_vec_new(&result->key, 1, 1);
	result->has_value = false;
	return txn_record_read(&read);
}

/* txn_record_advanced_frame_t is BaseAdvanced's encode's state across its pad: its kept base and packs' ids. */
typedef struct txn_record_advanced_frame_t
{
	const TxnRecordBaseAdvanced *self;
	StdVec	   *result;
	StdVec		kept;
	bool		kept_live;
	StdVec		packs;
	bool		packs_live;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
	ChunksRecordRecordWriter nested;
	bool		nested_live;
	StdVec		value;
	bool		value_live;
} txn_record_advanced_frame_t;

/* txn_record_advanced_body writes a fold's fields, a chain's entries as a nested record of their chain values. */
static void
txn_record_advanced_body(void *frame_)
{
	txn_record_advanced_frame_t *frame = frame_;
	const TxnRecordBaseAdvanced *self = frame->self;
	ChunksRecordRecordWriter *w = &frame->writer;

	txn_state_kept(&self->base, &frame->kept);
	frame->kept_live = true;
	std_vec_with_capacity_Checked(&frame->packs, 16 * self->packs.len, 1, 1);
	frame->packs_live = true;
	for (size_t i = 0; i < self->packs.len; i++)
		std_vec_extend_from_slice_Checked(&frame->packs, STD_VEC_AT(&self->packs, ChunksPackPackId, i).f0, 16, 1, 1);
	frame->writer = chunks_record_RecordWriter_with_capacity(64 + frame->kept.len + frame->packs.len);
	frame->writer_live = true;
	chunks_record_RecordWriter_uint(w, 2, self->storage.tablespace);
	chunks_record_RecordWriter_uint(w, 3, self->storage.database);
	chunks_record_RecordWriter_uint(w, 4, self->storage.relation);
	chunks_record_RecordWriter_uint(w, 6, self->folded);
	chunks_record_RecordWriter_uint(w, 7, self->max_folded);
	chunks_record_RecordWriter_field(w, 8, frame->kept.ptr, frame->kept.len);
	chunks_record_RecordWriter_field(w, 9, frame->packs.ptr, frame->packs.len);
	if (self->built)
		chunks_record_RecordWriter_uint(w, 10, 1);
	if (self->horizon != 0)
		chunks_record_RecordWriter_uint(w, 11, self->horizon);
	if (self->has_chain)
	{
		frame->nested = chunks_record_RecordWriter_new();
		frame->nested_live = true;
		for (size_t i = 0; i < self->chain.len; i++)
		{
			txn_base_chain_value(&STD_VEC_AT(&self->chain, TxnBasePending, i), &frame->value);
			frame->value_live = true;
			chunks_record_RecordWriter_field(&frame->nested, (uint32) (i + 1), frame->value.ptr, frame->value.len);
			frame->value_live = false;
			std_vec_free_Checked(&frame->value, 1, 1);
		}
		frame->nested_live = false;
		chunks_record_RecordWriter_record(w, 13, &frame->nested);
	}
	*frame->result = chunks_record_RecordWriter_finish(w);
}

/* txn_record_advanced_pad frees what the encode holds live. */
static void
txn_record_advanced_pad(void *frame_)
{
	txn_record_advanced_frame_t *frame = frame_;

	if (frame->value_live)
		std_vec_free_Checked(&frame->value, 1, 1);
	if (frame->nested_live)
		chunks_record_RecordWriter_drop(&frame->nested);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
	if (frame->packs_live)
		std_vec_free_Checked(&frame->packs, 1, 1);
	if (frame->kept_live)
		std_vec_free_Checked(&frame->kept, 1, 1);
	frame->value_live = frame->nested_live = frame->writer_live = frame->packs_live = frame->kept_live = false;
}

/*
 * txn_record_BaseAdvanced_encode returns the record's body: the storage, the
 * fold point, the largest applied xid, the base record in a state's kept
 * form, the packs' ids, a built base's mark and its horizon, and a chain
 * record's entries.
 */
void
txn_record_BaseAdvanced_encode(const TxnRecordBaseAdvanced *self, StdVec *result)
{
	txn_record_advanced_frame_t frame = {.self = self, .result = result};

	std_landing(&frame, txn_record_advanced_body, txn_record_advanced_pad);
	txn_record_advanced_pad(&frame);
}

/* txn_record_chain_drop frees a chain of transforms' entries. */
static void
txn_record_chain_drop(StdVec *chain)
{
	for (size_t i = 0; i < chain->len; i++)
		txn_base_Pending_drop(&STD_VEC_AT(chain, TxnBasePending, i));
	std_vec_free_Checked(chain, sizeof(TxnBasePending), _Alignof(TxnBasePending));
}

/* txn_record_BaseAdvanced_drop frees a fold's base, packs, and chain. */
void
txn_record_BaseAdvanced_drop(TxnRecordBaseAdvanced *self)
{
	txn_base_Base_drop(&self->base);
	std_vec_free_Checked(&self->packs, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	if (self->has_chain)
		txn_record_chain_drop(&self->chain);
}

/* TxnRecordAdvancedParts is what a fold's decode has built, each while live, for its drop on a failure. */
typedef struct TxnRecordAdvancedParts
{
	TxnRecordBaseAdvanced *result;
	bool		base_live;
	bool		packs_live;
	bool		chain_live;
	TxnBasePending pending;
	bool		pending_live;
} TxnRecordAdvancedParts;

/* txn_record_advanced_parts_drop frees the parts a fold's decode had built. */
static void
txn_record_advanced_parts_drop(void *parts_)
{
	TxnRecordAdvancedParts *parts = parts_;

	if (parts->pending_live)
		txn_base_Pending_drop(&parts->pending);
	if (parts->chain_live)
		txn_record_chain_drop(&parts->result->chain);
	if (parts->packs_live)
		std_vec_free_Checked(&parts->result->packs, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	if (parts->base_live)
		txn_base_Base_drop(&parts->result->base);
}

/* txn_record_damaged returns the error of a kept base record that doesn't read, with the state's text. */
static HostError
txn_record_damaged(const char *text, size_t text_len)
{
	StdString	message;

	std_string_from_Checked(&message, text, text_len);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/* txn_record_advanced_read reads a fold's body in the Rust's order. */
static bool
txn_record_advanced_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {2, 3, 4, 6, 7, 8, 9, 10, 11, 13};
	TxnRecordAdvancedParts *parts = read->result;
	TxnRecordBaseAdvanced *result = parts->result;
	ChunksRecordRecord *r = &read->record;
	size_t		len;
	const uint8 *bytes;
	bool		some;
	uint64		built;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read))
		return false;
	bytes = chunks_record_Record_bytes(r, 8, &len);
	if (bytes == NULL)
		return txn_record_refuse(read);
	if (!txn_state_unkept(bytes, len, txn_record_damaged, &some, &result->base, read->error))
		return false;
	if (!some)
		return txn_record_refuse(read);
	parts->base_live = true;
	bytes = chunks_record_Record_bytes(r, 9, &len);
	if (bytes == NULL || len % 16 != 0)
		return txn_record_refuse(read);
	std_vec_with_capacity_Checked(&result->packs, len / 16, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	parts->packs_live = true;
	for (size_t at = 0; at < len; at += 16)
	{
		ChunksPackPackId id;

		memcpy(id.f0, bytes + at, 16);
		std_vec_push_Checked(&result->packs, &id, sizeof(id), _Alignof(ChunksPackPackId));
	}
	result->built = false;
	if (txn_record_has(read, 10))
	{
		if (!txn_record_uint(r, 10, &built, read->error))
			return false;
		if (built != 1)
			return txn_record_refuse(read);
		result->built = true;
	}
	result->horizon = 0;
	if (txn_record_has(read, 11))
	{
		if (!result->built)
			return txn_record_refuse(read);
		if (!txn_record_uint(r, 11, &result->horizon, read->error))
			return false;
		if (result->horizon == 0)
			return txn_record_refuse(read);
	}
	result->has_chain = false;
	for (size_t i = 0; i < read->known.len; i++)
	{
		const ChunksRecordField *field = &STD_VEC_AT(&read->known, ChunksRecordField, i);
		ChunksErrorError failed;

		if (field->number != 13)
			continue;
		if (result->built)
			return txn_record_refuse(read);
		if (!chunks_record_fields(field->bytes, field->bytes_len, &read->nested, &failed))
		{
			*read->error = txn_base_storage(&failed);
			return false;
		}
		read->nested_live = true;
		std_vec_new(&result->chain, sizeof(TxnBasePending), _Alignof(TxnBasePending));
		result->has_chain = true;
		parts->chain_live = true;
		for (size_t place = 0; place < read->nested.len; place++)
		{
			const ChunksRecordField *entry = &STD_VEC_AT(&read->nested, ChunksRecordField, place);

			if (entry->number != place + 1)
				return txn_record_refuse(read);
			if (!txn_base_chain_read(entry->bytes, entry->bytes_len, &parts->pending, read->error))
				return false;
			parts->pending_live = true;
			std_vec_push_Checked(&result->chain, &parts->pending, sizeof(TxnBasePending), _Alignof(TxnBasePending));
			parts->pending_live = false;
		}
		break;
	}
	if (!txn_record_storage(r, &result->storage, read->error) ||
		!txn_record_uint(r, 6, &result->folded, read->error) ||
		!txn_record_uint(r, 7, &result->max_folded, read->error))
		return false;
	parts->base_live = parts->packs_live = parts->chain_live = false;
	return true;
}

/*
 * txn_record_BaseAdvanced_decode reads a fold's body, refusing any other form
 * as data corruption: a field it doesn't know, a storage field past 32 bits,
 * a missing or damaged base record, packs' ids not a multiple of sixteen
 * bytes, a built mark other than 1, a horizon on a fold's record or of 0, and
 * chain entries on a built record, out of sequence, or of a form chain_read
 * refuses.
 */
bool
txn_record_BaseAdvanced_decode(const uint8 *body, size_t body_len, TxnRecordBaseAdvanced *result,
							   HostError *error)
{
	TxnRecordAdvancedParts parts = {.result = result};
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = &parts,
	.drop = txn_record_advanced_parts_drop, .result_live = true, .read = txn_record_advanced_read};

	return txn_record_read(&read);
}

/* TXN_RECORD_RUN is the size and alignment of a run of tuple ids: its block, first offset, count, and bytes. */
#define TXN_RECORD_RUN sizeof(std_TupleU32U16U16U64_t), _Alignof(std_TupleU32U16U16U64_t)

/* TXN_RECORD_COUNTED is the size and alignment of a value id and its count. */
#define TXN_RECORD_COUNTED sizeof(std_TupleU32U64_t), _Alignof(std_TupleU32U64_t)

/* TXN_RECORD_TAKE is the size and alignment of a carried take. */
#define TXN_RECORD_TAKE sizeof(TxnRecordCarriedTake), _Alignof(TxnRecordCarriedTake)

/* TXN_RECORD_WRITER is the size and alignment of a record writer, a list's item. */
#define TXN_RECORD_WRITER sizeof(ChunksRecordRecordWriter), _Alignof(ChunksRecordRecordWriter)

/* txn_record_take_drop frees a carried take's entry and its values mark. */
static void
txn_record_take_drop(TxnRecordCarriedTake *take)
{
	if (take->has_value)
		std_vec_free_Checked(&take->value, 1, 1);
	if (take->has_mark)
		std_vec_free_Checked(&take->mark, TXN_RECORD_ID);
}

/* txn_record_writers_drop frees a vector of record writers, each dropped. */
static void
txn_record_writers_drop(StdVec *writers)
{
	for (size_t i = 0; i < writers->len; i++)
		chunks_record_RecordWriter_drop(&STD_VEC_AT(writers, ChunksRecordRecordWriter, i));
	std_vec_free_Checked(writers, TXN_RECORD_WRITER);
}

/* txn_record_carried_frame_t is MapCarried's encode's state across its pad: its items, its item, and its writer. */
typedef struct txn_record_carried_frame_t
{
	const TxnRecordMapCarried *self;
	StdVec	   *result;
	StdVec		items;
	bool		items_live;
	ChunksRecordRecordWriter item;
	bool		item_live;
	StdVec		ids;
	bool		ids_live;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
} txn_record_carried_frame_t;

/* txn_record_carried_body writes each version as a list's item, then the carry's fields. */
static void
txn_record_carried_body(void *frame_)
{
	txn_record_carried_frame_t *frame = frame_;
	const TxnRecordMapCarried *self = frame->self;
	ChunksRecordRecordWriter *w = &frame->writer;

	std_vec_with_capacity_Checked(&frame->items, self->versions.len, TXN_RECORD_WRITER);
	frame->items_live = true;
	for (size_t i = 0; i < self->versions.len; i++)
	{
		const TxnRecordCarriedTake *version = &STD_VEC_AT(&self->versions, TxnRecordCarriedTake, i);
		ChunksRecordRecordWriter *item = &frame->item;
		uint64		op = version->has_mark ? 3 : version->has_value ? 1 : 2;

		frame->item = chunks_record_RecordWriter_new();
		frame->item_live = true;
		chunks_record_RecordWriter_uint(item, 1, version->writer.xid);
		if (version->writer.has_top)
			chunks_record_RecordWriter_uint(item, 2, version->writer.top);
		chunks_record_RecordWriter_uint(item, 3, version->writer.command);
		chunks_record_RecordWriter_uint(item, 4, op);
		if (version->tuple.f0 != 0)
			chunks_record_RecordWriter_uint(item, 5, version->tuple.f0);
		if (version->has_value)
			chunks_record_RecordWriter_field(item, 6, version->value.ptr, version->value.len);
		if (version->ender != 0)
			chunks_record_RecordWriter_uint(item, 7, version->ender);
		if (version->has_mark)
		{
			std_vec_with_capacity_Checked(&frame->ids, 4 * version->mark.len, 1, 1);
			frame->ids_live = true;
			for (size_t j = 0; j < version->mark.len; j++)
			{
				uint32		id = STD_VEC_AT(&version->mark, uint32, j);
				uint8		bytes[4] = {(uint8) id, (uint8) (id >> 8), (uint8) (id >> 16), (uint8) (id >> 24)};

				std_vec_extend_from_slice_Checked(&frame->ids, bytes, 4, 1, 1);
			}
			chunks_record_RecordWriter_field(item, 8, frame->ids.ptr, frame->ids.len);
			frame->ids_live = false;
			std_vec_free_Checked(&frame->ids, 1, 1);
		}
		std_vec_push_Checked(&frame->items, item, TXN_RECORD_WRITER);
		frame->item_live = false;
	}
	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	chunks_record_RecordWriter_uint(w, 2, self->storage.tablespace);
	chunks_record_RecordWriter_uint(w, 3, self->storage.database);
	chunks_record_RecordWriter_uint(w, 4, self->storage.relation);
	chunks_record_RecordWriter_uint(w, 5, self->key);
	frame->items_live = false;
	chunks_record_RecordWriter_list(w, 6, &frame->items);
	if (self->map == TXN_MEMTABLE_MAP_MAP_VALUES)
		chunks_record_RecordWriter_uint(w, 7, 2);
	*frame->result = chunks_record_RecordWriter_finish(w);
}

/* txn_record_carried_pad frees what the encode holds live. */
static void
txn_record_carried_pad(void *frame_)
{
	txn_record_carried_frame_t *frame = frame_;

	if (frame->ids_live)
		std_vec_free_Checked(&frame->ids, 1, 1);
	if (frame->item_live)
		chunks_record_RecordWriter_drop(&frame->item);
	if (frame->items_live)
		txn_record_writers_drop(&frame->items);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
	frame->ids_live = frame->item_live = frame->items_live = frame->writer_live = false;
}

/*
 * txn_record_MapCarried_encode returns the record's body: the storage, the
 * key, the versions as a list, each its writer, its op, its tuple id, a put's
 * entry, an ender, and a mark's value ids, and the map.
 */
void
txn_record_MapCarried_encode(const TxnRecordMapCarried *self, StdVec *result)
{
	txn_record_carried_frame_t frame = {.self = self, .result = result};

	std_landing(&frame, txn_record_carried_body, txn_record_carried_pad);
	txn_record_carried_pad(&frame);
}

/* txn_record_MapCarried_drop frees a carry's versions. */
void
txn_record_MapCarried_drop(TxnRecordMapCarried *self)
{
	for (size_t i = 0; i < self->versions.len; i++)
		txn_record_take_drop(&STD_VEC_AT(&self->versions, TxnRecordCarriedTake, i));
	std_vec_free_Checked(&self->versions, TXN_RECORD_TAKE);
}

/* txn_record_MapCarried_drop_void is MapCarried's drop of a decode's result. */
static void
txn_record_MapCarried_drop_void(void *self)
{
	txn_record_MapCarried_drop(self);
}

/* txn_record_records_drop frees a vector of records a list read, each dropped. */
static void
txn_record_records_drop(StdVec *records)
{
	for (size_t i = 0; i < records->len; i++)
		chunks_record_Record_drop(&STD_VEC_AT(records, ChunksRecordRecord, i));
	std_vec_free_Checked(records, sizeof(ChunksRecordRecord), _Alignof(ChunksRecordRecord));
}

/* txn_record_carried_take reads one carried version from its item, refusing every form decode refuses of it. */
static bool
txn_record_carried_take(TxnRecordRead *read, const ChunksRecordRecord *item, TxnMemtableMapMap map,
						TxnRecordCarriedTake *take)
{
	uint64		op;
	size_t		value_len;
	const uint8 *value;
	size_t		ids_len = 0;
	const uint8 *ids;
	bool		named;
	bool		formed;
	bool		has_ender = chunks_record_Record_field(item, 7) != NULL;

	if (!txn_record_uint(item, 1, &take->writer.xid, read->error) ||
		!txn_record_optional(item, 2, &take->writer.has_top, &take->writer.top, read->error) ||
		!txn_record_narrow(item, 3, &take->writer.command, read->error) ||
		!txn_record_uint(item, 5, &take->tuple.f0, read->error))
		return false;
	value = chunks_record_Record_bytes(item, 6, &value_len);
	if (!txn_record_uint(item, 7, &take->ender, read->error) || !txn_record_uint(item, 4, &op, read->error))
		return false;
	ids = chunks_record_Record_bytes(item, 8, &ids_len);
	if (ids == NULL)
		ids_len = 0;
	named = map == TXN_MEMTABLE_MAP_MAP_FREE ? take->tuple.f0 != 0 || !has_ender :
		chunks_record_Record_field(item, 5) == NULL && !has_ender;
	switch (op)
	{
		case 1:
			formed = value != NULL && (take->ender == 0 || take->ender >= TXN_STATUS_FIRST_NORMAL);
			break;
		case 2:
			formed = value == NULL && !has_ender;
			break;
		case 3:
			formed = map == TXN_MEMTABLE_MAP_MAP_FREE && value == NULL && take->tuple.f0 != 0 && ids_len != 0 &&
				ids_len % 4 == 0 && (take->ender == 0 || take->ender >= TXN_STATUS_FIRST_NORMAL);
			break;
		default:
			formed = false;
			break;
	}
	formed = formed && take->writer.xid != 0 && named && (op == 3 || ids == NULL);
	if (!formed)
		return txn_record_refuse(read);
	take->has_value = value != NULL;
	if (value != NULL)
		std_slice_to_vec_Checked(&take->value, value, value_len, 1, 1);
	take->has_mark = op == 3;
	if (op == 3)
	{
		std_vec_with_capacity_Checked(&take->mark, ids_len / 4, TXN_RECORD_ID);
		for (size_t at = 0; at + 4 <= ids_len; at += 4)
		{
			uint32		id = std_u32_from_le_bytes(ids + at);

			std_vec_push_Checked(&take->mark, &id, TXN_RECORD_ID);
		}
	}
	return true;
}

/* txn_record_carried_read reads a carry's body in the Rust's order. */
static bool
txn_record_carried_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {2, 3, 4, 5, 6, 7};
	TxnRecordMapCarried *result = read->result;
	ChunksRecordRecord *r = &read->record;
	bool		has_map;
	uint64		map;
	const ChunksRecordField *list;
	ChunksErrorError failed;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read) ||
		!txn_record_optional(r, 7, &has_map, &map, read->error))
		return false;
	if (!has_map || map == 1)
		result->map = TXN_MEMTABLE_MAP_MAP_FREE;
	else if (map == 2)
		result->map = TXN_MEMTABLE_MAP_MAP_VALUES;
	else
		return txn_record_refuse(read);
	list = chunks_record_Record_field(r, 6);
	if (list == NULL)
		return txn_record_refuse(read);
	if (!chunks_record_Record_list(r, list, &read->records, &failed))
	{
		*read->error = txn_base_storage(&failed);
		return false;
	}
	read->records_live = true;
	for (size_t i = 0; i < read->records.len; i++)
	{
		TxnRecordCarriedTake take = {0};
		TxnRecordCarriedTake *taking;

		std_vec_push_Checked(&result->versions, &take, TXN_RECORD_TAKE);
		taking = &STD_VEC_AT(&result->versions, TxnRecordCarriedTake, result->versions.len - 1);
		if (!txn_record_carried_take(read, &STD_VEC_AT(&read->records, ChunksRecordRecord, i), result->map,
									 taking))
			return false;
	}
	if (result->versions.len == 0)
		return txn_record_refuse(read);
	return txn_record_narrow(r, 2, &result->storage.tablespace, read->error) &&
		txn_record_narrow(r, 3, &result->storage.database, read->error) &&
		txn_record_narrow(r, 4, &result->storage.relation, read->error) &&
		txn_record_narrow(r, 5, &result->key, read->error);
}

/*
 * txn_record_MapCarried_decode reads a carry's body, refusing any other form
 * as data corruption: a field it doesn't know, an unknown map, a storage
 * field or a key past 32 bits, a key with no versions, and a version of a
 * form its op and its map don't take.
 */
bool
txn_record_MapCarried_decode(const uint8 *body, size_t body_len, TxnRecordMapCarried *result, HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.drop = txn_record_MapCarried_drop_void, .result_live = true, .read = txn_record_carried_read};

	std_vec_new(&result->versions, TXN_RECORD_TAKE);
	return txn_record_read(&read);
}

/* txn_record_emptied_fill writes an emptying's storage (fields 2 to 4). */
static void
txn_record_emptied_fill(ChunksRecordRecordWriter *w, const void *self_)
{
	const TxnRecordStorageEmptied *self = self_;

	chunks_record_RecordWriter_uint(w, 2, self->storage.tablespace);
	chunks_record_RecordWriter_uint(w, 3, self->storage.database);
	chunks_record_RecordWriter_uint(w, 4, self->storage.relation);
}

/* txn_record_StorageEmptied_encode returns the record's body: the storage (fields 2 to 4). */
void
txn_record_StorageEmptied_encode(const TxnRecordStorageEmptied *self, StdVec *result)
{
	txn_record_written(txn_record_emptied_fill, self, NULL, 32, result);
}

/* txn_record_emptied_read reads an emptying's body. */
static bool
txn_record_emptied_read(TxnRecordRead *read)
{
	static const uint32 allowed[] = {2, 3, 4};
	TxnRecordStorageEmptied *result = read->result;

	return txn_record_known(read, allowed, lengthof(allowed)) && txn_record_value(read) &&
		txn_record_storage(&read->record, &result->storage, read->error);
}

/*
 * txn_record_StorageEmptied_decode reads an emptying's body, refusing any
 * other form as data corruption: a field it doesn't know, and a storage field
 * missing or past 32 bits.
 */
bool
txn_record_StorageEmptied_decode(const uint8 *body, size_t body_len, TxnRecordStorageEmptied *result,
								 HostError *error)
{
	TxnRecordRead read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.read = txn_record_emptied_read};

	return txn_record_read(&read);
}

/* txn_record_put_runs appends runs of tuple ids as varints: each run's block, first offset, count, and bytes. */
static void
txn_record_put_runs(const StdVec *runs, StdVec *out)
{
	for (size_t i = 0; i < runs->len; i++)
	{
		const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(runs, std_TupleU32U16U16U64_t, i);

		chunks_varint_put_varint(out, run->f0);
		chunks_varint_put_varint(out, run->f1);
		chunks_varint_put_varint(out, run->f2);
		chunks_varint_put_varint(out, run->f3);
	}
}

/* txn_record_freed_frame_t is TidsFreed's encode's state across its pad: its runs' bytes and its writer. */
typedef struct txn_record_freed_frame_t
{
	const TxnRecordTidsFreed *self;
	StdVec	   *result;
	StdVec		bytes;
	bool		bytes_live;
	ChunksRecordRecordWriter writer;
	bool		writer_live;
} txn_record_freed_frame_t;

/* txn_record_freed_body writes a free's fields, each run field and the values' pairs made just before it. */
static void
txn_record_freed_body(void *frame_)
{
	txn_record_freed_frame_t *frame = frame_;
	const TxnRecordTidsFreed *self = frame->self;
	ChunksRecordRecordWriter *w = &frame->writer;

	frame->writer = chunks_record_RecordWriter_new();
	frame->writer_live = true;
	chunks_record_RecordWriter_uint(w, 1, self->storage.tablespace);
	chunks_record_RecordWriter_uint(w, 2, self->storage.database);
	chunks_record_RecordWriter_uint(w, 3, self->storage.relation);
	chunks_record_RecordWriter_uint(w, 4, self->horizon);
	std_vec_new(&frame->bytes, 1, 1);
	frame->bytes_live = true;
	txn_record_put_runs(&self->runs, &frame->bytes);
	chunks_record_RecordWriter_field(w, 5, frame->bytes.ptr, frame->bytes.len);
	if (self->dead.len != 0)
	{
		frame->bytes.len = 0;
		txn_record_put_runs(&self->dead, &frame->bytes);
		chunks_record_RecordWriter_field(w, 6, frame->bytes.ptr, frame->bytes.len);
	}
	if (self->has_values)
	{
		frame->bytes.len = 0;
		for (size_t i = 0; i < self->values.f1.len; i++)
		{
			const std_TupleU32U64_t *value = &STD_VEC_AT(&self->values.f1, std_TupleU32U64_t, i);

			chunks_varint_put_varint(&frame->bytes, value->f0);
			chunks_varint_put_varint(&frame->bytes, value->f1);
		}
		chunks_record_RecordWriter_uint(w, 7, self->values.f0.tablespace);
		chunks_record_RecordWriter_uint(w, 8, self->values.f0.relation);
		chunks_record_RecordWriter_field(w, 9, frame->bytes.ptr, frame->bytes.len);
	}
	*frame->result = chunks_record_RecordWriter_finish(w);
}

/* txn_record_freed_pad frees what the encode holds live. */
static void
txn_record_freed_pad(void *frame_)
{
	txn_record_freed_frame_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->writer_live)
		chunks_record_RecordWriter_drop(&frame->writer);
	frame->bytes_live = frame->writer_live = false;
}

/*
 * txn_record_TidsFreed_encode returns the record's body: the storage, the
 * horizon, the freed runs, the dead runs, present only when there is one, and
 * the values taken down, present only when there are any.
 */
void
txn_record_TidsFreed_encode(const TxnRecordTidsFreed *self, StdVec *result)
{
	txn_record_freed_frame_t frame = {.self = self, .result = result};

	std_landing(&frame, txn_record_freed_body, txn_record_freed_pad);
	txn_record_freed_pad(&frame);
}

/* txn_record_TidsFreed_drop frees a free's runs and values. */
void
txn_record_TidsFreed_drop(TxnRecordTidsFreed *self)
{
	std_vec_free_Checked(&self->runs, TXN_RECORD_RUN);
	std_vec_free_Checked(&self->dead, TXN_RECORD_RUN);
	if (self->has_values)
		std_vec_free_Checked(&self->values.f1, TXN_RECORD_COUNTED);
}

/* txn_record_TidsFreed_drop_void is TidsFreed's drop of a decode's result. */
static void
txn_record_TidsFreed_drop_void(void *self)
{
	txn_record_TidsFreed_drop(self);
}

/*
 * txn_record_runs reads runs of tuple ids as put_runs writes them, refusing
 * as data corruption a run that is empty, starts at offset zero, passes a
 * page's most tuples, isn't past the run before it, or gives back more bytes
 * than a page holds, and an integer past its width.
 */
static bool
txn_record_runs(TxnRecordRead *read, const uint8 *bytes, size_t len, uint16 most, uint64 page, StdVec *runs)
{
	size_t		at = 0;
	uint64		past = 0;
	ChunksVarintMalformed malformed;

	while (at < len)
	{
		uint64		block;
		uint64		first;
		uint64		count;
		uint64		taken;
		uint64		start;
		std_TupleU32U16U16U64_t run;

		if (!chunks_varint_decode(bytes, len, &at, &block, &malformed) ||
			!chunks_varint_decode(bytes, len, &at, &first, &malformed) ||
			!chunks_varint_decode(bytes, len, &at, &count, &malformed) ||
			!chunks_varint_decode(bytes, len, &at, &taken, &malformed) || block > PG_UINT32_MAX ||
			first > PG_UINT16_MAX || count > PG_UINT16_MAX || first == 0 || count == 0)
			return txn_record_refuse(read);
		start = block << 16 | first;
		if (first + count - 1 > most || start < past || taken > page)
			return txn_record_refuse(read);
		past = start + count;
		run.f0 = (uint32) block;
		run.f1 = (uint16) first;
		run.f2 = (uint16) count;
		run.f3 = taken;
		std_vec_push_Checked(runs, &run, TXN_RECORD_RUN);
	}
	return true;
}

/* txn_record_block_order orders a block's freed bytes by their blocks. */
static int
txn_record_block_order(const void *a, const void *b)
{
	uint32		left = ((const std_TupleU32U64_t *) a)->f0;
	uint32		right = ((const std_TupleU32U64_t *) b)->f0;

	return left < right ? -1 : left > right;
}

/* txn_record_over refuses a body whose runs of both fields give one block back more bytes than a page holds. */
static bool
txn_record_over(TxnRecordRead *read, const TxnRecordTidsFreed *result, uint64 page)
{
	const StdVec *both[2] = {&result->runs, &result->dead};
	uint64		sum = 0;

	std_vec_with_capacity_Checked(&read->blocks, result->runs.len + result->dead.len, TXN_RECORD_COUNTED);
	read->blocks_live = true;
	for (int k = 0; k < 2; k++)
		for (size_t i = 0; i < both[k]->len; i++)
		{
			const std_TupleU32U16U16U64_t *run = &STD_VEC_AT(both[k], std_TupleU32U16U16U64_t, i);
			std_TupleU32U64_t block = {run->f0, run->f3};

			std_vec_push_Checked(&read->blocks, &block, TXN_RECORD_COUNTED);
		}
	if (read->blocks.len > 1)
		qsort(read->blocks.ptr, read->blocks.len, sizeof(std_TupleU32U64_t), txn_record_block_order);
	for (size_t i = 0; i < read->blocks.len; i++)
	{
		const std_TupleU32U64_t *block = &STD_VEC_AT(&read->blocks, std_TupleU32U64_t, i);

		sum = STD_ADD(u64, sum, block->f1, 1241, 85);
		if (i + 1 < read->blocks.len && STD_VEC_AT(&read->blocks, std_TupleU32U64_t, i + 1).f0 == block->f0)
			continue;
		if (sum > page)
			return txn_record_refuse(read);
		sum = 0;
	}
	return true;
}

/* txn_record_freed_read reads a free's body in the Rust's order. */
static bool
txn_record_freed_read(TxnRecordRead *read, uint16 most, uint64 page)
{
	static const uint32 allowed[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
	TxnRecordTidsFreed *result = read->result;
	ChunksRecordRecord *r = &read->record;
	size_t		len;
	const uint8 *bytes;
	const uint8 *pairs;
	size_t		pairs_len;
	bool		toast;
	bool		relation;

	if (!txn_record_known(read, allowed, lengthof(allowed)) || !txn_record_value(read))
		return false;
	bytes = chunks_record_Record_bytes(r, 6, &len);
	if (bytes != NULL && len == 0)
		return txn_record_refuse(read);
	if (bytes != NULL && !txn_record_runs(read, bytes, len, most, page, &result->dead))
		return false;
	bytes = chunks_record_Record_bytes(r, 5, &len);
	if (bytes == NULL)
		return txn_record_refuse(read);
	if (!txn_record_runs(read, bytes, len, most, page, &result->runs) || !txn_record_over(read, result, page) ||
		!txn_record_narrow(r, 1, &result->storage.tablespace, read->error) ||
		!txn_record_narrow(r, 2, &result->storage.database, read->error) ||
		!txn_record_narrow(r, 3, &result->storage.relation, read->error))
		return false;
	toast = chunks_record_Record_field(r, 7) != NULL;
	relation = chunks_record_Record_field(r, 8) != NULL;
	pairs = chunks_record_Record_bytes(r, 9, &pairs_len);
	if (!toast && !relation && pairs == NULL)
		result->has_values = false;
	else if (toast && relation && pairs != NULL && pairs_len != 0)
	{
		size_t		at = 0;
		ChunksVarintMalformed malformed;

		if (!txn_record_narrow(r, 7, &result->values.f0.tablespace, read->error) ||
			!txn_record_narrow(r, 8, &result->values.f0.relation, read->error))
			return false;
		result->values.f0.database = result->storage.database;
		std_vec_new(&result->values.f1, TXN_RECORD_COUNTED);
		result->has_values = true;
		while (at < pairs_len)
		{
			uint64		id;
			std_TupleU32U64_t value;

			if (!chunks_varint_decode(pairs, pairs_len, &at, &id, &malformed) || id > PG_UINT32_MAX ||
				!chunks_varint_decode(pairs, pairs_len, &at, &value.f1, &malformed))
				return txn_record_refuse(read);
			value.f0 = (uint32) id;
			if (value.f1 == 0 || (result->values.f1.len != 0 &&
								  STD_VEC_AT(&result->values.f1, std_TupleU32U64_t,
											 result->values.f1.len - 1).f0 >= value.f0))
				return txn_record_refuse(read);
			std_vec_push_Checked(&result->values.f1, &value, TXN_RECORD_COUNTED);
		}
	}
	else
		return txn_record_refuse(read);
	return txn_record_uint(r, 4, &result->horizon, read->error);
}

/* TxnRecordFreedRead is a free's decode: the shared read with the page's most tuples and its bytes. */
typedef struct TxnRecordFreedRead
{
	TxnRecordRead read;
	uint16		most;
	uint64		page;
} TxnRecordFreedRead;

/* txn_record_freed_read_at reads a free's body at the read's page shape. */
static bool
txn_record_freed_read_at(TxnRecordRead *read)
{
	TxnRecordFreedRead *freed = (TxnRecordFreedRead *) read;

	return txn_record_freed_read(read, freed->most, freed->page);
}

/*
 * txn_record_TidsFreed_decode reads a free's body, refusing any other form as
 * data corruption: a field it doesn't know or lacks, a field 6 with no run, a
 * run runs refuses, a block whose runs of both fields give back more bytes
 * than a page holds, fields 7 to 9 not all present or all absent, and values
 * that are empty, out of id order, or of a count of zero.
 */
bool
txn_record_TidsFreed_decode(const uint8 *body, size_t body_len, uint16 most, uint64 page, TxnRecordTidsFreed *result,
							HostError *error)
{
	TxnRecordFreedRead freed = {.read = {.body = body, .body_len = body_len, .error = error, .result = result,
	.drop = txn_record_TidsFreed_drop_void, .result_live = true, .read = txn_record_freed_read_at},
	.most = most, .page = page};

	std_vec_new(&result->runs, TXN_RECORD_RUN);
	std_vec_new(&result->dead, TXN_RECORD_RUN);
	result->has_values = false;
	return txn_record_read(&freed.read);
}

/* TxnRecordList is a vector to format as a list: its elements' size and Debug. */
typedef struct TxnRecordList
{
	const StdVec *vec;
	size_t		size;
	StdFmtFn	fmt;
} TxnRecordList;

/* txn_record_List_Debug_fmt is a Vec's Debug, a list of its elements. */
static bool
txn_record_List_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordList *list = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t i = 0; i < list->vec->len; i++)
		std_DebugList_entry(&l, (const char *) list->vec->ptr + i * list->size, list->fmt);
	return std_DebugList_finish(&l);
}

/* txn_record_blob_eq is (Address, u64)'s PartialEq: a stored value's root and its length. */
static bool
txn_record_blob_eq(const void *a, const void *b)
{
	const chunks_TupleAddressU64_t *self = a;
	const chunks_TupleAddressU64_t *other = b;

	return chunks_address_Address_PartialEq_eq(&self->f0, &other->f0) && self->f1 == other->f1;
}

/* txn_record_blob_Debug_fmt is (Address, u64)'s Debug. */
static bool
txn_record_blob_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_TupleAddressU64_t *self = value;
	StdTupleRef tuple = {2, {{&self->f0, chunks_address_Address_Debug_fmt}, {&self->f1, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_record_usize_eq is usize's PartialEq. */
static bool
txn_record_usize_eq(const void *a, const void *b)
{
	return *(const size_t *) a == *(const size_t *) b;
}

/* txn_record_u32_eq is u32's PartialEq. */
static bool
txn_record_u32_eq(const void *a, const void *b)
{
	return *(const uint32 *) a == *(const uint32 *) b;
}

/* txn_record_Toast_PartialEq_eq reports whether two TOAST relations are equal field by field. */
bool
txn_record_Toast_PartialEq_eq(const TxnRecordToast *self, const TxnRecordToast *other)
{
	return self->tablespace == other->tablespace && self->relation == other->relation && self->table == other->table;
}

/* txn_record_Toast_Debug_fmt is Toast's derived Debug. */
bool
txn_record_Toast_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordToast *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Toast", 5);

	std_DebugStruct_field(&s, "tablespace", 10, &self->tablespace, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "relation", 8, &self->relation, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "table", 5, &self->table, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_RowChange_PartialEq_eq reports whether two row changes are equal field by field. */
bool
txn_record_RowChange_PartialEq_eq(const TxnRecordRowChange *self, const TxnRecordRowChange *other)
{
	return txn_memtable_version_Writer_PartialEq_eq(&self->writer, &other->writer) &&
		host_Storage_PartialEq_eq(&self->storage, &other->storage) && self->table == other->table &&
		trees_store_VecU8_PartialEq_eq(&self->key, &other->key) && txn_memtable_Op_PartialEq_eq(&self->op, &other->op) &&
		self->no_key == other->no_key && self->frozen == other->frozen && self->heap_bytes == other->heap_bytes &&
		self->has_toast == other->has_toast &&
		(!self->has_toast || txn_record_Toast_PartialEq_eq(&self->toast, &other->toast)) &&
		std_vec_PartialEq_eq(&self->blobs, &other->blobs, sizeof(chunks_TupleAddressU64_t), txn_record_blob_eq) &&
		std_vec_PartialEq_eq(&self->kept, &other->kept, sizeof(size_t), txn_record_usize_eq) &&
		self->speculative == other->speculative && self->has_replaced == other->has_replaced &&
		(!self->has_replaced || host_Row_PartialEq_eq(&self->replaced, &other->replaced)) &&
		self->has_index == other->has_index && (!self->has_index || self->index == other->index);
}

/* txn_record_RowChange_Debug_fmt is RowChange's derived Debug. */
bool
txn_record_RowChange_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordRowChange *self = value;
	StdOptionRef toast = {self->has_toast, {&self->toast, txn_record_Toast_Debug_fmt}};
	TxnRecordList blobs = {&self->blobs, sizeof(chunks_TupleAddressU64_t), txn_record_blob_Debug_fmt};
	TxnRecordList kept = {&self->kept, sizeof(size_t), std_usize_Debug_fmt};
	StdOptionRef replaced = {self->has_replaced, {&self->replaced, host_Row_Debug_fmt}};
	StdOptionRef index = {self->has_index, {&self->index, std_u32_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "RowChange", 9);

	std_DebugStruct_field(&s, "writer", 6, &self->writer, txn_memtable_version_Writer_Debug_fmt);
	std_DebugStruct_field(&s, "storage", 7, &self->storage, host_Storage_Debug_fmt);
	std_DebugStruct_field(&s, "table", 5, &self->table, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "key", 3, &self->key, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "op", 2, &self->op, txn_memtable_Op_Debug_fmt);
	std_DebugStruct_field(&s, "no_key", 6, &self->no_key, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "frozen", 6, &self->frozen, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "heap_bytes", 10, &self->heap_bytes, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "toast", 5, &toast, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "blobs", 5, &blobs, txn_record_List_Debug_fmt);
	std_DebugStruct_field(&s, "kept", 4, &kept, txn_record_List_Debug_fmt);
	std_DebugStruct_field(&s, "speculative", 11, &self->speculative, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "replaced", 8, &replaced, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "index", 5, &index, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_PageLeft_PartialEq_eq reports whether two leaves are equal field by field. */
bool
txn_record_PageLeft_PartialEq_eq(const TxnRecordPageLeft *self, const TxnRecordPageLeft *other)
{
	return txn_memtable_version_Writer_PartialEq_eq(&self->writer, &other->writer) &&
		host_Storage_PartialEq_eq(&self->storage, &other->storage) && self->to == other->to;
}

/* txn_record_PageLeft_Debug_fmt is PageLeft's derived Debug. */
bool
txn_record_PageLeft_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordPageLeft *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "PageLeft", 8);

	std_DebugStruct_field(&s, "writer", 6, &self->writer, txn_memtable_version_Writer_Debug_fmt);
	std_DebugStruct_field(&s, "storage", 7, &self->storage, host_Storage_Debug_fmt);
	std_DebugStruct_field(&s, "to", 2, &self->to, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_BuiltNode_PartialEq_eq reports whether two built nodes' parts are equal field by field. */
bool
txn_record_BuiltNode_PartialEq_eq(const TxnRecordBuiltNode *self, const TxnRecordBuiltNode *other)
{
	return trees_store_VecU8_PartialEq_eq(&self->stamps, &other->stamps) && self->tablespace == other->tablespace &&
		self->relation == other->relation && self->point == other->point && self->writer == other->writer;
}

/* txn_record_BuiltNode_Debug_fmt is BuiltNode's derived Debug. */
bool
txn_record_BuiltNode_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordBuiltNode *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "BuiltNode", 9);

	std_DebugStruct_field(&s, "stamps", 6, &self->stamps, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "tablespace", 10, &self->tablespace, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "relation", 8, &self->relation, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "point", 5, &self->point, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "writer", 6, &self->writer, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_ChunkWrite_PartialEq_eq reports whether two chunk writes are equal field by field. */
bool
txn_record_ChunkWrite_PartialEq_eq(const TxnRecordChunkWrite *self, const TxnRecordChunkWrite *other)
{
	return self->repository == other->repository &&
		chunks_address_Address_PartialEq_eq(&self->address, &other->address) &&
		trees_store_VecU8_PartialEq_eq(&self->bytes, &other->bytes) && self->has_node == other->has_node &&
		(!self->has_node || txn_record_BuiltNode_PartialEq_eq(&self->node, &other->node)) &&
		self->has_writer == other->has_writer &&
		(!self->has_writer || txn_memtable_version_Writer_PartialEq_eq(&self->writer, &other->writer));
}

/* txn_record_ChunkWrite_Debug_fmt is ChunkWrite's derived Debug. */
bool
txn_record_ChunkWrite_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordChunkWrite *self = value;
	StdOptionRef node = {self->has_node, {&self->node, txn_record_BuiltNode_Debug_fmt}};
	StdOptionRef writer = {self->has_writer, {&self->writer, txn_memtable_version_Writer_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "ChunkWrite", 10);

	std_DebugStruct_field(&s, "repository", 10, &self->repository, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "address", 7, &self->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "bytes", 5, &self->bytes, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "node", 4, &node, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "writer", 6, &writer, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_Tree_Debug_fmt is Tree's derived Debug: its variant's name. */
bool
txn_record_Tree_Debug_fmt(const void *value, StdFormatter *f)
{
	Assert(*(const TxnRecordTree *) value == TXN_RECORD_TREE_IDENTITY);
	return std_fmt_write_str(f, "Identity", 8);
}

/* txn_record_DatabaseChange_PartialEq_eq reports whether two database changes are equal field by field. */
bool
txn_record_DatabaseChange_PartialEq_eq(const TxnRecordDatabaseChange *self, const TxnRecordDatabaseChange *other)
{
	return self->tree == other->tree && txn_memtable_version_Writer_PartialEq_eq(&self->writer, &other->writer) &&
		self->repository == other->repository && trees_store_VecU8_PartialEq_eq(&self->key, &other->key) &&
		self->has_value == other->has_value && (!self->has_value || trees_store_VecU8_PartialEq_eq(&self->value, &other->value));
}

/* txn_record_DatabaseChange_Debug_fmt is DatabaseChange's derived Debug. */
bool
txn_record_DatabaseChange_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordDatabaseChange *self = value;
	StdOptionRef changed = {self->has_value, {&self->value, chunks_chunk_VecU8_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "DatabaseChange", 14);

	std_DebugStruct_field(&s, "tree", 4, &self->tree, txn_record_Tree_Debug_fmt);
	std_DebugStruct_field(&s, "writer", 6, &self->writer, txn_memtable_version_Writer_Debug_fmt);
	std_DebugStruct_field(&s, "repository", 10, &self->repository, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "key", 3, &self->key, chunks_chunk_VecU8_Debug_fmt);
	std_DebugStruct_field(&s, "value", 5, &changed, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_pack_eq is PackId's PartialEq over a vector's elements. */
static bool
txn_record_pack_eq(const void *a, const void *b)
{
	return chunks_pack_PackId_PartialEq_eq(a, b);
}

/* txn_record_pending_eq is Pending's PartialEq over a vector's elements. */
static bool
txn_record_pending_eq(const void *a, const void *b)
{
	return txn_base_Pending_PartialEq_eq(a, b);
}

/* txn_record_BaseAdvanced_PartialEq_eq reports whether two folds' records are equal field by field. */
bool
txn_record_BaseAdvanced_PartialEq_eq(const TxnRecordBaseAdvanced *self, const TxnRecordBaseAdvanced *other)
{
	return host_Storage_PartialEq_eq(&self->storage, &other->storage) && self->folded == other->folded &&
		self->max_folded == other->max_folded && txn_base_Base_PartialEq_eq(&self->base, &other->base) &&
		std_vec_PartialEq_eq(&self->packs, &other->packs, sizeof(ChunksPackPackId), txn_record_pack_eq) &&
		self->built == other->built && self->horizon == other->horizon && self->has_chain == other->has_chain &&
		(!self->has_chain ||
		 std_vec_PartialEq_eq(&self->chain, &other->chain, sizeof(TxnBasePending), txn_record_pending_eq));
}

/* txn_record_BaseAdvanced_Debug_fmt is BaseAdvanced's derived Debug. */
bool
txn_record_BaseAdvanced_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordBaseAdvanced *self = value;
	TxnRecordList packs = {&self->packs, sizeof(ChunksPackPackId), chunks_pack_PackId_Debug_fmt};
	TxnRecordList chain = {&self->chain, sizeof(TxnBasePending), txn_base_Pending_Debug_fmt};
	StdOptionRef chained = {self->has_chain, {&chain, txn_record_List_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "BaseAdvanced", 12);

	std_DebugStruct_field(&s, "storage", 7, &self->storage, host_Storage_Debug_fmt);
	std_DebugStruct_field(&s, "folded", 6, &self->folded, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "max_folded", 10, &self->max_folded, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "base", 4, &self->base, txn_base_Base_Debug_fmt);
	std_DebugStruct_field(&s, "packs", 5, &packs, txn_record_List_Debug_fmt);
	std_DebugStruct_field(&s, "built", 5, &self->built, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "horizon", 7, &self->horizon, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "chain", 5, &chained, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_CarriedTake_PartialEq_eq reports whether two carried takes are equal field by field. */
bool
txn_record_CarriedTake_PartialEq_eq(const TxnRecordCarriedTake *self, const TxnRecordCarriedTake *other)
{
	return txn_memtable_version_Writer_PartialEq_eq(&self->writer, &other->writer) &&
		host_Row_PartialEq_eq(&self->tuple, &other->tuple) && self->has_value == other->has_value &&
		(!self->has_value || trees_store_VecU8_PartialEq_eq(&self->value, &other->value)) && self->ender == other->ender &&
		self->has_mark == other->has_mark &&
		(!self->has_mark || std_vec_PartialEq_eq(&self->mark, &other->mark, sizeof(uint32), txn_record_u32_eq));
}

/* txn_record_CarriedTake_Debug_fmt is CarriedTake's derived Debug. */
bool
txn_record_CarriedTake_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordCarriedTake *self = value;
	StdOptionRef taken = {self->has_value, {&self->value, chunks_chunk_VecU8_Debug_fmt}};
	TxnRecordList mark = {&self->mark, sizeof(uint32), std_u32_Debug_fmt};
	StdOptionRef marked = {self->has_mark, {&mark, txn_record_List_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "CarriedTake", 11);

	std_DebugStruct_field(&s, "writer", 6, &self->writer, txn_memtable_version_Writer_Debug_fmt);
	std_DebugStruct_field(&s, "tuple", 5, &self->tuple, host_Row_Debug_fmt);
	std_DebugStruct_field(&s, "value", 5, &taken, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "ender", 5, &self->ender, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "mark", 4, &marked, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_take_eq is CarriedTake's PartialEq over a vector's elements. */
static bool
txn_record_take_eq(const void *a, const void *b)
{
	return txn_record_CarriedTake_PartialEq_eq(a, b);
}

/* txn_record_MapCarried_PartialEq_eq reports whether two carried map versions' records are equal field by field. */
bool
txn_record_MapCarried_PartialEq_eq(const TxnRecordMapCarried *self, const TxnRecordMapCarried *other)
{
	return host_Storage_PartialEq_eq(&self->storage, &other->storage) && self->map == other->map &&
		self->key == other->key &&
		std_vec_PartialEq_eq(&self->versions, &other->versions, sizeof(TxnRecordCarriedTake), txn_record_take_eq);
}

/* txn_record_MapCarried_Debug_fmt is MapCarried's derived Debug. */
bool
txn_record_MapCarried_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordMapCarried *self = value;
	TxnRecordList versions = {&self->versions, sizeof(TxnRecordCarriedTake), txn_record_CarriedTake_Debug_fmt};
	StdDebugStruct s = std_fmt_debug_struct(f, "MapCarried", 10);

	std_DebugStruct_field(&s, "storage", 7, &self->storage, host_Storage_Debug_fmt);
	std_DebugStruct_field(&s, "map", 3, &self->map, txn_memtable_map_Map_Debug_fmt);
	std_DebugStruct_field(&s, "key", 3, &self->key, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "versions", 8, &versions, txn_record_List_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_record_run_eq is (u32, u16, u16, u64)'s PartialEq: a run's block, first offset, count, and bytes. */
static bool
txn_record_run_eq(const void *a, const void *b)
{
	const std_TupleU32U16U16U64_t *self = a;
	const std_TupleU32U16U16U64_t *other = b;

	return self->f0 == other->f0 && self->f1 == other->f1 && self->f2 == other->f2 && self->f3 == other->f3;
}

/* txn_record_run_Debug_fmt is (u32, u16, u16, u64)'s Debug. */
static bool
txn_record_run_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU32U16U16U64_t *self = value;
	StdTupleRef tuple = {4, {{&self->f0, std_u32_Debug_fmt}, {&self->f1, std_u16_Debug_fmt},
	{&self->f2, std_u16_Debug_fmt}, {&self->f3, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_record_count_eq is (u32, u64)'s PartialEq: a value id and its count. */
static bool
txn_record_count_eq(const void *a, const void *b)
{
	const std_TupleU32U64_t *self = a;
	const std_TupleU32U64_t *other = b;

	return self->f0 == other->f0 && self->f1 == other->f1;
}

/* txn_record_count_Debug_fmt is (u32, u64)'s Debug. */
static bool
txn_record_count_Debug_fmt(const void *value, StdFormatter *f)
{
	const std_TupleU32U64_t *self = value;
	StdTupleRef tuple = {2, {{&self->f0, std_u32_Debug_fmt}, {&self->f1, std_u64_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_record_values_Debug_fmt is (Storage, Vec<(u32, u64)>)'s Debug: a TOAST storage and its values' counts. */
static bool
txn_record_values_Debug_fmt(const void *value, StdFormatter *f)
{
	const host_TupleStorageVecTupleU32U64_t *self = value;
	TxnRecordList counts = {&self->f1, sizeof(std_TupleU32U64_t), txn_record_count_Debug_fmt};
	StdTupleRef tuple = {2, {{&self->f0, host_Storage_Debug_fmt}, {&counts, txn_record_List_Debug_fmt}}};

	return std_tuple_Debug_fmt(&tuple, f);
}

/* txn_record_TidsFreed_PartialEq_eq reports whether two frees are equal field by field. */
bool
txn_record_TidsFreed_PartialEq_eq(const TxnRecordTidsFreed *self, const TxnRecordTidsFreed *other)
{
	return host_Storage_PartialEq_eq(&self->storage, &other->storage) && self->horizon == other->horizon &&
		std_vec_PartialEq_eq(&self->runs, &other->runs, sizeof(std_TupleU32U16U16U64_t), txn_record_run_eq) &&
		std_vec_PartialEq_eq(&self->dead, &other->dead, sizeof(std_TupleU32U16U16U64_t), txn_record_run_eq) &&
		self->has_values == other->has_values &&
		(!self->has_values ||
		 (host_Storage_PartialEq_eq(&self->values.f0, &other->values.f0) &&
		  std_vec_PartialEq_eq(&self->values.f1, &other->values.f1, sizeof(std_TupleU32U64_t), txn_record_count_eq)));
}

/* txn_record_TidsFreed_Debug_fmt is TidsFreed's derived Debug. */
bool
txn_record_TidsFreed_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnRecordTidsFreed *self = value;
	TxnRecordList runs = {&self->runs, sizeof(std_TupleU32U16U16U64_t), txn_record_run_Debug_fmt};
	TxnRecordList dead = {&self->dead, sizeof(std_TupleU32U16U16U64_t), txn_record_run_Debug_fmt};
	StdOptionRef values = {self->has_values, {&self->values, txn_record_values_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "TidsFreed", 9);

	std_DebugStruct_field(&s, "storage", 7, &self->storage, host_Storage_Debug_fmt);
	std_DebugStruct_field(&s, "horizon", 7, &self->horizon, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "runs", 4, &runs, txn_record_List_Debug_fmt);
	std_DebugStruct_field(&s, "dead", 4, &dead, txn_record_List_Debug_fmt);
	std_DebugStruct_field(&s, "values", 6, &values, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}
