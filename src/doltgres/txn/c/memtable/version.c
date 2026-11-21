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
 * A range's versions: each a record appended to the range's own pages with a
 * line pointer of two of a page's words, its link to its key's older version
 * and its hint, and a key's newest version named by its node's head. A
 * version is pushed only while its range's record is open, all it stores
 * placed in one call and every fallible step before the store that publishes
 * it, and read newest first under the range's lock either way.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/txn/memtable/pages.h"
#include "doltgres/txn/memtable/version.h"

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/testing.h"
#endif

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "version.rs"

/*
 * The flags of a record: TOP, a subtransaction's top-level xid follows;
 * LSN, the end of its WAL record follows; VALUES, its count of value ids
 * follows; REPLAYED, redo pushed it; COMBO, a combo command id follows;
 * ENDING, its writer's command shows for its own row and a combo id for the
 * row it ends; HEAP, its row's heap bytes follow; ENDER, a run's take's
 * row's ender's 32-bit xid follows last.
 */
#define TXN_MEMTABLE_VERSION_TOP 1
#define TXN_MEMTABLE_VERSION_LSN 2
#define TXN_MEMTABLE_VERSION_VALUES 4
#define TXN_MEMTABLE_VERSION_REPLAYED 8
#define TXN_MEMTABLE_VERSION_COMBO 16
#define TXN_MEMTABLE_VERSION_ENDING 32
#define TXN_MEMTABLE_VERSION_HEAP 64
#define TXN_MEMTABLE_VERSION_ENDER 128

/*
 * TXN_MEMTABLE_VERSION_CARRIES is the bit of a record's lock byte that says
 * its put's carried locker's full xid follows its value ids, before any
 * ender.
 */
#define TXN_MEMTABLE_VERSION_CARRIES 4

/* TXN_MEMTABLE_VERSION_ROW is the size of the tuple id of the row a record names. */
#define TXN_MEMTABLE_VERSION_ROW 8

/* TXN_MEMTABLE_VERSION_ADDRESS is the size of an out-of-line address in a record: its offset and its locator. */
#define TXN_MEMTABLE_VERSION_ADDRESS (4 + CHUNKS_TRAILER_LOCATOR_LEN)

/* TXN_MEMTABLE_VERSION_VALUE is the size of a value id in a record. */
#define TXN_MEMTABLE_VERSION_VALUE 4

/* TXN_MEMTABLE_VERSION_WORD is the count of a handle's bits that name its line pointer's first word. */
#define TXN_MEMTABLE_VERSION_WORD 13

/* TXN_MEMTABLE_VERSION_OFFSET is the first bit of a record's offset in its line pointer's link. */
#define TXN_MEMTABLE_VERSION_OFFSET 44

/* The tags each kind of op's tags start at, after a put's 0, a delete's 1, and a move's 2 and 3. */
#define TXN_MEMTABLE_VERSION_ADD_TAG 4
#define TXN_MEMTABLE_VERSION_REMOVE_TAG 5
#define TXN_MEMTABLE_VERSION_ROW_TAG 8
#define TXN_MEMTABLE_VERSION_NAMED_TAG 13
#define TXN_MEMTABLE_VERSION_ROW_LOCK_TAG 15
#define TXN_MEMTABLE_VERSION_LOCK_TAG 19

/* The hint word's parts: its cached outcome, the killed mark, its high half's shift, and its ended and shown marks. */
#define TXN_MEMTABLE_VERSION_HINT UINT64CONST(3)
#define TXN_MEMTABLE_VERSION_KILLED UINT64CONST(4)
#define TXN_MEMTABLE_VERSION_TOKEN 32
#define TXN_MEMTABLE_VERSION_ENDED UINT64CONST(16)
#define TXN_MEMTABLE_VERSION_SHOWN UINT64CONST(8)

/*
 * TXN_MEMTABLE_VERSION_COUNTED is the hint word's mark of a version its push
 * counted at its writer's commit status entry, which its removal gives back;
 * it lives only in the delta, so no run or WAL record holds it.
 */
#define TXN_MEMTABLE_VERSION_COUNTED UINT64CONST(32)

/* TXN_MEMTABLE_VERSION_SPILLED is the bit of a spilled version's handle. */
#define TXN_MEMTABLE_VERSION_SPILLED (UINT64CONST(1) << 63)

/* TXN_MEMTABLE_VERSION_OLDER is the bits of a link that hold its key's older version's handle. */
#define TXN_MEMTABLE_VERSION_OLDER ((UINT64CONST(1) << TXN_MEMTABLE_VERSION_OFFSET) - 1)

/*
 * TxnMemtableVersionFields is what a record holds of what its version does:
 * its tag, the row of its key it names, its value, its addresses, either
 * pairs or a record's bytes of them, its value ids, either numbers or a
 * record's bytes of them, its tuple id, and its row's heap bytes.
 */
typedef struct TxnMemtableVersionFields
{
	uint8		tag;
	bool		has_row;
	HostRow		row;
	const uint8 *value;
	size_t		value_len;
	const chunks_TupleU32Locator_t *addresses;
	const uint8 *address_bytes;
	size_t		addresses_len;
	const uint32 *values;
	const uint8 *value_bytes;
	size_t		values_len;
	HostRow		tuple;
	uint32		heap;
	bool		has_carried;
	uint64		carried;
} TxnMemtableVersionFields;

/* txn_memtable_version_extras returns the length of the fields a record's flags say follow its header. */
static size_t
txn_memtable_version_extras(uint8 flags)
{
	return 8 * ((flags & TXN_MEMTABLE_VERSION_TOP) != 0) + 8 * ((flags & TXN_MEMTABLE_VERSION_LSN) != 0) +
		4 * ((flags & (TXN_MEMTABLE_VERSION_COMBO | TXN_MEMTABLE_VERSION_ENDING)) != 0) +
		2 * ((flags & TXN_MEMTABLE_VERSION_VALUES) != 0) + 4 * ((flags & TXN_MEMTABLE_VERSION_HEAP) != 0);
}

/* txn_memtable_version_counted returns the place in a record of its count of value ids. */
static size_t
txn_memtable_version_counted(uint8 flags)
{
	return TXN_MEMTABLE_VERSION_HEADER +
		txn_memtable_version_extras(flags & (TXN_MEMTABLE_VERSION_TOP | TXN_MEMTABLE_VERSION_LSN |
											 TXN_MEMTABLE_VERSION_COMBO | TXN_MEMTABLE_VERSION_ENDING));
}


/* txn_memtable_version_Handle_named returns the version a link or a head names, false for none. */
bool
txn_memtable_version_Handle_named(uint64 value, TxnMemtableVersionHandle *result)
{
	if (value == TXN_MEMTABLE_INDEX_NONE)
		return false;
	result->f0 = value;
	return true;
}

/*
 * txn_memtable_version_Handle_spilled returns the handle of a version read
 * back from a spill's file: the low bits of its transaction's xid, its file's
 * number, and its place among the file's versions of its key, above a bit no
 * line pointer's handle has.
 */
TxnMemtableVersionHandle
txn_memtable_version_Handle_spilled(uint64 xid, uint32 number, uint32 place)
{
	TxnMemtableVersionHandle handle = {TXN_MEMTABLE_VERSION_SPILLED | (xid & 0x7fffffff) << 32 |
	(uint64) (number & 0xffff) << 16 | (uint64) (place & 0xffff)};

	return handle;
}

#ifdef DOLTGRES_TEST_TXN
/* txn_memtable_version_Handle_page returns the index of the version's page in its range's list. */
uint64
txn_memtable_version_Handle_page(TxnMemtableVersionHandle self)
{
	return self.f0 >> TXN_MEMTABLE_VERSION_WORD;
}
#endif


/* txn_memtable_version_put returns what a version that puts a value holds in its record. */
static TxnMemtableVersionFields
txn_memtable_version_put(uint8 tag, bool has_row, HostRow row, const TxnMemtablePut *put)
{
	TxnMemtableVersionFields fields = {.tag = tag, .has_row = has_row, .row = row,
		.value = put->value.ptr, .value_len = put->value.len, .addresses = put->addresses.ptr,
		.addresses_len = put->addresses.len, .values = put->values.ptr, .values_len = put->values.len,
	.tuple = put->tuple, .heap = put->heap_bytes, .has_carried = put->has_carried, .carried = put->carried};

	return fields;
}

/*
 * txn_memtable_version_moved returns what a version that removes or deletes
 * holds in its record, by where an update moved the row, whose two kinds take
 * the two tags after its own.
 */
static TxnMemtableVersionFields
txn_memtable_version_moved(uint8 tag, bool has_row, HostRow row, bool has_successor,
						   const TxnMemtableSuccessor *successor, HostRow tuple)
{
	TxnMemtableVersionFields fields = {.tag = tag, .has_row = has_row, .row = row, .tuple = tuple};

	if (has_successor && successor->tag == TXN_MEMTABLE_SUCCESSOR_KEY)
	{
		fields.tag = tag + 1;
		fields.value = successor->u.key.ptr;
		fields.value_len = successor->u.key.len;
	}
	else if (has_successor)
		fields.tag = tag + 2;
	return fields;
}

/* txn_memtable_version_fields returns what a version doing an op holds in its record. */
static TxnMemtableVersionFields
txn_memtable_version_fields(const TxnMemtableOp *op)
{
	TxnMemtableVersionFields fields = {0};
	HostRow		none = {0};

	switch (op->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			return txn_memtable_version_put(0, false, none, &op->u.put);
		case TXN_MEMTABLE_OP_DELETE:
			fields.tag = 1;
			break;
		case TXN_MEMTABLE_OP_EMPTIED:
			fields.tag = 1;
			fields.tuple = op->u.emptied;
			break;
		case TXN_MEMTABLE_OP_NAMED:
			fields.tag = TXN_MEMTABLE_VERSION_NAMED_TAG;
			fields.values = op->u.named.f1.ptr;
			fields.values_len = op->u.named.f1.len;
			fields.tuple = op->u.named.f0;
			break;
		case TXN_MEMTABLE_OP_MOVE:
			return txn_memtable_version_moved(1, false, none, true, &op->u.move, none);
		case TXN_MEMTABLE_OP_ADD:
			return txn_memtable_version_put(TXN_MEMTABLE_VERSION_ADD_TAG, false, none, &op->u.add);
		case TXN_MEMTABLE_OP_REMOVE:
			return txn_memtable_version_moved(TXN_MEMTABLE_VERSION_REMOVE_TAG, false, none, op->u.remove.has_f1,
											  &op->u.remove.f1, op->u.remove.f0);
		case TXN_MEMTABLE_OP_LOCK:
			fields.tag = (uint8) (TXN_MEMTABLE_VERSION_LOCK_TAG + (uint8) op->u.lock);
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			return txn_memtable_version_put(TXN_MEMTABLE_VERSION_ROW_TAG, false, none, &op->u.add_row);
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			return txn_memtable_version_put(TXN_MEMTABLE_VERSION_ROW_TAG + 1, true, op->u.update_row.f0,
											&op->u.update_row.f1);
		case TXN_MEMTABLE_OP_DELETE_ROW:
			return txn_memtable_version_moved(TXN_MEMTABLE_VERSION_ROW_TAG + 2, true, op->u.delete_row.f0,
											  op->u.delete_row.has_f1, &op->u.delete_row.f1, none);
		case TXN_MEMTABLE_OP_LOCK_ROW:
			fields.tag = (uint8) (TXN_MEMTABLE_VERSION_ROW_LOCK_TAG + (uint8) op->u.lock_row.f1);
			fields.has_row = true;
			fields.row = op->u.lock_row.f0;
			break;
	}
	return fields;
}

/* txn_memtable_version_combo_shown reports whether a version's shown command carries a combo command id. */
static bool
txn_memtable_version_combo_shown(TxnMemtableVersionShown shown)
{
	return shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO || shown.tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING;
}

/* txn_memtable_version_fields_len returns the length of a record of fields, which encode_fields writes. */
static size_t
txn_memtable_version_fields_len(const TxnMemtableVersionFields *fields, bool has_top,
								TxnMemtableVersionShown shown, bool has_lsn)
{
	return TXN_MEMTABLE_VERSION_HEADER + 8 * has_top + 8 * has_lsn + 4 * txn_memtable_version_combo_shown(shown) +
		2 * (fields->values_len != 0) + 4 * (fields->heap != 0) + fields->value_len +
		(fields->has_row ? TXN_MEMTABLE_VERSION_ROW : 0) + fields->addresses_len * TXN_MEMTABLE_VERSION_ADDRESS +
		fields->values_len * TXN_MEMTABLE_VERSION_VALUE + 8 * fields->has_carried;
}

/* txn_memtable_version_encoded returns the length of a version's record, which encode writes. */
size_t
txn_memtable_version_encoded(const TxnMemtableVersionWriter *writer, const TxnMemtableOp *op,
							 TxnMemtableVersionShown shown, bool has_lsn, uint64 lsn)
{
	TxnMemtableVersionFields fields = txn_memtable_version_fields(op);

	return txn_memtable_version_fields_len(&fields, writer->has_top, shown, has_lsn);
}

/*
 * txn_memtable_version_largest returns the length of the longest record of a
 * version whose value is at most a length and holds no addresses: a
 * subtransaction's row's, with an LSN, a combo command id, and its heap
 * bytes.
 */
size_t
txn_memtable_version_largest(size_t value)
{
	return TXN_MEMTABLE_VERSION_HEADER +
		txn_memtable_version_extras(TXN_MEMTABLE_VERSION_TOP | TXN_MEMTABLE_VERSION_LSN |
									TXN_MEMTABLE_VERSION_COMBO | TXN_MEMTABLE_VERSION_HEAP) + value;
}

/*
 * txn_memtable_version_reserve returns the size of a page that holds a
 * version's record of at most a length with its line pointer and a
 * full-height node of a key, which a prepared push takes before its window.
 */
HostDeltaPageSize
txn_memtable_version_reserve(uint64 newest, size_t key, size_t record)
{
	return txn_memtable_pages_size(newest != 0, newest * 32, (uint64) (key + record),
								   TXN_MEMTABLE_VERSION_LINE + TXN_MEMTABLE_INDEX_NODE);
}

/* txn_memtable_version_le appends a number's bytes, little-endian, low bytes of a width. */
static void
txn_memtable_version_le(StdVec *out, uint64 value, int width)
{
	uint8		bytes[8];

	for (int i = 0; i < width; i++)
		bytes[i] = (uint8) (value >> (8 * i));
	std_vec_extend_from_slice_Checked(out, bytes, width, 1, 1);
}

/*
 * txn_memtable_version_encode_fields writes a record of fields into a buffer,
 * over what it held: its writer, its row lock, the command it shows, and the
 * end of its WAL record when it has one; addresses and value ids a record's
 * bytes hold are read and written again, as the Rust's owned copy does.
 */
