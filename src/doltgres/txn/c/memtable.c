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
 * The C port of txn's memtable.rs: what a version does to its key, the
 * operations that the delta's versions and the WAL's row changes share, and
 * their derived drops, Debug, and PartialEq.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/trailer.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable.h"

/* txn_memtable_Put_drop is Put's drop glue: its value, its addresses, and its value ids. */
void
txn_memtable_Put_drop(TxnMemtablePut *self)
{
	std_vec_free_Checked(&self->value, 1, 1);
	std_vec_free_Checked(&self->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_free_Checked(&self->values, sizeof(uint32), _Alignof(uint32));
}

/* txn_memtable_addresses_Debug_fmt is the Debug of a Vec<(u32, Locator)>, a list of tuples. */
static bool
txn_memtable_addresses_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *addresses = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = addresses->len; k < k_len; k++)
	{
		const chunks_TupleU32Locator_t *address = &STD_VEC_AT(addresses, chunks_TupleU32Locator_t, k);
		StdTupleRef tuple = {2, {{&address->f0, std_u32_Debug_fmt}, {&address->f1, chunks_trailer_Locator_Debug_fmt}}};

		std_DebugList_entry(&list, &tuple, std_tuple_Debug_fmt);
	}
	return std_DebugList_finish(&list);
}

/* txn_memtable_values_Debug_fmt is the Debug of a Vec<u32>, a list. */
static bool
txn_memtable_values_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *values = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = values->len; k < k_len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(values, uint32, k), std_u32_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_bytes_Debug_fmt is the Debug of a Vec<u8>, a list of its bytes. */
static bool
txn_memtable_bytes_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *vec = value;
	StdBytes	bytes = {vec->ptr, vec->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* txn_memtable_Put_Debug_fmt is Put's derived Debug. */
bool
txn_memtable_Put_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtablePut *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Put", strlen("Put"));

	std_DebugStruct_field(&s, "value", strlen("value"), &self->value, txn_memtable_bytes_Debug_fmt);
	std_DebugStruct_field(&s, "addresses", strlen("addresses"), &self->addresses, txn_memtable_addresses_Debug_fmt);
	std_DebugStruct_field(&s, "values", strlen("values"), &self->values, txn_memtable_values_Debug_fmt);
	std_DebugStruct_field(&s, "tuple", strlen("tuple"), &self->tuple, host_Row_Debug_fmt);
	std_DebugStruct_field(&s, "heap_bytes", strlen("heap_bytes"), &self->heap_bytes, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "has_carried", strlen("has_carried"), &self->has_carried, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "carried", strlen("carried"), &self->carried, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* txn_memtable_vec_eq reports whether two vectors of plain elements hold the same bytes. */
static bool
txn_memtable_vec_eq(const StdVec *a, const StdVec *b, size_t size)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->ptr, std_wrapping_mul_usize(a->len, size)) == 0);
}

/* txn_memtable_Put_PartialEq_eq is Put's derived PartialEq. */
bool
txn_memtable_Put_PartialEq_eq(const TxnMemtablePut *self, const TxnMemtablePut *other)
{
	if (!txn_memtable_vec_eq(&self->value, &other->value, 1) || self->addresses.len != other->addresses.len)
		return false;
	for (size_t k = 0, k_len = self->addresses.len; k < k_len; k++)
	{
		const chunks_TupleU32Locator_t *a = &STD_VEC_AT(&self->addresses, chunks_TupleU32Locator_t, k);
		const chunks_TupleU32Locator_t *b = &STD_VEC_AT(&other->addresses, chunks_TupleU32Locator_t, k);

		if (a->f0 != b->f0 || !chunks_trailer_Locator_PartialEq_eq(&a->f1, &b->f1))
			return false;
	}
	return txn_memtable_vec_eq(&self->values, &other->values, sizeof(uint32)) &&
		host_Row_PartialEq_eq(&self->tuple, &other->tuple) && self->heap_bytes == other->heap_bytes &&
		self->has_carried == other->has_carried && self->carried == other->carried;
}

/* txn_memtable_LockMode_Debug_fmt is LockMode's derived Debug: its variant's name. */
bool
txn_memtable_LockMode_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const names[] = {"KeyShare", "Share", "NoKeyUpdate", "Update"};
	const TxnMemtableLockMode *self = value;
	const char *name = names[*self];

	return std_fmt_write_str(f, name, strlen(name));
}