static void
txn_memtable_version_encode_fields(const TxnMemtableVersionFields *fields, const TxnMemtableVersionWriter *writer,
								   TxnMemtableLockMode lock, TxnMemtableVersionShown shown, bool has_lsn, uint64 lsn,
								   StdVec *out)
{
	size_t		len = fields->value_len + (fields->has_row ? TXN_MEMTABLE_VERSION_ROW : 0);
	uint8		flags = 0;
	uint8		tail[3];

	out->len = 0;
	txn_memtable_version_le(out, writer->xid, 8);
	txn_memtable_version_le(out, writer->command, 4);
	txn_memtable_version_le(out, fields->tuple.f0, 8);
	txn_memtable_version_le(out, (uint32) len, 4);
	txn_memtable_version_le(out, (uint16) fields->addresses_len, 2);
	flags |= writer->has_top ? TXN_MEMTABLE_VERSION_TOP : 0;
	flags |= has_lsn ? TXN_MEMTABLE_VERSION_LSN : 0;
	flags |= fields->values_len != 0 ? TXN_MEMTABLE_VERSION_VALUES : 0;
	flags |= fields->heap != 0 ? TXN_MEMTABLE_VERSION_HEAP : 0;
	switch (shown.tag)
	{
		case TXN_MEMTABLE_VERSION_SHOWN_COMMAND:
			break;
		case TXN_MEMTABLE_VERSION_SHOWN_COMBO:
			flags |= TXN_MEMTABLE_VERSION_COMBO;
			break;
		case TXN_MEMTABLE_VERSION_SHOWN_REPLAYED:
			flags |= TXN_MEMTABLE_VERSION_REPLAYED;
			break;
		case TXN_MEMTABLE_VERSION_SHOWN_ENDING:
			flags |= TXN_MEMTABLE_VERSION_ENDING;
			break;
	}
	tail[0] = fields->tag;
	tail[1] = (uint8) lock | (fields->has_carried ? TXN_MEMTABLE_VERSION_CARRIES : 0);
	tail[2] = flags;
	std_vec_extend_from_slice_Checked(out, tail, 3, 1, 1);
	if (writer->has_top)
		txn_memtable_version_le(out, writer->top, 8);
	if (has_lsn)
		txn_memtable_version_le(out, lsn, 8);
	if (shown.tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO)
		txn_memtable_version_le(out, shown.u.combo, 4);
	else if (shown.tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING)
		txn_memtable_version_le(out, shown.u.ending, 4);
	if (fields->values_len != 0)
		txn_memtable_version_le(out, (uint16) fields->values_len, 2);
	if (fields->heap != 0)
		txn_memtable_version_le(out, fields->heap, 4);
	if (fields->has_row)
		txn_memtable_version_le(out, fields->row.f0, 8);
	if (fields->value_len != 0)
		std_vec_extend_from_slice_Checked(out, fields->value, fields->value_len, 1, 1);
	for (size_t i = 0; i < fields->addresses_len; i++)
	{
		chunks_TupleU32Locator_t address;

		if (fields->address_bytes != NULL)
		{
			const uint8 *at = fields->address_bytes + i * TXN_MEMTABLE_VERSION_ADDRESS;

			address.f0 = std_u32_from_le_bytes(at);
			address.f1 = chunks_trailer_Locator_read(at + 4, CHUNKS_TRAILER_LOCATOR_LEN);
		}
		else
			address = fields->addresses[i];
		txn_memtable_version_le(out, address.f0, 4);
		chunks_trailer_Locator_write(&address.f1, out);
	}
	for (size_t i = 0; i < fields->values_len; i++)
		txn_memtable_version_le(out, fields->value_bytes != NULL ?
								std_u32_from_le_bytes(fields->value_bytes + i * TXN_MEMTABLE_VERSION_VALUE) :
								fields->values[i], 4);
	if (fields->has_carried)
		txn_memtable_version_le(out, fields->carried, 8);
}

/*
 * txn_memtable_version_encode writes a version's record into a buffer, over
 * what it held: its writer, what it does, the row lock it holds while its
 * transaction runs, the command it shows, and the end of its WAL record when
 * it has one. A buffer that holds encoded bytes takes it with no allocation.
 */
void
txn_memtable_version_encode(const TxnMemtableVersionWriter *writer, const TxnMemtableOp *op,
							TxnMemtableLockMode lock, TxnMemtableVersionShown shown, bool has_lsn, uint64 lsn,
							StdVec *out)
{
	TxnMemtableVersionFields fields = txn_memtable_version_fields(op);

	txn_memtable_version_encode_fields(&fields, writer, lock, shown, has_lsn, lsn, out);
}

/* txn_memtable_version_word8 reads eight bytes at an offset of a record as a number. */
static uint64
txn_memtable_version_word8(const uint8 *bytes, size_t at)
{
	return std_u64_from_le_bytes(bytes + at);
}

/*
 * txn_memtable_version_length returns the length of the record a header
 * starts with a count of value ids, from its flags, its value's length, and
 * its count of addresses.
 */
static size_t
txn_memtable_version_length(const uint8 *header, size_t values)
{
	size_t		value = std_u32_from_le_bytes(header + 20);
	size_t		addresses = std_u16_from_le_bytes(header + 24);

	return TXN_MEMTABLE_VERSION_HEADER + txn_memtable_version_extras(header[28]) + value +
		addresses * TXN_MEMTABLE_VERSION_ADDRESS + values * TXN_MEMTABLE_VERSION_VALUE +
		8 * ((header[27] & TXN_MEMTABLE_VERSION_CARRIES) != 0) + 4 * ((header[28] & TXN_MEMTABLE_VERSION_ENDER) != 0);
}

/* txn_memtable_version_mode returns the lock mode a number names, among Postgres' four. */
static TxnMemtableLockMode
txn_memtable_version_mode(uint8 number)
{
	switch (number)
	{
		case 0:
			return TXN_MEMTABLE_LOCK_MODE_KEY_SHARE;
		case 1:
			return TXN_MEMTABLE_LOCK_MODE_SHARE;
		case 2:
			return TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE;
		default:
			return TXN_MEMTABLE_LOCK_MODE_UPDATE;
	}
}

/* txn_memtable_version_Record_of returns a record the memtable encoded, read in place, whose form is encode's. */
TxnMemtableVersionRecord
txn_memtable_version_Record_of(const uint8 *bytes, size_t bytes_len)
{
	TxnMemtableVersionRecord record = {{bytes, bytes_len}};

	return record;
}

/* txn_memtable_version_Record_bytes returns the record's bytes, as a run stores them. */
StdBytes
txn_memtable_version_Record_bytes(const TxnMemtableVersionRecord *self)
{
	return self->f0;
}

/* txn_memtable_version_Record_writer returns the record's writer. */
TxnMemtableVersionWriter
txn_memtable_version_Record_writer(const TxnMemtableVersionRecord *self)
{
	const uint8 *bytes = self->f0.ptr;
	TxnMemtableVersionWriter writer = {.xid = txn_memtable_version_word8(bytes, 0),
	.command = std_u32_from_le_bytes(bytes + 8)};

	writer.has_top = (bytes[28] & TXN_MEMTABLE_VERSION_TOP) != 0;
	if (writer.has_top)
		writer.top = txn_memtable_version_word8(bytes, TXN_MEMTABLE_VERSION_HEADER);
	return writer;
}

/*
 * txn_memtable_version_Record_ender returns the 32-bit xid of the ender of the
 * row a free space run's take named, which the run carries for the take's
 * hint word, or false for none.
 */
bool
txn_memtable_version_Record_ender(const TxnMemtableVersionRecord *self, uint32 *result)
{
	const uint8 *bytes = self->f0.ptr;

	if ((bytes[28] & TXN_MEMTABLE_VERSION_ENDER) == 0)
		return false;
	*result = std_u32_from_le_bytes(bytes + self->f0.len - 4);
	return true;
}

/* txn_memtable_version_Record_lsn returns the end of the version's WAL record, or false for a version with none. */
bool
txn_memtable_version_Record_lsn(const TxnMemtableVersionRecord *self, uint64 *result)
{
	const uint8 *bytes = self->f0.ptr;

	if ((bytes[28] & TXN_MEMTABLE_VERSION_LSN) == 0)
		return false;
	*result = txn_memtable_version_word8(bytes, TXN_MEMTABLE_VERSION_HEADER +
										 txn_memtable_version_extras(bytes[28] & TXN_MEMTABLE_VERSION_TOP));
	return true;
}

/* txn_memtable_version_Record_shown returns the command the version shows as the raw command of the rows it changes. */
TxnMemtableVersionShown
txn_memtable_version_Record_shown(const TxnMemtableVersionRecord *self)
{
	const uint8 *bytes = self->f0.ptr;
	uint8		flags = bytes[28];
	size_t		at = TXN_MEMTABLE_VERSION_HEADER +
		txn_memtable_version_extras(flags & (TXN_MEMTABLE_VERSION_TOP | TXN_MEMTABLE_VERSION_LSN));
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};

	if ((flags & TXN_MEMTABLE_VERSION_REPLAYED) != 0)
		shown.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED;
	else if ((flags & TXN_MEMTABLE_VERSION_COMBO) != 0)
	{
		shown.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO;
		shown.u.combo = std_u32_from_le_bytes(bytes + at);
	}
	else if ((flags & TXN_MEMTABLE_VERSION_ENDING) != 0)
	{
		shown.tag = TXN_MEMTABLE_VERSION_SHOWN_ENDING;
		shown.u.ending = std_u32_from_le_bytes(bytes + at);
	}
	return shown;
}

/*
 * txn_memtable_version_Record_raw returns the raw command the version shows,
 * cmin and cmax as heap's t_cid holds them.
 */
uint32
txn_memtable_version_Record_raw(const TxnMemtableVersionRecord *self)
{
	TxnMemtableVersionShown shown = txn_memtable_version_Record_shown(self);

	switch (shown.tag)
	{
		case TXN_MEMTABLE_VERSION_SHOWN_COMBO:
			return shown.u.combo;
		case TXN_MEMTABLE_VERSION_SHOWN_REPLAYED:
			return 0;
		default:
			return txn_memtable_version_Record_writer(self).command;
	}
}

/*
 * txn_memtable_version_Record_ended returns the raw command the version shows
 * as cmin and cmax of a row it ends or locks: the combo command id of a Put
 * that ended its own transaction's row, else its own raw command.
 */
uint32
txn_memtable_version_Record_ended(const TxnMemtableVersionRecord *self)
{
	TxnMemtableVersionShown shown = txn_memtable_version_Record_shown(self);

	if (shown.tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING)
		return shown.u.ending;
	return txn_memtable_version_Record_raw(self);
}

/* txn_memtable_version_Record_lock returns the row lock the version holds while its transaction runs. */
TxnMemtableLockMode
txn_memtable_version_Record_lock(const TxnMemtableVersionRecord *self)
{
	return txn_memtable_version_mode(((const uint8 *) self->f0.ptr)[27] & ~TXN_MEMTABLE_VERSION_CARRIES);
}

/* txn_memtable_version_bytes returns a record's bytes from one offset to another. */
static StdBytes
txn_memtable_version_bytes(const uint8 *bytes, size_t from, size_t to)
{
	StdBytes	slice = {bytes + from, to - from};

	return slice;
}

/* txn_memtable_version_Record_op returns what the version does to its key. */
TxnMemtableVersionOpRef
txn_memtable_version_Record_op(const TxnMemtableVersionRecord *self)
{
	const uint8 *bytes = self->f0.ptr;
	size_t		start = TXN_MEMTABLE_VERSION_HEADER + txn_memtable_version_extras(bytes[28]);
	size_t		len = std_u32_from_le_bytes(bytes + 20);
	size_t		ends = start + len + (size_t) std_u16_from_le_bytes(bytes + 24) * TXN_MEMTABLE_VERSION_ADDRESS;
	bool		carries = (bytes[27] & TXN_MEMTABLE_VERSION_CARRIES) != 0;
	size_t		last = self->f0.len - 8 * carries - 4 * ((bytes[28] & TXN_MEMTABLE_VERSION_ENDER) != 0);
	HostRow		tuple = {txn_memtable_version_word8(bytes, 12)};
	uint32		heap_bytes = 0;
	uint8		tag = bytes[26];
	TxnMemtableVersionOpRef op = {0};
	TxnMemtableVersionPutRef put;
	size_t		from = start;

	if ((bytes[28] & TXN_MEMTABLE_VERSION_HEAP) != 0)
		heap_bytes = std_u32_from_le_bytes(bytes + start - 4);
	if (tag == 9 || (tag >= 10 && tag < 13) || (tag >= TXN_MEMTABLE_VERSION_ROW_LOCK_TAG &&
												tag < TXN_MEMTABLE_VERSION_LOCK_TAG))
		from = start + TXN_MEMTABLE_VERSION_ROW;
	put.value = txn_memtable_version_bytes(bytes, from, start + len);
	put.addresses = txn_memtable_version_bytes(bytes, start + len, ends);
	put.values = txn_memtable_version_bytes(bytes, ends, last);
	put.tuple = tuple;
	put.heap_bytes = heap_bytes;
	put.has_carried = carries;
	put.carried = carries ? txn_memtable_version_word8(bytes, last) : 0;
	switch (tag)
	{
		case 0:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_PUT;
			op.u.put = put;
			break;
		case 1:
			op.tag = tuple.f0 == 0 ? TXN_MEMTABLE_VERSION_OP_REF_DELETE : TXN_MEMTABLE_VERSION_OP_REF_EMPTIED;
			op.u.emptied = tuple;
			break;
		case TXN_MEMTABLE_VERSION_NAMED_TAG:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_NAMED;
			op.u.named.f0 = tuple;
			op.u.named.f1 = put;
			op.u.named.f1.value = txn_memtable_version_bytes(bytes, 0, 0);
			op.u.named.f1.addresses = txn_memtable_version_bytes(bytes, 0, 0);
			break;
		case 2:
		case 3:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_MOVE;
			op.u.move.tag = tag == 2 ? TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY :
				TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE;
			op.u.move.u.key = put.value;
			break;
		case TXN_MEMTABLE_VERSION_ADD_TAG:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_ADD;
			op.u.add = put;
			break;
		case TXN_MEMTABLE_VERSION_REMOVE_TAG:
		case TXN_MEMTABLE_VERSION_REMOVE_TAG + 1:
		case TXN_MEMTABLE_VERSION_REMOVE_TAG + 2:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_REMOVE;
			op.u.remove.f0 = tuple;
			op.u.remove.has_f1 = tag != TXN_MEMTABLE_VERSION_REMOVE_TAG;
			op.u.remove.f1.tag = tag == TXN_MEMTABLE_VERSION_REMOVE_TAG + 1 ? TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY :
				TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE;
			op.u.remove.f1.u.key = put.value;
			break;
		case TXN_MEMTABLE_VERSION_ROW_TAG:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW;
			op.u.add_row = put;
			break;
		case 9:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW;
			op.u.update_row.f0.f0 = txn_memtable_version_word8(bytes, start);
			op.u.update_row.f1 = put;
			break;
		case 10:
		case 11:
		case 12:
			op.tag = TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW;
			op.u.delete_row.f0.f0 = txn_memtable_version_word8(bytes, start);
			op.u.delete_row.has_f1 = tag != 10;
			op.u.delete_row.f1.tag = tag == 11 ? TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY :
				TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE;
			op.u.delete_row.f1.u.key = put.value;
			break;
		default:
			if (tag >= TXN_MEMTABLE_VERSION_ROW_LOCK_TAG && tag < TXN_MEMTABLE_VERSION_LOCK_TAG)
			{
				op.tag = TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW;
				op.u.lock_row.f0.f0 = txn_memtable_version_word8(bytes, start);
				op.u.lock_row.f1 = txn_memtable_version_mode((uint8) (tag - TXN_MEMTABLE_VERSION_ROW_LOCK_TAG));
			}
			else
			{
				op.tag = TXN_MEMTABLE_VERSION_OP_REF_LOCK;
				op.u.lock = txn_memtable_version_mode((uint8) (tag - TXN_MEMTABLE_VERSION_LOCK_TAG));
			}
			break;
	}
	return op;
}

/*
 * txn_memtable_version_PutRef_addresses returns an iterator of a value's out-
 * of-line addresses, each its offset and locator.
 */
TxnMemtableVersionAddresses
txn_memtable_version_PutRef_addresses(const TxnMemtableVersionPutRef *self)
{
	TxnMemtableVersionAddresses addresses = {self->addresses.ptr, self->addresses.len / TXN_MEMTABLE_VERSION_ADDRESS};

	return addresses;
}

/* txn_memtable_version_Addresses_Iterator_next takes the next address, false after the last. */
bool
txn_memtable_version_Addresses_Iterator_next(TxnMemtableVersionAddresses *self, chunks_TupleU32Locator_t *result)
{
	if (self->left == 0)
		return false;
	result->f0 = std_u32_from_le_bytes(self->at);
	result->f1 = chunks_trailer_Locator_read(self->at + 4, CHUNKS_TRAILER_LOCATOR_LEN);
	self->at += TXN_MEMTABLE_VERSION_ADDRESS;
	self->left--;
	return true;
}