/* txn_memtable_Successor_drop is Successor's drop glue: another key's bytes. */
void
txn_memtable_Successor_drop(TxnMemtableSuccessor *self)
{
	if (self->tag == TXN_MEMTABLE_SUCCESSOR_KEY)
		std_vec_free_Checked(&self->u.key, 1, 1);
}

/* txn_memtable_Successor_Debug_fmt is Successor's derived Debug. */
bool
txn_memtable_Successor_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtableSuccessor *self = value;
	StdDebugTuple t;

	if (self->tag == TXN_MEMTABLE_SUCCESSOR_OTHER_TABLE)
		return std_fmt_write_str(f, "OtherTable", strlen("OtherTable"));
	t = std_fmt_debug_tuple(f, "Key", strlen("Key"));
	std_DebugTuple_field(&t, &self->u.key, txn_memtable_bytes_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_memtable_Successor_PartialEq_eq is Successor's derived PartialEq. */
bool
txn_memtable_Successor_PartialEq_eq(const TxnMemtableSuccessor *self, const TxnMemtableSuccessor *other)
{
	return self->tag == other->tag &&
		(self->tag != TXN_MEMTABLE_SUCCESSOR_KEY || txn_memtable_vec_eq(&self->u.key, &other->u.key, 1));
}

/* txn_memtable_successor_eq is Option<Successor>'s PartialEq. */
static bool
txn_memtable_successor_eq(bool has, const TxnMemtableSuccessor *self, bool other_has,
						  const TxnMemtableSuccessor *other)
{
	return has == other_has && (!has || txn_memtable_Successor_PartialEq_eq(self, other));
}

/* txn_memtable_Op_drop is Op's drop glue: what its variant holds. */
void
txn_memtable_Op_drop(TxnMemtableOp *self)
{
	switch (self->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			txn_memtable_Put_drop(&self->u.put);
			break;
		case TXN_MEMTABLE_OP_NAMED:
			std_vec_free_Checked(&self->u.named.f1, sizeof(uint32), _Alignof(uint32));
			break;
		case TXN_MEMTABLE_OP_MOVE:
			txn_memtable_Successor_drop(&self->u.move);
			break;
		case TXN_MEMTABLE_OP_ADD:
			txn_memtable_Put_drop(&self->u.add);
			break;
		case TXN_MEMTABLE_OP_REMOVE:
			if (self->u.remove.has_f1)
				txn_memtable_Successor_drop(&self->u.remove.f1);
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			txn_memtable_Put_drop(&self->u.add_row);
			break;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			txn_memtable_Put_drop(&self->u.update_row.f1);
			break;
		case TXN_MEMTABLE_OP_DELETE_ROW:
			if (self->u.delete_row.has_f1)
				txn_memtable_Successor_drop(&self->u.delete_row.f1);
			break;
		case TXN_MEMTABLE_OP_DELETE:
		case TXN_MEMTABLE_OP_EMPTIED:
		case TXN_MEMTABLE_OP_LOCK:
		case TXN_MEMTABLE_OP_LOCK_ROW:
			break;
	}
}

/* txn_memtable_Op_PartialEq_eq is Op's derived PartialEq. */
bool
txn_memtable_Op_PartialEq_eq(const TxnMemtableOp *self, const TxnMemtableOp *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			return txn_memtable_Put_PartialEq_eq(&self->u.put, &other->u.put);
		case TXN_MEMTABLE_OP_DELETE:
			return true;
		case TXN_MEMTABLE_OP_EMPTIED:
			return host_Row_PartialEq_eq(&self->u.emptied, &other->u.emptied);
		case TXN_MEMTABLE_OP_NAMED:
			return host_Row_PartialEq_eq(&self->u.named.f0, &other->u.named.f0) &&
				txn_memtable_vec_eq(&self->u.named.f1, &other->u.named.f1, sizeof(uint32));
		case TXN_MEMTABLE_OP_MOVE:
			return txn_memtable_Successor_PartialEq_eq(&self->u.move, &other->u.move);
		case TXN_MEMTABLE_OP_ADD:
			return txn_memtable_Put_PartialEq_eq(&self->u.add, &other->u.add);
		case TXN_MEMTABLE_OP_REMOVE:
			return host_Row_PartialEq_eq(&self->u.remove.f0, &other->u.remove.f0) &&
				txn_memtable_successor_eq(self->u.remove.has_f1, &self->u.remove.f1, other->u.remove.has_f1,
										  &other->u.remove.f1);
		case TXN_MEMTABLE_OP_LOCK:
			return self->u.lock == other->u.lock;
		case TXN_MEMTABLE_OP_ADD_ROW:
			return txn_memtable_Put_PartialEq_eq(&self->u.add_row, &other->u.add_row);
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			return host_Row_PartialEq_eq(&self->u.update_row.f0, &other->u.update_row.f0) &&
				txn_memtable_Put_PartialEq_eq(&self->u.update_row.f1, &other->u.update_row.f1);
		case TXN_MEMTABLE_OP_DELETE_ROW:
			return host_Row_PartialEq_eq(&self->u.delete_row.f0, &other->u.delete_row.f0) &&
				txn_memtable_successor_eq(self->u.delete_row.has_f1, &self->u.delete_row.f1,
										  other->u.delete_row.has_f1, &other->u.delete_row.f1);
		case TXN_MEMTABLE_OP_LOCK_ROW:
			return host_Row_PartialEq_eq(&self->u.lock_row.f0, &other->u.lock_row.f0) &&
				self->u.lock_row.f1 == other->u.lock_row.f1;
	}
	return false;
}