/* txn_memtable_version_PutRef_values returns an iterator of the value ids of the row's out-of-line values. */
TxnMemtableVersionValues
txn_memtable_version_PutRef_values(const TxnMemtableVersionPutRef *self)
{
	TxnMemtableVersionValues values = {self->values.ptr, self->values.len / TXN_MEMTABLE_VERSION_VALUE};

	return values;
}

/* txn_memtable_version_Values_Iterator_next takes the next value id, false after the last. */
bool
txn_memtable_version_Values_Iterator_next(TxnMemtableVersionValues *self, uint32 *result)
{
	if (self->left == 0)
		return false;
	*result = std_u32_from_le_bytes(self->at);
	self->at += TXN_MEMTABLE_VERSION_VALUE;
	self->left--;
	return true;
}

/* txn_memtable_version_put_ref returns what a version that puts a value read in place holds in its record. */
static TxnMemtableVersionFields
txn_memtable_version_put_ref(uint8 tag, bool has_row, HostRow row, const TxnMemtableVersionPutRef *put)
{
	TxnMemtableVersionFields fields = {.tag = tag, .has_row = has_row, .row = row,
		.value = put->value.ptr, .value_len = put->value.len, .address_bytes = put->addresses.ptr,
		.addresses_len = put->addresses.len / TXN_MEMTABLE_VERSION_ADDRESS, .value_bytes = put->values.ptr,
	.values_len = put->values.len / TXN_MEMTABLE_VERSION_VALUE, .tuple = put->tuple, .heap = put->heap_bytes, .has_carried = put->has_carried, .carried = put->carried};

	return fields;
}

/*
 * txn_memtable_version_moved_ref returns what a version that removes or
 * deletes holds, by where an update moved the row.
 */
static TxnMemtableVersionFields
txn_memtable_version_moved_ref(uint8 tag, bool has_row, HostRow row, bool has_successor,
							   const TxnMemtableVersionSuccessorRef *successor, HostRow tuple)
{
	TxnMemtableVersionFields fields = {.tag = tag, .has_row = has_row, .row = row, .tuple = tuple};

	if (has_successor && successor->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY)
	{
		fields.tag = tag + 1;
		fields.value = successor->u.key.ptr;
		fields.value_len = successor->u.key.len;
	}
	else if (has_successor)
		fields.tag = tag + 2;
	return fields;
}

/*
 * txn_memtable_version_fields_ref returns what the owned copy of an op read
 * in place holds in its record, which encode writes as it writes the copy's.
 */
static TxnMemtableVersionFields
txn_memtable_version_fields_ref(const TxnMemtableVersionOpRef *op)
{
	TxnMemtableVersionFields fields = {0};
	HostRow		none = {0};

	switch (op->tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			return txn_memtable_version_put_ref(0, false, none, &op->u.put);
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
			fields.tag = 1;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
			fields.tag = 1;
			fields.tuple = op->u.emptied;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
			fields.tag = TXN_MEMTABLE_VERSION_NAMED_TAG;
			fields.value_bytes = op->u.named.f1.values.ptr;
			fields.values_len = op->u.named.f1.values.len / TXN_MEMTABLE_VERSION_VALUE;
			fields.tuple = op->u.named.f0;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			return txn_memtable_version_moved_ref(1, false, none, true, &op->u.move, none);
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			return txn_memtable_version_put_ref(TXN_MEMTABLE_VERSION_ADD_TAG, false, none, &op->u.add);
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			return txn_memtable_version_moved_ref(TXN_MEMTABLE_VERSION_REMOVE_TAG, false, none, op->u.remove.has_f1,
												  &op->u.remove.f1, op->u.remove.f0);
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK:
			fields.tag = (uint8) (TXN_MEMTABLE_VERSION_LOCK_TAG + (uint8) op->u.lock);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			return txn_memtable_version_put_ref(TXN_MEMTABLE_VERSION_ROW_TAG, false, none, &op->u.add_row);
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			return txn_memtable_version_put_ref(TXN_MEMTABLE_VERSION_ROW_TAG + 1, true, op->u.update_row.f0,
												&op->u.update_row.f1);
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			return txn_memtable_version_moved_ref(TXN_MEMTABLE_VERSION_ROW_TAG + 2, true, op->u.delete_row.f0,
												  op->u.delete_row.has_f1, &op->u.delete_row.f1, none);
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
			fields.tag = (uint8) (TXN_MEMTABLE_VERSION_ROW_LOCK_TAG + (uint8) op->u.lock_row.f1);
			fields.has_row = true;
			fields.row = op->u.lock_row.f0;
			break;
	}
	return fields;
}

/* txn_memtable_version_stored_frame_t is stored's state across its pad: the record encoded again, while live. */
typedef struct txn_memtable_version_stored_frame_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	bool		same;
	StdVec		again;
	bool		again_live;
} txn_memtable_version_stored_frame_t;

/* txn_memtable_version_stored_body encodes the record's fields again and compares the bytes. */
static void
txn_memtable_version_stored_body(void *frame_)
{
	txn_memtable_version_stored_frame_t *frame = frame_;
	TxnMemtableVersionRecord record = txn_memtable_version_Record_of(frame->bytes, frame->bytes_len);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	TxnMemtableVersionFields fields = txn_memtable_version_fields_ref(&op);
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
	uint64		lsn = 0;
	bool		has_lsn = txn_memtable_version_Record_lsn(&record, &lsn);
	uint32		ender;

	std_vec_new(&frame->again, 1, 1);
	frame->again_live = true;
	txn_memtable_version_encode_fields(&fields, &writer, txn_memtable_version_Record_lock(&record),
									   txn_memtable_version_Record_shown(&record), has_lsn, lsn, &frame->again);
	if (txn_memtable_version_Record_ender(&record, &ender))
	{
		((uint8 *) frame->again.ptr)[28] |= TXN_MEMTABLE_VERSION_ENDER;
		txn_memtable_version_le(&frame->again, ender, 4);
	}
	frame->same = frame->again.len == frame->bytes_len && memcmp(frame->again.ptr, frame->bytes, frame->bytes_len) == 0;
	frame->again_live = false;
	std_vec_free_Checked(&frame->again, 1, 1);
}

/* txn_memtable_version_stored_pad frees the record encoded again, while live. */
static void
txn_memtable_version_stored_pad(void *frame_)
{
	txn_memtable_version_stored_frame_t *frame = frame_;

	if (frame->again_live)
		std_vec_free_Checked(&frame->again, 1, 1);
}

/* txn_memtable_version_corrupt returns the error of a stored version's record that doesn't read. */
static HostError
txn_memtable_version_corrupt(void)
{
	static const char message[] = "a stored version's record is corrupt";
	StdString text;

	std_string_from_Checked(&text, message, sizeof(message) - 1);
	return host_Error_fork((const uint8 *) "XX001", &text);
}

/*
 * txn_memtable_version_stored reads a version's record from stored bytes, a
 * run's, refusing as data corruption every record encode doesn't write: first
 * what can't be read, a record shorter than its header, a tag no op has, a
 * value shorter than the row its op names, and a length its flags, value
 * length, and counts don't give; then any difference from the record its
 * fields encode again, with a run's ENDER and its xid added before the compare.
 */
bool
txn_memtable_version_stored(const uint8 *bytes, size_t bytes_len, TxnMemtableVersionRecord *result,
							HostError *error)
{
	txn_memtable_version_stored_frame_t frame = {.bytes = bytes, .bytes_len = bytes_len};
	uint8		tag;
	uint8		flags;
	size_t		value;
	size_t		values = 0;
	bool		rowed;

	if (bytes_len < TXN_MEMTABLE_VERSION_HEADER)
	{
		*error = txn_memtable_version_corrupt();
		return false;
	}
	tag = bytes[26];
	flags = bytes[28];
	value = std_u32_from_le_bytes(bytes + 20);
	rowed = (tag >= TXN_MEMTABLE_VERSION_ROW_TAG + 1 && tag < TXN_MEMTABLE_VERSION_NAMED_TAG) ||
		(tag >= TXN_MEMTABLE_VERSION_ROW_LOCK_TAG && tag < TXN_MEMTABLE_VERSION_LOCK_TAG);
	if ((tag >= TXN_MEMTABLE_VERSION_NAMED_TAG + 1 && tag < TXN_MEMTABLE_VERSION_ROW_LOCK_TAG) ||
		(rowed && value < TXN_MEMTABLE_VERSION_ROW))
	{
		*error = txn_memtable_version_corrupt();
		return false;
	}
	if ((flags & TXN_MEMTABLE_VERSION_VALUES) != 0)
	{
		size_t		at = txn_memtable_version_counted(flags);

		if (at + 2 > bytes_len)
		{
			*error = txn_memtable_version_corrupt();
			return false;
		}
		values = std_u16_from_le_bytes(bytes + at);
	}
	if (txn_memtable_version_length(bytes, values) != bytes_len)
	{
		*error = txn_memtable_version_corrupt();
		return false;
	}
	std_landing(&frame, txn_memtable_version_stored_body, txn_memtable_version_stored_pad);
	if (!frame.same)
	{
		*error = txn_memtable_version_corrupt();
		return false;
	}
	*result = txn_memtable_version_Record_of(bytes, bytes_len);
	return true;
}

/* txn_memtable_version_owned_frame_t is OpRef::owned's state across its pad: the vectors it made, while live. */
typedef struct txn_memtable_version_owned_frame_t
{
	const TxnMemtableVersionOpRef *self;
	TxnMemtableOp *result;
	StdVec	   *made[3];
	size_t		sizes[3];
	size_t		aligns[3];
	int			live;
} txn_memtable_version_owned_frame_t;

/* txn_memtable_version_owned_made holds a vector the frame made live, with its element size and alignment. */
static void
txn_memtable_version_owned_made(txn_memtable_version_owned_frame_t *frame, StdVec *made, size_t size, size_t align)
{
	frame->made[frame->live] = made;
	frame->sizes[frame->live] = size;
	frame->aligns[frame->live] = align;
	frame->live++;
}

/* txn_memtable_version_owned_bytes copies bytes into a vector the frame holds live. */
static void
txn_memtable_version_owned_bytes(txn_memtable_version_owned_frame_t *frame, StdVec *out, StdBytes bytes)
{
	std_slice_to_vec_Checked(out, bytes.ptr, bytes.len, 1, 1);
	txn_memtable_version_owned_made(frame, out, 1, 1);
}

/* txn_memtable_version_PutRef_owned copies a value read in place into a Put, its vectors held live by the frame. */
static void
txn_memtable_version_PutRef_owned(txn_memtable_version_owned_frame_t *frame, const TxnMemtableVersionPutRef *put,
								  TxnMemtablePut *out)
{
	TxnMemtableVersionAddresses addresses = txn_memtable_version_PutRef_addresses(put);
	TxnMemtableVersionValues values = txn_memtable_version_PutRef_values(put);
	chunks_TupleU32Locator_t address;
	uint32		id;

	txn_memtable_version_owned_bytes(frame, &out->value, put->value);
	std_vec_with_capacity_Checked(&out->addresses, addresses.left, sizeof(chunks_TupleU32Locator_t),
								  _Alignof(chunks_TupleU32Locator_t));
	txn_memtable_version_owned_made(frame, &out->addresses, sizeof(chunks_TupleU32Locator_t),
									_Alignof(chunks_TupleU32Locator_t));
	while (txn_memtable_version_Addresses_Iterator_next(&addresses, &address))
		std_vec_push_Checked(&out->addresses, &address, sizeof(address), _Alignof(chunks_TupleU32Locator_t));
	std_vec_with_capacity_Checked(&out->values, values.left, sizeof(uint32), _Alignof(uint32));
	txn_memtable_version_owned_made(frame, &out->values, sizeof(uint32), _Alignof(uint32));
	while (txn_memtable_version_Values_Iterator_next(&values, &id))
		std_vec_push_Checked(&out->values, &id, sizeof(id), _Alignof(uint32));
	out->tuple = put->tuple;
	out->heap_bytes = put->heap_bytes;
	out->has_carried = put->has_carried;
	out->carried = put->carried;
}

/* txn_memtable_version_SuccessorRef_owned copies where an update moved a row into a Successor. */
static void
txn_memtable_version_SuccessorRef_owned(txn_memtable_version_owned_frame_t *frame,
										const TxnMemtableVersionSuccessorRef *successor, TxnMemtableSuccessor *out)
{
	if (successor->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY)
	{
		out->tag = TXN_MEMTABLE_SUCCESSOR_KEY;
		txn_memtable_version_owned_bytes(frame, &out->u.key, successor->u.key);
	}
	else
		out->tag = TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE;
}

/* txn_memtable_version_owned_body is OpRef::owned's body: each variant's fields copied. */
static void
txn_memtable_version_owned_body(void *frame_)
{
	txn_memtable_version_owned_frame_t *frame = frame_;
	const TxnMemtableVersionOpRef *self = frame->self;
	TxnMemtableOp *op = frame->result;
	TxnMemtableVersionValues values;
	uint32		id;

	switch (self->tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			op->tag = TXN_MEMTABLE_OP_PUT;
			txn_memtable_version_PutRef_owned(frame, &self->u.put, &op->u.put);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
			op->tag = TXN_MEMTABLE_OP_DELETE;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
			op->tag = TXN_MEMTABLE_OP_EMPTIED;
			op->u.emptied = self->u.emptied;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
			op->tag = TXN_MEMTABLE_OP_NAMED;
			op->u.named.f0 = self->u.named.f0;
			values = txn_memtable_version_PutRef_values(&self->u.named.f1);
			std_vec_with_capacity_Checked(&op->u.named.f1, values.left, sizeof(uint32), _Alignof(uint32));
			txn_memtable_version_owned_made(frame, &op->u.named.f1, sizeof(uint32), _Alignof(uint32));
			while (txn_memtable_version_Values_Iterator_next(&values, &id))
				std_vec_push_Checked(&op->u.named.f1, &id, sizeof(id), _Alignof(uint32));
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			op->tag = TXN_MEMTABLE_OP_MOVE;
			txn_memtable_version_SuccessorRef_owned(frame, &self->u.move, &op->u.move);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			op->tag = TXN_MEMTABLE_OP_ADD;
			txn_memtable_version_PutRef_owned(frame, &self->u.add, &op->u.add);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			op->tag = TXN_MEMTABLE_OP_REMOVE;
			op->u.remove.f0 = self->u.remove.f0;
			op->u.remove.has_f1 = self->u.remove.has_f1;
			if (self->u.remove.has_f1)
				txn_memtable_version_SuccessorRef_owned(frame, &self->u.remove.f1, &op->u.remove.f1);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK:
			op->tag = TXN_MEMTABLE_OP_LOCK;
			op->u.lock = self->u.lock;
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			op->tag = TXN_MEMTABLE_OP_ADD_ROW;
			txn_memtable_version_PutRef_owned(frame, &self->u.add_row, &op->u.add_row);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			op->tag = TXN_MEMTABLE_OP_UPDATE_ROW;
			op->u.update_row.f0 = self->u.update_row.f0;
			txn_memtable_version_PutRef_owned(frame, &self->u.update_row.f1, &op->u.update_row.f1);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			op->tag = TXN_MEMTABLE_OP_DELETE_ROW;
			op->u.delete_row.f0 = self->u.delete_row.f0;
			op->u.delete_row.has_f1 = self->u.delete_row.has_f1;
			if (self->u.delete_row.has_f1)
				txn_memtable_version_SuccessorRef_owned(frame, &self->u.delete_row.f1, &op->u.delete_row.f1);
			break;
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
			op->tag = TXN_MEMTABLE_OP_LOCK_ROW;
			op->u.lock_row.f0 = self->u.lock_row.f0;
			op->u.lock_row.f1 = self->u.lock_row.f1;
			break;
	}
	frame->live = 0;
}

/* txn_memtable_version_owned_pad frees the vectors OpRef::owned made before it unwound. */
static void
txn_memtable_version_owned_pad(void *frame_)
{
	txn_memtable_version_owned_frame_t *frame = frame_;

	for (int i = 0; i < frame->live; i++)
		std_vec_free_Checked(frame->made[i], frame->sizes[i], frame->aligns[i]);
}

/*
 * txn_memtable_version_OpRef_owned copies what a version does, for a push of
 * a version read back from a run, which the loader's push encodes again into
 * the same record.
 */
TxnMemtableOp
txn_memtable_version_OpRef_owned(const TxnMemtableVersionOpRef *self)
{
	TxnMemtableOp op = {0};
	txn_memtable_version_owned_frame_t frame = {.self = self, .result = &op};

	std_landing(&frame, txn_memtable_version_owned_body, txn_memtable_version_owned_pad);
	return op;
}

#ifdef DOLTGRES_TEST_TXN
/*
 * txn_memtable_version_tests_raced is the tests' one-shot seam inside
 * updated's read-modify-write: when set, updated runs it once between the
 * word's load and its compare-exchange and clears it, as a reader's store
 * landing there would.
 */
static void (*txn_memtable_version_tests_raced) (void *captures);

/* txn_memtable_version_tests_raced_captures is the seam's captures. */
static void *txn_memtable_version_tests_raced_captures;
#endif

/*
 * txn_memtable_version_updated changes a version's hint word by a
 * read-modify-write, a compare-exchange retried until it lands, so that
 * stores of its hint, its token, and its killed mark never overwrite each
 * other: the new word keeps the bits of a mask and sets others.
 */
static void
txn_memtable_version_updated(const HostDeltaWord *word, uint64 keep, uint64 set)
{
	uint64		current = host_delta_Word_load(word);

	for (;;)
	{
		uint64		held;

#ifdef DOLTGRES_TEST_TXN
		if (txn_memtable_version_tests_raced != NULL)
		{
			void		(*raced) (void *captures) = txn_memtable_version_tests_raced;

			txn_memtable_version_tests_raced = NULL;
			raced(txn_memtable_version_tests_raced_captures);
		}
#endif
		held = host_delta_Word_compare_exchange(word, current, (current & keep) | set);
		if (held == current)
			return;
		current = held;
	}
}

/*
 * txn_memtable_version_held returns the handle of the newest of a key's
 * versions, newest first, that its memtable holds, or false when it holds
 * none: a spilled version is older than every version the memtable holds.
 */
bool
txn_memtable_version_held(const TxnMemtableVersionVersion *versions, size_t versions_len,
						  TxnMemtableVersionHandle *result)
{
	if (versions_len == 0 || (versions[0].handle.f0 & TXN_MEMTABLE_VERSION_SPILLED) != 0)
		return false;
	*result = versions[0].handle;
	return true;
}

/* txn_memtable_version_Version_spilled returns a version read back from a spill's file, taking its record. */
TxnMemtableVersionVersion
txn_memtable_version_Version_spilled(TxnMemtableVersionHandle handle, StdVec *record)
{
	TxnMemtableVersionVersion version = {.handle = handle, .record = {.tag = TXN_MEMTABLE_VERSION_BODY_SPILLED}};

	version.record.u.spilled = *record;
	return version;
}

/* txn_memtable_version_Version_record returns the version's record. */
TxnMemtableVersionRecord
txn_memtable_version_Version_record(const TxnMemtableVersionVersion *self)
{
	StdBytes	bytes;

	if (self->record.tag == TXN_MEMTABLE_VERSION_BODY_LENT)
		bytes = host_delta_Lent_Deref_deref(&self->record.u.lent);
	else
	{
		bytes.ptr = self->record.u.spilled.ptr;
		bytes.len = self->record.u.spilled.len;
	}
	return txn_memtable_version_Record_of(bytes.ptr, bytes.len);
}

/* txn_memtable_version_Version_word loads the version's hint word, zero for a spilled version. */
static uint64
txn_memtable_version_Version_word(const TxnMemtableVersionVersion *self)
{
	return self->has_hint ? host_delta_Word_load(&self->hint) : 0;
}

/* txn_memtable_version_Version_update changes the version's hint word, which a spilled version has none of. */
static void
txn_memtable_version_Version_update(const TxnMemtableVersionVersion *self, uint64 keep, uint64 set)
{
	if (self->has_hint)
		txn_memtable_version_updated(&self->hint, keep, set);
}

/*
 * txn_memtable_version_Version_deleter returns what the version shows as the
 * deleter of its key's next older version.
 */
TxnMemtableVersionDeleter
txn_memtable_version_Version_deleter(const TxnMemtableVersionVersion *self)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(self);
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
	TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
	TxnMemtableVersionDeleter deleter = {.xid = writer.xid};

	deleter.has_command = op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK && op.tag != TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW;
	if (deleter.has_command)
		deleter.command = writer.command;
	return deleter;
}

/* txn_memtable_version_Version_hint loads the version's cached outcome of its writer. */
TxnMemtableVersionHint
txn_memtable_version_Version_hint(const TxnMemtableVersionVersion *self)
{
	switch (txn_memtable_version_Version_word(self) & TXN_MEMTABLE_VERSION_HINT)
	{
		case 1:
			return TXN_MEMTABLE_VERSION_HINT_COMMITTED;
		case 2:
			return TXN_MEMTABLE_VERSION_HINT_ABORTED;
		default:
			return TXN_MEMTABLE_VERSION_HINT_UNKNOWN;
	}
}

/*
 * txn_memtable_version_Version_set_hint caches the outcome of the version's
 * writer, which a reader stores at any time.
 */
void
txn_memtable_version_Version_set_hint(TxnMemtableVersionVersion *self, TxnMemtableVersionHint hint)
{
	txn_memtable_version_Version_update(self, ~TXN_MEMTABLE_VERSION_HINT, (uint64) hint);
}

/*
 * txn_memtable_version_Version_token returns the speculative token of a
 * running speculative insert's version, or false once its insert is confirmed
 * or for any other version.
 */
bool
txn_memtable_version_Version_token(const TxnMemtableVersionVersion *self, uint32 *result)
{
	uint64		word = txn_memtable_version_Version_word(self);
	uint32		token = (uint32) (word >> TXN_MEMTABLE_VERSION_TOKEN);

	if (token == 0 || (word & TXN_MEMTABLE_VERSION_SHOWN) != 0)
		return false;
	*result = token;
	return true;
}

/*
 * txn_memtable_version_Version_raw returns the raw command the version's row
 * shows: the combo command id a change of the row by its own transaction
 * stored, else its record's.
 */
uint32
txn_memtable_version_Version_raw(const TxnMemtableVersionVersion *self)
{
	uint64		word = txn_memtable_version_Version_word(self);
	TxnMemtableVersionRecord record;

	if ((word & TXN_MEMTABLE_VERSION_SHOWN) != 0)
		return (uint32) (word >> TXN_MEMTABLE_VERSION_TOKEN);
	record = txn_memtable_version_Version_record(self);
	return txn_memtable_version_Record_raw(&record);
}

/*
 * txn_memtable_version_Version_ender returns the 32-bit xid a fold's drop
 * stored of the ender of the row a free space take's tuple id held, or false.
 */
bool
txn_memtable_version_Version_ender(const TxnMemtableVersionVersion *self, uint32 *result)
{
	uint64		word = txn_memtable_version_Version_word(self);

	if ((word & TXN_MEMTABLE_VERSION_ENDED) == 0)
		return false;
	*result = (uint32) (word >> TXN_MEMTABLE_VERSION_TOKEN);
	return true;
}

/*
 * txn_memtable_version_Version_end stores the 32-bit xid of the ender of the
 * row a free space take's tuple id held, for VACUUM's conflict horizon.
 */
void
txn_memtable_version_Version_end(TxnMemtableVersionVersion *self, uint32 ender)
{
	txn_memtable_version_Version_update(self, TXN_MEMTABLE_VERSION_HINT | TXN_MEMTABLE_VERSION_COUNTED,
										TXN_MEMTABLE_VERSION_ENDED | (uint64) ender << TXN_MEMTABLE_VERSION_TOKEN);
}

/*
 * txn_memtable_version_Version_stored returns the version's record as a run
 * holds it: its own, with ENDER and the xid of its row's ender added when its
 * hint word holds one.
 */
void
txn_memtable_version_Version_stored(const TxnMemtableVersionVersion *self, StdVec *result)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(self);
	uint32		ender;

	std_slice_to_vec_Checked(result, record.f0.ptr, record.f0.len, 1, 1);
	if (txn_memtable_version_Version_ender(self, &ender))
		txn_memtable_version_ended(result, ender);
}

/* txn_memtable_version_ended adds to a free space take's encoded record ENDER and the 32-bit xid of the ender of the row its tuple id held, as a run holds it. */
void
txn_memtable_version_ended(StdVec *record, uint32 ender)
{
	((uint8 *) record->ptr)[28] |= TXN_MEMTABLE_VERSION_ENDER;
	txn_memtable_version_le(record, ender, 4);
}

/* txn_memtable_version_Version_confirm clears a speculative insert's token, as heap_finish_speculative does. */
void
txn_memtable_version_Version_confirm(TxnMemtableVersionVersion *self)
{
	txn_memtable_version_Version_update(self, (UINT64CONST(1) << TXN_MEMTABLE_VERSION_TOKEN) - 1, 0);
}

/* txn_memtable_version_Version_counted reports whether the version's push counted it at its writer's entry. */
bool
txn_memtable_version_Version_counted(const TxnMemtableVersionVersion *self)
{
	return (txn_memtable_version_Version_word(self) & TXN_MEMTABLE_VERSION_COUNTED) != 0;
}

/* txn_memtable_version_Version_killed reports whether a speculative insert's writer killed the version. */
bool
txn_memtable_version_Version_killed(const TxnMemtableVersionVersion *self)
{
	return (txn_memtable_version_Version_word(self) & TXN_MEMTABLE_VERSION_KILLED) != 0;
}

/*
 * txn_memtable_version_Version_kill marks a speculative insert's version dead
 * to every reader, as heap_abort_speculative does, and clears its token.
 */
void
txn_memtable_version_Version_kill(TxnMemtableVersionVersion *self)
{
	txn_memtable_version_Version_update(self, (UINT64CONST(1) << TXN_MEMTABLE_VERSION_TOKEN) - 1,
										TXN_MEMTABLE_VERSION_KILLED);
}

/* txn_memtable_version_Version_drop frees the version's record: its lent bytes, or the spilled copy. */
void
txn_memtable_version_Version_drop(TxnMemtableVersionVersion *self)
{
	if (self->record.tag == TXN_MEMTABLE_VERSION_BODY_LENT)
		host_delta_Lent_drop(&self->record.u.lent);
	else
		std_vec_free_Checked(&self->record.u.spilled, 1, 1);
}

/*
 * txn_memtable_version_versions_drop frees a Vec<Version>, dropping the
 * versions from a place on, which are still its own, then its buffer.
 */