/* txn_memtable_Op_Debug_fmt is Op's derived Debug: its variant's name and fields. */
bool
txn_memtable_Op_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const names[] = {"Put", "Delete", "Emptied", "Named", "Move", "Add", "Remove", "Lock",
	"AddRow", "UpdateRow", "DeleteRow", "LockRow"};
	const TxnMemtableOp *self = value;
	const char *name = names[self->tag];
	StdDebugTuple t;
	StdOptionRef successor;

	if (self->tag == TXN_MEMTABLE_OP_DELETE)
		return std_fmt_write_str(f, name, strlen(name));
	t = std_fmt_debug_tuple(f, name, strlen(name));
	switch (self->tag)
	{
		case TXN_MEMTABLE_OP_PUT:
			std_DebugTuple_field(&t, &self->u.put, txn_memtable_Put_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_EMPTIED:
			std_DebugTuple_field(&t, &self->u.emptied, host_Row_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_NAMED:
			std_DebugTuple_field(&t, &self->u.named.f0, host_Row_Debug_fmt);
			std_DebugTuple_field(&t, &self->u.named.f1, txn_memtable_values_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_MOVE:
			std_DebugTuple_field(&t, &self->u.move, txn_memtable_Successor_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_ADD:
			std_DebugTuple_field(&t, &self->u.add, txn_memtable_Put_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_REMOVE:
			successor.has = self->u.remove.has_f1;
			successor.some.value = &self->u.remove.f1;
			successor.some.fmt = txn_memtable_Successor_Debug_fmt;
			std_DebugTuple_field(&t, &self->u.remove.f0, host_Row_Debug_fmt);
			std_DebugTuple_field(&t, &successor, std_option_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_LOCK:
			std_DebugTuple_field(&t, &self->u.lock, txn_memtable_LockMode_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_ADD_ROW:
			std_DebugTuple_field(&t, &self->u.add_row, txn_memtable_Put_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_UPDATE_ROW:
			std_DebugTuple_field(&t, &self->u.update_row.f0, host_Row_Debug_fmt);
			std_DebugTuple_field(&t, &self->u.update_row.f1, txn_memtable_Put_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_DELETE_ROW:
			successor.has = self->u.delete_row.has_f1;
			successor.some.value = &self->u.delete_row.f1;
			successor.some.fmt = txn_memtable_Successor_Debug_fmt;
			std_DebugTuple_field(&t, &self->u.delete_row.f0, host_Row_Debug_fmt);
			std_DebugTuple_field(&t, &successor, std_option_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_LOCK_ROW:
			std_DebugTuple_field(&t, &self->u.lock_row.f0, host_Row_Debug_fmt);
			std_DebugTuple_field(&t, &self->u.lock_row.f1, txn_memtable_LockMode_Debug_fmt);
			break;
		case TXN_MEMTABLE_OP_DELETE:
			break;
	}
	return std_DebugTuple_finish(&t);
}