void
txn_memtable_version_versions_drop(StdVec *versions, size_t from)
{
	for (size_t i = from; i < versions->len; i++)
		txn_memtable_version_Version_drop(&STD_VEC_AT(versions, TxnMemtableVersionVersion, i));
	std_vec_free_Checked(versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
}

/* txn_memtable_version_Handle_at returns the handle of a line pointer of a page of the range's list. */
static TxnMemtableVersionHandle
txn_memtable_version_Handle_at(uint64 page, uint64 word)
{
	TxnMemtableVersionHandle handle = {page << TXN_MEMTABLE_VERSION_WORD | word};

	return handle;
}

/*
 * txn_memtable_version_linked returns a line pointer's link: its record's
 * offset above its key's older version's handle.
 */
static uint64
txn_memtable_version_linked(uint32 offset, uint64 older)
{
	return (uint64) offset << TXN_MEMTABLE_VERSION_OFFSET | older;
}

/* txn_memtable_version_recorded returns the length of the record at an offset of a page, from its header. */
static bool
txn_memtable_version_recorded(const HostDeltaPageRef *page, uint64 offset, uint64 *result, HostError *error)
{
	HostDeltaLent header;
	StdBytes	bytes;
	size_t		values = 0;

	if (!host_delta_PageRef_bytes(page, offset, TXN_MEMTABLE_VERSION_HEADER, &header, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&header);
	if ((bytes.ptr[28] & TXN_MEMTABLE_VERSION_VALUES) != 0)
	{
		HostDeltaLent count;
		StdBytes	counted;

		if (!host_delta_PageRef_bytes(page, STD_ADD(u64, offset, txn_memtable_version_counted(bytes.ptr[28]), 789, 36),
									  2, &count, error))
		{
			host_delta_Lent_drop(&header);
			return false;
		}
		counted = host_delta_Lent_Deref_deref(&count);
		values = std_u16_from_le_bytes(counted.ptr);
		host_delta_Lent_drop(&count);
	}
	*result = txn_memtable_version_length(bytes.ptr, values);
	host_delta_Lent_drop(&header);
	return true;
}

/*
 * txn_memtable_version_line reads the page of the range's list a handle's
 * line pointer is on, by its record's pages list, and the line pointer's
 * word on it.
 */
static bool
txn_memtable_version_line(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						  const HostDeltaHold *hold, TxnMemtableVersionHandle handle, HostDeltaPageRef *page,
						  uint64 *word, HostError *error)
{
	HostDeltaListListView list = host_delta_list_Guard_list(guard, keyed->closes.shape, TXN_MEMTABLE_INDEX_PAGES);
	uint64		named;

	if (!host_delta_list_ListView_get(&list, hold, handle.f0 >> TXN_MEMTABLE_VERSION_WORD, &named, error) ||
		!host_delta_Hold_page(hold, host_delta_page_Page_named(named), page, error))
		return false;
	*word = handle.f0 & ((UINT64CONST(1) << TXN_MEMTABLE_VERSION_WORD) - 1);
	return true;
}

/*
 * txn_memtable_version_carried copies the lines on a page of an open
 * record's list that the chains from a set of heads hold onto the page's
 * copy, a compaction's: each line's record appended to the copy and its link
 * and hint stored at the same indexes, with only the record's offset anew.
 */
bool
txn_memtable_version_carried(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, const uint64 *heads,
							 size_t heads_len, uint64 entry, HostDeltaOwned *copy, HostError *error)
{
	for (size_t i = 0; i < heads_len; i++)
	{
		TxnMemtableVersionHandle handle;
		bool		has = txn_memtable_version_Handle_named(heads[i], &handle);

		while (has)
		{
			HostDeltaPageRef page;
			uint64		word;
			HostDeltaWord link_word;
			uint64		link;
			uint64		older;

			if (!txn_memtable_version_line(open->keyed, open->open.guard, hold, handle, &page, &word, error) ||
				!host_delta_PageRef_word(&page, word, &link_word, error))
				return false;
			link = host_delta_Word_load(&link_word);
			older = link & TXN_MEMTABLE_VERSION_OLDER;
			if (handle.f0 >> TXN_MEMTABLE_VERSION_WORD == entry)
			{
				uint64		offset = link >> TXN_MEMTABLE_VERSION_OFFSET;
				uint64		len;
				HostDeltaLent record;
				StdBytes	bytes;
				uint32		moved;
				HostDeltaWord hint;
				bool		ok;

				if (!txn_memtable_version_recorded(&page, offset, &len, error) ||
					!host_delta_PageRef_bytes(&page, offset, len, &record, error))
					return false;
				bytes = host_delta_Lent_Deref_deref(&record);
				ok = host_delta_Owned_append(copy, bytes.ptr, bytes.len, &moved, error) &&
					host_delta_Owned_store(copy, word, txn_memtable_version_linked(moved, older), error) &&
					host_delta_PageRef_word(&page, STD_ADD(u64, word, 1, 820, 28), &hint, error) &&
					host_delta_Owned_store(copy, word + 1, host_delta_Word_load(&hint), error);
				host_delta_Lent_drop(&record);
				if (!ok)
					return false;
			}
			has = txn_memtable_version_Handle_named(older, &handle);
		}
	}
	return true;
}

/* txn_memtable_version_version reads the version a handle names under its record's lock. */
bool
txn_memtable_version_version(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
							 const HostDeltaHold *hold, TxnMemtableVersionHandle handle,
							 TxnMemtableVersionVersion *result, HostError *error)
{
	HostDeltaPageRef page;
	uint64		word;
	HostDeltaWord link_word;
	uint64		link;
	uint64		offset;
	uint64		len;

	if (!txn_memtable_version_line(keyed, guard, hold, handle, &page, &word, error) ||
		!host_delta_PageRef_word(&page, word, &link_word, error))
		return false;
	link = host_delta_Word_load(&link_word);
	offset = link >> TXN_MEMTABLE_VERSION_OFFSET;
	result->handle = handle;
	result->record.tag = TXN_MEMTABLE_VERSION_BODY_LENT;
	if (!txn_memtable_version_recorded(&page, offset, &len, error) ||
		!host_delta_PageRef_bytes(&page, offset, len, &result->record.u.lent, error))
		return false;
	result->has_older = txn_memtable_version_Handle_named(link & TXN_MEMTABLE_VERSION_OLDER, &result->older);
	if (!host_delta_PageRef_word(&page, STD_ADD(u64, word, 1, 839, 84), &result->hint, error))
	{
		host_delta_Lent_drop(&result->record.u.lent);
		return false;
	}
	result->has_hint = true;
	return true;
}

/*
 * txn_memtable_version_speculate stores a speculative insert's token in the
 * hint word of the version a handle names, under its range's lock.
 */
bool
txn_memtable_version_speculate(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
							   const HostDeltaHold *hold, TxnMemtableVersionHandle handle, uint32 token,
							   HostError *error)
{
	TxnMemtableVersionVersion version;

	if (!txn_memtable_version_version(keyed, guard, hold, handle, &version, error))
		return false;
	txn_memtable_version_Version_update(&version, PG_UINT64_MAX, (uint64) token << TXN_MEMTABLE_VERSION_TOKEN);
	txn_memtable_version_Version_drop(&version);
	return true;
}

/*
 * txn_memtable_version_show stores a combo command id as the raw command of
 * the row of the version a handle names, under its range's lock in the
 * window of the change that ends or locks the row. A spilled version has no
 * hint word, so its row shows its record's command.
 */
bool
txn_memtable_version_show(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						  const HostDeltaHold *hold, TxnMemtableVersionHandle handle, uint32 combo, HostError *error)
{
	TxnMemtableVersionVersion version;

	if ((handle.f0 & TXN_MEMTABLE_VERSION_SPILLED) != 0)
		return true;
	if (!txn_memtable_version_version(keyed, guard, hold, handle, &version, error))
		return false;
	txn_memtable_version_Version_update(&version,
										TXN_MEMTABLE_VERSION_HINT | TXN_MEMTABLE_VERSION_KILLED |
										TXN_MEMTABLE_VERSION_COUNTED,
										TXN_MEMTABLE_VERSION_SHOWN | (uint64) combo << TXN_MEMTABLE_VERSION_TOKEN);
	txn_memtable_version_Version_drop(&version);
	return true;
}

/*
 * txn_memtable_version_head returns the handle of a key's newest version from
 * its node, or false for a key whose versions are gone.
 */
bool
txn_memtable_version_head(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						  const HostDeltaHold *hold, TxnMemtableIndexNode node, bool *has_result,
						  TxnMemtableVersionHandle *result, HostError *error)
{
	uint64		head;

	if (!txn_memtable_index_head(keyed, guard, hold, node, &head, error))
		return false;
	*has_result = txn_memtable_version_Handle_named(head, result);
	return true;
}

/* txn_memtable_version_chain returns a key's versions from its node's head, newest first. */
bool
txn_memtable_version_chain(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						   const HostDeltaHold *hold, TxnMemtableIndexNode node, TxnMemtableVersionChain *result,
						   HostError *error)
{
	uint64		head;

	if (!txn_memtable_index_head(keyed, guard, hold, node, &head, error))
		return false;
	result->keyed = keyed;
	result->guard = guard;
	result->hold = hold;
	result->has_next = txn_memtable_version_Handle_named(head, &result->next);
	return true;
}

/*
 * txn_memtable_version_Chain_Iterator_next is Chain's next: the next older
 * version, its read's error, or false once the chain ends; an error ends it.
 */
bool
txn_memtable_version_Chain_Iterator_next(TxnMemtableVersionChain *self, bool *has_result,
										 TxnMemtableVersionVersion *result, HostError *error)
{
	TxnMemtableVersionHandle next = self->next;

	*has_result = self->has_next;
	if (!self->has_next)
		return true;
	self->has_next = false;
	if (!txn_memtable_version_version(self->keyed, self->guard, self->hold, next, result, error))
		return false;
	self->has_next = result->has_older;
	self->next = result->older;
	return true;
}

/*
 * txn_memtable_version_push makes a version its key's newest in an open
 * record: its record, encoded into the buffer given, and line pointer, and a
 * new key's node with the version as its head, are placed together on the
 * range's newest page or on the fresh page given; every step that can fail
 * runs first, and the store of the key's head, or of the new node's links,
 * publishes it. A counted version's hint word carries the counted mark.
 */
bool
txn_memtable_version_push(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, const uint8 *key,
						  size_t key_len, const TxnMemtableVersionChange *change, bool counted, bool *has_fresh,
						  HostDeltaOwned *fresh, StdVec *spare, StdVec *buffer, TxnMemtableVersionPushed *result,
						  HostError *error)
{
	TxnMemtableIndexFound found;
	size_t		bytes;
	uint64		words;
	uint64		older = TXN_MEMTABLE_INDEX_NONE;
	bool		ok;
	TxnMemtablePagesRoom room;
	TxnMemtableVersionHandle handle;
	uint32		offset;
	TxnMemtableIndexKeyedOpen *finished;

	txn_memtable_version_encode(&change->writer, change->op, change->lock, change->shown, change->has_lsn,
								change->lsn, buffer);
	if (!txn_memtable_index_lookup(open, hold, key, key_len, &found, error))
		return false;
	if (found.tag == TXN_MEMTABLE_INDEX_FOUND_NODE)
	{
		bytes = buffer->len;
		words = TXN_MEMTABLE_VERSION_LINE;
		if (!txn_memtable_index_head(open->keyed, open->open.guard, hold, found.u.node, &older, error))
			return false;
	}
	else
	{
		bytes = STD_ADD(usize, buffer->len, key_len, 919, 29);
		words = STD_ADD(u64, TXN_MEMTABLE_VERSION_LINE, txn_memtable_index_New_words(&found.u.new_), 919, 55);
	}
	if (!txn_memtable_pages_room(open, hold, bytes, words, has_fresh, fresh, spare->ptr, spare->len, &ok, &room,
								 &result->u.wants, error))
		return false;
	if (!ok)
	{
		result->tag = TXN_MEMTABLE_VERSION_PUSHED_WANTS;
		return true;
	}
	handle = txn_memtable_version_Handle_at(room.page, room.first);
	if (!txn_memtable_pages_Room_append(&room, buffer->ptr, buffer->len, &offset, error) ||
		!txn_memtable_pages_Room_store(&room, 0, txn_memtable_version_linked(offset, older), error) ||
		!txn_memtable_pages_Room_store(&room, 1,
									   TXN_MEMTABLE_VERSION_HINT_UNKNOWN | (counted ? TXN_MEMTABLE_VERSION_COUNTED : 0),
									   error))
	{
		txn_memtable_pages_Room_drop(&room);
		return false;
	}
	if (found.tag == TXN_MEMTABLE_INDEX_FOUND_NEW)
	{
		uint32		at;
		TxnMemtableIndexNode node;

		if (!txn_memtable_pages_Room_append(&room, key, key_len, &at, error) ||
			!txn_memtable_index_New_store(&found.u.new_, &room, TXN_MEMTABLE_VERSION_LINE, at, key, key_len,
										  handle.f0, &node, error))
		{
			txn_memtable_pages_Room_drop(&room);
			return false;
		}
		if (!txn_memtable_pages_Room_finish(room, hold, spare, &finished, error) ||
			!txn_memtable_index_New_publish(found.u.new_, finished, hold, node, error))
			return false;
	}
	else if (!txn_memtable_pages_Room_finish(room, hold, spare, &finished, error) ||
			 !txn_memtable_index_set_head(finished, hold, found.u.node, handle.f0, error))
		return false;
	result->tag = TXN_MEMTABLE_VERSION_PUSHED_VERSION;
	result->u.version = handle;
	return true;
}

/*
 * txn_memtable_version_removed_frame_t is removed's state across its pad:
 * the pages' list entries it took versions from, while live.
 */
typedef struct txn_memtable_version_removed_frame_t
{
	const TxnMemtableIndexKeyedOpen *open;
	const HostDeltaHold *hold;
	TxnMemtableIndexNode node;
	const TxnMemtableVersionHandle *handles;
	size_t		handles_len;
	bool	   *taken;
	size_t		taken_len;
	StdVec	   *counted;
	StdVec		touched;
	bool		touched_live;
	bool	   *result_0;
	StdVec	   *result_1;
	uint64	   *result_2;
	HostError  *error;
	bool		ok;
} txn_memtable_version_removed_frame_t;

/* txn_memtable_version_removed_closure_0 is removed's line: the page a handle's line pointer is on, and its word. */
static bool
txn_memtable_version_removed_closure_0(const txn_memtable_version_removed_frame_t *frame,
									   TxnMemtableVersionHandle handle, HostDeltaPageRef *page, uint64 *word,
									   HostError *error)
{
	return txn_memtable_version_line(frame->open->keyed, frame->open->open.guard, frame->hold, handle, page, word,
									 error);
}

/*
 * txn_memtable_version_removed_counted appends to removed's counted list the
 * top-level xid of a version it takes out whose hint word carries the counted
 * mark, read from its record at an offset of its page.
 */
static bool
txn_memtable_version_removed_counted(txn_memtable_version_removed_frame_t *frame, const HostDeltaPageRef *page,
									 uint64 word, uint64 offset, uint64 bytes)
{
	HostDeltaWord hint;
	HostDeltaLent lent;
	StdBytes	read;
	TxnMemtableVersionRecord record;
	TxnMemtableVersionWriter writer;
	uint64		top;

	if (!host_delta_PageRef_word(page, word + 1, &hint, frame->error))
		return false;
	if ((host_delta_Word_load(&hint) & TXN_MEMTABLE_VERSION_COUNTED) == 0)
		return true;
	if (!host_delta_PageRef_bytes(page, offset, bytes, &lent, frame->error))
		return false;
	read = host_delta_Lent_Deref_deref(&lent);
	record = txn_memtable_version_Record_of(read.ptr, read.len);
	writer = txn_memtable_version_Record_writer(&record);
	top = writer.has_top ? writer.top : writer.xid;
	host_delta_Lent_drop(&lent);
	if (!std_vec_try_reserve(frame->counted, 1, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(frame->counted, &top, sizeof(uint64), _Alignof(uint64));
	return true;
}

/*
 * txn_memtable_version_removed_body walks the chain from its head, linking
 * past each version a handle names and counting its page's live record and
 * dead bytes.
 */
static void
txn_memtable_version_removed_body(void *frame_)
{
	txn_memtable_version_removed_frame_t *frame = frame_;
	const TxnMemtableIndexKeyedOpen *open = frame->open;
	bool		has_newer = false;
	HostDeltaWord newer;
	uint64		head;
	TxnMemtableVersionHandle handle;
	bool		has;
	bool		kept = false;
	uint64		removed = 0;

	std_vec_new(&frame->touched, sizeof(uint64), _Alignof(uint64));
	frame->touched_live = true;
	if (!txn_memtable_index_head(open->keyed, open->open.guard, frame->hold, frame->node, &head, frame->error))
		return;
	has = txn_memtable_version_Handle_named(head, &handle);
	while (has)
	{
		HostDeltaPageRef page;
		uint64		word;
		HostDeltaWord link;
		uint64		older;
		size_t		place = 0;

		if (!txn_memtable_version_removed_closure_0(frame, handle, &page, &word, frame->error) ||
			!host_delta_PageRef_word(&page, word, &link, frame->error))
			return;
		older = host_delta_Word_load(&link) & TXN_MEMTABLE_VERSION_OLDER;
		while (place < frame->handles_len && frame->handles[place].f0 != handle.f0)
			place++;
		if (place < frame->handles_len)
		{
			uint64		bytes;
			HostDeltaWord live;
			HostDeltaWord dead;
			uint64		entry = handle.f0 >> TXN_MEMTABLE_VERSION_WORD;
			bool		touched = false;

			if (!txn_memtable_version_recorded(&page, host_delta_Word_load(&link) >> TXN_MEMTABLE_VERSION_OFFSET,
											   &bytes, frame->error))
				return;
			if (has_newer)
				host_delta_Word_store(&newer, host_delta_Word_load(&newer) >> TXN_MEMTABLE_VERSION_OFFSET <<
									  TXN_MEMTABLE_VERSION_OFFSET | older);
			else if (!txn_memtable_index_set_head(open, frame->hold, frame->node, older, frame->error))
				return;
			if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LIVE, &live, frame->error))
				return;
			host_delta_Word_fetch_add(&live, PG_UINT64_MAX);
			if (!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_DEAD, &dead, frame->error))
				return;
			host_delta_Word_fetch_add(&dead, bytes);
			removed = STD_ADD(u64, removed, bytes, 974, 17);
			std_index_check(place, frame->taken_len, RUST_FILE, 975, 17);
			frame->taken[place] = true;
			if (!txn_memtable_version_removed_counted(frame, &page, word, host_delta_Word_load(&link) >>
													  TXN_MEMTABLE_VERSION_OFFSET, bytes))
				return;
			for (size_t i = 0; i < frame->touched.len && !touched; i++)
				touched = STD_VEC_AT(&frame->touched, uint64, i) == entry;
			if (!touched)
			{
				if (!std_vec_try_reserve(&frame->touched, 1, sizeof(uint64), _Alignof(uint64)))
				{
					*frame->error = host_Error_out_of_memory();
					return;
				}
				std_vec_push_Checked(&frame->touched, &entry, sizeof(uint64), _Alignof(uint64));
			}
		}
		else
		{
			kept = true;
			has_newer = true;
			newer = link;
		}
		has = txn_memtable_version_Handle_named(older, &handle);
	}
	*frame->result_0 = !kept;
	*frame->result_1 = frame->touched;
	*frame->result_2 = removed;
	frame->touched_live = false;
	frame->ok = true;
}

/* txn_memtable_version_removed_pad frees what removed holds live: the pages' list entries. */
static void
txn_memtable_version_removed_pad(void *frame_)
{
	txn_memtable_version_removed_frame_t *frame = frame_;

	if (frame->touched_live)
		std_vec_free_Checked(&frame->touched, sizeof(uint64), _Alignof(uint64));
	frame->touched_live = false;
}

/*
 * txn_memtable_version_removed takes versions out of a key's chain in an
 * open record, newest first, passing over a handle the chain doesn't hold:
 * the key's head moves to a removed newest version's older, or the newer
 * version's link takes the removed one's older, and each removed version's
 * page counts one live record less and its record's bytes dead. It marks
 * each handle it took out at its place in taken, appends to counted the
 * top-level xid of each it took out that its push counted, and answers
 * whether the chain is left empty, the list entries of the pages it took
 * versions from, and the bytes of the records it took out.
 */
bool
txn_memtable_version_removed(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
							 TxnMemtableIndexNode node, const TxnMemtableVersionHandle *handles, size_t handles_len,
							 bool *taken, size_t taken_len, StdVec *counted, bool *result_0, StdVec *result_1,
							 uint64 *result_2, HostError *error)
{
	txn_memtable_version_removed_frame_t frame = {.open = open, .hold = hold, .node = node, .handles = handles,
		.handles_len = handles_len, .taken = taken, .taken_len = taken_len, .counted = counted,
	.result_0 = result_0, .result_1 = result_1, .result_2 = result_2, .error = error};

	std_landing(&frame, txn_memtable_version_removed_body, txn_memtable_version_removed_pad);
	txn_memtable_version_removed_pad(&frame);
	return frame.ok;
}

/* txn_memtable_version_bytes_eq reports whether two byte slices are equal, as [u8]'s PartialEq compares them. */
static bool
txn_memtable_version_bytes_eq(StdBytes self, StdBytes other)
{
	return self.len == other.len && (self.len == 0 || memcmp(self.ptr, other.ptr, self.len) == 0);
}

/* txn_memtable_version_Writer_PartialEq_eq reports whether two writers are equal field by field. */
bool
txn_memtable_version_Writer_PartialEq_eq(const TxnMemtableVersionWriter *self, const TxnMemtableVersionWriter *other)
{
	return self->xid == other->xid && self->has_top == other->has_top && (!self->has_top || self->top == other->top) &&
		self->command == other->command;
}

/* txn_memtable_version_Writer_Debug_fmt is Writer's derived Debug. */
bool
txn_memtable_version_Writer_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionWriter *self = value;
	StdOptionRef top = {self->has_top, {&self->top, std_u64_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Writer", 6);

	std_DebugStruct_field(&s, "xid", 3, &self->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "top", 3, &top, std_option_Debug_fmt);
	std_DebugStruct_field(&s, "command", 7, &self->command, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_memtable_version_Shown_PartialEq_eq reports whether two shown commands are the same variant and value. */
bool
txn_memtable_version_Shown_PartialEq_eq(const TxnMemtableVersionShown *self, const TxnMemtableVersionShown *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO)
		return self->u.combo == other->u.combo;
	if (self->tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING)
		return self->u.ending == other->u.ending;
	return true;
}

/* txn_memtable_version_variant_Debug_fmt writes a tuple struct's or variant's Debug of one field, or of two. */
static bool
txn_memtable_version_variant_Debug_fmt(StdFormatter *f, const char *name, const void *first, StdFmtFn first_fmt,
									   const void *second, StdFmtFn second_fmt)
{
	StdDebugTuple t = std_fmt_debug_tuple(f, name, strlen(name));

	std_DebugTuple_field(&t, first, first_fmt);
	if (second != NULL)
		std_DebugTuple_field(&t, second, second_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_memtable_version_Shown_Debug_fmt is Shown's derived Debug. */
bool
txn_memtable_version_Shown_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionShown *self = value;

	switch (self->tag)
	{
		case TXN_MEMTABLE_VERSION_SHOWN_COMMAND:
			return std_fmt_write_str(f, "Command", 7);
		case TXN_MEMTABLE_VERSION_SHOWN_COMBO:
			return txn_memtable_version_variant_Debug_fmt(f, "Combo", &self->u.combo, std_u32_Debug_fmt, NULL, NULL);
		case TXN_MEMTABLE_VERSION_SHOWN_REPLAYED:
			return std_fmt_write_str(f, "Replayed", 8);
		case TXN_MEMTABLE_VERSION_SHOWN_ENDING:
			return txn_memtable_version_variant_Debug_fmt(f, "Ending", &self->u.ending, std_u32_Debug_fmt, NULL, NULL);
	}
	pg_unreachable();
}

/* txn_memtable_version_Handle_PartialEq_eq reports whether two handles name the same version. */
bool
txn_memtable_version_Handle_PartialEq_eq(const TxnMemtableVersionHandle *self, const TxnMemtableVersionHandle *other)
{
	return self->f0 == other->f0;
}

/* txn_memtable_version_Handle_Debug_fmt is Handle's derived Debug. */
bool
txn_memtable_version_Handle_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionHandle *self = value;

	return txn_memtable_version_variant_Debug_fmt(f, "Handle", &self->f0, std_u64_Debug_fmt, NULL, NULL);
}

/* txn_memtable_version_Hint_Debug_fmt is Hint's derived Debug: its variant's name. */
bool
txn_memtable_version_Hint_Debug_fmt(const void *value, StdFormatter *f)
{
	switch (*(const TxnMemtableVersionHint *) value)
	{
		case TXN_MEMTABLE_VERSION_HINT_UNKNOWN:
			return std_fmt_write_str(f, "Unknown", 7);
		case TXN_MEMTABLE_VERSION_HINT_COMMITTED:
			return std_fmt_write_str(f, "Committed", 9);
		case TXN_MEMTABLE_VERSION_HINT_ABORTED:
			return std_fmt_write_str(f, "Aborted", 7);
	}
	pg_unreachable();
}

/* txn_memtable_version_Deleter_PartialEq_eq reports whether two deleters are equal field by field. */
bool
txn_memtable_version_Deleter_PartialEq_eq(const TxnMemtableVersionDeleter *self,
										  const TxnMemtableVersionDeleter *other)
{
	return self->xid == other->xid && self->has_command == other->has_command &&
		(!self->has_command || self->command == other->command);
}

/* txn_memtable_version_Deleter_Debug_fmt is Deleter's derived Debug. */
bool
txn_memtable_version_Deleter_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionDeleter *self = value;
	StdOptionRef command = {self->has_command, {&self->command, std_u32_Debug_fmt}};
	StdDebugStruct s = std_fmt_debug_struct(f, "Deleter", 7);

	std_DebugStruct_field(&s, "xid", 3, &self->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "command", 7, &command, std_option_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_memtable_version_PutRef_PartialEq_eq reports whether two puts are equal field by field. */
bool
txn_memtable_version_PutRef_PartialEq_eq(const TxnMemtableVersionPutRef *self, const TxnMemtableVersionPutRef *other)
{
	return txn_memtable_version_bytes_eq(self->value, other->value) &&
		txn_memtable_version_bytes_eq(self->addresses, other->addresses) &&
		txn_memtable_version_bytes_eq(self->values, other->values) &&
		host_Row_PartialEq_eq(&self->tuple, &other->tuple) &&
		self->heap_bytes == other->heap_bytes && self->has_carried == other->has_carried &&
		self->carried == other->carried;
}

/* txn_memtable_version_PutRef_Debug_fmt is PutRef's derived Debug. */
bool
txn_memtable_version_PutRef_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionPutRef *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "PutRef", 6);

	std_DebugStruct_field(&s, "value", 5, &self->value, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "addresses", 9, &self->addresses, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "values", 6, &self->values, std_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "tuple", 5, &self->tuple, host_Row_Debug_fmt);
	std_DebugStruct_field(&s, "heap_bytes", 10, &self->heap_bytes, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "has_carried", 11, &self->has_carried, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "carried", 7, &self->carried, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_memtable_version_SuccessorRef_PartialEq_eq reports whether two successors are the same variant and key. */
bool
txn_memtable_version_SuccessorRef_PartialEq_eq(const TxnMemtableVersionSuccessorRef *self,
											   const TxnMemtableVersionSuccessorRef *other)
{
	return self->tag == other->tag &&
		(self->tag != TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY ||
		 txn_memtable_version_bytes_eq(self->u.key, other->u.key));
}

/* txn_memtable_version_SuccessorRef_Debug_fmt is SuccessorRef's derived Debug. */
bool
txn_memtable_version_SuccessorRef_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionSuccessorRef *self = value;

	if (self->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE)
		return std_fmt_write_str(f, "OtherTable", 10);
	return txn_memtable_version_variant_Debug_fmt(f, "Key", &self->u.key, std_bytes_Debug_fmt, NULL, NULL);
}

/* txn_memtable_version_OpRef_Debug_fmt is OpRef's derived Debug. */
bool
txn_memtable_version_OpRef_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionOpRef *self = value;
	StdOptionRef successor = {.some.fmt = txn_memtable_version_SuccessorRef_Debug_fmt};

	switch (self->tag)
	{
		case TXN_MEMTABLE_VERSION_OP_REF_PUT:
			return txn_memtable_version_variant_Debug_fmt(f, "Put", &self->u.put,
														  txn_memtable_version_PutRef_Debug_fmt, NULL, NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE:
			return std_fmt_write_str(f, "Delete", 6);
		case TXN_MEMTABLE_VERSION_OP_REF_EMPTIED:
			return txn_memtable_version_variant_Debug_fmt(f, "Emptied", &self->u.emptied, host_Row_Debug_fmt, NULL,
														  NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_NAMED:
			return txn_memtable_version_variant_Debug_fmt(f, "Named", &self->u.named.f0, host_Row_Debug_fmt,
														  &self->u.named.f1, txn_memtable_version_PutRef_Debug_fmt);
		case TXN_MEMTABLE_VERSION_OP_REF_MOVE:
			return txn_memtable_version_variant_Debug_fmt(f, "Move", &self->u.move,
														  txn_memtable_version_SuccessorRef_Debug_fmt, NULL, NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_ADD:
			return txn_memtable_version_variant_Debug_fmt(f, "Add", &self->u.add,
														  txn_memtable_version_PutRef_Debug_fmt, NULL, NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_REMOVE:
			successor.has = self->u.remove.has_f1;
			successor.some.value = &self->u.remove.f1;
			return txn_memtable_version_variant_Debug_fmt(f, "Remove", &self->u.remove.f0, host_Row_Debug_fmt,
														  &successor, std_option_Debug_fmt);
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK:
			return txn_memtable_version_variant_Debug_fmt(f, "Lock", &self->u.lock, txn_memtable_LockMode_Debug_fmt,
														  NULL, NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW:
			return txn_memtable_version_variant_Debug_fmt(f, "AddRow", &self->u.add_row,
														  txn_memtable_version_PutRef_Debug_fmt, NULL, NULL);
		case TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW:
			return txn_memtable_version_variant_Debug_fmt(f, "UpdateRow", &self->u.update_row.f0,
														  host_Row_Debug_fmt, &self->u.update_row.f1,
														  txn_memtable_version_PutRef_Debug_fmt);
		case TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW:
			successor.has = self->u.delete_row.has_f1;
			successor.some.value = &self->u.delete_row.f1;
			return txn_memtable_version_variant_Debug_fmt(f, "DeleteRow", &self->u.delete_row.f0,
														  host_Row_Debug_fmt, &successor, std_option_Debug_fmt);
		case TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW:
			return txn_memtable_version_variant_Debug_fmt(f, "LockRow", &self->u.lock_row.f0, host_Row_Debug_fmt,
														  &self->u.lock_row.f1, txn_memtable_LockMode_Debug_fmt);
	}
	pg_unreachable();
}

/* txn_memtable_version_Pushed_Debug_fmt is Pushed's derived Debug. */
bool
txn_memtable_version_Pushed_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableVersionPushed *self = value;

	if (self->tag == TXN_MEMTABLE_VERSION_PUSHED_VERSION)
		return txn_memtable_version_variant_Debug_fmt(f, "Version", &self->u.version,
													  txn_memtable_version_Handle_Debug_fmt, NULL, NULL);
	return txn_memtable_version_variant_Debug_fmt(f, "Wants", &self->u.wants, txn_memtable_pages_Want_Debug_fmt, NULL,
												  NULL);
}

#ifdef DOLTGRES_TEST_TXN
/* TXN_MEMTABLE_VERSION_TESTS_UNWRAP is Result::unwrap's panic text. */
#define TXN_MEMTABLE_VERSION_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* txn_memtable_version_tests_ok unwraps a host result at a line. */
static void
txn_memtable_version_tests_ok(bool ok, HostError *error, uint32 line)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, 0, TXN_MEMTABLE_VERSION_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* txn_memtable_version_tests_read_t is read's (u64, Deleter, Hint): a version's handle, its older version's deleter, and its hint. */
typedef struct txn_memtable_version_tests_read_t
{
	uint64		handle;
	TxnMemtableVersionDeleter deleter;
	TxnMemtableVersionHint hint;
} txn_memtable_version_tests_read_t;

/* txn_memtable_version_tests_opened takes a hold, the Keys record at a location, and its lock shared, then finds a key's node. */
static void
txn_memtable_version_tests_opened(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len, HostDeltaHold *hold,
								  HostSharedGuard *guard, TxnMemtableIndexNode *node, uint32 line)
{
	HostSharedRecord record;
	bool		some = false;
	HostError	error;

	txn_memtable_version_tests_ok(host_delta_hold(io, hold, &error) &&
								  host_delta_Hold_record(hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record,
														 &error) &&
								  host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io),
																 TXN_MEMTABLE_TESTING_KEYS_LOCK, guard, &error) &&
								  txn_memtable_index_find(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, guard, hold, key,
														  key_len, &some, node, &error), &error, line);
	STD_ASSERT_MSG(line, 0, some, "no key");
}

/* txn_memtable_version_tests_newest returns a found key's newest version, which unwraps at a line. */
static TxnMemtableVersionVersion
txn_memtable_version_tests_newest(const HostSharedGuard *guard, const HostDeltaHold *hold, TxnMemtableIndexNode node,
								  uint32 line)
{
	TxnMemtableVersionChain chain;
	TxnMemtableVersionVersion version;
	bool		some = false;
	HostError	error;

	txn_memtable_version_tests_ok(txn_memtable_version_chain(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, guard, hold, node,
															 &chain, &error) &&
								  txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, &error), &error,
								  line);
	STD_ASSERT_MSG(line, 0, some, "a key with no version");
	return version;
}

/* txn_memtable_version_tests_read reads a key's versions newest first, each as its handle, its older version's deleter, and its hint. */
static StdVec
txn_memtable_version_tests_read(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len)
{
	HostDeltaHold hold;
	HostSharedGuard guard;
	TxnMemtableIndexNode node;
	TxnMemtableVersionChain chain;
	StdVec		read;
	HostError	error;

	txn_memtable_version_tests_opened(io, keys, key, key_len, &hold, &guard, &node, __LINE__);
	txn_memtable_version_tests_ok(txn_memtable_version_chain(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &hold, node,
															 &chain, &error), &error, __LINE__);
	std_vec_new(&read, sizeof(txn_memtable_version_tests_read_t), _Alignof(txn_memtable_version_tests_read_t));
	for (;;)
	{
		TxnMemtableVersionVersion version;
		txn_memtable_version_tests_read_t one;
		bool		some;

		txn_memtable_version_tests_ok(txn_memtable_version_Chain_Iterator_next(&chain, &some, &version, &error),
									  &error, __LINE__);
		if (!some)
			break;
		one.handle = version.handle.f0;
		one.deleter = txn_memtable_version_Version_deleter(&version);
		one.hint = txn_memtable_version_Version_hint(&version);
		txn_memtable_version_Version_drop(&version);
		std_vec_push_Checked(&read, &one, sizeof(one), _Alignof(txn_memtable_version_tests_read_t));
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return read;
}

/* txn_memtable_version_tests_pushed pushes a key's change, answering the version it pushed, which unwraps at a line. */
static uint64
txn_memtable_version_tests_pushed(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len, TxnMemtableOp op,
								  HostDeltaPageSize size, uint32 line)
{
	TxnMemtableVersionPushed pushed;
	HostError	error;

	txn_memtable_version_tests_ok(txn_memtable_testing_pushed(io, keys, key, key_len, &op, size, &pushed, &error),
								  &error, line);
	txn_memtable_Op_drop(&op);
	STD_ASSERT_MSG(line, 0, pushed.tag == TXN_MEMTABLE_VERSION_PUSHED_VERSION, "explicit panic");
	return pushed.u.version.f0;
}

/* txn_memtable_version_tests_numbered returns key 1 of the testing keys. */
static StdVec
txn_memtable_version_tests_one_key(void)
{
	StdVec		key;

	txn_memtable_testing_numbered(1, &key);
	return key;
}

/* txn_memtable_version_tests_put returns a put of a test's value with its addresses and value ids, under row 9. */
static TxnMemtableOp
txn_memtable_version_tests_put(uint8 tag, size_t addresses, size_t values, uint32 heap_bytes)
{
	ChunksTrailerLocator locator = chunks_trailer_Locator_at(UINT64CONST(0xabcdef), UINT64CONST(0x123456789a),
															 UINT64CONST(0xfedcba));
	static const uint32 ids[] = {11, 0xfffffffe};
	TxnMemtableOp op = {.tag = tag};

	std_slice_to_vec_Checked(&op.u.put.value, "value", 5, 1, 1);
	std_vec_new(&op.u.put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	for (uint32 k = 0; k < addresses; k++)
	{
		chunks_TupleU32Locator_t address = {3 + k, locator};

		std_vec_push_Checked(&op.u.put.addresses, &address, sizeof(address), _Alignof(chunks_TupleU32Locator_t));
	}
	std_slice_to_vec_Checked(&op.u.put.values, ids, values, sizeof(uint32), _Alignof(uint32));
	op.u.put.tuple.f0 = 9;
	op.u.put.heap_bytes = heap_bytes;
	return op;
}

/* txn_memtable_version_tests_moved returns a successor: another key, "moved", or another table. */
static TxnMemtableSuccessor
txn_memtable_version_tests_moved(bool other)
{
	TxnMemtableSuccessor successor = {.tag = other ? TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE : TXN_MEMTABLE_SUCCESSOR_KEY};

	if (!other)
		std_slice_to_vec_Checked(&successor.u.key, "moved", 5, 1, 1);
	return successor;
}

/* txn_memtable_version_tests_ops returns every op the record test writes, in the Rust test's order. */
static StdVec
txn_memtable_version_tests_ops(void)
{
	StdVec		ops;
	TxnMemtableOp op;

	std_vec_new(&ops, sizeof(TxnMemtableOp), _Alignof(TxnMemtableOp));
#define TXN_MEMTABLE_VERSION_TESTS_OP(made) \
	do { op = (made); std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp)); } while (0)
	TXN_MEMTABLE_VERSION_TESTS_OP(txn_memtable_version_tests_put(TXN_MEMTABLE_OP_PUT, 2, 2, 44));
	TXN_MEMTABLE_VERSION_TESTS_OP(txn_memtable_version_tests_put(TXN_MEMTABLE_OP_PUT, 1, 0, 32));
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_DELETE
	};
	std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_EMPTIED
	};
	op.u.emptied.f0 = 9;
	std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	for (int other = 0; other < 2; other++)
	{
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_MOVE
		};
		op.u.move = txn_memtable_version_tests_moved(other);
		std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	}
	TXN_MEMTABLE_VERSION_TESTS_OP(txn_memtable_version_tests_put(TXN_MEMTABLE_OP_ADD, 2, 2, 44));
	for (int moved = 0; moved < 3; moved++)
	{
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_REMOVE
		};
		op.u.remove.f0.f0 = 5;
		op.u.remove.has_f1 = moved != 0;
		if (moved != 0)
			op.u.remove.f1 = txn_memtable_version_tests_moved(moved == 2);
		std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	}
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_LOCK
	};
	op.u.lock = TXN_MEMTABLE_LOCK_MODE_SHARE;
	std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	TXN_MEMTABLE_VERSION_TESTS_OP(txn_memtable_version_tests_put(TXN_MEMTABLE_OP_ADD_ROW, 2, 2, 44));
	{
		TxnMemtableOp put = txn_memtable_version_tests_put(TXN_MEMTABLE_OP_PUT, 2, 2, 44);

		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_UPDATE_ROW
		};
		op.u.update_row.f0.f0 = 6;
		op.u.update_row.f1 = put.u.put;
		std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	}
	for (int moved = 0; moved < 3; moved++)
	{
		op = (TxnMemtableOp)
		{
			.tag = TXN_MEMTABLE_OP_DELETE_ROW
		};
		op.u.delete_row.f0.f0 = 6;
		op.u.delete_row.has_f1 = moved != 0;
		if (moved != 0)
			op.u.delete_row.f1 = txn_memtable_version_tests_moved(moved == 2);
		std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
	}
	op = (TxnMemtableOp)
	{
		.tag = TXN_MEMTABLE_OP_LOCK_ROW
	};
	op.u.lock_row.f0.f0 = 6;
	op.u.lock_row.f1 = TXN_MEMTABLE_LOCK_MODE_KEY_SHARE;
	std_vec_push_Checked(&ops, &op, sizeof(op), _Alignof(TxnMemtableOp));
#undef TXN_MEMTABLE_VERSION_TESTS_OP
	return ops;
}

/* txn_memtable_version_tests_puts reports whether a put read in place holds what a put was written with. */
static bool
txn_memtable_version_tests_puts(const TxnMemtableVersionPutRef *read, const TxnMemtablePut *put)
{
	TxnMemtableVersionAddresses addresses = txn_memtable_version_PutRef_addresses(read);
	TxnMemtableVersionValues values = txn_memtable_version_PutRef_values(read);
	chunks_TupleU32Locator_t address;
	uint32		value;
	size_t		k = 0;

	if (read->value.len != put->value.len || memcmp(read->value.ptr, put->value.ptr, put->value.len) != 0 ||
		read->tuple.f0 != put->tuple.f0)
		return false;
	for (; txn_memtable_version_Addresses_Iterator_next(&addresses, &address); k++)
	{
		const chunks_TupleU32Locator_t *written = &STD_VEC_AT(&put->addresses, chunks_TupleU32Locator_t, k);

		if (k >= put->addresses.len || address.f0 != written->f0 ||
			!chunks_trailer_Locator_PartialEq_eq(&address.f1, &written->f1))
			return false;
	}
	if (k != put->addresses.len)
		return false;
	for (k = 0; txn_memtable_version_Values_Iterator_next(&values, &value); k++)
		if (k >= put->values.len || value != STD_VEC_AT(&put->values, uint32, k))
			return false;
	return k == put->values.len;
}

/* txn_memtable_version_tests_one reports whether a successor read in place is the one written. */
static bool
txn_memtable_version_tests_one(const TxnMemtableVersionSuccessorRef *read, const TxnMemtableSuccessor *written)
{
	if (written->tag == TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE)
		return read->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE;
	return read->tag == TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY && read->u.key.len == written->u.key.len &&
		memcmp(read->u.key.ptr, written->u.key.ptr, read->u.key.len) == 0;
}

/* txn_memtable_version_tests_moved_is reports whether an optional successor read in place is the one written. */
static bool
txn_memtable_version_tests_moved_is(bool has_read, const TxnMemtableVersionSuccessorRef *read, bool has_written,
									const TxnMemtableSuccessor *written)
{
	return has_read == has_written && (!has_read || txn_memtable_version_tests_one(read, written));
}

/* txn_memtable_version_tests_same reports whether a record's op reads back as the op it was written with. */
static bool
txn_memtable_version_tests_same(const TxnMemtableVersionOpRef *read, const TxnMemtableOp *op)
{
	switch (op->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_PUT && txn_memtable_version_tests_puts(&read->u.put,
																								   &op->u.put);
		case TXN_MEMTABLE_OP_ADD:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_ADD && txn_memtable_version_tests_puts(&read->u.add,
																								   &op->u.add);
		case TXN_MEMTABLE_OP_ADD_ROW:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW &&
				txn_memtable_version_tests_puts(&read->u.add_row, &op->u.add_row);
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW &&
				read->u.update_row.f0.f0 == op->u.update_row.f0.f0 &&
				txn_memtable_version_tests_puts(&read->u.update_row.f1, &op->u.update_row.f1);
		case TXN_MEMTABLE_OP_DELETE:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE;
		case TXN_MEMTABLE_OP_EMPTIED:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_EMPTIED && read->u.emptied.f0 == op->u.emptied.f0;
		case TXN_MEMTABLE_OP_MOVE:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_MOVE && txn_memtable_version_tests_one(&read->u.move,
																								   &op->u.move);
		case TXN_MEMTABLE_OP_REMOVE:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_REMOVE && read->u.remove.f0.f0 == op->u.remove.f0.f0 &&
				txn_memtable_version_tests_moved_is(read->u.remove.has_f1, &read->u.remove.f1, op->u.remove.has_f1,
													&op->u.remove.f1);
		case TXN_MEMTABLE_OP_DELETE_ROW:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW &&
				read->u.delete_row.f0.f0 == op->u.delete_row.f0.f0 &&
				txn_memtable_version_tests_moved_is(read->u.delete_row.has_f1, &read->u.delete_row.f1,
													op->u.delete_row.has_f1, &op->u.delete_row.f1);
		case TXN_MEMTABLE_OP_LOCK:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK && read->u.lock == op->u.lock;
		case TXN_MEMTABLE_OP_LOCK_ROW:
			return read->tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW &&
				read->u.lock_row.f0.f0 == op->u.lock_row.f0.f0 && read->u.lock_row.f1 == op->u.lock_row.f1;
		default:
			return false;
	}
}

/*
 * test_txn_memtable_version_tests_every_ops_record_reads_back_as_written:
 * every op's record reads back what it was written with, with and without a
 * top-level transaction's id and an LSN (at 0, 1, and the largest), with
 * each command it shows, a row, a value, addresses, and value ids, and its
 * length is what its header and its count of value ids say.
 */
void
test_txn_memtable_version_tests_every_ops_record_reads_back_as_written(void)
{
	StdVec		ops = txn_memtable_version_tests_ops();
	TxnMemtableVersionShown shown[] = {{.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
	{.tag = TXN_MEMTABLE_VERSION_SHOWN_COMBO, .u.combo = 0xfedcba98},
	{.tag = TXN_MEMTABLE_VERSION_SHOWN_REPLAYED},
	{.tag = TXN_MEMTABLE_VERSION_SHOWN_ENDING, .u.ending = 0x12345678}};
	bool		has_lsns[] = {false, true, true, true};
	uint64		lsns[] = {0, 0, 1, PG_UINT64_MAX};

	for (int top = 0; top < 2; top++)
		for (size_t l = 0; l < lengthof(lsns); l++)
			for (size_t s = 0; s < lengthof(shown); s++)
				for (size_t k = 0; k < ops.len; k++)
				{
					const TxnMemtableOp *op = &STD_VEC_AT(&ops, TxnMemtableOp, k);
					TxnMemtableVersionWriter writer = {.xid = UINT64CONST(0x100000007), .has_top = top == 1,
					.top = top == 1 ? UINT64CONST(0x100000002) : 0, .command = 3};
					StdVec		bytes;
					size_t		values = 0;
					TxnMemtableVersionRecord record;
					TxnMemtableVersionWriter read_writer;
					uint64		lsn;
					bool		has_lsn;
					TxnMemtableVersionShown read_shown;
					uint32		raw = 3;
					uint32		ended = 3;
					TxnMemtableVersionOpRef read_op;
					HostError	error;

					std_vec_new(&bytes, 1, 1);
					txn_memtable_version_encode(&writer, op, TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE, shown[s],
												has_lsns[l], lsns[l], &bytes);
					STD_ASSERT_MSG(__LINE__, 9, bytes.len == txn_memtable_version_encoded(&writer, op, shown[s],
																						  has_lsns[l], lsns[l]),
								   "the record's length is what encoded says");
					if ((((uint8 *) bytes.ptr)[28] & TXN_MEMTABLE_VERSION_VALUES) != 0)
					{
						size_t		at = txn_memtable_version_counted(((uint8 *) bytes.ptr)[28]);

						values = ((uint8 *) bytes.ptr)[at] | (size_t) ((uint8 *) bytes.ptr)[at + 1] << 8;
					}
					STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_length(bytes.ptr, values) == bytes.len,
								   "the header's length is the record's");
					record = txn_memtable_version_Record_of(bytes.ptr, bytes.len);
					read_writer = txn_memtable_version_Record_writer(&record);
					has_lsn = txn_memtable_version_Record_lsn(&record, &lsn);
					read_shown = txn_memtable_version_Record_shown(&record);
					STD_ASSERT_MSG(__LINE__, 9, read_writer.xid == writer.xid && read_writer.has_top == writer.has_top &&
								   read_writer.top == writer.top && read_writer.command == writer.command &&
								   txn_memtable_version_Record_lock(&record) == TXN_MEMTABLE_LOCK_MODE_NO_KEY_UPDATE &&
								   has_lsn == has_lsns[l] && (!has_lsn || lsn == lsns[l]) &&
								   read_shown.tag == shown[s].tag &&
								   (read_shown.tag != TXN_MEMTABLE_VERSION_SHOWN_COMBO ||
									read_shown.u.combo == shown[s].u.combo) &&
								   (read_shown.tag != TXN_MEMTABLE_VERSION_SHOWN_ENDING ||
									read_shown.u.ending == shown[s].u.ending),
								   "(record.writer(), record.lock(), record.lsn(), record.shown()) read back");
					if (shown[s].tag == TXN_MEMTABLE_VERSION_SHOWN_COMBO)
						raw = ended = shown[s].u.combo;
					else if (shown[s].tag == TXN_MEMTABLE_VERSION_SHOWN_REPLAYED)
						raw = ended = 0;
					else if (shown[s].tag == TXN_MEMTABLE_VERSION_SHOWN_ENDING)
						ended = shown[s].u.ending;
					STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Record_raw(&record) == raw &&
								   txn_memtable_version_Record_ended(&record) == ended,
								   "(record.raw(), record.ended()) == (raw, ended)");
					read_op = txn_memtable_version_Record_op(&record);
					STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_tests_same(&read_op, op), "an op read as another");
					txn_memtable_version_tests_ok(txn_memtable_version_stored(bytes.ptr, bytes.len, &record, &error),
												  &error, __LINE__);
					{
						StdBytes	stored = txn_memtable_version_Record_bytes(&record);

						STD_ASSERT_MSG(__LINE__, 9, stored.len == bytes.len &&
									   memcmp(stored.ptr, bytes.ptr, bytes.len) == 0,
									   "stored(&bytes).unwrap().bytes() == &bytes[..]");
					}
					std_vec_free_Checked(&bytes, 1, 1);
				}
	for (size_t k = 0; k < ops.len; k++)
		txn_memtable_Op_drop(&STD_VEC_AT(&ops, TxnMemtableOp, k));
	std_vec_free_Checked(&ops, sizeof(TxnMemtableOp), _Alignof(TxnMemtableOp));
}

/* txn_memtable_version_tests_whole returns a record encode writes of an op, by writer 7 with LSN 9. */
static StdVec
txn_memtable_version_tests_whole(TxnMemtableOp op)
{
	TxnMemtableVersionWriter writer = {.xid = 7};
	TxnMemtableVersionShown shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND};
	StdVec		bytes;

	std_vec_new(&bytes, 1, 1);
	txn_memtable_version_encode(&writer, &op, TXN_MEMTABLE_LOCK_MODE_UPDATE, shown, true, 9, &bytes);
	txn_memtable_Op_drop(&op);
	return bytes;
}

/* txn_memtable_version_tests_changed returns a copy of a record with one byte changed. */
static StdVec
txn_memtable_version_tests_changed(const StdVec *bytes, size_t at, uint8 to)
{
	StdVec		changed;

	std_slice_to_vec_Checked(&changed, bytes->ptr, bytes->len, 1, 1);
	((uint8 *) changed.ptr)[at] = to;
	return changed;
}

/* txn_memtable_version_tests_spliced inserts bytes into a record at a place. */
static void
txn_memtable_version_tests_spliced(StdVec *bytes, size_t at, const uint8 *inserted, size_t inserted_len)
{
	for (size_t k = 0; k < inserted_len; k++)
		std_vec_insert(bytes, at + k, &inserted[k], 1, 1, RUST_FILE, 0, 0);
}

/*
 * test_txn_memtable_version_tests_a_stored_record_is_refused_unless_encode_writes_it:
 * a stored record is one encode writes: a record cut short in its header or
 * after it, one with a byte past its end, a tag no op has, a row's op with
 * no row, a flag no record has, a lock mode past the last, VALUES with no
 * ids, REPLAYED with COMBO, and COMBO with ENDING are each refused as data
 * corruption.
 */
void
test_txn_memtable_version_tests_a_stored_record_is_refused_unless_encode_writes_it(void)
{
	static const uint8 count[] = {0, 0};
	static const uint8 combo[] = {1, 0, 0, 0};
	TxnMemtableOp locked = {.tag = TXN_MEMTABLE_OP_LOCK, .u.lock = TXN_MEMTABLE_LOCK_MODE_SHARE};
	StdVec		row = txn_memtable_version_tests_whole(txn_memtable_testing_put((const uint8 *) "a value past a row's bytes",
																				strlen("a value past a row's bytes")));
	StdVec		lock = txn_memtable_version_tests_whole(locked);
	uint8		flags = ((uint8 *) row.ptr)[28];
	StdVec		refused[13];
	TxnMemtableVersionRecord record;
	HostError	error;

	std_slice_to_vec_Checked(&refused[0], row.ptr, TXN_MEMTABLE_VERSION_HEADER - 1, 1, 1);
	std_slice_to_vec_Checked(&refused[1], row.ptr, row.len - 1, 1, 1);
	std_slice_to_vec_Checked(&refused[2], row.ptr, row.len, 1, 1);
	std_vec_u8_push_Checked(&refused[2], 0);
	refused[3] = txn_memtable_version_tests_changed(&row, 26, 13);
	refused[4] = txn_memtable_version_tests_changed(&row, 26, 14);
	refused[5] = txn_memtable_version_tests_changed(&row, 26, TXN_MEMTABLE_VERSION_LOCK_TAG + 4);
	refused[6] = txn_memtable_version_tests_changed(&lock, 26, TXN_MEMTABLE_VERSION_ROW_TAG + 1);
	refused[7] = txn_memtable_version_tests_changed(&lock, 26, TXN_MEMTABLE_VERSION_ROW_LOCK_TAG);
	refused[8] = txn_memtable_version_tests_changed(&row, 28, flags | 128);
	refused[9] = txn_memtable_version_tests_changed(&row, 27, 4);
	refused[10] = txn_memtable_version_tests_changed(&row, 28, flags | TXN_MEMTABLE_VERSION_VALUES);
	txn_memtable_version_tests_spliced(&refused[10], txn_memtable_version_counted(flags | TXN_MEMTABLE_VERSION_VALUES),
									   count, sizeof(count));
	refused[11] = txn_memtable_version_tests_changed(&row, 28, flags | TXN_MEMTABLE_VERSION_REPLAYED |
													 TXN_MEMTABLE_VERSION_COMBO);
	txn_memtable_version_tests_spliced(&refused[11], txn_memtable_version_counted(flags | TXN_MEMTABLE_VERSION_COMBO) - 4,
									   combo, sizeof(combo));
	refused[12] = txn_memtable_version_tests_changed(&row, 28, flags | TXN_MEMTABLE_VERSION_COMBO |
													 TXN_MEMTABLE_VERSION_ENDING);
	txn_memtable_version_tests_spliced(&refused[12], txn_memtable_version_counted(flags | TXN_MEMTABLE_VERSION_COMBO) - 4,
									   combo, sizeof(combo));
	txn_memtable_version_tests_ok(txn_memtable_version_stored(row.ptr, row.len, &record, &error), &error, __LINE__);
	for (size_t at = 0; at < lengthof(refused); at++)
	{
		bool		corrupt = false;

		if (!txn_memtable_version_stored(refused[at].ptr, refused[at].len, &record, &error))
		{
			corrupt = memcmp(host_Error_sqlstate(&error), "XX001", 5) == 0;
			host_Error_drop(&error);
		}
		STD_ASSERT_EQ(__LINE__, 13, corrupt, &at, std_usize_Debug_fmt, &at, std_usize_Debug_fmt);
		std_vec_free_Checked(&refused[at], 1, 1);
	}
	std_vec_free_Checked(&lock, 1, 1);
	std_vec_free_Checked(&row, 1, 1);
}

/*
 * test_txn_memtable_version_tests_a_link_holds_its_offset_and_older_handle_at_their_limits:
 * a link holds its record's offset and its key's older version's handle at
 * their limits.
 */
void
test_txn_memtable_version_tests_a_link_holds_its_offset_and_older_handle_at_their_limits(void)
{
	uint64		handle = (HOST_DELTA_PAGE_SLOTS - 1) << TXN_MEMTABLE_VERSION_WORD |
		((UINT64CONST(1) << TXN_MEMTABLE_VERSION_WORD) - 1);
	uint64		link = txn_memtable_version_linked((uint32) (TXN_MEMTABLE_PAGES_LARGEST - 1), handle);
	TxnMemtableVersionHandle named;
	bool		has = txn_memtable_version_Handle_named(link & ((UINT64CONST(1) << TXN_MEMTABLE_VERSION_OFFSET) - 1),
														&named);

	STD_ASSERT_MSG(__LINE__, 9, link >> TXN_MEMTABLE_VERSION_OFFSET == TXN_MEMTABLE_PAGES_LARGEST - 1 && has &&
				   named.f0 == handle, "(link >> OFFSET, Handle::named(...)) == (LARGEST - 1, Some(handle))");
}

/*
 * test_txn_memtable_version_tests_a_keys_versions_read_newest_first_with_their_deleters:
 * a key's versions read newest first, a new key's first version is its
 * node's head, each version shows the next newer one as its deleter, a
 * lock's with no command, and a hint is stored and loaded.
 */
void
test_txn_memtable_version_tests_a_keys_versions_read_newest_first_with_their_deleters(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys;
	StdVec		key = txn_memtable_version_tests_one_key();
	TxnMemtableOp lock_op = {.tag = TXN_MEMTABLE_OP_LOCK, .u.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE};
	uint64		first;
	uint64		lock;
	uint64		last;
	StdVec		versions;
	HostError	error;

	txn_memtable_version_tests_ok(txn_memtable_testing_keys(io, &held, &keys, &error), &error, __LINE__);
	first = txn_memtable_version_tests_pushed(io, keys, key.ptr, key.len,
											  txn_memtable_testing_put((const uint8 *) "a", 1),
											  TXN_MEMTABLE_TESTING_PAGE, __LINE__);
	versions = txn_memtable_version_tests_read(io, keys, key.ptr, key.len);
	STD_ASSERT_MSG(__LINE__, 9, versions.len == 1 &&
				   STD_VEC_AT(&versions, txn_memtable_version_tests_read_t, 0).handle == first,
				   "a new key's first version is its node's head");
	std_vec_free_Checked(&versions, sizeof(txn_memtable_version_tests_read_t),
						 _Alignof(txn_memtable_version_tests_read_t));
	lock = txn_memtable_version_tests_pushed(io, keys, key.ptr, key.len, lock_op, TXN_MEMTABLE_TESTING_PAGE, __LINE__);
	last = txn_memtable_version_tests_pushed(io, keys, key.ptr, key.len,
											 txn_memtable_testing_put((const uint8 *) "b", 1),
											 TXN_MEMTABLE_TESTING_PAGE, __LINE__);
	versions = txn_memtable_version_tests_read(io, keys, key.ptr, key.len);
	{
		const txn_memtable_version_tests_read_t *v = versions.ptr;

		STD_ASSERT_MSG(__LINE__, 9, versions.len == 3 && v[0].handle == last && v[1].handle == lock &&
					   v[2].handle == first, "the versions read newest first");
		STD_ASSERT_MSG(__LINE__, 9, v[0].deleter.xid == 7 && v[0].deleter.has_command && v[0].deleter.command == 0,
					   "versions[0].1 == Deleter { xid: 7, command: Some(0) }");
		STD_ASSERT_MSG(__LINE__, 9, v[1].deleter.xid == 7 && !v[1].deleter.has_command,
					   "versions[1].1 == Deleter { xid: 7, command: None }");
		for (size_t k = 0; k < versions.len; k++)
			STD_ASSERT_MSG(__LINE__, 9, v[k].hint == TXN_MEMTABLE_VERSION_HINT_UNKNOWN, "a version's hint is unknown");
	}
	std_vec_free_Checked(&versions, sizeof(txn_memtable_version_tests_read_t),
						 _Alignof(txn_memtable_version_tests_read_t));
	{
		HostDeltaHold hold;
		HostSharedGuard guard;
		TxnMemtableIndexNode node;
		TxnMemtableVersionVersion newest;

		txn_memtable_version_tests_opened(io, keys, key.ptr, key.len, &hold, &guard, &node, __LINE__);
		newest = txn_memtable_version_tests_newest(&guard, &hold, node, __LINE__);
		txn_memtable_version_Version_set_hint(&newest, TXN_MEMTABLE_VERSION_HINT_COMMITTED);
		txn_memtable_version_Version_drop(&newest);
		host_shared_Guard_drop(&guard);
		host_delta_Hold_drop(&hold);
	}
	versions = txn_memtable_version_tests_read(io, keys, key.ptr, key.len);
	{
		const txn_memtable_version_tests_read_t *v = versions.ptr;

		STD_ASSERT_MSG(__LINE__, 9, versions.len == 3 && v[0].hint == TXN_MEMTABLE_VERSION_HINT_COMMITTED &&
					   v[1].hint == TXN_MEMTABLE_VERSION_HINT_UNKNOWN && v[2].hint == TXN_MEMTABLE_VERSION_HINT_UNKNOWN,
					   "hints == [Committed, Unknown, Unknown]");
	}
	std_vec_free_Checked(&versions, sizeof(txn_memtable_version_tests_read_t),
						 _Alignof(txn_memtable_version_tests_read_t));
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_memtable_version_tests_a_version_on_a_fresh_page_is_named_by_its_index_in_the_list:
 * a version a push puts on a fresh page is named by that page's index in
 * the range's list, read from its handle alone, with no walk of the index.
 */
void
test_txn_memtable_version_tests_a_version_on_a_fresh_page_is_named_by_its_index_in_the_list(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys;
	HostDeltaPageSize small = {(uint32) (TXN_MEMTABLE_VERSION_HEADER + 40 + 40),
	(uint32) TXN_MEMTABLE_PAGES_FIRST + 2 + 2 + 8 + 2};
	uint8		a[40];
	uint8		b[40];
	uint64		first;
	uint64		second;
	HostError	error;

	memset(a, 'a', sizeof(a));
	memset(b, 'b', sizeof(b));
	txn_memtable_version_tests_ok(txn_memtable_testing_keys(io, &held, &keys, &error), &error, __LINE__);
	first = txn_memtable_version_tests_pushed(io, keys, a, sizeof(a), txn_memtable_testing_put(NULL, 0), small,
											  __LINE__);
	second = txn_memtable_version_tests_pushed(io, keys, b, sizeof(b), txn_memtable_testing_put(NULL, 0), small,
											   __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, first >> TXN_MEMTABLE_VERSION_WORD == 0 && second >> TXN_MEMTABLE_VERSION_WORD == 1,
				   "(first.0 >> WORD, second.0 >> WORD) == (0, 1)");
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/* txn_memtable_version_tests_hinted is the race test's seam: a reader's hint store landing inside updated's read-modify-write. */
static void
txn_memtable_version_tests_hinted(void *captures)
{
	txn_memtable_version_Version_set_hint(captures, TXN_MEMTABLE_VERSION_HINT_COMMITTED);
}

/*
 * test_txn_memtable_version_tests_a_hint_and_a_token_clear_racing_both_survive:
 * the hint word's read-modify-write (CommPF 7, 17:02), a reader's hint store
 * landing between a writer's read of the word and its compare-exchange makes
 * the exchange retry, so the hint and the token's clear both survive; and a
 * kill keeps the hint and clears the token.
 */
void
test_txn_memtable_version_tests_a_hint_and_a_token_clear_racing_both_survive(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys;
	StdVec		key = txn_memtable_version_tests_one_key();
	HostDeltaHold hold;
	HostSharedGuard guard;
	TxnMemtableIndexNode node;
	TxnMemtableVersionVersion version;
	uint32		token;
	HostError	error;

	txn_memtable_version_tests_ok(txn_memtable_testing_keys(io, &held, &keys, &error), &error, __LINE__);
	(void) txn_memtable_version_tests_pushed(io, keys, key.ptr, key.len,
											 txn_memtable_testing_put((const uint8 *) "a", 1),
											 TXN_MEMTABLE_TESTING_PAGE, __LINE__);
	txn_memtable_version_tests_opened(io, keys, key.ptr, key.len, &hold, &guard, &node, __LINE__);
	version = txn_memtable_version_tests_newest(&guard, &hold, node, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, version.has_hint, "version.hint.as_ref().unwrap()");
	host_delta_Word_store(&version.hint, UINT64CONST(7) << TXN_MEMTABLE_VERSION_TOKEN);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_token(&version, &token) && token == 7,
				   "version.token() == Some(7)");
	txn_memtable_version_tests_raced = txn_memtable_version_tests_hinted;
	txn_memtable_version_tests_raced_captures = &version;
	txn_memtable_version_updated(&version.hint, (UINT64CONST(1) << TXN_MEMTABLE_VERSION_TOKEN) - 1, 0);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_tests_raced == NULL, "the seam ran inside the update");
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_hint(&version) == TXN_MEMTABLE_VERSION_HINT_COMMITTED &&
				   !txn_memtable_version_Version_token(&version, &token) && !txn_memtable_version_Version_killed(&version),
				   "(version.hint(), version.token(), version.killed()) == (Hint::Committed, None, false)");
	host_delta_Word_store(&version.hint, UINT64CONST(9) << TXN_MEMTABLE_VERSION_TOKEN | TXN_MEMTABLE_VERSION_HINT_ABORTED);
	txn_memtable_version_Version_kill(&version);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_hint(&version) == TXN_MEMTABLE_VERSION_HINT_ABORTED &&
				   !txn_memtable_version_Version_token(&version, &token) && txn_memtable_version_Version_killed(&version),
				   "(version.hint(), version.token(), version.killed()) == (Hint::Aborted, None, true)");
	txn_memtable_version_Version_drop(&version);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

/*
 * test_txn_memtable_version_tests_a_shown_combo_is_the_rows_raw_command_and_no_token:
 * a combo command id show stores is the raw command the version's row
 * shows, in place of its record's, keeps the hint a reader stored, and reads
 * as no token; a version with none shows its record's.
 */
void
test_txn_memtable_version_tests_a_shown_combo_is_the_rows_raw_command_and_no_token(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys;
	StdVec		key = txn_memtable_version_tests_one_key();
	HostDeltaHold hold;
	HostSharedGuard guard;
	TxnMemtableIndexNode node;
	TxnMemtableVersionVersion version;
	TxnMemtableVersionRecord record;
	uint32		token;
	HostError	error;

	txn_memtable_version_tests_ok(txn_memtable_testing_keys(io, &held, &keys, &error), &error, __LINE__);
	(void) txn_memtable_version_tests_pushed(io, keys, key.ptr, key.len,
											 txn_memtable_testing_put((const uint8 *) "a", 1),
											 TXN_MEMTABLE_TESTING_PAGE, __LINE__);
	txn_memtable_version_tests_opened(io, keys, key.ptr, key.len, &hold, &guard, &node, __LINE__);
	version = txn_memtable_version_tests_newest(&guard, &hold, node, __LINE__);
	record = txn_memtable_version_Version_record(&version);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_raw(&version) == txn_memtable_version_Record_raw(&record),
				   "version.raw() == version.record().raw()");
	txn_memtable_version_Version_set_hint(&version, TXN_MEMTABLE_VERSION_HINT_COMMITTED);
	txn_memtable_version_tests_ok(txn_memtable_version_show(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &hold,
															version.handle, 0xfedcba98, &error), &error, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_raw(&version) == 0xfedcba98 &&
				   !txn_memtable_version_Version_token(&version, &token) &&
				   txn_memtable_version_Version_hint(&version) == TXN_MEMTABLE_VERSION_HINT_COMMITTED,
				   "(version.raw(), version.token(), version.hint()) == (0xfedc_ba98, None, Hint::Committed)");
	txn_memtable_version_tests_ok(txn_memtable_version_show(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &hold,
															version.handle, 1, &error), &error, __LINE__);
	STD_ASSERT_MSG(__LINE__, 9, txn_memtable_version_Version_raw(&version) == 1, "version.raw() == 1");
	txn_memtable_version_Version_drop(&version);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&key, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
	simc_Process_drop(&process);
}

#endif							/* DOLTGRES_TEST_TXN */
