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
 * Index keys of Postgres' types (access' key.rs, the storage design's
 * II.6): each type's encoding as a key column, whose bytes compare as
 * Postgres' btree compares the values, and the decoding of the types whose
 * keys are exact. A value's stored bytes are Postgres' own form of it,
 * without a varlena's header, read here field by field.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "doltgres/access/key.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "held.h"
#ifdef DOLTGRES_TEST_ACCESS
#include "doltgres/std/test.h"
#endif
#ifndef FRONTEND
#include "doltgres/access/heaptuple.h"
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/lib.h"
#include "doltgres/encoding/tuple.h"
#include "doltgres/pg/word.h"
#endif

#define RUST_FILE "access" STD_SEP "src" STD_SEP "key.rs"

/* The element layouts of key's vectors. */
#define ACCESS_KEY_BYTES 1, 1
#define ACCESS_KEY_KEY_TYPE sizeof(AccessKeyKeyType), _Alignof(AccessKeyKeyType)
#define ACCESS_KEY_ATTRIBUTE sizeof(AccessKeyAttribute), _Alignof(AccessKeyAttribute)
#define ACCESS_KEY_LABEL sizeof(access_TupleU32F32_t), _Alignof(access_TupleU32F32_t)

/* access_key_box_drop frees a boxed key type, its own held key types first. */
static void
access_key_box_drop(AccessKeyKeyType *boxed)
{
	access_key_KeyType_drop(boxed);
	std_free(boxed, ACCESS_KEY_KEY_TYPE);
}

/* access_key_attribute_drop is Attribute's drop glue as a vector's element: its key type's. */
static void
access_key_attribute_drop(void *attribute)
{
	access_key_KeyType_drop(&((AccessKeyAttribute *) attribute)->key_type);
}

/* access_key_Inner_drop frees what an inner holds: a boxed key type, a row's attributes, or an enum's labels. */
void
access_key_Inner_drop(AccessKeyInner *self)
{
	switch (self->tag)
	{
		case ACCESS_KEY_INNER_NONE:
		case ACCESS_KEY_INNER_COMPARED:
			break;
		case ACCESS_KEY_INNER_ARRAY:
			access_key_box_drop(self->u.array.key_type);
			break;
		case ACCESS_KEY_INNER_RANGE:
			access_key_box_drop(self->u.range.key_type);
			break;
		case ACCESS_KEY_INNER_MULTIRANGE:
			access_key_box_drop(self->u.multirange);
			break;
		case ACCESS_KEY_INNER_ROW:
			std_vec_drop_Checked(&self->u.row, ACCESS_KEY_ATTRIBUTE, access_key_attribute_drop);
			break;
		case ACCESS_KEY_INNER_ENUM:
			std_vec_free_Checked(&self->u.labels, ACCESS_KEY_LABEL);
			break;
	}
}

/* access_key_KeyType_drop frees what a key type holds. */
void
access_key_KeyType_drop(AccessKeyKeyType *self)
{
	access_key_Inner_drop(&self->inner);
}

/* access_TupleUsizeOrderKeyType_drop is (usize, Order, KeyType)'s drop glue, an element of a key's columns. */
void
access_TupleUsizeOrderKeyType_drop(void *keyed)
{
	access_key_KeyType_drop(&((access_TupleUsizeOrderKeyType_t *) keyed)->f2);
}

/*
 * access_key_box_try_clone makes a new box of a clone of a boxed key type,
 * the box made before its clone, as Box's Clone makes it, answering false
 * with nothing made when an allocation fails.
 */
static bool
access_key_box_try_clone(const AccessKeyKeyType *boxed, AccessKeyKeyType **result)
{
	AccessKeyKeyType *made = std_global_alloc(ACCESS_KEY_KEY_TYPE);

	if (made == NULL)
		return false;
	if (!access_key_KeyType_try_clone(boxed, made))
	{
		std_free(made, ACCESS_KEY_KEY_TYPE);
		return false;
	}
	*result = made;
	return true;
}

/* access_key_Attribute_try_clone copies an attribute, its key type with it, answering false with nothing made when an allocation fails. */
static bool
access_key_Attribute_try_clone(const AccessKeyAttribute *self, AccessKeyAttribute *result)
{
	if (!access_key_KeyType_try_clone(&self->key_type, &result->key_type))
		return false;
	result->len = self->len;
	result->by_value = self->by_value;
	result->align = self->align;
	result->dropped = self->dropped;
	return true;
}

/* access_key_Attribute_Clone_clone copies an attribute, its key type with it. */
void
access_key_Attribute_Clone_clone(const AccessKeyAttribute *self, AccessKeyAttribute *result)
{
	if (!access_key_Attribute_try_clone(self, result))
		std_handle_alloc_error(ACCESS_KEY_KEY_TYPE);
}

/* access_key_row_try_clone copies a row's attributes, answering false with nothing made when an allocation fails. */
static bool
access_key_row_try_clone(const StdVec *row, StdVec *result)
{
	std_vec_new(result, ACCESS_KEY_ATTRIBUTE);
	if (!std_vec_try_reserve(result, row->len, ACCESS_KEY_ATTRIBUTE))
		return false;
	for (size_t i = 0; i < row->len; i++)
	{
		AccessKeyAttribute copy;

		if (!access_key_Attribute_try_clone(&STD_VEC_AT(row, AccessKeyAttribute, i), &copy))
		{
			std_vec_drop_Checked(result, ACCESS_KEY_ATTRIBUTE, access_key_attribute_drop);
			return false;
		}
		std_vec_push_Checked(result, &copy, ACCESS_KEY_ATTRIBUTE);
	}
	return true;
}

/*
 * access_key_Inner_try_clone copies an inner, what it holds with it,
 * answering false with nothing made when an allocation fails, so a caller
 * under a lock clones with nothing that unwinds (bugs row 517).
 */
static bool
access_key_Inner_try_clone(const AccessKeyInner *self, AccessKeyInner *result)
{
	*result = *self;
	switch (self->tag)
	{
		case ACCESS_KEY_INNER_NONE:
		case ACCESS_KEY_INNER_COMPARED:
			break;
		case ACCESS_KEY_INNER_ARRAY:
			return access_key_box_try_clone(self->u.array.key_type, &result->u.array.key_type);
		case ACCESS_KEY_INNER_RANGE:
			return access_key_box_try_clone(self->u.range.key_type, &result->u.range.key_type);
		case ACCESS_KEY_INNER_MULTIRANGE:
			return access_key_box_try_clone(self->u.multirange, &result->u.multirange);
		case ACCESS_KEY_INNER_ROW:
			return access_key_row_try_clone(&self->u.row, &result->u.row);
		case ACCESS_KEY_INNER_ENUM:
			std_vec_new(&result->u.labels, ACCESS_KEY_LABEL);
			if (!std_vec_try_reserve(&result->u.labels, self->u.labels.len, ACCESS_KEY_LABEL))
				return false;
			std_vec_extend_from_slice_Checked(&result->u.labels, self->u.labels.ptr, self->u.labels.len,
											  ACCESS_KEY_LABEL);
			break;
	}
	return true;
}

/* access_key_Inner_Clone_clone copies an inner, what it holds with it. */
void
access_key_Inner_Clone_clone(const AccessKeyInner *self, AccessKeyInner *result)
{
	if (!access_key_Inner_try_clone(self, result))
		std_handle_alloc_error(ACCESS_KEY_KEY_TYPE);
}

/*
 * access_key_KeyType_try_clone copies a key type, what it holds with it,
 * answering false with nothing made when an allocation fails (bugs row 517).
 */
bool
access_key_KeyType_try_clone(const AccessKeyKeyType *self, AccessKeyKeyType *result)
{
	result->typid = self->typid;
	result->collation = self->collation;
	return access_key_Inner_try_clone(&self->inner, &result->inner);
}

/* access_key_KeyType_Clone_clone copies a key type, what it holds with it. */
void
access_key_KeyType_Clone_clone(const AccessKeyKeyType *self, AccessKeyKeyType *result)
{
	if (!access_key_KeyType_try_clone(self, result))
		std_handle_alloc_error(ACCESS_KEY_KEY_TYPE);
}

/* access_key_Collation_PartialEq_eq reports whether two collations are the same variant with the same collation. */
bool
access_key_Collation_PartialEq_eq(const AccessKeyCollation *self, const AccessKeyCollation *other)
{
	if (self->tag != other->tag)
		return false;
	return self->tag == ACCESS_KEY_COLLATION_BYTES ||
		(self->id == other->id && self->deterministic == other->deterministic);
}

/* access_key_KeyType_of returns the key type of a type that holds no values of other key types, text in byte order. */
AccessKeyKeyType
access_key_KeyType_of(uint32 typid)
{
	AccessKeyKeyType key_type = {.typid = typid};

	return key_type;
}

/* access_key_le32 appends a u32's little-endian bytes. */
static void
access_key_le32(StdVec *out, uint32 value)
{
	uint8		le[4] = {(uint8) value, (uint8) (value >> 8), (uint8) (value >> 16), (uint8) (value >> 24)};

	std_vec_extend_from_slice_Checked(out, le, sizeof(le), ACCESS_KEY_BYTES);
}

/* access_key_push appends a byte. */
static void
access_key_push(StdVec *out, uint8 byte)
{
	std_vec_push_Checked(out, &byte, ACCESS_KEY_BYTES);
}

/* access_key_KeyType_write_closure_0 is write's stored: a held type's stored length, by-value flag, and alignment. */
static void
access_key_KeyType_write_closure_0(StdVec *out, int16 len, bool by_value, uint8 align)
{
	uint8		bytes[4] = {(uint8) len, (uint8) ((uint16) len >> 8), (uint8) by_value, align};

	std_vec_extend_from_slice_Checked(out, bytes, 2, ACCESS_KEY_BYTES);
	std_vec_extend_from_slice_Checked(out, bytes + 2, 2, ACCESS_KEY_BYTES);
}

/*
 * access_key_KeyType_write appends the key type as bytes that live only in
 * memory: its type, its collation, and what it holds, each held key type in
 * turn.
 */
void
access_key_KeyType_write(const AccessKeyKeyType *self, StdVec *out)
{
	access_key_le32(out, self->typid);
	if (self->collation.tag == ACCESS_KEY_COLLATION_BYTES)
		access_key_push(out, 0);
	else
	{
		access_key_push(out, STD_ADD(u8, 1, (uint8) !self->collation.deterministic, 124, 26));
		access_key_le32(out, self->collation.id);
	}
	switch (self->inner.tag)
	{
		case ACCESS_KEY_INNER_NONE:
			access_key_push(out, 0);
			break;
		case ACCESS_KEY_INNER_ARRAY:
			access_key_push(out, 1);
			access_key_KeyType_write_closure_0(out, self->inner.u.array.len, self->inner.u.array.by_value,
											   self->inner.u.array.align);
			access_key_KeyType_write(self->inner.u.array.key_type, out);
			break;
		case ACCESS_KEY_INNER_RANGE:
			access_key_push(out, 2);
			access_key_KeyType_write_closure_0(out, self->inner.u.range.len, self->inner.u.range.by_value,
											   self->inner.u.range.align);
			access_key_KeyType_write(self->inner.u.range.key_type, out);
			break;
		case ACCESS_KEY_INNER_MULTIRANGE:
			access_key_push(out, 3);
			access_key_KeyType_write(self->inner.u.multirange, out);
			break;
		case ACCESS_KEY_INNER_ENUM:
			access_key_push(out, 5);
			access_key_le32(out, (uint32) self->inner.u.labels.len);
			for (size_t i = 0; i < self->inner.u.labels.len; i++)
			{
				const access_TupleU32F32_t *label = &STD_VEC_AT(&self->inner.u.labels, access_TupleU32F32_t, i);
				uint32		order;

				memcpy(&order, &label->f1, sizeof(order));
				access_key_le32(out, label->f0);
				access_key_le32(out, order);
			}
			break;
		case ACCESS_KEY_INNER_COMPARED:
			{
				uint8		bytes[2] = {6, (uint8) self->inner.u.by_value};

				std_vec_extend_from_slice_Checked(out, bytes, sizeof(bytes), ACCESS_KEY_BYTES);
			}
			break;
		case ACCESS_KEY_INNER_ROW:
			access_key_push(out, 4);
			access_key_le32(out, (uint32) self->inner.u.row.len);
			for (size_t i = 0; i < self->inner.u.row.len; i++)
			{
				const AccessKeyAttribute *attribute = &STD_VEC_AT(&self->inner.u.row, AccessKeyAttribute, i);

				access_key_KeyType_write_closure_0(out, attribute->len, attribute->by_value, (uint8) attribute->align);
				access_key_push(out, (uint8) attribute->dropped);
				access_key_KeyType_write(&attribute->key_type, out);
			}
			break;
	}
}

/* access_key_take takes the first n bytes of some bytes, or answers false when they are fewer. */
static bool
access_key_take(StdBytes *rest, size_t n, const uint8 **taken)
{
	if (rest->len < n)
		return false;
	*taken = rest->ptr;
	rest->ptr += n;
	rest->len -= n;
	return true;
}

/* access_key_u32_le returns the u32 of four little-endian bytes. */
static uint32
access_key_u32_le(const uint8 *bytes)
{
	return (uint32) bytes[0] | (uint32) bytes[1] << 8 | (uint32) bytes[2] << 16 | (uint32) bytes[3] << 24;
}

/*
 * access_key_read_storage answers a held type's stored length, whether it
 * is passed by value, and its alignment, as write writes them at the start
 * of some bytes, moving past them, or false for bytes too short.
 */
static bool
access_key_read_storage(StdBytes *rest, int16 *len, bool *by_value, uint8 *align)
{
	const uint8 *taken;

	if (!access_key_take(rest, 4, &taken))
		return false;
	*len = (int16) ((uint16) taken[0] | (uint16) taken[1] << 8);
	*by_value = taken[2] != 0;
	*align = taken[3];
	return true;
}

/*
 * access_key_read_frame_t is a key type's reading's state across its pad:
 * the box of a held key type, the attributes or labels read so far, each
 * while live, and what it reads.
 */
typedef struct access_key_read_frame_t
{
	StdBytes	rest;
	AccessKeyKeyType *held;
	bool		held_live;
	StdVec		read;
	bool		read_live;
	AccessKeyKeyType *result_0;
	StdBytes   *result_1;
	bool		ok;
} access_key_read_frame_t;

/* access_key_KeyType_read_pad frees a held key type's box and the attributes or labels read so far. */
static void
access_key_KeyType_read_pad(void *frame_)
{
	access_key_read_frame_t *frame = frame_;

	if (frame->held_live)
		std_free(frame->held, ACCESS_KEY_KEY_TYPE);
	if (frame->read_live && frame->result_0->inner.tag == ACCESS_KEY_INNER_ROW)
		std_vec_drop_Checked(&frame->read, ACCESS_KEY_ATTRIBUTE, access_key_attribute_drop);
	else if (frame->read_live)
		std_vec_free_Checked(&frame->read, ACCESS_KEY_LABEL);
	frame->held_live = frame->read_live = false;
}

/* access_key_KeyType_read_body reads a key type's type, collation, and inner. */
static void
access_key_KeyType_read_body(void *frame_)
{
	access_key_read_frame_t *frame = frame_;
	AccessKeyKeyType *result = frame->result_0;
	const uint8 *taken;
	uint8		kind;
	uint32		count;

	if (!access_key_take(&frame->rest, 4, &taken))
		return;
	result->typid = access_key_u32_le(taken);
	if (!access_key_take(&frame->rest, 1, &taken))
		return;
	kind = taken[0];
	if (kind == 0)
		result->collation.tag = ACCESS_KEY_COLLATION_BYTES;
	else if (kind == 1 || kind == 2)
	{
		if (!access_key_take(&frame->rest, 4, &taken))
			return;
		result->collation.tag = ACCESS_KEY_COLLATION_SORTED;
		result->collation.id = access_key_u32_le(taken);
		result->collation.deterministic = kind == 1;
	}
	else
		return;
	if (!access_key_take(&frame->rest, 1, &taken))
		return;
	switch (taken[0])
	{
		case 0:
			result->inner.tag = ACCESS_KEY_INNER_NONE;
			break;
		case 1:
		case 2:
			{
				AccessKeyStored stored;
				uint8		inner = taken[0];

				if (!access_key_read_storage(&frame->rest, &stored.len, &stored.by_value, &stored.align))
					return;
				frame->held = std_alloc(ACCESS_KEY_KEY_TYPE);
				frame->held_live = true;
				if (!access_key_KeyType_read(frame->rest.ptr, frame->rest.len, frame->held, &frame->rest))
					return;
				stored.key_type = frame->held;
				frame->held_live = false;
				result->inner.tag = inner == 1 ? ACCESS_KEY_INNER_ARRAY : ACCESS_KEY_INNER_RANGE;
				if (inner == 1)
					result->inner.u.array = stored;
				else
					result->inner.u.range = stored;
			}
			break;
		case 3:
			frame->held = std_alloc(ACCESS_KEY_KEY_TYPE);
			frame->held_live = true;
			if (!access_key_KeyType_read(frame->rest.ptr, frame->rest.len, frame->held, &frame->rest))
				return;
			frame->held_live = false;
			result->inner.tag = ACCESS_KEY_INNER_MULTIRANGE;
			result->inner.u.multirange = frame->held;
			break;
		case 4:
			if (!access_key_take(&frame->rest, 4, &taken))
				return;
			count = access_key_u32_le(taken);
			result->inner.tag = ACCESS_KEY_INNER_ROW;
			std_vec_new(&frame->read, ACCESS_KEY_ATTRIBUTE);
			frame->read_live = true;
			for (uint32 i = 0; i < count; i++)
			{
				AccessKeyAttribute attribute;
				uint8		align;

				if (!access_key_read_storage(&frame->rest, &attribute.len, &attribute.by_value, &align) ||
					!access_key_take(&frame->rest, 1, &taken))
					return;
				attribute.dropped = taken[0] != 0;
				attribute.align = align;
				std_vec_reserve_Checked(&frame->read, 1, ACCESS_KEY_ATTRIBUTE);
				if (!access_key_KeyType_read(frame->rest.ptr, frame->rest.len, &attribute.key_type, &frame->rest))
					return;
				std_vec_push_Checked(&frame->read, &attribute, ACCESS_KEY_ATTRIBUTE);
			}
			result->inner.u.row = frame->read;
			frame->read_live = false;
			break;
		case 5:
			if (!access_key_take(&frame->rest, 4, &taken))
				return;
			count = access_key_u32_le(taken);
			result->inner.tag = ACCESS_KEY_INNER_ENUM;
			std_vec_new(&frame->read, ACCESS_KEY_LABEL);
			frame->read_live = true;
			for (uint32 i = 0; i < count; i++)
			{
				access_TupleU32F32_t label;
				uint32		order;

				if (!access_key_take(&frame->rest, 8, &taken))
					return;
				label.f0 = access_key_u32_le(taken);
				order = access_key_u32_le(taken + 4);
				memcpy(&label.f1, &order, sizeof(order));
				std_vec_push_Checked(&frame->read, &label, ACCESS_KEY_LABEL);
			}
			result->inner.u.labels = frame->read;
			frame->read_live = false;
			break;
		case 6:
			if (!access_key_take(&frame->rest, 1, &taken))
				return;
			result->inner.tag = ACCESS_KEY_INNER_COMPARED;
			result->inner.u.by_value = taken[0] != 0;
			break;
		default:
			return;
	}
	*frame->result_1 = frame->rest;
	frame->ok = true;
}

/*
 * access_key_KeyType_read answers the key type write wrote at the start of
 * some bytes and the bytes after it, or false for bytes it didn't write.
 */
bool
access_key_KeyType_read(const uint8 *bytes, size_t bytes_len, AccessKeyKeyType *result_0, StdBytes *result_1)
{
	access_key_read_frame_t frame = {.rest = {bytes, bytes_len}, .result_0 = result_0, .result_1 = result_1};

	std_landing(&frame, access_key_KeyType_read_body, access_key_KeyType_read_pad);
	access_key_KeyType_read_pad(&frame);
	return frame.ok;
}

#ifndef FRONTEND
/* access_key_CatalogCaps_SortKeys_sort_key is CatalogCaps' SortKeys: a text's sort key, which Postgres gives. */
static bool
access_key_CatalogCaps_SortKeys_sort_key(const void *self, uint32 collation, const uint8 *str, size_t str_len,
										 StdVec *result, PgPgError *error)
{
	const PgCatalogCaps *caps = self;

	return pg_sort_key_of(pg_CatalogCaps_may_raise(caps), pg_CatalogCaps_catalog(caps), collation, str, str_len,
						  result, error);
}

/* ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS is CatalogCaps' SortKeys. */
const AccessKeySortKeysOps ACCESS_KEY_CATALOG_CAPS_SORT_KEYS_OPS = {access_key_CatalogCaps_SortKeys_sort_key};

/* access_key_fork returns the fork's error of a code and a message. */
static PgPgError
access_key_fork(const char *code, const char *message)
{
	StdString	str;

	std_string_from_Checked(&str, message, strlen(message));
	return pg_PgError_fork((const uint8 *) code, &str);
}

/* access_key_unsupported returns the error of a key type no key is made of, which only a caller's mistake asks for. */
static PgPgError
access_key_unsupported(uint32 ty)
{
	char		message[64];

	snprintf(message, sizeof(message), "type %u has no key encoding", ty);
	return access_key_fork("XX000", message);
}

/* access_key_corrupt returns the error of a key that does not decode. */
static PgPgError
access_key_corrupt(void)
{
	return access_key_fork("XX000", "an index key is corrupt");
}

/* ACCESS_KEY_NAME_LEN is the length of a stored name, Postgres' NAMEDATALEN. */
#define ACCESS_KEY_NAME_LEN 64

/*
 * ACCESS_KEY_IPV4 is the family byte of a stored IPv4 address, Postgres'
 * PGSQL_AF_INET; an IPv6 address has the next.
 */
#define ACCESS_KEY_IPV4 2

/* access_key_malformed returns the error of a stored value that is not its type's form. */
static PgPgError
access_key_malformed(const char *what)
{
	char		message[64];

	snprintf(message, sizeof(message), "stored %s value is corrupt", what);
	return access_key_fork("XX001", message);
}

/* access_key_fail sets an error and answers false. */
static bool
access_key_fail(PgPgError made, PgPgError *error)
{
	*error = made;
	return false;
}

/* access_key_word_keyed reports whether a type's key is made from its datum's word. */
static bool
access_key_word_keyed(uint32 ty)
{
	switch (ty)
	{
		case BOOLOID:
		case CHAROID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case MONEYOID:
		case DATEOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case PG_LSNOID:
		case XID8OID:
			return true;
	}
	return false;
}

/* access_key_bytes_keyed reports whether a type's key is made from its value's stored bytes. */
static bool
access_key_bytes_keyed(uint32 ty)
{
	switch (ty)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case BYTEAOID:
		case UUIDOID:
		case MACADDROID:
		case MACADDR8OID:
		case NAMEOID:
		case TIDOID:
		case INTERVALOID:
		case TIMETZOID:
		case INETOID:
		case CIDROID:
		case BITOID:
		case VARBITOID:
		case OIDVECTOROID:
		case NUMERICOID:
		case JSONBOID:
		case TSVECTOROID:
		case TSQUERYOID:
			return true;
	}
	return false;
}

/*
 * access_key_address_len answers the length of a stored network address of
 * a family and a mask length, or false for a family or a mask no address
 * has.
 */
static bool
access_key_address_len(uint8 family, uint8 mask, size_t *result)
{
	size_t		len;

	if (family == ACCESS_KEY_IPV4)
		len = 4;
	else if (family == ACCESS_KEY_IPV4 + 1)
		len = 16;
	else
		return false;
	if ((size_t) mask > STD_MUL(usize, len, 8, 339, 27))
		return false;
	*result = len;
	return true;
}

/* access_key_oids_closure_1 is oids' field: the u32 of a stored oidvector's header at an offset. */
static uint32
access_key_oids_closure_1(const uint8 *header, size_t at)
{
	uint8		bytes[4];

	std_index_check(at, 20, RUST_FILE, 347, 49);
	bytes[0] = header[at];
	std_index_check(STD_ADD(usize, at, 1, 347, 68), 20, RUST_FILE, 347, 61);
	bytes[1] = header[at + 1];
	std_index_check(STD_ADD(usize, at, 2, 347, 84), 20, RUST_FILE, 347, 77);
	bytes[2] = header[at + 2];
	std_index_check(STD_ADD(usize, at, 3, 347, 100), 20, RUST_FILE, 347, 93);
	bytes[3] = header[at + 3];
	return access_key_u32_le(bytes);
}

/*
 * access_key_oids answers the OIDs a stored oidvector holds, a Vec<u32>: an
 * array of one dimension with no NULLs, as Postgres checks every oidvector
 * to be.
 */
static bool
access_key_oids(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error)
{
	const void *values;
	size_t		values_len;
	const void *rest;
	size_t		rest_len;

	if (bytes_len < 20)
		return access_key_fail(access_key_malformed("oidvector"), error);
	std_slice_as_chunks(bytes + 20, bytes_len - 20, ACCESS_KEY_BYTES, 4, &values, &values_len, &rest, &rest_len,
						RUST_FILE, 348, 26);
	if (access_key_oids_closure_1(bytes, 0) != 1 || access_key_oids_closure_1(bytes, 4) != 0 ||
		(size_t) access_key_oids_closure_1(bytes, 12) != values_len || rest_len != 0)
		return access_key_fail(access_key_malformed("oidvector"), error);
	std_vec_with_capacity_Checked(result, values_len, sizeof(uint32), _Alignof(uint32));
	for (size_t i = 0; i < values_len; i++)
	{
		uint32		oid = access_key_u32_le((const uint8 *) values + 4 * i);

		std_vec_push_Checked(result, &oid, sizeof(uint32), _Alignof(uint32));
	}
	return true;
}

/* AccessKeyNumericFn is with_numeric's f: what it does with a stored numeric as a key's numeric. */
typedef void (*AccessKeyNumericFn) (void *captures, const EncodingKeyNumeric *numeric);

/*
 * access_key_with_numeric_frame_t is with_numeric's state across its pad:
 * the decimal digits made so far, while live, and what it reads and gives.
 */
typedef struct access_key_with_numeric_frame_t
{
	StdBytes	digits;
	bool		negative;
	int16		weight;
	AccessKeyNumericFn f;
	void	   *captures;
	StdVec decimal;
	bool		live;
	PgPgError  *error;
	bool		ok;
} access_key_with_numeric_frame_t;

/* access_key_with_numeric_pad frees the decimal digits made so far. */
static void
access_key_with_numeric_pad(void *frame_)
{
	access_key_with_numeric_frame_t *frame = frame_;

	if (frame->live)
		std_vec_free_Checked(&frame->decimal, ACCESS_KEY_BYTES);
	frame->live = false;
}

/* access_key_with_numeric_body makes a finite numeric's decimal digits and gives f the numeric. */
static void
access_key_with_numeric_body(void *frame_)
{
	access_key_with_numeric_frame_t *frame = frame_;
	size_t		count = frame->digits.len / 2;
	EncodingKeyNumeric numeric = {.tag = ENCODING_KEY_NUMERIC_FINITE};

	std_vec_with_capacity_Checked(&frame->decimal, STD_MUL(usize, count, 4, 387, 42),
								  ACCESS_KEY_BYTES);
	frame->live = true;
	for (size_t i = 0; i < count; i++)
	{
		uint16		digit = (uint16) frame->digits.ptr[2 * i] | (uint16) frame->digits.ptr[2 * i + 1] << 8;
		uint8		four[4];

		if (digit >= 10000)
		{
			*frame->error = access_key_fork("XX001", "stored numeric value has a digit out of range");
			return;
		}
		four[0] = (uint8) (digit / 1000);
		four[1] = (uint8) (digit / 100 % 10);
		four[2] = (uint8) (digit / 10 % 10);
		four[3] = (uint8) (digit % 10);
		std_vec_extend_from_slice_Checked(&frame->decimal, four, sizeof(four), ACCESS_KEY_BYTES);
	}
	if (!encoding_key_Digits_new(frame->decimal.ptr, frame->decimal.len, &numeric.u.finite.digits))
	{
		*frame->error = access_key_malformed("numeric");
		return;
	}
	numeric.	u.finite.negative = frame->negative;
	numeric.	u.finite.exponent = STD_MUL(i32, 4, STD_ADD(i32, (int32) frame->weight, 1, 396, 75),
											396, 70);

	frame->f(frame->captures, &numeric);
	frame->ok = true;
}

/*
 * access_key_with_numeric gives f a stored numeric as a key's numeric: its
 * sign and weight from its short or long header, and its base-10000 digits
 * as decimal digits, or one of its three special values.
 */
static bool
access_key_with_numeric(const uint8 *bytes, size_t bytes_len, AccessKeyNumericFn f, void *captures,
						PgPgError *error)
{
	access_key_with_numeric_frame_t frame = {.f = f, .captures = captures, .error = error};
	EncodingKeyNumeric special = {0};
	uint16		header;

	if (bytes_len < 2)
		return access_key_fail(access_key_malformed("numeric"), error);
	header = (uint16) bytes[0] | (uint16) bytes[1] << 8;
	switch (header & 0xf000)
	{
		case 0xc000:
			special.tag = ENCODING_KEY_NUMERIC_NA_N;
			f(captures, &special);
			return true;
		case 0xd000:
			special.tag = ENCODING_KEY_NUMERIC_INFINITY;
			f(captures, &special);
			return true;
		case 0xf000:
			special.tag = ENCODING_KEY_NUMERIC_NEGATIVE_INFINITY;
			f(captures, &special);
			return true;
		case 0xe000:
			return access_key_fail(access_key_malformed("numeric"), error);
	}
	if ((header & 0x8000) != 0)
	{
		frame.weight = (int16) ((header & 0x003f) | ((header & 0x0040) != 0 ? ~0x003f : 0));
		frame.negative = (header & 0x2000) != 0;
		frame.digits.ptr = bytes + 2;
		frame.digits.len = bytes_len - 2;
	}
	else
	{
		if (bytes_len < 4)
			return access_key_fail(access_key_malformed("numeric"), error);
		frame.negative = (header & 0xc000) == 0x4000;
		frame.weight = (int16) ((uint16) bytes[2] | (uint16) bytes[3] << 8);
		frame.digits.ptr = bytes + 4;
		frame.digits.len = bytes_len - 4;
	}
	if (frame.digits.len % 2 != 0)
		return access_key_fail(access_key_malformed("numeric"), error);
	std_landing(&frame, access_key_with_numeric_body, access_key_with_numeric_pad);
	access_key_with_numeric_pad(&frame);
	return frame.ok;
}

/* access_key_column_t is a key column's write's captures: the bytes it writes. */
typedef struct access_key_column_t
{
	const uint8 *bytes;
	size_t		len;
} access_key_column_t;

/* access_key_numeric_key_closure_0_0 is numeric_key's write: a numeric's key. */
static void
access_key_numeric_key_closure_0_0(void *captures, StdVec *out)
{
	encoding_key_numeric(out, captures);
}

/* access_key_numeric_key_t is numeric_key's f's captures: the key and the column's order. */
typedef struct access_key_numeric_key_t
{
	StdVec	   *out;
	EncodingKeyOrder order;
} access_key_numeric_key_t;

/* access_key_numeric_key_closure_0 is numeric_key's f: the numeric's key column. */
static void
access_key_numeric_key_closure_0(void *captures, const EncodingKeyNumeric *numeric)
{
	access_key_numeric_key_t *key = captures;

	encoding_key_column(key->out, key->order, access_key_numeric_key_closure_0_0, (void *) numeric, NULL);
}

/* access_key_numeric_key appends a stored numeric as a key column, the numeric with_numeric reads. */
static bool
access_key_numeric_key(StdVec *out, const uint8 *bytes, size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_numeric_key_t key = {out, order};

	return access_key_with_numeric(bytes, bytes_len, access_key_numeric_key_closure_0, &key, error);
}

/*
 * access_key_sorted_t is sorted's answer, an Option<(Vec<u8>, bool)>: a text's
 * sort key and whether its bytes follow.
 */
typedef struct access_key_sorted_t
{
	bool		has;
	StdVec		sort_key;
	bool		deterministic;
} access_key_sorted_t;

/*
 * access_key_sorted answers a text's sort key under a collation and whether
 * its bytes follow the sort key, or none for text in byte order.
 */
static bool
access_key_sorted(AccessKeySortKeys sorter, AccessKeyCollation collation, const uint8 *str, size_t str_len,
				  access_key_sorted_t *result, PgPgError *error)
{
	result->has = collation.tag == ACCESS_KEY_COLLATION_SORTED;
	if (!result->has)
		return true;
	result->deterministic = collation.deterministic;
	return sorter.ops->sort_key(sorter.self, collation.id, str, str_len, &result->sort_key, error);
}

/*
 * access_key_collated appends a text's key without a column's marker: its
 * bytes, or its sort key and then, under a deterministic collation, its bytes.
 */
static void
access_key_collated(StdVec *out, const access_key_sorted_t *sorted, const uint8 *str, size_t str_len)
{
	if (!sorted->has)
		encoding_key_text(out, str, str_len);
	else
	{
		encoding_key_bytes(out, sorted->sort_key.ptr, sorted->sort_key.len);
		if (sorted->deterministic)
			encoding_key_text(out, str, str_len);
	}
}

/* access_key_text_key_t is text_key's state across its pad and its write's captures: the sort key, and the text. */
typedef struct access_key_text_key_t
{
	access_key_held_t held;
	StdVec	   *out;
	AccessKeySortKeys sorter;
	AccessKeyCollation collation;
	const uint8 *str;
	size_t		str_len;
	EncodingKeyOrder order;
	access_key_sorted_t sorted;
	PgPgError  *error;
	bool		ok;
} access_key_text_key_t;

/* access_key_text_key_closure_0 is text_key's write: the text collated. */
static void
access_key_text_key_closure_0(void *captures, StdVec *out)
{
	access_key_text_key_t *frame = captures;

	access_key_collated(out, &frame->sorted, frame->str, frame->str_len);
}

/* access_key_text_key_body sorts the text and writes its column. */
static void
access_key_text_key_body(void *frame_)
{
	access_key_text_key_t *frame = frame_;

	if (!access_key_sorted(frame->sorter, frame->collation, frame->str, frame->str_len, &frame->sorted,
						   frame->error))
		return;
	if (frame->sorted.has)
	{
		frame->held.vec[0] = frame->sorted.sort_key;
		frame->held.size[0] = 1;
		frame->held.align[0] = 1;
		frame->held.live[0] = true;
	}
	encoding_key_column(frame->out, frame->order, access_key_text_key_closure_0, frame, NULL);
	frame->ok = true;
}

/*
 * access_key_text_key appends a text's key column under a collation: its
 * bytes, or its sort key and then, under a deterministic collation, its
 * bytes.
 */
static bool
access_key_text_key(StdVec *out, AccessKeySortKeys sorter, AccessKeyCollation collation, const uint8 *str,
					size_t str_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_text_key_t frame = {.out = out, .sorter = sorter, .collation = collation, .str = str,
	.str_len = str_len, .order = order, .error = error};

	std_landing(&frame, access_key_text_key_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_bytes_key_closure_0 is bytes_key's write of a uuid or a MAC address: its bytes raw. */
static void
access_key_bytes_key_closure_0(void *captures, StdVec *out)
{
	access_key_column_t *column = captures;

	encoding_key_raw(out, column->bytes, column->len);
}

/* access_key_bytes_key_closure_1 is bytes_key's write of a bytea: its bytes escaped. */
static void
access_key_bytes_key_closure_1(void *captures, StdVec *out)
{
	access_key_column_t *column = captures;

	encoding_key_bytes(out, column->bytes, column->len);
}

/* access_key_tid_t is bytes_key's tid's write's captures: its block and offset. */
typedef struct access_key_tid_t
{
	uint32		block;
	uint16		offset;
} access_key_tid_t;

/* access_key_bytes_key_closure_5 is bytes_key's write of a tid: its block, then its offset big-endian. */
static void
access_key_bytes_key_closure_5(void *captures, StdVec *out)
{
	access_key_tid_t *tid = captures;
	uint8		be[2] = {(uint8) (tid->offset >> 8), (uint8) tid->offset};

	encoding_key_u32(out, tid->block);
	encoding_key_raw(out, be, sizeof(be));
}

/* access_key_interval_t is bytes_key's interval's write's captures. */
typedef struct access_key_interval_t
{
	int32		months;
	int32		days;
	int64		time;
} access_key_interval_t;

/* access_key_bytes_key_closure_7 is bytes_key's write of an interval. */
static void
access_key_bytes_key_closure_7(void *captures, StdVec *out)
{
	access_key_interval_t *interval = captures;

	encoding_key_interval(out, interval->months, interval->days, interval->time);
}

/* access_key_bytes_key_closure_10 is bytes_key's write of a time with time zone. */
static void
access_key_bytes_key_closure_10(void *captures, StdVec *out)
{
	encoding_key_timetz(out, *(EncodingKeyTimeWithZone *) captures);
}

/* access_key_inet_t is bytes_key's network address's write's captures. */
typedef struct access_key_inet_t
{
	bool		ipv6;
	uint8		mask;
	const uint8 *address;
	size_t		address_len;
} access_key_inet_t;

/* access_key_bytes_key_closure_11 is bytes_key's write of a network address. */
static void
access_key_bytes_key_closure_11(void *captures, StdVec *out)
{
	access_key_inet_t *inet = captures;

	encoding_key_inet(out, inet->ipv6, inet->mask, inet->address, inet->address_len);
}

/* access_key_bits_t is bytes_key's bit string's write's captures. */
typedef struct access_key_bits_t
{
	const uint8 *bits;
	size_t		bits_len;
	uint32		len;
} access_key_bits_t;

/* access_key_bytes_key_closure_13 is bytes_key's write of a bit string. */
static void
access_key_bytes_key_closure_13(void *captures, StdVec *out)
{
	access_key_bits_t *bits = captures;

	encoding_key_bits(out, bits->bits, bits->bits_len, bits->len);
}

/* access_key_bytes_key_closure_14 is bytes_key's write of an oidvector: its count, then each OID. */
static void
access_key_bytes_key_closure_14(void *captures, StdVec *out)
{
	const StdVec *oids = captures;

	encoding_key_u32(out, (uint32) oids->len);
	for (size_t i = 0; i < oids->len; i++)
		encoding_key_u32(out, STD_VEC_AT(oids, uint32, i));
}

/* access_key_bytes_key_closure_15 is bytes_key's write of a tsvector: its tokens raw. */
static void
access_key_bytes_key_closure_15(void *captures, StdVec *out)
{
	const StdVec *tokens = captures;

	encoding_key_raw(out, tokens->ptr, tokens->len);
}

/* access_key_bytes_key_closure_16 is bytes_key's write of a tsquery: its tokens raw. */
static void
access_key_bytes_key_closure_16(void *captures, StdVec *out)
{
	const StdVec *tokens = captures;

	encoding_key_raw(out, tokens->ptr, tokens->len);
}

static bool access_key_jsonb_key(StdVec *out, AccessKeySortKeys sorter, AccessKeyCollation collation,
								 const uint8 *bytes, size_t bytes_len, EncodingKeyOrder order, PgPgError *error);
static bool access_key_tsvector_key(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error);
static bool access_key_tsquery_key(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error);

/* access_key_owned_column_t is the state of a key column written from a vector it owns, across its pad. */
typedef struct access_key_owned_column_t
{
	access_key_held_t held;
	StdVec	   *out;
	EncodingKeyOrder order;
	void		(*write) (void *captures, StdVec *out);
} access_key_owned_column_t;

/* access_key_owned_column_body writes the column from the owned vector. */
static void
access_key_owned_column_body(void *frame_)
{
	access_key_owned_column_t *frame = frame_;

	encoding_key_column(frame->out, frame->order, frame->write, &frame->held.vec[0], NULL);
}

/* access_key_owned_column writes a key column from a vector it takes, which it frees after, an unwind included. */
static void
access_key_owned_column(StdVec *out, EncodingKeyOrder order, void (*write) (void *captures, StdVec *out),
						StdVec *owned, size_t size, size_t align)
{
	access_key_owned_column_t frame = {.out = out, .order = order, .write = write};

	frame.held.vec[0] = *owned;
	frame.held.size[0] = size;
	frame.held.align[0] = align;
	frame.held.live[0] = true;
	std_landing(&frame, access_key_owned_column_body, access_key_held_pad);
	access_key_held_pad(&frame);
}

/* access_key_bytes_key appends the key column of a value of a type keyed from its stored bytes. */
static bool
access_key_bytes_key(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *key_type, const uint8 *bytes,
					 size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	uint32		ty = key_type->typid;
	access_key_column_t column = {bytes, bytes_len};

	switch (ty)
	{
		case UUIDOID:
		case MACADDROID:
		case MACADDR8OID:
			encoding_key_column(out, order, access_key_bytes_key_closure_0, &column, NULL);
			return true;
		case BYTEAOID:
			encoding_key_column(out, order, access_key_bytes_key_closure_1, &column, NULL);
			return true;
		case BPCHAROID:
			{
				size_t		end = 0;

				for (size_t i = bytes_len; i-- > 0;)
				{
					if (bytes[i] != ' ')
					{
						end = STD_ADD(usize, i, 1, 413, 76);
						break;
					}
				}
				std_range_to_check(end, bytes_len, RUST_FILE, 414, 56);
				return access_key_text_key(out, sorter, key_type->collation, bytes, end, order, error);
			}
		case NAMEOID:
			{
				const uint8 *nul = memchr(bytes, 0, bytes_len);
				size_t		end = nul != NULL ? (size_t) (nul - bytes) : bytes_len;

				std_range_to_check(end, bytes_len, RUST_FILE, 418, 56);
				return access_key_text_key(out, sorter, key_type->collation, bytes, end, order, error);
			}
		case TIDOID:
			{
				access_key_tid_t tid;

				if (bytes_len != 6)
					return access_key_fail(access_key_malformed("tid"), error);
				tid.block = ((uint32) ((uint16) bytes[0] | (uint16) bytes[1] << 8)) << 16 |
					(uint32) ((uint16) bytes[2] | (uint16) bytes[3] << 8);
				tid.offset = (uint16) bytes[4] | (uint16) bytes[5] << 8;
				encoding_key_column(out, order, access_key_bytes_key_closure_5, &tid, NULL);
				return true;
			}
		case INTERVALOID:
			{
				access_key_interval_t interval;
				uint64		time = 0;

				if (bytes_len != 16)
					return access_key_fail(access_key_malformed("interval"), error);
				for (int i = 7; i >= 0; i--)
					time = time << 8 | bytes[i];
				interval.time = (int64) time;
				interval.days = (int32) access_key_u32_le(bytes + 8);
				interval.months = (int32) access_key_u32_le(bytes + 12);
				encoding_key_column(out, order, access_key_bytes_key_closure_7, &interval, NULL);
				return true;
			}
		case TIMETZOID:
			{
				EncodingKeyTimeWithZone value;
				uint64		time = 0;

				if (bytes_len != 12)
					return access_key_fail(access_key_malformed("time with time zone"), error);
				for (int i = 7; i >= 0; i--)
					time = time << 8 | bytes[i];
				if (!encoding_key_TimeWithZone_new((int64) time, (int32) access_key_u32_le(bytes + 8), &value))
					return access_key_fail(access_key_fork("XX001",
														   "stored time with time zone value is out of range"),
										   error);
				encoding_key_column(out, order, access_key_bytes_key_closure_10, &value, NULL);
				return true;
			}
		case INETOID:
		case CIDROID:
			{
				access_key_inet_t inet;
				size_t		len;

				if (bytes_len < 2)
					return access_key_fail(access_key_malformed("network address"), error);
				inet.		ipv6 = bytes[0] != ACCESS_KEY_IPV4;
				inet.		mask = bytes[1];
				inet.		address = bytes + 2;
				inet.		address_len = bytes_len - 2;

				if (!access_key_address_len(bytes[0], bytes[1], &len) || len != inet.address_len)
					return access_key_fail(access_key_malformed("network address"), error);
				encoding_key_column(out, order, access_key_bytes_key_closure_11, &inet, NULL);
				return true;
			}
		case BITOID:
		case VARBITOID:
			{
				access_key_bits_t bits;

				if (bytes_len < 4)
					return access_key_fail(access_key_malformed("bit string"), error);
				bits.len = access_key_u32_le(bytes);
				bits.bits = bytes + 4;
				bits.bits_len = bytes_len - 4;
				encoding_key_column(out, order, access_key_bytes_key_closure_13, &bits, NULL);
				return true;
			}
		case OIDVECTOROID:
			{
				StdVec		oids;

				if (!access_key_oids(bytes, bytes_len, &oids, error))
					return false;
				access_key_owned_column(out, order, access_key_bytes_key_closure_14, &oids, sizeof(uint32),
										_Alignof(uint32));
				return true;
			}
		case NUMERICOID:
			return access_key_numeric_key(out, bytes, bytes_len, order, error);
		case JSONBOID:
			return access_key_jsonb_key(out, sorter, key_type->collation, bytes, bytes_len, order, error);
		case TSVECTOROID:
			{
				StdVec		tokens;

				if (!access_key_tsvector_key(bytes, bytes_len, &tokens, error))
					return false;
				access_key_owned_column(out, order, access_key_bytes_key_closure_15, &tokens, ACCESS_KEY_BYTES);
				return true;
			}
		case TSQUERYOID:
			{
				StdVec		tokens;

				if (!access_key_tsquery_key(bytes, bytes_len, &tokens, error))
					return false;
				access_key_owned_column(out, order, access_key_bytes_key_closure_16, &tokens, ACCESS_KEY_BYTES);
				return true;
			}
	}
	return access_key_text_key(out, sorter, key_type->collation, bytes, bytes_len, order, error);
}

/* ACCESS_KEY_MAXIMUM_ALIGNOF is Postgres' largest alignment, which an array's elements start at past its header. */
#define ACCESS_KEY_MAXIMUM_ALIGNOF 8

/* ACCESS_KEY_ARRAY_HEADER is the length of an array's stored form before its dimensions. */
#define ACCESS_KEY_ARRAY_HEADER 16

/* ACCESS_KEY_VARLENA_HEADER is the length of a varlena's four-byte header, which a value's bytes here come without. */
#define ACCESS_KEY_VARLENA_HEADER 4

/* access_key_aligned answers an offset raised to an alignment Postgres' typalign names. */
static bool
access_key_aligned(size_t offset, uint8 align, size_t *result, PgPgError *error)
{
	size_t		by;

	switch (align)
	{
		case 'c':
			by = 1;
			break;
		case 's':
			by = 2;
			break;
		case 'i':
			by = 4;
			break;
		case 'd':
			by = 8;
			break;
		default:
			return access_key_fail(access_key_corrupt(), error);
	}
	*result = std_next_multiple_of_usize(offset, by);
	return true;
}

/*
 * access_key_element_at answers an array element's stored bytes at an
 * offset of the array's bytes, and the offset past them: its type's fixed
 * length, a varlena's whole datum with its header, or a C string with its
 * end.
 */
static bool
access_key_element_at(const uint8 *bytes, size_t bytes_len, size_t at, int16 len, StdBytes *result_0,
					  size_t *result_1, PgPgError *error)
{
	size_t		size;
	size_t		end;

	if (len >= 1)
		size = (size_t) len;
	else if (len == -1)
	{
		uint8		first;

		if (at >= bytes_len)
			return access_key_fail(access_key_corrupt(), error);
		first = bytes[at];
		if ((first & 0x01) == 0x01 && first != 0x01)
			size = first >> 1;
		else if ((first & 0x03) == 0x00)
		{
			size_t		header_end = STD_ADD(usize, at, ACCESS_KEY_VARLENA_HEADER, 506, 44);

			if (header_end > bytes_len)
				return access_key_fail(access_key_corrupt(), error);
			size = access_key_u32_le(bytes + at) >> 2;
		}
		else
			return access_key_fail(access_key_corrupt(), error);
	}
	else if (len == -2)
	{
		const uint8 *nul = at <= bytes_len ? memchr(bytes + at, 0, bytes_len - at) : NULL;

		if (nul == NULL)
			return access_key_fail(access_key_corrupt(), error);
		size = STD_ADD(usize, 1, (size_t) (nul - (bytes + at)), 512, 15);
	}
	else
		return access_key_fail(access_key_corrupt(), error);
	if (!std_checked_add_usize(at, size, &end) || end > bytes_len)
		return access_key_fail(access_key_corrupt(), error);
	result_0->ptr = bytes + at;
	result_0->len = size;
	*result_1 = end;
	return true;
}

/*
 * access_key_element_value answers the value a stored element or bound
 * gives its key type: a by-value one's word, a varlena's bytes past its
 * header, a C string's past its end, and any other's bytes.
 */
static bool
access_key_element_value(StdBytes stored, int16 len, bool by_value, AccessKeyValue *result, PgPgError *error)
{
	if (by_value)
	{
		result->tag = ACCESS_KEY_VALUE_WORD;
		return access_heaptuple_word_of(stored.ptr, stored.len, &result->u.word, error);
	}
	result->tag = ACCESS_KEY_VALUE_BYTES;
	result->u.bytes = stored;
	if (len == -1)
	{
		std_index_check(0, stored.len, RUST_FILE, 524, 24);
		if ((stored.ptr[0] & 0x01) == 0x01)
		{
			std_range_from_check(1, stored.len, RUST_FILE, 524, 66);
			result->u.bytes.ptr = stored.ptr + 1;
			result->u.bytes.len = stored.len - 1;
		}
		else
		{
			std_range_from_check(ACCESS_KEY_VARLENA_HEADER, stored.len, RUST_FILE, 525, 38);
			result->u.bytes.ptr = stored.ptr + ACCESS_KEY_VARLENA_HEADER;
			result->u.bytes.len = stored.len - ACCESS_KEY_VARLENA_HEADER;
		}
	}
	else if (len == -2)
	{
		size_t		end = STD_SUB(usize, stored.len, 1, 526, 47);

		std_range_to_check(end, stored.len, RUST_FILE, 526, 38);
		result->u.bytes.len = end;
	}
	return true;
}

static bool access_key_encode_option(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *key_type,
									 const AccessKeyValue *value, EncodingKeyOrder order, PgPgError *error);

/* access_key_word4 returns the four bytes of a slice of four, each read checked as Rust's index is at its place. */
static void
access_key_word4(const uint8 *word, size_t len, uint32 line, uint32 col0, uint32 col1, uint32 col2, uint32 col3,
				 uint8 result[4])
{
	std_index_check(0, len, RUST_FILE, line, col0);
	result[0] = word[0];
	std_index_check(1, len, RUST_FILE, line, col1);
	result[1] = word[1];
	std_index_check(2, len, RUST_FILE, line, col2);
	result[2] = word[2];
	std_index_check(3, len, RUST_FILE, line, col3);
	result[3] = word[3];
}

/*
 * access_key_array_key_closure_0 is array_key's int: the i32 at an offset of
 * the array's bytes, or corruption past them.
 */
static bool
access_key_array_key_closure_0(const uint8 *bytes, size_t bytes_len, size_t at, int32 *result, PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 4, 545, 34);
	uint8		word[4];

	if (at > end || end > bytes_len)
		return access_key_fail(access_key_corrupt(), error);
	access_key_word4(bytes + at, 4, 546, 32, 41, 50,
					 59, word);
	*result = (int32) access_key_u32_le(word);
	return true;
}

/* access_key_array_key_closure_7 is array_key's write: the elements' keys escaped. */
static void
access_key_array_key_closure_7(void *captures, StdVec *out)
{
	const StdVec *elements = captures;

	encoding_key_bytes(out, elements->ptr, elements->len);
}

/*
 * access_key_array_key_t is array_key's state across its pad: its dimensions,
 * lower bounds, element keys, and bounds.
 */
typedef struct access_key_array_key_t
{
	access_key_held_t held;
	StdVec	   *out;
	AccessKeySortKeys sorter;
	const AccessKeyKeyType *element;
	int16		len;
	bool		by_value;
	uint8		align;
	const uint8 *bytes;
	size_t		bytes_len;
	EncodingKeyOrder order;
	PgPgError  *error;
	bool		ok;
} access_key_array_key_t;

/*
 * ACCESS_KEY_DIMS, ACCESS_KEY_LBS, ACCESS_KEY_ELEMENTS, and ACCESS_KEY_BOUNDS
 * are array_key's slots of what it holds.
 */
#define ACCESS_KEY_DIMS 0
#define ACCESS_KEY_LBS 1
#define ACCESS_KEY_ELEMENTS 2
#define ACCESS_KEY_BOUNDS 3

/* access_key_array_key_body reads the array's header and elements and writes its key. */
static void
access_key_array_key_body(void *frame_)
{
	access_key_array_key_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		bytes_len = frame->bytes_len;
	int32		read;
	size_t		dimensions;
	size_t		offset;
	StdVec	   *dims;
	StdVec	   *lbs;
	size_t		count = 1;
	size_t		nulls;
	size_t		start;
	size_t		at;
	StdVec	   *elements;
	StdVec	   *bounds;

	if (!access_key_array_key_closure_0(bytes, bytes_len, 0, &read, frame->error))
		return;
	if (read < 0)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	dimensions = (size_t) read;
	if (!access_key_array_key_closure_0(bytes, bytes_len, 4, &read, frame->error))
		return;
	if (read < 0)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	offset = (size_t) read;
	dims = access_key_held_new(&frame->held, ACCESS_KEY_DIMS, sizeof(int32), _Alignof(int32));
	for (size_t i = 0; i < dimensions; i++)
	{
		size_t		place = STD_ADD(usize, 12, STD_MUL(usize, 4, i, 550, 49), 550, 44);

		if (!access_key_array_key_closure_0(bytes, bytes_len, place, &read, frame->error))
			return;
		std_vec_push_Checked(dims, &read, sizeof(int32), _Alignof(int32));
	}
	lbs = access_key_held_new(&frame->held, ACCESS_KEY_LBS, sizeof(int32), _Alignof(int32));
	for (size_t i = 0; i < dimensions; i++)
	{
		size_t		place = STD_ADD(usize, 12, STD_MUL(usize, 4,
													   STD_ADD(usize, dimensions, i, 551, 53),
													   551, 48),
									551, 43);

		if (!access_key_array_key_closure_0(bytes, bytes_len, place, &read, frame->error))
			return;
		std_vec_push_Checked(lbs, &read, sizeof(int32), _Alignof(int32));
	}
	if (dimensions == 0)
		count = 0;
	for (size_t i = 0; dimensions > 0 && i < dims->len; i++)
	{
		int32		dim = STD_VEC_AT(dims, int32, i);

		if (dim < 0 || !std_checked_mul_usize(count, (size_t) dim, &count))
		{
			*frame->error = access_key_corrupt();
			return;
		}
	}
	nulls = STD_ADD(usize, 12, STD_MUL(usize, 8, dimensions, 557, 22), 557, 17);
	start = offset == 0 ?
		std_next_multiple_of_usize(STD_ADD(usize, ACCESS_KEY_ARRAY_HEADER,
										   STD_MUL(usize, 8, dimensions, 559, 30),
										   559, 15),
								   ACCESS_KEY_MAXIMUM_ALIGNOF) : offset;
	if (start < ACCESS_KEY_VARLENA_HEADER)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	at = start - ACCESS_KEY_VARLENA_HEADER;
	elements = access_key_held_new(&frame->held, ACCESS_KEY_ELEMENTS, ACCESS_KEY_BYTES);
	for (size_t i = 0; i < count; i++)
	{
		bool		short_header;
		StdBytes	stored;
		size_t		next;
		AccessKeyValue value;

		if (offset != 0)
		{
			size_t		null_at = STD_ADD(usize, nulls, i / 8, 565, 37);

			if (null_at >= bytes_len)
			{
				*frame->error = access_key_corrupt();
				return;
			}
			if ((bytes[null_at] & STD_SHL(u8, 1, i % 8, 565, 76)) == 0)
			{
				if (!access_key_encode_option(elements, frame->sorter, frame->element, NULL, ENCODING_KEY_ORDER_ASC,
											  frame->error))
					return;
				continue;
			}
		}
		short_header = frame->len == -1 && at < bytes_len && bytes[at] != 0;
		if (!short_header)
		{
			size_t		raised;

			if (!access_key_aligned(STD_ADD(usize, at, ACCESS_KEY_VARLENA_HEADER, 571, 26),
									frame->align, &raised, frame->error))
				return;
			at = STD_SUB(usize, raised, ACCESS_KEY_VARLENA_HEADER, 571, 18);
		}
		if (!access_key_element_at(bytes, bytes_len, at, frame->len, &stored, &next, frame->error) ||
			!access_key_element_value(stored, frame->len, frame->by_value, &value, frame->error) ||
			!access_key_encode_option(elements, frame->sorter, frame->element, &value, ENCODING_KEY_ORDER_ASC,
									  frame->error))
			return;
		at = next;
	}
	bounds = access_key_held_new(&frame->held, ACCESS_KEY_BOUNDS, sizeof(encoding_key_array_tail_dimensions_t),
								 _Alignof(encoding_key_array_tail_dimensions_t));
	std_vec_reserve_Checked(bounds, Min(dims->len, lbs->len), sizeof(encoding_key_array_tail_dimensions_t),
							_Alignof(encoding_key_array_tail_dimensions_t));
	for (size_t i = 0; i < dims->len && i < lbs->len; i++)
	{
		encoding_key_array_tail_dimensions_t bound = {STD_VEC_AT(dims, int32, i), STD_VEC_AT(lbs, int32, i)};

		std_vec_push_Checked(bounds, &bound, sizeof(bound), _Alignof(encoding_key_array_tail_dimensions_t));
	}
	std_vec_free_Checked(lbs, sizeof(int32), _Alignof(int32));
	frame->held.live[ACCESS_KEY_LBS] = false;
	std_vec_free_Checked(dims, sizeof(int32), _Alignof(int32));
	frame->held.live[ACCESS_KEY_DIMS] = false;
	encoding_key_array_tail(elements, bounds->ptr, bounds->len);
	encoding_key_column(frame->out, frame->order, access_key_array_key_closure_7, elements, NULL);
	frame->ok = true;
}

/*
 * access_key_array_key appends an array's key: each element's key in its key
 * type, ascending, then the array's dimensions and lower bounds, all one
 * escaped column, so arrays order as Postgres' array_cmp orders them.
 */
static bool
access_key_array_key(StdVec *out, AccessKeySortKeys sorter, const AccessKeyStored *stored, const uint8 *bytes,
					 size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_array_key_t frame = {.out = out, .sorter = sorter, .element = stored->key_type, .len = stored->len,
		.by_value = stored->by_value, .align = stored->align, .bytes = bytes, .bytes_len = bytes_len,
	.order = order, .error = error};

	std_landing(&frame, access_key_array_key_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * access_key_has_own_key reports whether a type that holds no values of other
 * key types has a key encoding of its own.
 */
bool
access_key_has_own_key(uint32 ty)
{
	return access_key_word_keyed(ty) || access_key_bytes_keyed(ty);
}

/* A stored range's flags, Postgres'. */
#define ACCESS_KEY_RANGE_EMPTY 0x01
#define ACCESS_KEY_RANGE_LB_INC 0x02
#define ACCESS_KEY_RANGE_UB_INC 0x04
#define ACCESS_KEY_RANGE_LB_INF 0x08
#define ACCESS_KEY_RANGE_UB_INF 0x10

/* access_key_bound_key_t is bound_key's state across its pad: the key made so far. */
typedef struct access_key_bound_key_t
{
	access_key_held_t held;
	AccessKeySortKeys sorter;
	const AccessKeyKeyType *subtype;
	AccessKeyValue value;
	PgPgError  *error;
	bool		ok;
} access_key_bound_key_t;

/* access_key_bound_key_body encodes the bound and takes its marker off. */
static void
access_key_bound_key_body(void *frame_)
{
	access_key_bound_key_t *frame = frame_;
	StdVec	   *key = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	uint8		marker;

	if (!access_key_encode_option(key, frame->sorter, frame->subtype, &frame->value, ENCODING_KEY_ORDER_ASC,
								  frame->error))
		return;
	std_vec_remove(key, 0, &marker, ACCESS_KEY_BYTES, RUST_FILE, 752, 5);
	frame->ok = true;
}

/* access_key_bound_key answers a range bound's key, a Vec<u8>: its subtype's key column without its marker. */
static bool
access_key_bound_key(AccessKeySortKeys sorter, const AccessKeyKeyType *subtype, AccessKeyValue value,
					 StdVec *result, PgPgError *error)
{
	access_key_bound_key_t frame = {.sorter = sorter, .subtype = subtype, .value = value, .error = error};

	std_landing(&frame, access_key_bound_key_body, access_key_held_pad);
	if (frame.ok)
		*result = access_key_held_take(&frame.held, 0);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* The key::range instance at a Vec<u8> bound, a bound's key, which range_bounds writes. */
#define ENCODING_KEY_SCOPE static
#define ENCODING_KEY_DECLARE
#define ENCODING_KEY_DEFINE
#define ENCODING_KEY_RANGE_TS VecU8
#define ENCODING_KEY_RANGE_T StdVec
#define ENCODING_KEY_RANGE_T_DROP(place) std_vec_free_Checked((place), ACCESS_KEY_BYTES)
#include "doltgres/encoding/key_template.h"

/* access_key_range_bounds_closure_1 is range_bounds' write: a bound's key, which it takes. */
static void
access_key_range_bounds_closure_1(void *captures, StdVec *out, StdVec *key)
{
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_extend_from_slice_Checked(out, key->ptr, key->len, ACCESS_KEY_BYTES);
		std_unwind_pop(&unwind);
	}
	else
	{
		std_vec_free_Checked(key, ACCESS_KEY_BYTES);
		std_unwind_resume(&unwind);
	}
	std_vec_free_Checked(key, ACCESS_KEY_BYTES);
}

/* access_key_range_bounds_t is range_bounds' state across its pad: the range's bytes and its bounds' keys. */
typedef struct access_key_range_bounds_t
{
	access_key_held_t held;
	AccessKeySortKeys sorter;
	const AccessKeyStored *stored;
	const uint8 *bytes;
	size_t		bytes_len;
	size_t		at;
	uint8		flags;
	PgPgError  *error;
	bool		ok;
} access_key_range_bounds_t;

/* ACCESS_KEY_RANGE, ACCESS_KEY_LOWER, and ACCESS_KEY_UPPER are range_bounds' slots of what it holds. */
#define ACCESS_KEY_RANGE 0
#define ACCESS_KEY_LOWER 1
#define ACCESS_KEY_UPPER 2

/* access_key_range_bounds_body reads the range's bounds its flags say it stores and writes them. */
static void
access_key_range_bounds_body(void *frame_)
{
	access_key_range_bounds_t *frame = frame_;
	const AccessKeyStored *stored = frame->stored;
	StdVec	   *range = access_key_held_new(&frame->held, ACCESS_KEY_RANGE, ACCESS_KEY_BYTES);
	bool		has_lower = (frame->flags & ACCESS_KEY_RANGE_LB_INF) == 0;
	bool		has_upper = (frame->flags & ACCESS_KEY_RANGE_UB_INF) == 0;
	StdBytes	bound;
	size_t		next;
	AccessKeyValue value;
	encoding_key_rangeVecU8_lower_t lower = {0};
	encoding_key_rangeVecU8_lower_t upper = {0};

	if ((frame->flags & ACCESS_KEY_RANGE_EMPTY) != 0)
	{
		encoding_key_range_empty(range);
		frame->ok = true;
		return;
	}
	if (has_lower)
	{
		if (!access_key_element_at(frame->bytes, frame->bytes_len, frame->at, stored->len, &bound, &next,
								   frame->error))
			return;
		frame->at = next;
		if (!access_key_element_value(bound, stored->len, stored->by_value, &value, frame->error) ||
			!access_key_bound_key(frame->sorter, stored->key_type, value, &frame->held.vec[ACCESS_KEY_LOWER],
								  frame->error))
			return;
		frame->held.size[ACCESS_KEY_LOWER] = 1;
		frame->held.align[ACCESS_KEY_LOWER] = 1;
		frame->held.live[ACCESS_KEY_LOWER] = true;
	}
	if (has_upper)
	{
		if (!(stored->len == -1 && frame->at < frame->bytes_len && frame->bytes[frame->at] != 0))
		{
			size_t		raised;

			if (!access_key_aligned(STD_ADD(usize, frame->at, ACCESS_KEY_VARLENA_HEADER, 639, 34),
									stored->align, &raised, frame->error))
				return;
			frame->at = STD_SUB(usize, raised, ACCESS_KEY_VARLENA_HEADER, 639, 26);
		}
		if (!access_key_element_at(frame->bytes, frame->bytes_len, frame->at, stored->len, &bound, &next,
								   frame->error) ||
			!access_key_element_value(bound, stored->len, stored->by_value, &value, frame->error) ||
			!access_key_bound_key(frame->sorter, stored->key_type, value, &frame->held.vec[ACCESS_KEY_UPPER],
								  frame->error))
			return;
		frame->held.size[ACCESS_KEY_UPPER] = 1;
		frame->held.align[ACCESS_KEY_UPPER] = 1;
		frame->held.live[ACCESS_KEY_UPPER] = true;
	}
	lower.f0 = frame->held.vec[ACCESS_KEY_LOWER];
	lower.f1 = (frame->flags & ACCESS_KEY_RANGE_LB_INC) != 0;
	upper.f0 = frame->held.vec[ACCESS_KEY_UPPER];
	upper.f1 = (frame->flags & ACCESS_KEY_RANGE_UB_INC) != 0;
	frame->held.live[ACCESS_KEY_LOWER] = frame->held.live[ACCESS_KEY_UPPER] = false;
	encoding_key_rangeVecU8(range, has_lower, &lower, has_upper, &upper, access_key_range_bounds_closure_1, NULL,
							NULL);
	frame->ok = true;
}

/*
 * access_key_range_bounds answers a range's key column's bytes before they
 * are escaped, a Vec<u8>: empty, or key::range over the bounds its flags say
 * a range stores from an offset of some bytes.
 */
static bool
access_key_range_bounds(AccessKeySortKeys sorter, const AccessKeyStored *stored, const uint8 *bytes,
						size_t bytes_len, size_t at, uint8 flags, StdVec *result, PgPgError *error)
{
	access_key_range_bounds_t frame = {.sorter = sorter, .stored = stored, .bytes = bytes, .bytes_len = bytes_len,
	.at = at, .flags = flags, .error = error};

	std_landing(&frame, access_key_range_bounds_body, access_key_held_pad);
	if (frame.ok)
		*result = access_key_held_take(&frame.held, ACCESS_KEY_RANGE);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_range_key_closure_0 is range_key's write: the range's bytes escaped. */
static void
access_key_range_key_closure_0(void *captures, StdVec *out)
{
	const StdVec *range = captures;

	encoding_key_bytes(out, range->ptr, range->len);
}

/*
 * access_key_range_key appends a range's key, key::range over its bounds,
 * each its subtype's key without the column's marker, so ranges order as
 * Postgres' range_cmp orders them when the subtype's order is its key's.
 */
static bool
access_key_range_key(StdVec *out, AccessKeySortKeys sorter, const AccessKeyStored *stored, const uint8 *bytes,
					 size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	StdVec		range;

	if (bytes_len == 0)
		return access_key_fail(access_key_corrupt(), error);
	if (!access_key_range_bounds(sorter, stored, bytes, bytes_len, 4, bytes[bytes_len - 1], &range, error))
		return false;
	access_key_owned_column(out, order, access_key_range_key_closure_0, &range, ACCESS_KEY_BYTES);
	return true;
}

/* access_key_multirange_key_closure_0 is multirange_key's word: the u32 at an offset of the multirange's bytes. */
static bool
access_key_multirange_key_closure_0(const uint8 *bytes, size_t bytes_len, size_t at, uint32 *result,
									PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 4, 667, 34);
	uint8		word[4];

	if (end > bytes_len)
		return access_key_fail(access_key_corrupt(), error);
	access_key_word4(bytes + at, 4, 668, 32, 41, 50,
					 59, word);
	*result = access_key_u32_le(word);
	return true;
}

/* access_key_multirange_key_closure_3 is multirange_key's write: the ranges' bytes escaped. */
static void
access_key_multirange_key_closure_3(void *captures, StdVec *out)
{
	const StdVec *ranges = captures;

	encoding_key_bytes(out, ranges->ptr, ranges->len);
}

/* access_key_multirange_key_t is multirange_key's state across its pad: the ranges' bytes and a range's bounds. */
typedef struct access_key_multirange_key_t
{
	access_key_held_t held;
	StdVec	   *out;
	AccessKeySortKeys sorter;
	const AccessKeyKeyType *range;
	const uint8 *bytes;
	size_t		bytes_len;
	EncodingKeyOrder order;
	PgPgError  *error;
	bool		ok;
} access_key_multirange_key_t;

/* access_key_multirange_key_body reads each range's flags and bounds and writes the multirange's key. */
static void
access_key_multirange_key_body(void *frame_)
{
	access_key_multirange_key_t *frame = frame_;
	const AccessKeyStored *stored;
	uint32		word;
	size_t		count;
	StdVec	   *ranges;

	if (frame->range->inner.tag != ACCESS_KEY_INNER_RANGE)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	stored = &frame->range->inner.u.range;
	if (!access_key_multirange_key_closure_0(frame->bytes, frame->bytes_len, 4, &word, frame->error))
		return;
	count = word;
	ranges = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	if (count > 0)
	{
		size_t		flags_at = STD_ADD(usize, 8, STD_MUL(usize, 4, STD_SUB(usize, count, 1, 673, 33),
														 673, 28),
									   673, 24);
		size_t		boundaries;

		if (!access_key_aligned(STD_ADD(usize, STD_ADD(usize, flags_at, count, 674, 34),
										ACCESS_KEY_VARLENA_HEADER, 674, 34),
								stored->align, &boundaries, frame->error))
			return;
		boundaries = STD_SUB(usize, boundaries, ACCESS_KEY_VARLENA_HEADER, 674, 26);
		for (size_t i = 0; i < count; i++)
		{
			size_t		offset = 0;
			size_t		item = i;
			size_t		flags_place;
			StdVec		bounds;

			while (item > 0)
			{
				uint32		entry;

				if (!access_key_multirange_key_closure_0(frame->bytes, frame->bytes_len,
														 STD_ADD(usize, 8, STD_MUL(usize, 4, STD_SUB(usize, item, 1, 679, 43),
																				   679, 38),
																 679, 34),
														 &entry, frame->error))
					return;
				offset = STD_ADD(usize, offset, entry & 0x7FFFFFFF, 680, 17);
				if ((entry & 0x80000000) != 0)
					break;
				item = STD_SUB(usize, item, 1, 684, 17);
			}
			flags_place = STD_ADD(usize, flags_at, i, 686, 36);
			if (flags_place >= frame->bytes_len)
			{
				*frame->error = access_key_corrupt();
				return;
			}
			if (!access_key_range_bounds(frame->sorter, stored, frame->bytes, frame->bytes_len,
										 STD_ADD(usize, boundaries, offset, 687, 90),
										 frame->bytes[flags_place], &bounds, frame->error))
				return;
			frame->held.vec[1] = bounds;
			frame->held.size[1] = 1;
			frame->held.align[1] = 1;
			frame->held.live[1] = true;
			access_key_push(ranges, ENCODING_KEY_ELEMENT);
			encoding_key_bytes(ranges, bounds.ptr, bounds.len);
			std_vec_free_Checked(&frame->held.vec[1], ACCESS_KEY_BYTES);
			frame->held.live[1] = false;
		}
	}
	access_key_push(ranges, ENCODING_KEY_END);
	encoding_key_column(frame->out, frame->order, access_key_multirange_key_closure_3, ranges, NULL);
	frame->ok = true;
}

/*
 * access_key_multirange_key appends a multirange's key: each of its ranges'
 * keys, unmarked and escaped, after an element's mark, then the end, all one
 * escaped column, so multiranges order as Postgres' multirange_cmp orders
 * them.
 */
static bool
access_key_multirange_key(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *range, const uint8 *bytes,
						  size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_multirange_key_t frame = {.out = out, .sorter = sorter, .range = range, .bytes = bytes,
	.bytes_len = bytes_len, .order = order, .error = error};

	std_landing(&frame, access_key_multirange_key_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * HEAP_HASNULL and HEAP_NATTS_MASK are a tuple's t_infomask NULL bitmap bit
 * and t_infomask2's count of attributes, Postgres'.
 */
#define ACCESS_KEY_HEAP_HASNULL 0x0001
#define ACCESS_KEY_HEAP_NATTS_MASK 0x07FF

/* access_key_row_key_closure_0 is row_key's half: the u16 at an offset of the row's bytes. */
static bool
access_key_row_key_closure_0(const uint8 *bytes, size_t bytes_len, size_t at, uint16 *result, PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 2, 717, 34);

	if (end > bytes_len)
		return access_key_fail(access_key_corrupt(), error);
	std_index_check(0, 2, RUST_FILE, 718, 32);
	std_index_check(1, 2, RUST_FILE, 718, 41);
	*result = (uint16) bytes[at] | (uint16) bytes[at + 1] << 8;
	return true;
}

/* access_key_row_key_closure_2 is row_key's write: the attributes' keys escaped. */
static void
access_key_row_key_closure_2(void *captures, StdVec *out)
{
	const StdVec *keys = captures;

	encoding_key_bytes(out, keys->ptr, keys->len);
}

/* access_key_row_key_t is row_key's state across its pad: the attributes' keys made so far. */
typedef struct access_key_row_key_t
{
	access_key_held_t held;
	StdVec	   *out;
	AccessKeySortKeys sorter;
	const StdVec *attributes;
	const uint8 *bytes;
	size_t		bytes_len;
	EncodingKeyOrder order;
	PgPgError  *error;
	bool		ok;
} access_key_row_key_t;

/* access_key_row_key_body reads each live attribute and writes the row's key. */
static void
access_key_row_key_body(void *frame_)
{
	access_key_row_key_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		bytes_len = frame->bytes_len;
	uint16		half;
	size_t		held;
	bool		nulls;
	size_t		start;
	size_t		at;
	StdVec	   *keys;
	size_t		count = 0;

	if (!access_key_row_key_closure_0(bytes, bytes_len, 14, &half, frame->error))
		return;
	held = half & ACCESS_KEY_HEAP_NATTS_MASK;
	if (!access_key_row_key_closure_0(bytes, bytes_len, 16, &half, frame->error))
		return;
	nulls = (half & ACCESS_KEY_HEAP_HASNULL) != 0;
	if (bytes_len <= 18)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	start = bytes[18];
	if (start < ACCESS_KEY_VARLENA_HEADER)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	at = start - ACCESS_KEY_VARLENA_HEADER;
	keys = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	for (size_t i = 0; i < frame->attributes->len; i++)
	{
		const AccessKeyAttribute *attribute = &STD_VEC_AT(frame->attributes, AccessKeyAttribute, i);
		bool		present = i < held;
		AccessKeyValue value;

		count = std_Enumerate_count_step(count);
		if (present && nulls)
		{
			size_t		null_at = STD_ADD(usize, 19, i / 8, 726, 56);

			if (null_at >= bytes_len)
			{
				*frame->error = access_key_corrupt();
				return;
			}
			present = (bytes[null_at] & STD_SHL(u8, 1, i % 8, 726, 92)) != 0;
		}
		if (present)
		{
			bool		short_header = attribute->len == -1 && at < bytes_len && bytes[at] != 0;
			StdBytes	stored;
			size_t		next;

			if (!short_header)
			{
				size_t		raised;

				if (!std_checked_next_multiple_of_usize(STD_ADD(usize, at, ACCESS_KEY_VARLENA_HEADER,
																731, 35),
														attribute->align, &raised))
				{
					*frame->error = access_key_corrupt();
					return;
				}
				at = STD_SUB(usize, raised, ACCESS_KEY_VARLENA_HEADER, 732, 26);
			}
			if (!access_key_element_at(bytes, bytes_len, at, attribute->len, &stored, &next, frame->error))
				return;
			at = next;
			if (!access_key_element_value(stored, attribute->len, attribute->by_value, &value, frame->error))
				return;
		}
		if (!attribute->dropped &&
			!access_key_encode_option(keys, frame->sorter, &attribute->key_type, present ? &value : NULL,
									  ENCODING_KEY_ORDER_ASC, frame->error))
			return;
	}
	encoding_key_column(frame->out, frame->order, access_key_row_key_closure_2, keys, NULL);
	frame->ok = true;
}

/*
 * access_key_row_key appends a row's key: each live attribute's key in its
 * key type, ascending, in order, all one escaped column, so rows order as
 * Postgres' btrecordcmp orders them.
 */
static bool
access_key_row_key(StdVec *out, AccessKeySortKeys sorter, const StdVec *attributes, const uint8 *bytes,
				   size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_row_key_t frame = {.out = out, .sorter = sorter, .attributes = attributes, .bytes = bytes,
	.bytes_len = bytes_len, .order = order, .error = error};

	std_landing(&frame, access_key_row_key_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* The parts of a jsonb container's header and entries, Postgres'. */
#define ACCESS_KEY_JB_CMASK 0x0FFFFFFF
#define ACCESS_KEY_JB_FSCALAR 0x10000000
#define ACCESS_KEY_JB_FOBJECT 0x20000000
#define ACCESS_KEY_JB_FARRAY 0x40000000
#define ACCESS_KEY_JENTRY_OFFLENMASK 0x0FFFFFFF
#define ACCESS_KEY_JENTRY_TYPEMASK 0x70000000
#define ACCESS_KEY_JENTRY_HAS_OFF 0x80000000
#define ACCESS_KEY_JENTRY_ISSTRING 0x00000000
#define ACCESS_KEY_JENTRY_ISNUMERIC 0x10000000
#define ACCESS_KEY_JENTRY_ISBOOL_FALSE 0x20000000
#define ACCESS_KEY_JENTRY_ISBOOL_TRUE 0x30000000
#define ACCESS_KEY_JENTRY_ISNULL 0x40000000
#define ACCESS_KEY_JENTRY_ISCONTAINER 0x50000000

/* access_key_jsonb_key_closure_0 is jsonb_key's write: the tokens escaped. */
static void
access_key_jsonb_key_closure_0(void *captures, StdVec *out)
{
	const StdVec *tokens = captures;

	encoding_key_bytes(out, tokens->ptr, tokens->len);
}

static bool access_key_jsonb_container(StdVec *tokens, AccessKeySortKeys sorter, AccessKeyCollation collation,
									   const uint8 *bytes, size_t bytes_len, PgPgError *error);

/* access_key_jsonb_key_t is jsonb_key's state across its pad: the tokens made so far. */
typedef struct access_key_jsonb_key_t
{
	access_key_held_t held;
	StdVec	   *out;
	AccessKeySortKeys sorter;
	AccessKeyCollation collation;
	const uint8 *bytes;
	size_t		bytes_len;
	EncodingKeyOrder order;
	PgPgError  *error;
	bool		ok;
} access_key_jsonb_key_t;

/* access_key_jsonb_key_body walks the document's tokens and writes its key. */
static void
access_key_jsonb_key_body(void *frame_)
{
	access_key_jsonb_key_t *frame = frame_;
	StdVec	   *tokens = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);

	if (!access_key_jsonb_container(tokens, frame->sorter, frame->collation, frame->bytes, frame->bytes_len,
									frame->error))
		return;
	encoding_key_column(frame->out, frame->order, access_key_jsonb_key_closure_0, tokens, NULL);
	frame->ok = true;
}

/*
 * access_key_jsonb_key appends a jsonb document's key: its tokens as
 * Postgres' compareJsonbContainers walks them, all one escaped column.
 */
static bool
access_key_jsonb_key(StdVec *out, AccessKeySortKeys sorter, AccessKeyCollation collation, const uint8 *bytes,
					 size_t bytes_len, EncodingKeyOrder order, PgPgError *error)
{
	access_key_jsonb_key_t frame = {.out = out, .sorter = sorter, .collation = collation, .bytes = bytes,
	.bytes_len = bytes_len, .order = order, .error = error};

	std_landing(&frame, access_key_jsonb_key_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_jsonb_container_closure_1 is jsonb_container's word: the u32 at an offset of the container's bytes. */
static bool
access_key_jsonb_container_closure_1(const uint8 *bytes, size_t bytes_len, size_t at, uint32 *result,
									 PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 4, 852, 34);
	uint8		word[4];

	if (end > bytes_len)
		return access_key_fail(access_key_malformed("jsonb"), error);
	access_key_word4(bytes + at, 4, 853, 32, 41, 50,
					 59, word);
	*result = access_key_u32_le(word);
	return true;
}

/* access_key_jsonb_span_t is a jsonb child's span, a (u32, usize, usize): its kind, start, and end. */
typedef struct access_key_jsonb_span_t
{
	uint32		f0;
	size_t		f1;
	size_t		f2;
} access_key_jsonb_span_t;

/* access_key_jsonb_string_t is jsonb_container's string's write's captures: its sort key and text. */
typedef struct access_key_jsonb_string_t
{
	const access_key_sorted_t *sorted;
	const uint8 *str;
	size_t		str_len;
} access_key_jsonb_string_t;

/* access_key_jsonb_container_closure_3 is jsonb_container's string's write: the text collated. */
static void
access_key_jsonb_container_closure_3(void *captures, StdVec *out)
{
	access_key_jsonb_string_t *string = captures;

	access_key_collated(out, string->sorted, string->str, string->str_len);
}

/* access_key_jsonb_container_closure_4 is jsonb_container's number: a numeric's jsonb token. */
static void
access_key_jsonb_container_closure_4(void *captures, const EncodingKeyNumeric *numeric)
{
	encoding_key_jsonb_numeric(captures, numeric);
}

/*
 * access_key_jsonb_container_t is jsonb_container's state across its pad: its
 * children's spans and visiting order, and a string's sort key.
 */
typedef struct access_key_jsonb_container_t
{
	access_key_held_t held;
	StdVec	   *tokens;
	AccessKeySortKeys sorter;
	AccessKeyCollation collation;
	const uint8 *bytes;
	size_t		bytes_len;
	PgPgError  *error;
	bool		ok;
} access_key_jsonb_container_t;

/* ACCESS_KEY_SPANS, ACCESS_KEY_VISITED, and ACCESS_KEY_SORT_KEY are jsonb_container's slots of what it holds. */
#define ACCESS_KEY_SPANS 0
#define ACCESS_KEY_VISITED 1
#define ACCESS_KEY_SORT_KEY 2

/* access_key_jsonb_container_body walks the container's children and appends their tokens. */
static void
access_key_jsonb_container_body(void *frame_)
{
	access_key_jsonb_container_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		bytes_len = frame->bytes_len;
	uint32		header;
	size_t		count;
	bool		object;
	size_t		children;
	size_t		data;
	StdVec	   *spans;
	size_t		start = 0;
	StdVec	   *visited;

	if (!access_key_jsonb_container_closure_1(bytes, bytes_len, 0, &header, frame->error))
		return;
	count = header & ACCESS_KEY_JB_CMASK;
	if ((header & ACCESS_KEY_JB_FARRAY) != 0 && (header & ACCESS_KEY_JB_FOBJECT) == 0)
	{
		encoding_key_jsonb_array(frame->tokens, (uint32) count, (header & ACCESS_KEY_JB_FSCALAR) != 0);
		object = false;
	}
	else if ((header & ACCESS_KEY_JB_FARRAY) == 0 && (header & ACCESS_KEY_JB_FOBJECT) != 0)
	{
		encoding_key_jsonb_object(frame->tokens, (uint32) count);
		object = true;
	}
	else
	{
		*frame->error = access_key_malformed("jsonb");
		return;
	}
	children = object ? STD_MUL(usize, 2, count, 868, 32) : count;
	data = STD_ADD(usize, 4, STD_MUL(usize, 4, children, 869, 20), 869, 16);
	spans = access_key_held_new(&frame->held, ACCESS_KEY_SPANS, sizeof(access_key_jsonb_span_t),
								_Alignof(access_key_jsonb_span_t));
	std_vec_reserve_Checked(spans, children, sizeof(access_key_jsonb_span_t), _Alignof(access_key_jsonb_span_t));
	for (size_t i = 0; i < children; i++)
	{
		uint32		entry;
		size_t		field;
		size_t		end;
		access_key_jsonb_span_t span;

		if (!access_key_jsonb_container_closure_1(bytes, bytes_len,
												  STD_ADD(usize, 4, STD_MUL(usize, 4, i, 873, 30),
														  873, 26),
												  &entry, frame->error))
			return;
		field = entry & ACCESS_KEY_JENTRY_OFFLENMASK;
		end = (entry & ACCESS_KEY_JENTRY_HAS_OFF) != 0 ? field : STD_ADD(usize, start, field, 875, 67);
		span.f0 = entry & ACCESS_KEY_JENTRY_TYPEMASK;
		span.f1 = start;
		span.f2 = Max(end, start);
		std_vec_push_Checked(spans, &span, sizeof(span), _Alignof(access_key_jsonb_span_t));
		start = end;
	}
	visited = access_key_held_new(&frame->held, ACCESS_KEY_VISITED, sizeof(size_t), _Alignof(size_t));
	for (size_t i = 0; i < count; i++)
	{
		std_vec_push_Checked(visited, &i, sizeof(size_t), _Alignof(size_t));
		if (object)
		{
			size_t		value = STD_ADD(usize, i, count, 880, 45);

			std_vec_push_Checked(visited, &value, sizeof(size_t), _Alignof(size_t));
		}
	}
	for (size_t v = 0; v < visited->len; v++)
	{
		size_t		i = STD_VEC_AT(visited, size_t, v);
		access_key_jsonb_span_t span;
		size_t		held_len;
		size_t		aligned;

		std_index_check(i, spans->len, RUST_FILE, 884, 34);
		span = STD_VEC_AT(spans, access_key_jsonb_span_t, i);
		held_len = STD_ADD(usize, data, span.f2, 885, 32);
		if (held_len > bytes_len)
		{
			*frame->error = access_key_malformed("jsonb");
			return;
		}
		aligned = STD_ADD(usize, data, std_next_multiple_of_usize(span.f1, 4), 886, 23);
		switch (span.f0)
		{
			case ACCESS_KEY_JENTRY_ISSTRING:
				{
					size_t		text_at = STD_ADD(usize, data, span.f1, 889, 37);
					access_key_jsonb_string_t string;
					access_key_sorted_t sorted;

					if (text_at > held_len)
					{
						*frame->error = access_key_malformed("jsonb");
						return;
					}
					if (!access_key_sorted(frame->sorter, frame->collation, bytes + text_at, held_len - text_at,
										   &sorted, frame->error))
						return;
					if (sorted.has)
					{
						frame->held.vec[ACCESS_KEY_SORT_KEY] = sorted.sort_key;
						frame->held.size[ACCESS_KEY_SORT_KEY] = 1;
						frame->held.align[ACCESS_KEY_SORT_KEY] = 1;
						frame->held.live[ACCESS_KEY_SORT_KEY] = true;
					}
					string.sorted = &sorted;
					string.str = bytes + text_at;
					string.str_len = held_len - text_at;
					encoding_key_jsonb_string(frame->tokens, access_key_jsonb_container_closure_3, &string, NULL);
					if (sorted.has)
					{
						std_vec_free_Checked(&frame->held.vec[ACCESS_KEY_SORT_KEY], ACCESS_KEY_BYTES);
						frame->held.live[ACCESS_KEY_SORT_KEY] = false;
					}
				}
				break;
			case ACCESS_KEY_JENTRY_ISNUMERIC:
				{
					StdBytes	stored;
					size_t		next;
					AccessKeyValue numeric;

					if (!access_key_element_at(bytes, held_len, aligned, -1, &stored, &next, frame->error) ||
						!access_key_element_value(stored, -1, false, &numeric, frame->error))
						return;
					if (numeric.tag != ACCESS_KEY_VALUE_BYTES)
					{
						*frame->error = access_key_malformed("jsonb");
						return;
					}
					if (!access_key_with_numeric(numeric.u.bytes.ptr, numeric.u.bytes.len,
												 access_key_jsonb_container_closure_4, frame->tokens, frame->error))
						return;
				}
				break;
			case ACCESS_KEY_JENTRY_ISBOOL_FALSE:
				encoding_key_jsonb_bool(frame->tokens, false);
				break;
			case ACCESS_KEY_JENTRY_ISBOOL_TRUE:
				encoding_key_jsonb_bool(frame->tokens, true);
				break;
			case ACCESS_KEY_JENTRY_ISNULL:
				encoding_key_jsonb_null(frame->tokens);
				break;
			case ACCESS_KEY_JENTRY_ISCONTAINER:
				if (aligned > held_len)
				{
					*frame->error = access_key_malformed("jsonb");
					return;
				}
				if (!access_key_jsonb_container(frame->tokens, frame->sorter, frame->collation, bytes + aligned,
												held_len - aligned, frame->error))
					return;
				break;
			default:
				*frame->error = access_key_malformed("jsonb");
				return;
		}
	}
	frame->ok = true;
}

/*
 * access_key_jsonb_container appends the tokens of a jsonb container at the
 * start of some bytes: its kind and its count, then each element, or each
 * key and then its value, in their stored order.
 */
static bool
access_key_jsonb_container(StdVec *tokens, AccessKeySortKeys sorter, AccessKeyCollation collation,
						   const uint8 *bytes, size_t bytes_len, PgPgError *error)
{
	access_key_jsonb_container_t frame = {.tokens = tokens, .sorter = sorter, .collation = collation, .bytes = bytes,
	.bytes_len = bytes_len, .error = error};

	std_landing(&frame, access_key_jsonb_container_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_be16 appends a u16's big-endian bytes raw. */
static void
access_key_be16(StdVec *out, uint16 value)
{
	uint8		be[2] = {(uint8) (value >> 8), (uint8) value};

	encoding_key_raw(out, be, sizeof(be));
}

/* access_key_tsvector_key_closure_1 is tsvector_key's half: the u16 at an offset of the tsvector's bytes. */
static bool
access_key_tsvector_key_closure_1(const uint8 *bytes, size_t bytes_len, size_t at, uint16 *result,
								  PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 2, 920, 34);

	if (end > bytes_len)
		return access_key_fail(access_key_malformed("tsvector"), error);
	std_index_check(0, 2, RUST_FILE, 921, 32);
	std_index_check(1, 2, RUST_FILE, 921, 41);
	*result = (uint16) bytes[at] | (uint16) bytes[at + 1] << 8;
	return true;
}

/*
 * access_key_tokens_t is the state of a tsvector's or tsquery's key's making
 * across its pad: its tokens and pending items.
 */
typedef struct access_key_tokens_t
{
	access_key_held_t held;
	const uint8 *bytes;
	size_t		bytes_len;
	PgPgError  *error;
	bool		ok;
} access_key_tokens_t;

/* access_key_tsvector_key_body writes the tsvector's size, count, and lexemes as tokens. */
static void
access_key_tsvector_key_body(void *frame_)
{
	access_key_tokens_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		bytes_len = frame->bytes_len;
	uint32		count;
	size_t		data;
	StdVec	   *tokens;

	if (bytes_len < 4)
	{
		*frame->error = access_key_malformed("tsvector");
		return;
	}
	count = access_key_u32_le(bytes);
	data = STD_ADD(usize, 4, STD_MUL(usize, 4, (size_t) count, 925, 20), 925, 16);
	tokens = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	encoding_key_u32(tokens, (uint32) STD_ADD(usize, ACCESS_KEY_VARLENA_HEADER, bytes_len, 927, 28));
	encoding_key_u32(tokens, count);
	for (size_t i = 0; i < (size_t) count; i++)
	{
		size_t		entry_at = STD_ADD(usize, 4, STD_MUL(usize, 4, i, 930, 41), 930, 37);
		uint32		entry;
		bool		has_positions;
		size_t		len;
		size_t		at;
		size_t		word_start;
		size_t		word_end;

		if (entry_at > bytes_len || bytes_len - entry_at < 4)
		{
			*frame->error = access_key_malformed("tsvector");
			return;
		}
		entry = access_key_u32_le(bytes + entry_at);
		has_positions = (entry & 1) != 0;
		len = (entry >> 1) & 0x7ff;
		at = entry >> 12;
		word_start = STD_ADD(usize, data, at, 933, 30);
		word_end = STD_ADD(usize, STD_ADD(usize, data, at, 933, 41), len, 933, 41);
		if (word_start > word_end || word_end > bytes_len)
		{
			*frame->error = access_key_malformed("tsvector");
			return;
		}
		if (memchr(bytes + word_start, 0, len) != NULL)
		{
			*frame->error = access_key_malformed("tsvector");
			return;
		}
		encoding_key_byte(tokens, (uint8) !has_positions);
		encoding_key_text(tokens, bytes + word_start, len);
		if (has_positions)
		{
			size_t		counted = std_next_multiple_of_usize(STD_ADD(usize,
																	 STD_ADD(usize, data, at, 940, 28),
																	 len, 940, 28), 2);
			uint16		positions;

			if (!access_key_tsvector_key_closure_1(bytes, bytes_len, counted, &positions, frame->error))
				return;
			access_key_be16(tokens, (uint16) ~positions);
			for (size_t j = 0; j < (size_t) positions; j++)
			{
				uint16		position;
				size_t		past = STD_ADD(usize, counted, 2, 944, 37);
				size_t		twice = STD_MUL(usize, 2, j, 944, 51);

				if (!access_key_tsvector_key_closure_1(bytes, bytes_len,
													   STD_ADD(usize, past, twice, 944, 37),
													   &position, frame->error))
					return;
				access_key_be16(tokens, (uint16) ~((uint16) ((position & 0x3fff) << 2) | (uint16) (position >> 14)));
			}
		}
	}
	frame->ok = true;
}

/*
 * access_key_tsvector_key answers a tsvector's key, a Vec<u8> written bare
 * as Doltgrust's is, ordered as Postgres' silly_cmp_tsvector orders
 * tsvectors.
 */
static bool
access_key_tsvector_key(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error)
{
	access_key_tokens_t frame = {.bytes = bytes, .bytes_len = bytes_len, .error = error};

	std_landing(&frame, access_key_tsvector_key_body, access_key_held_pad);
	if (frame.ok)
		*result = access_key_held_take(&frame.held, 0);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* The kinds of a tsquery's items and its operators with one operand and with a distance, Postgres'. */
#define ACCESS_KEY_QI_VAL 1
#define ACCESS_KEY_QI_OPR 2
#define ACCESS_KEY_OP_NOT 1
#define ACCESS_KEY_OP_PHRASE 4

/* ACCESS_KEY_QUERY_ITEM_LEN is the length of a stored tsquery item, Postgres' sizeof(QueryItem). */
#define ACCESS_KEY_QUERY_ITEM_LEN 12

/* access_key_tsquery_key_closure_1 is tsquery_key's word: the four bytes at an offset of the tsquery's bytes. */
static bool
access_key_tsquery_key_closure_1(const uint8 *bytes, size_t bytes_len, size_t at, uint32 *result,
								 PgPgError *error)
{
	size_t		end = STD_ADD(usize, at, 4, 974, 23);

	if (end > bytes_len)
		return access_key_fail(access_key_malformed("tsquery"), error);
	*result = access_key_u32_le(bytes + at);
	return true;
}

/*
 * access_key_pending_t is a tsquery walk's pending step, a Result<usize, i16>:
 * an item to walk, or a distance to write.
 */
typedef struct access_key_pending_t
{
	bool		is_ok;
	size_t		item;
	int16		distance;
} access_key_pending_t;

/* access_key_tsquery_key_body walks the tsquery's items as QTNodeCompare does and writes them as tokens. */
static void
access_key_tsquery_key_body(void *frame_)
{
	access_key_tokens_t *frame = frame_;
	const uint8 *bytes = frame->bytes;
	size_t		bytes_len = frame->bytes_len;
	uint32		count;
	size_t		operands;
	StdVec	   *tokens;
	StdVec	   *pending;
	access_key_pending_t step = {true, 0, 0};
	uint32		walked = 0;

	if (!access_key_tsquery_key_closure_1(bytes, bytes_len, 0, &count, frame->error))
		return;
	operands = STD_ADD(usize, 4, STD_MUL(usize, ACCESS_KEY_QUERY_ITEM_LEN, (size_t) count,
										 977, 24),
					   977, 20);
	tokens = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	encoding_key_u32(tokens, count);
	encoding_key_u32(tokens, (uint32) STD_ADD(usize, ACCESS_KEY_VARLENA_HEADER, bytes_len, 980, 28));
	if (count == 0)
	{
		frame->ok = true;
		return;
	}
	pending = access_key_held_new(&frame->held, 1, sizeof(access_key_pending_t), _Alignof(access_key_pending_t));
	std_vec_push_Checked(pending, &step, sizeof(step), _Alignof(access_key_pending_t));
	while (std_vec_pop(pending, &step, sizeof(step)))
	{
		size_t		item;
		size_t		at;

		if (!step.is_ok)
		{
			access_key_be16(tokens, (uint16) ~(uint16) step.distance);
			continue;
		}
		if (step.item >= (size_t) count || walked >= count)
		{
			*frame->error = access_key_malformed("tsquery");
			return;
		}
		item = step.item;
		walked = STD_ADD(u32, walked, 1, 995, 9);
		at = STD_ADD(usize, 4, STD_MUL(usize, ACCESS_KEY_QUERY_ITEM_LEN, item, 996, 22),
					 996, 18);
		if (at >= bytes_len)
		{
			*frame->error = access_key_malformed("tsquery");
			return;
		}
		if (bytes[at] == ACCESS_KEY_QI_VAL)
		{
			uint32		crc;
			uint32		place;
			size_t		len;
			size_t		offset;
			size_t		text_start;
			size_t		text_end;
			uint8		be[4];

			if (!access_key_tsquery_key_closure_1(bytes, bytes_len, STD_ADD(usize, at, 4, 999, 51), &crc,
												  frame->error) ||
				!access_key_tsquery_key_closure_1(bytes, bytes_len, STD_ADD(usize, at, 8, 1000, 53), &place,
												  frame->error))
				return;
			len = place & 0xfff;
			offset = place >> 12;
			text_start = STD_ADD(usize, operands, offset, 1002, 38);
			text_end = STD_ADD(usize, STD_ADD(usize, operands, offset, 1002, 57), len,
							   1002, 57);
			if (text_start > text_end || text_end > bytes_len || memchr(bytes + text_start, 0, len) != NULL)
			{
				*frame->error = access_key_malformed("tsquery");
				return;
			}
			crc = ~(crc ^ 0x80000000);
			be[0] = (uint8) (crc >> 24);
			be[1] = (uint8) (crc >> 16);
			be[2] = (uint8) (crc >> 8);
			be[3] = (uint8) crc;
			encoding_key_byte(tokens, (uint8) ~ACCESS_KEY_QI_VAL);
			encoding_key_raw(tokens, be, sizeof(be));
			encoding_key_text(tokens, bytes + text_start, len);
		}
		else if (bytes[at] == ACCESS_KEY_QI_OPR)
		{
			size_t		oper_at = STD_ADD(usize, at, 1, 1011, 39);
			uint8		oper;
			uint8		low;
			uint8		high;
			int16		distance;
			uint32		left;
			uint8		children;
			uint8		raw[3];
			size_t		right;
			access_key_pending_t next = {true, 0, 0};

			if (oper_at >= bytes_len)
			{
				*frame->error = access_key_malformed("tsquery");
				return;
			}
			oper = bytes[oper_at];
			if (STD_ADD(usize, at, 2, 1013, 32) >= bytes_len)
			{
				*frame->error = access_key_malformed("tsquery");
				return;
			}
			low = bytes[at + 2];
			if (STD_ADD(usize, at, 3, 1014, 32) >= bytes_len)
			{
				*frame->error = access_key_malformed("tsquery");
				return;
			}
			high = bytes[at + 3];
			distance = (int16) ((uint16) low | (uint16) high << 8);
			if (!access_key_tsquery_key_closure_1(bytes, bytes_len, STD_ADD(usize, at, 4, 1016, 52), &left,
												  frame->error))
				return;
			children = oper == ACCESS_KEY_OP_NOT ? 1 : 2;
			raw[0] = (uint8) ~ACCESS_KEY_QI_OPR;
			raw[1] = (uint8) ~oper;
			raw[2] = (uint8) ~children;
			encoding_key_raw(tokens, raw, sizeof(raw));
			if (oper == ACCESS_KEY_OP_PHRASE)
			{
				access_key_pending_t phrase = {false, 0, distance};

				std_vec_push_Checked(pending, &phrase, sizeof(phrase), _Alignof(access_key_pending_t));
			}
			if (oper != ACCESS_KEY_OP_NOT)
			{
				if (!std_checked_add_usize(item, (size_t) left, &right) || left <= 1)
				{
					*frame->error = access_key_malformed("tsquery");
					return;
				}
				next.item = right;
				std_vec_push_Checked(pending, &next, sizeof(next), _Alignof(access_key_pending_t));
			}
			next.item = STD_ADD(usize, item, 1, 1025, 33);
			std_vec_push_Checked(pending, &next, sizeof(next), _Alignof(access_key_pending_t));
		}
		else
		{
			*frame->error = access_key_malformed("tsquery");
			return;
		}
	}
	if (walked != count)
	{
		*frame->error = access_key_malformed("tsquery");
		return;
	}
	frame->ok = true;
}

/*
 * access_key_tsquery_key answers a tsquery's key, a Vec<u8> written bare
 * as Doltgrust's is, ordered as Postgres' CompareTSQ orders tsqueries.
 */
static bool
access_key_tsquery_key(const uint8 *bytes, size_t bytes_len, StdVec *result, PgPgError *error)
{
	access_key_tokens_t frame = {.bytes = bytes, .bytes_len = bytes_len, .error = error};

	std_landing(&frame, access_key_tsquery_key_body, access_key_held_pad);
	if (frame.ok)
		*result = access_key_held_take(&frame.held, 0);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_encode_closure_2 is encode's write of an enum's label: its sort order. */
static void
access_key_encode_closure_2(void *captures, StdVec *out)
{
	encoding_key_f32(out, *(const float4 *) captures);
}

/* access_key_encode_closure_3 is encode's write of a compared word: its bytes, low byte first, escaped. */
static void
access_key_encode_closure_3(void *captures, StdVec *out)
{
	uint64		datum = *(const uint64 *) captures;
	uint8		le[8];

	for (int i = 0; i < 8; i++)
		le[i] = (uint8) (datum >> (8 * i));
	encoding_key_bytes(out, le, sizeof(le));
}

/* access_key_encode_closure_4 is encode's write of a compared value's bytes, escaped. */
static void
access_key_encode_closure_4(void *captures, StdVec *out)
{
	const StdBytes *bytes = captures;

	encoding_key_bytes(out, bytes->ptr, bytes->len);
}

/* access_key_word_t is encode's word's write's captures: the type and the datum. */
typedef struct access_key_word_t
{
	uint32		ty;
	uint64		datum;
} access_key_word_t;

/* access_key_encode_closure_5 is encode's write of a value keyed from its datum's word. */
static void
access_key_encode_closure_5(void *captures, StdVec *out)
{
	const access_key_word_t *word = captures;

	switch (word->ty)
	{
		case BOOLOID:
			encoding_key_bool(out, pg_word_to_bool(word->datum));
			break;
		case CHAROID:
			encoding_key_byte(out, pg_word_to_char(word->datum));
			break;
		case INT2OID:
			encoding_key_i16(out, pg_word_to_int16(word->datum));
			break;
		case INT4OID:
		case DATEOID:
			encoding_key_i32(out, pg_word_to_int32(word->datum));
			break;
		case PG_LSNOID:
		case XID8OID:
			encoding_key_u64(out, word->datum);
			break;
		case OIDOID:
			encoding_key_u32(out, pg_word_to_oid(word->datum));
			break;
		case FLOAT4OID:
			encoding_key_f32(out, pg_word_to_float4(word->datum));
			break;
		case FLOAT8OID:
			encoding_key_f64(out, pg_word_to_float8(word->datum));
			break;
		default:
			encoding_key_i64(out, pg_word_to_int64(word->datum));
			break;
	}
}

/* ACCESS_KEY_NO_VALUE is the value encode_option passes encode for NULL, which encode doesn't read. */
static const AccessKeyValue ACCESS_KEY_NO_VALUE = {0};

/* access_key_encode_option is encode over a borrowed value, none for NULL. */
static bool
access_key_encode_option(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *key_type,
						 const AccessKeyValue *value, EncodingKeyOrder order, PgPgError *error)
{
	return access_key_encode(out, sorter, key_type, value != NULL, value != NULL ? *value : ACCESS_KEY_NO_VALUE,
							 order, error);
}

/* access_key_encode appends a value of a key type, or NULL, as a key column in the order. */
bool
access_key_encode(StdVec *out, AccessKeySortKeys sorter, const AccessKeyKeyType *key_type, bool has_value,
				  AccessKeyValue value, EncodingKeyOrder order, PgPgError *error)
{
	uint32		ty = key_type->typid;
	bool		bytes = has_value && value.tag == ACCESS_KEY_VALUE_BYTES;
	bool		word = has_value && value.tag == ACCESS_KEY_VALUE_WORD;

	switch (key_type->inner.tag)
	{
		case ACCESS_KEY_INNER_ARRAY:
			if (bytes)
				return access_key_array_key(out, sorter, &key_type->inner.u.array, value.u.bytes.ptr,
											value.u.bytes.len, order, error);
			break;
		case ACCESS_KEY_INNER_RANGE:
			if (bytes)
				return access_key_range_key(out, sorter, &key_type->inner.u.range, value.u.bytes.ptr,
											value.u.bytes.len, order, error);
			break;
		case ACCESS_KEY_INNER_MULTIRANGE:
			if (bytes)
				return access_key_multirange_key(out, sorter, key_type->inner.u.multirange, value.u.bytes.ptr,
												 value.u.bytes.len, order, error);
			break;
		case ACCESS_KEY_INNER_ROW:
			if (bytes)
				return access_key_row_key(out, sorter, &key_type->inner.u.row, value.u.bytes.ptr, value.u.bytes.len,
										  order, error);
			break;
		case ACCESS_KEY_INNER_ENUM:
			if (word)
			{
				uint32		label = pg_word_to_oid(value.u.word);
				const StdVec *labels = &key_type->inner.u.labels;
				size_t		low = 0;
				size_t		high = labels->len;
				char		message[64];

				while (low < high)
				{
					size_t		mid = low + (high - low) / 2;
					uint32		found = STD_VEC_AT(labels, access_TupleU32F32_t, mid).f0;

					if (found == label)
					{
						std_index_check(mid, labels->len, RUST_FILE, 1061, 57);
						encoding_key_column(out, order, access_key_encode_closure_2,
											&STD_VEC_AT(labels, access_TupleU32F32_t, mid).f1, NULL);
						return true;
					}
					if (found < label)
						low = mid + 1;
					else
						high = mid;
				}
				snprintf(message, sizeof(message), "invalid internal value for enum: %u", label);
				return access_key_fail(access_key_fork("22P03", message), error);
			}
			break;
		case ACCESS_KEY_INNER_COMPARED:
			if (word)
			{
				encoding_key_column(out, order, access_key_encode_closure_3, &value.u.word, NULL);
				return true;
			}
			if (bytes)
			{
				encoding_key_column(out, order, access_key_encode_closure_4, &value.u.bytes, NULL);
				return true;
			}
			break;
		case ACCESS_KEY_INNER_NONE:
			break;
	}
	if (!has_value)
		encoding_key_null(out, order);
	else if (word && access_key_word_keyed(ty))
	{
		access_key_word_t captured = {ty, value.u.word};

		encoding_key_column(out, order, access_key_encode_closure_5, &captured, NULL);
	}
	else if (bytes && access_key_bytes_keyed(ty))
		return access_key_bytes_key(out, sorter, key_type, value.u.bytes.ptr, value.u.bytes.len, order, error);
	else
		return access_key_fail(access_key_unsupported(ty), error);
	return true;
}

/*
 * AccessKeyExactTag is Exact's variants: a value its type sizes, a varlena
 * with so many bytes past its key's, or a tsvector.
 */
typedef enum AccessKeyExactTag
{
	ACCESS_KEY_EXACT_SIZED,
	ACCESS_KEY_EXACT_VARLENA,
	ACCESS_KEY_EXACT_TSVECTOR,
} AccessKeyExactTag;

/*
 * access_key_exact answers what a key type's keys give back when they give
 * back their values, or false for a type whose keys don't.
 */
static bool
access_key_exact(const AccessKeyKeyType *key_type, AccessKeyExactTag *result, size_t *more)
{
	*more = 0;
	if (key_type->collation.tag == ACCESS_KEY_COLLATION_SORTED && !key_type->collation.deterministic)
		return false;
	if (key_type->inner.tag == ACCESS_KEY_INNER_COMPARED)
	{
		*result = key_type->inner.u.by_value ? ACCESS_KEY_EXACT_SIZED : ACCESS_KEY_EXACT_VARLENA;
		return true;
	}
	if (key_type->inner.tag == ACCESS_KEY_INNER_ENUM)
		return false;
	switch (key_type->typid)
	{
		case BOOLOID:
		case CHAROID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case MONEYOID:
		case DATEOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case PG_LSNOID:
		case XID8OID:
		case UUIDOID:
		case MACADDROID:
		case MACADDR8OID:
		case NAMEOID:
		case TIDOID:
		case TIMETZOID:
			*result = ACCESS_KEY_EXACT_SIZED;
			return true;
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
		case INETOID:
		case CIDROID:
		case BITOID:
		case VARBITOID:
			*result = ACCESS_KEY_EXACT_VARLENA;
			return true;
		case OIDVECTOROID:
			*result = ACCESS_KEY_EXACT_VARLENA;
			*more = 16;
			return true;
		case TSVECTOROID:
			*result = ACCESS_KEY_EXACT_TSVECTOR;
			return true;
	}
	return false;
}

/* access_key_is_exact reports whether a key type's keys give back the values they were made from. */
bool
access_key_is_exact(const AccessKeyKeyType *key_type)
{
	AccessKeyExactTag exact;
	size_t		more;

	return access_key_exact(key_type, &exact, &more);
}

/*
 * AccessKeyReader is what the byte closures of decode, end, and decoded_max
 * read: a key column's bytes past its marker, flipped for a descending
 * column, from a place on.
 */
typedef struct AccessKeyReader
{
	const uint8 *bytes;
	size_t		len;
	uint8		flip;
	size_t		from;

	/*
	 * line and col are where Rust's from + i is, for a reader from past the
	 * start.
	 */
	uint32		line;
	uint32		col;
} AccessKeyReader;

/* access_key_byte answers a reader's byte at a place, flipped, or false past the bytes. */
static bool
access_key_byte(const AccessKeyReader *byte, size_t i, uint8 *result)
{
	size_t		at = byte->from == 0 ? i : std_add_usize(byte->from, i, RUST_FILE, byte->line, byte->col);

	if (at >= byte->len)
		return false;
	*result = byte->bytes[at] ^ byte->flip;
	return true;
}

/* access_key_byte_or_corrupt answers a reader's byte at a place, or the error of a corrupt key past the bytes. */
static bool
access_key_byte_or_corrupt(const AccessKeyReader *byte, size_t i, uint8 *result, PgPgError *error)
{
	if (!access_key_byte(byte, i, result))
		return access_key_fail(access_key_corrupt(), error);
	return true;
}

/*
 * access_key_count_of reads the big-endian count at four places of a
 * tsvector's or tsquery's key, or false past the bytes.
 */
static bool
access_key_count_of(const AccessKeyReader *byte, size_t first, size_t *result)
{
	size_t		n = 0;

	for (size_t i = first; i < first + 4; i++)
	{
		uint8		b;

		if (!access_key_byte(byte, i, &b))
			return false;
		n = n << 8 | b;
	}
	*result = n;
	return true;
}

/*
 * access_key_tsvector_layout reads a tsvector's key, as Doltgrust's
 * TsVector::read_key reads it, and answers the size of the stored form it
 * decodes to and the key's length, or false when the bytes stop inside the
 * key.
 */
static bool
access_key_tsvector_layout(const AccessKeyReader *byte, size_t *result_0, size_t *result_1)
{
	size_t		count;
	size_t		at = 8;
	size_t		strings = 0;
	uint8		b;

	if (!access_key_count_of(byte, 4, &count))
		return false;
	for (size_t n = 0; n < count; n++)
	{
		bool		has_positions;

		if (!access_key_byte(byte, at, &b))
			return false;
		has_positions = b == 0;
		at = STD_ADD(usize, at, 1, 1158, 9);
		for (;;)
		{
			if (!access_key_byte(byte, at, &b))
				return false;
			if (b == 0)
				break;
			at = STD_ADD(usize, at, 1, 1160, 13);
			strings = STD_ADD(usize, strings, 1, 1161, 13);
		}
		at = STD_ADD(usize, at, 1, 1163, 9);
		if (has_positions)
		{
			uint8		high;
			uint8		low;
			size_t		positions;
			size_t		twice;
			size_t		raised;

			if (!access_key_byte(byte, at, &high) ||
				!access_key_byte(byte, STD_ADD(usize, at, 1, 1165, 78), &low))
				return false;
			positions = (uint16) ~((uint16) high << 8 | low);
			twice = STD_MUL(usize, 2, positions, 1166, 23);
			at = STD_ADD(usize, at, STD_ADD(usize, 2, twice, 1166, 19), 1166, 13);
			raised = STD_ADD(usize, std_next_multiple_of_usize(strings, 2), 2, 1167, 23);
			twice = STD_MUL(usize, 2, positions, 1167, 57);
			strings = STD_ADD(usize, raised, twice, 1167, 23);
		}
	}
	*result_0 = STD_ADD(usize, STD_ADD(usize, 4, STD_MUL(usize, 4, count, 1170, 15), 1170, 11),
						strings, 1170, 11);
	*result_1 = at;
	return true;
}

/* access_key_decoded_t is the state of a tsvector's decoding across its pad: its entries, strings, and stored form. */
typedef struct access_key_decoded_t
{
	access_key_held_t held;
	const AccessKeyReader *byte;
	StdVec	   *result_0;
	size_t	   *result_1;
	PgPgError  *error;
	bool		ok;
} access_key_decoded_t;

/* access_key_tsvector_decoded_closure_2 is tsvector_decoded's word: the two bytes at a place, inverted, as a u16. */
static bool
access_key_tsvector_decoded_closure_2(const AccessKeyReader *byte, size_t at, uint16 *result)
{
	uint8		high;
	uint8		low;

	if (!access_key_byte(byte, at, &high) ||
		!access_key_byte(byte, STD_ADD(usize, at, 1, 1195, 78), &low))
		return false;
	*result = (uint16) ~((uint16) high << 8 | low);
	return true;
}

/* access_key_le16 appends a u16's little-endian bytes. */
static void
access_key_le16(StdVec *out, uint16 value)
{
	uint8		le[2] = {(uint8) value, (uint8) (value >> 8)};

	std_vec_extend_from_slice_Checked(out, le, sizeof(le), ACCESS_KEY_BYTES);
}

/* access_key_tsvector_decoded_body builds the tsvector's stored form from its key. */
static void
access_key_tsvector_decoded_body(void *frame_)
{
	access_key_decoded_t *frame = frame_;
	const AccessKeyReader *byte = frame->byte;
	size_t		size;
	size_t		used;
	size_t		count;
	StdVec	   *entries;
	StdVec	   *strings;
	StdVec	   *stored;
	size_t		at = 8;

	if (!access_key_tsvector_layout(byte, &size, &used) || !access_key_count_of(byte, 4, &count))
	{
		*frame->error = access_key_corrupt();
		return;
	}
	entries = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	std_vec_reserve_Checked(entries, STD_MUL(usize, 4, count, 1181, 58), ACCESS_KEY_BYTES);
	strings = access_key_held_new(&frame->held, 1, ACCESS_KEY_BYTES);
	for (size_t n = 0; n < count; n++)
	{
		uint8		b;
		bool		has_positions;
		size_t		start;
		uint32		entry;

		if (!access_key_byte_or_corrupt(byte, at, &b, frame->error))
			return;
		has_positions = b == 0;
		at = STD_ADD(usize, at, 1, 1185, 9);
		start = strings->len;
		while (access_key_byte(byte, at, &b) && b != 0)
		{
			std_vec_push_Checked(strings, &b, ACCESS_KEY_BYTES);
			at = STD_ADD(usize, at, 1, 1189, 13);
		}
		at = STD_ADD(usize, at, 1, 1191, 9);
		entry = (uint32) has_positions | (uint32) STD_SUB(usize, strings->len, start, 1192, 50) << 1 |
			(uint32) start << 12;
		access_key_le32(entries, entry);
		if (has_positions)
		{
			uint16		positions;
			uint8		zero = 0;

			if (!access_key_tsvector_decoded_closure_2(byte, at, &positions))
			{
				*frame->error = access_key_corrupt();
				return;
			}
			at = STD_ADD(usize, at, 2, 1197, 13);
			std_vec_resize(strings, std_next_multiple_of_usize(strings->len, 2), &zero, ACCESS_KEY_BYTES);
			access_key_le16(strings, positions);
			for (uint16 p = 0; p < positions; p++)
			{
				uint16		packed;

				if (!access_key_tsvector_decoded_closure_2(byte, at, &packed))
				{
					*frame->error = access_key_corrupt();
					return;
				}
				access_key_le16(strings, (uint16) ((packed & 3) << 14 | packed >> 2));
				at = STD_ADD(usize, at, 2, 1203, 17);
			}
		}
	}
	stored = access_key_held_new(&frame->held, 2, ACCESS_KEY_BYTES);
	std_vec_reserve_Checked(stored, 4, ACCESS_KEY_BYTES);
	access_key_le32(stored, (uint32) count);
	std_vec_extend_from_slice_Checked(stored, entries->ptr, entries->len, ACCESS_KEY_BYTES);
	std_vec_extend_from_slice_Checked(stored, strings->ptr, strings->len, ACCESS_KEY_BYTES);
	if (stored->len != size || at != used)
	{
		*frame->error = access_key_corrupt();
		return;
	}
	*frame->result_0 = access_key_held_take(&frame->held, 2);
	*frame->result_1 = used;
	frame->ok = true;
}

/*
 * access_key_tsvector_decoded builds a tsvector's stored form, a Vec<u8>,
 * from its key, refusing as data corruption a form whose size isn't the
 * one tsvector_layout gives, and answers the form and the key's length.
 */
static bool
access_key_tsvector_decoded(const AccessKeyReader *byte, StdVec *result_0, size_t *result_1, PgPgError *error)
{
	access_key_decoded_t frame = {.byte = byte, .result_0 = result_0, .result_1 = result_1, .error = error};

	std_landing(&frame, access_key_tsvector_decoded_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_tsquery_len_t is tsquery_len's state across its pad: its pending distances and open operators. */
typedef struct access_key_tsquery_len_t
{
	access_key_held_t held;
	const AccessKeyReader *byte;
	bool	   *has_result;
	size_t	   *result;
	PgPgError  *error;
	bool		ok;
} access_key_tsquery_len_t;

/* access_key_tsquery_len_body walks the tsquery's key's items to its end. */
static void
access_key_tsquery_len_body(void *frame_)
{
	access_key_tsquery_len_t *frame = frame_;
	const AccessKeyReader *byte = frame->byte;
	size_t		size;
	size_t		at = 8;
	StdVec	   *distances;
	StdVec	   *open;

	*frame->has_result = false;
	frame->ok = true;
	if (!access_key_count_of(byte, 0, &size))
		return;
	distances = access_key_held_new(&frame->held, 0, sizeof(bool), _Alignof(bool));
	open = access_key_held_new(&frame->held, 1, sizeof(size_t), _Alignof(size_t));
	for (size_t n = 0; n < size; n++)
	{
		uint8		kind;

		if (!access_key_byte(byte, at, &kind))
			return;
		if ((uint8) ~kind == ACCESS_KEY_QI_VAL)
		{
			uint8		b;

			at = STD_ADD(usize, at, 5, 1227, 17);
			for (;;)
			{
				if (!access_key_byte(byte, at, &b))
					return;
				if (b == 0)
					break;
				at = STD_ADD(usize, at, 1, 1232, 36);
			}
			at = STD_ADD(usize, at, 1, 1235, 17);
			while (open->len > 0)
			{
				size_t	   *left = &STD_VEC_AT(open, size_t, open->len - 1);
				bool		distance;

				if (*left > 1)
				{
					*left = STD_SUB(usize, *left, 1, 1238, 25);
					break;
				}
				open->len--;
				if (std_vec_pop(distances, &distance, sizeof(bool)) && distance)
					at = STD_ADD(usize, at, 2, 1243, 25);
			}
		}
		else if ((uint8) ~kind == ACCESS_KEY_QI_OPR)
		{
			uint8		oper;
			uint8		children;
			size_t		opened;
			bool		phrase;

			if (!access_key_byte(byte, STD_ADD(usize, at, 1, 1248, 58), &oper) ||
				!access_key_byte(byte, STD_ADD(usize, at, 2, 1248, 72), &children))
				return;
			at = STD_ADD(usize, at, 3, 1249, 17);
			opened = (uint8) ~children;
			phrase = (uint8) ~oper == ACCESS_KEY_OP_PHRASE;
			std_vec_push_Checked(open, &opened, sizeof(size_t), _Alignof(size_t));
			std_vec_push_Checked(distances, &phrase, sizeof(bool), _Alignof(bool));
		}
		else
		{
			*frame->error = access_key_corrupt();
			frame->ok = false;
			return;
		}
	}
	*frame->has_result = true;
	*frame->result = at;
}

/*
 * access_key_tsquery_len answers the length of a tsquery's key, as
 * Doltgrust's TsQuery::key_len reads it, or none when the bytes stop inside
 * the key.
 */
static bool
access_key_tsquery_len(const AccessKeyReader *byte, bool *has_result, size_t *result, PgPgError *error)
{
	access_key_tsquery_len_t frame = {.byte = byte, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, access_key_tsquery_len_body, access_key_held_pad);
	access_key_held_pad(&frame);
	return frame.ok;
}

/* access_key_flip returns the flip of a column's order, every byte inverted for a descending one. */
static uint8
access_key_flip(EncodingKeyOrder order)
{
	return order.descending ? 0xff : 0;
}

/*
 * access_key_decoded_max answers the most bytes a varlena value read back
 * from a key column at the start of some bytes can take, and zero for a
 * type whose values are words or of a fixed length, or whose keys aren't
 * exact.
 */
bool
access_key_decoded_max(const AccessKeyKeyType *key_type, EncodingKeyOrder order, const uint8 *key, size_t key_len,
					   size_t *result, PgPgError *error)
{
	AccessKeyExactTag exact;
	size_t		more;

	*result = 0;
	if (!access_key_exact(key_type, &exact, &more))
		return true;
	if (exact == ACCESS_KEY_EXACT_VARLENA)
		*result = STD_ADD(usize, key_len, more, 1137, 39);
	else if (exact == ACCESS_KEY_EXACT_TSVECTOR && key_len > 0 && key[0] == ENCODING_KEY_VALUE)
	{
		AccessKeyReader byte = {key + 1, key_len - 1, access_key_flip(order)};
		size_t		used;

		if (!access_key_tsvector_layout(&byte, result, &used))
			return access_key_fail(access_key_corrupt(), error);
	}
	return true;
}

/*
 * ACCESS_KEY_CUT_MORE is what a cut key column counts for past its bound where
 * a bound is worked out, a constant of the format.
 */
#define ACCESS_KEY_CUT_MORE 33

/* ACCESS_KEY_ADDRESS_LEN is the length of the address a cut key column ends with. */
#define ACCESS_KEY_ADDRESS_LEN sizeof(ChunksAddressAddress)

static bool access_key_fixed_len(const AccessKeyKeyType *key_type, size_t *result);

/*
 * access_key_max_len answers the most bytes a key column of a type takes, its
 * marker included, or false for a type whose keys have no bound.
 */
static bool
access_key_max_len(const AccessKeyKeyType *key_type, size_t *result)
{
	size_t		len;

	if (access_key_fixed_len(key_type, &len))
	{
		*result = STD_ADD(usize, 1, len, 1272, 21);
		return true;
	}
	switch (key_type->typid)
	{
		case FLOAT4OID:
			*result = 5;
			return true;
		case FLOAT8OID:
			*result = 9;
			return true;
		case INTERVALOID:
			*result = 17;
			return true;
		case NAMEOID:
			{
				AccessKeyCollation bytes = {.tag = ACCESS_KEY_COLLATION_BYTES};

				if (!access_key_Collation_PartialEq_eq(&key_type->collation, &bytes))
					return false;
			}
			*result = 1 + ACCESS_KEY_NAME_LEN;
			return true;
		case INETOID:
		case CIDROID:
			*result = 1 + 1 + (2 * 16 + 2 + 4) + 16;
			return true;
	}
	return false;
}

/* access_key_KeyTypes_at returns a run of key types' key type at a position. */
static const AccessKeyKeyType *
access_key_KeyTypes_at(AccessKeyKeyTypes key, size_t i)
{
	return (const AccessKeyKeyType *) ((const char *) key.items + i * key.stride + key.offset);
}

/*
 * access_key_table_bound returns the bound of a table's own tree: how many
 * bytes of its encoding a primary key column keeps in a row's key before it
 * is cut, a function of the key's types alone.
 */
size_t
access_key_table_bound(AccessKeyKeyTypes key)
{
	size_t		shorts = 0;
	size_t		longs = 0;

	for (size_t i = 0; i < key.len; i++)
	{
		size_t		len;

		if (access_key_max_len(access_key_KeyTypes_at(key, i), &len) && len <= ACCESS_KEY_CUT_MORE)
			shorts = STD_ADD(usize, shorts, len, 1292, 45);
		else
			longs = STD_ADD(usize, longs, 1, 1293, 18);
	}
	if (longs == 0)
		return CHUNKS_MAX_ENTRY / 2;
	return std_saturating_sub_usize(STD_DIV(usize, std_saturating_sub_usize(CHUNKS_MAX_ENTRY / 2, shorts), longs,
											1299, 6),
									ACCESS_KEY_CUT_MORE);
}

/*
 * access_key_table_key_most returns the most bytes a table's own key takes:
 * each column its type keeps short at its most, and each other at its
 * table's bound with CUT_MORE bytes past it.
 */
size_t
access_key_table_key_most(AccessKeyKeyTypes key)
{
	size_t		bound = access_key_table_bound(key);
	size_t		sum = 0;

	for (size_t i = 0; i < key.len; i++)
	{
		size_t		len;

		if (!access_key_max_len(access_key_KeyTypes_at(key, i), &len) || len > ACCESS_KEY_CUT_MORE)
			len = STD_ADD(usize, bound, ACCESS_KEY_CUT_MORE, 1309, 14);
		sum = std_add_usize(sum, len, STD_ACCUM_FILE, 206, 1);
	}
	return sum;
}

/*
 * ACCESS_KEY_REFERENCE_LEN is the most bytes a reference to a value out of
 * line takes in a value's tuple, a constant of the format.
 */
#define ACCESS_KEY_REFERENCE_LEN (5 + ACCESS_KEY_ADDRESS_LEN)

/* access_key_index_bound_t is index_bound's state across its pad: its value's fields' bounds. */
typedef struct access_key_index_bound_t
{
	access_key_held_t held;
	const size_t *parts;
	size_t		parts_len;
	size_t		originals;
	size_t		result;
} access_key_index_bound_t;

/* access_key_index_bound_body works out the most the value's tuple takes. */
static void
access_key_index_bound_body(void *frame_)
{
	access_key_index_bound_t *frame = frame_;
	StdVec	   *fields = access_key_held_new(&frame->held, 0, sizeof(StdUsizeOption), _Alignof(StdUsizeOption));

	std_vec_reserve_Checked(fields, frame->parts_len,
							sizeof(StdUsizeOption), _Alignof(StdUsizeOption));
	for (size_t i = 0; i < frame->parts_len; i++)
	{
		StdUsizeOption field = {true, Min(frame->parts[i], ACCESS_KEY_REFERENCE_LEN)};

		std_vec_push_Checked(fields, &field, sizeof(field), _Alignof(StdUsizeOption));
	}
	for (size_t i = 0; i < frame->originals; i++)
	{
		StdUsizeOption field = {true, ACCESS_KEY_REFERENCE_LEN};

		std_vec_push_Checked(fields, &field, sizeof(field), _Alignof(StdUsizeOption));
	}
	frame->result = encoding_tuple_written_len(fields->ptr, fields->len, true, false, 0);
}

/*
 * access_key_index_bound returns the bound of an index's tree: how many
 * bytes of its encoding a key column keeps in an entry's key before it is
 * cut, a function of the definition alone.
 */
size_t
access_key_index_bound(AccessKeyKeyTypes key, size_t row_key, size_t prefix, const size_t *parts, size_t parts_len)
{
	size_t		shorts = 0;
	size_t		longs = 0;
	access_key_index_bound_t frame = {.parts = parts, .parts_len = parts_len};
	size_t		taken;

	for (size_t i = 0; i < key.len; i++)
	{
		const AccessKeyKeyType *key_type = access_key_KeyTypes_at(key, i);
		size_t		len;

		if (access_key_max_len(key_type, &len) && len <= ACCESS_KEY_CUT_MORE)
			shorts = STD_ADD(usize, shorts, len, 1335, 45);
		else
		{
			longs = STD_ADD(usize, longs, 1, 1337, 17);
			frame.originals = STD_ADD(usize, frame.originals, (size_t) access_key_is_exact(key_type),
									  1338, 17);
		}
	}
	if (longs == 0)
		return CHUNKS_MAX_ENTRY;
	std_landing(&frame, access_key_index_bound_body, access_key_held_pad);
	access_key_held_pad(&frame);
	taken = STD_ADD(usize, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, prefix, 1351, 31);
	taken = STD_ADD(usize, taken, shorts, 1351, 31);
	taken = STD_ADD(usize, taken, row_key, 1351, 31);
	taken = STD_ADD(usize, taken, frame.result, 1351, 31);
	return std_saturating_sub_usize(STD_DIV(usize, std_saturating_sub_usize(CHUNKS_MAX_ENTRY, taken), longs,
											1351, 6),
									ACCESS_KEY_CUT_MORE);
}

/*
 * access_key_can_be_cut reports whether a key column of a type can be cut at a
 * bound: its type doesn't keep its keys within the bound.
 */
bool
access_key_can_be_cut(const AccessKeyKeyType *key_type, size_t bound)
{
	size_t		len;

	return !access_key_max_len(key_type, &len) || len > bound;
}

/*
 * access_key_encode_within appends a value of a key type, or NULL, as a key
 * column in the order, cut at a bound: an encoding longer than the bound is
 * its first bytes up to the bound and then the address of the whole
 * encoding, and a comparator column's is OUT_OF_LINE and the name of the
 * blob of its value's stored bytes. It answers whether it cut the column.
 */
bool
access_key_encode_within(StdVec *out, AccessKeySortKeys sorter, AccessKeyNames names,
						 const AccessKeyKeyType *key_type, bool has_value, AccessKeyValue value,
						 EncodingKeyOrder order, size_t bound, bool outside, bool *result, PgPgError *error)
{
	bool		compared = has_value && value.tag == ACCESS_KEY_VALUE_BYTES &&
		(key_type->inner.tag == ACCESS_KEY_INNER_COMPARED || outside);
	size_t		start = out->len;
	ChunksAddressAddress address;

	if (!access_key_encode(out, sorter, key_type, has_value, value, order, error))
		return false;
	if (STD_SUB(usize, out->len, start, 1381, 8) <= bound)
	{
		*result = false;
		return true;
	}
	if (compared)
	{
		uint8		name[32];

		out->len = start;
		access_key_push(out, ENCODING_KEY_OUT_OF_LINE);
		names.ops->name(names.self, value.u.bytes.ptr, value.u.bytes.len, name);
		std_vec_extend_from_slice_Checked(out, name, sizeof(name), ACCESS_KEY_BYTES);
		*result = true;
		return true;
	}
	std_range_from_check(start, out->len, RUST_FILE, 1390, 40);
	address = chunks_address_Address_of((const uint8 *) out->ptr + start, out->len - start);
	out->len = Min(out->len, STD_ADD(usize, start, bound, 1391, 18));
	std_vec_extend_from_slice_Checked(out, address.f0, sizeof(address.f0), ACCESS_KEY_BYTES);
	*result = true;
	return true;
}

/* access_key_unescape_t is unescape's state across its pad: the bytes read back so far. */
typedef struct access_key_unescape_t
{
	access_key_held_t held;
	const AccessKeyReader *byte;
	size_t	   *result_1;
	PgPgError  *error;
	bool		ok;
} access_key_unescape_t;

/* access_key_unescape_body reads escaped bytes to their end. */
static void
access_key_unescape_body(void *frame_)
{
	access_key_unescape_t *frame = frame_;
	StdVec	   *value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
	size_t		i = 0;

	for (;;)
	{
		uint8		b;
		uint8		next;
		uint8		zero = 0;

		if (!access_key_byte_or_corrupt(frame->byte, i, &b, frame->error) ||
			!access_key_byte_or_corrupt(frame->byte, STD_ADD(usize, i, 1, 1401, 31), &next, frame->error))
			return;
		if (b == 0 && next == 1)
		{
			*frame->result_1 = STD_ADD(usize, i, 2, 1402, 41);
			frame->ok = true;
			return;
		}
		if (b == 0 && next == 0xff)
		{
			std_vec_push_Checked(value, &zero, ACCESS_KEY_BYTES);
			i = STD_ADD(usize, i, 2, 1405, 17);
		}
		else
		{
			std_vec_push_Checked(value, &b, ACCESS_KEY_BYTES);
			i = STD_ADD(usize, i, 1, 1409, 17);
		}
	}
}

/*
 * access_key_unescape reads bytes escaped as key::bytes escapes them,
 * answering them, a Vec<u8>, and the length of their escaped form, its end
 * included.
 */
static bool
access_key_unescape(const AccessKeyReader *byte, StdVec *result_0, size_t *result_1, PgPgError *error)
{
	access_key_unescape_t frame = {.byte = byte, .result_1 = result_1, .error = error};

	std_landing(&frame, access_key_unescape_body, access_key_held_pad);
	if (frame.ok)
		*result_0 = access_key_held_take(&frame.held, 0);
	access_key_held_pad(&frame);
	return frame.ok;
}

/*
 * access_key_fixed_at answers the big-endian word of len bytes at a place of a
 * reader's bytes, flipped, or corruption past them.
 */
static bool
access_key_fixed_at(const AccessKeyReader *byte, size_t at, size_t len, uint64 *result, PgPgError *error)
{
	size_t		end;
	uint64		word = 0;

	if (!std_checked_add_usize(at, len, &end) || at > end || end > byte->len)
		return access_key_fail(access_key_corrupt(), error);
	for (size_t i = at; i < end; i++)
		word = word << 8 | (uint64) (uint8) (byte->bytes[i] ^ byte->flip);
	*result = word;
	return true;
}

static bool access_key_escaped_end(const AccessKeyReader *byte, bool *has_result, size_t *result, PgPgError *error);

/* access_key_decode_t is decode's state across its pad: the value's bytes read back so far. */
typedef struct access_key_decode_t
{
	access_key_held_t held;
	const AccessKeyKeyType *key_type;
	AccessKeyReader byte;
	AccessKeyDecoded *result_0;
	size_t		used;
	PgPgError  *error;
	bool		ok;
} access_key_decode_t;

/* access_key_decode_bytes sets decode's value to the bytes it holds, which the answer takes. */
static void
access_key_decode_bytes(access_key_decode_t *frame)
{
	frame->result_0->tag = ACCESS_KEY_DECODED_BYTES;
	frame->result_0->u.bytes = access_key_held_take(&frame->held, 0);
}

/* access_key_decode_word sets decode's value to a datum's word. */
static void
access_key_decode_word(access_key_decode_t *frame, uint64 word, size_t used)
{
	frame->result_0->tag = ACCESS_KEY_DECODED_WORD;
	frame->result_0->u.word = word;
	frame->used = used;
}

/* access_key_decode_body reads the column's value by its key type. */
static void
access_key_decode_body(void *frame_)
{
	access_key_decode_t *frame = frame_;
	const AccessKeyReader *byte = &frame->byte;
	PgPgError  *error = frame->error;
	uint64		fixed;
	uint8		b;

	if (frame->key_type->inner.tag == ACCESS_KEY_INNER_COMPARED)
	{
		if (!access_key_unescape(byte, &frame->held.vec[0], &frame->used, error))
			return;
		frame->held.size[0] = frame->held.align[0] = 1;
		frame->held.live[0] = true;
		if (frame->key_type->inner.u.by_value)
		{
			uint64		word = 0;

			if (frame->held.vec[0].len != 8)
			{
				*error = access_key_corrupt();
				return;
			}
			for (int i = 7; i >= 0; i--)
				word = word << 8 | ((const uint8 *) frame->held.vec[0].ptr)[i];
			access_key_decode_word(frame, word, frame->used);
		}
		else
			access_key_decode_bytes(frame);
		frame->ok = true;
		return;
	}
	switch (frame->key_type->typid)
	{
		case BOOLOID:
			if (!access_key_byte_or_corrupt(byte, 0, &b, error))
				return;
			access_key_decode_word(frame, pg_word_from_bool(b != 0), 1);
			break;
		case CHAROID:
			if (!access_key_byte_or_corrupt(byte, 0, &b, error))
				return;
			access_key_decode_word(frame, pg_word_from_char(b), 1);
			break;
		case INT2OID:
			if (!access_key_fixed_at(byte, 0, 2, &fixed, error))
				return;
			access_key_decode_word(frame, pg_word_from_int16((int16) ((uint16) fixed ^ 0x8000)), 2);
			break;
		case INT4OID:
		case DATEOID:
			if (!access_key_fixed_at(byte, 0, 4, &fixed, error))
				return;
			access_key_decode_word(frame, pg_word_from_int32((int32) ((uint32) fixed ^ 0x80000000)), 4);
			break;
		case INT8OID:
		case MONEYOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			if (!access_key_fixed_at(byte, 0, 8, &fixed, error))
				return;
			access_key_decode_word(frame, pg_word_from_int64((int64) (fixed ^ ((uint64) 1 << 63))), 8);
			break;
		case PG_LSNOID:
		case XID8OID:
			if (!access_key_fixed_at(byte, 0, 8, &fixed, error))
				return;
			access_key_decode_word(frame, fixed, 8);
			break;
		case OIDOID:
			if (!access_key_fixed_at(byte, 0, 4, &fixed, error))
				return;
			access_key_decode_word(frame, pg_word_from_oid((uint32) fixed), 4);
			break;
		case UUIDOID:
		case MACADDROID:
		case MACADDR8OID:
			{
				size_t		len = frame->key_type->typid == UUIDOID ? 16 :
					frame->key_type->typid == MACADDROID ? 6 : 8;
				StdVec	   *value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);

				for (size_t i = 0; i < len; i++)
				{
					if (!access_key_byte_or_corrupt(byte, i, &b, error))
						return;
					std_vec_push_Checked(value, &b, ACCESS_KEY_BYTES);
				}
				access_key_decode_bytes(frame);
				frame->used = len;
			}
			break;
		case TEXTOID:
		case VARCHAROID:
		case NAMEOID:
			{
				size_t		sorted = 0;
				StdVec	   *value;
				uint8		zero = 0;

				if (frame->key_type->collation.tag == ACCESS_KEY_COLLATION_SORTED)
				{
					bool		has;

					if (!access_key_escaped_end(byte, &has, &sorted, error))
						return;
					if (!has)
					{
						*error = access_key_corrupt();
						return;
					}
				}
				value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
				for (;;)
				{
					if (!access_key_byte_or_corrupt(byte, STD_ADD(usize, sorted, value->len, 1465, 28),
													&b, error))
						return;
					if (b == 0)
						break;
					std_vec_push_Checked(value, &b, ACCESS_KEY_BYTES);
				}
				frame->used = STD_ADD(usize, STD_ADD(usize, sorted, value->len, 1470, 24), 1,
									  1470, 24);
				if (frame->key_type->typid == NAMEOID)
				{
					if (value->len >= ACCESS_KEY_NAME_LEN)
					{
						*error = access_key_corrupt();
						return;
					}
					std_vec_resize(value, ACCESS_KEY_NAME_LEN, &zero, ACCESS_KEY_BYTES);
				}
				access_key_decode_bytes(frame);
			}
			break;
		case TIDOID:
			{
				uint8		offset_high;
				uint8		offset_low;
				uint8		tid[6];

				if (!access_key_fixed_at(byte, 0, 4, &fixed, error) ||
					!access_key_byte_or_corrupt(byte, 4, &offset_high, error) ||
					!access_key_byte_or_corrupt(byte, 5, &offset_low, error))
					return;
				tid[0] = (uint8) (fixed >> 16);
				tid[1] = (uint8) (fixed >> 24);
				tid[2] = (uint8) fixed;
				tid[3] = (uint8) (fixed >> 8);
				tid[4] = offset_low;
				tid[5] = offset_high;
				std_slice_to_vec_Checked(&frame->held.vec[0], tid, sizeof(tid), ACCESS_KEY_BYTES);
				frame->held.size[0] = frame->held.align[0] = 1;
				frame->held.live[0] = true;
				access_key_decode_bytes(frame);
				frame->used = 6;
			}
			break;
		case TIMETZOID:
			{
				uint64		zone_word;
				int32		zone;
				int64		time;
				uint8		timetz[12];

				if (!access_key_fixed_at(byte, 8, 4, &zone_word, error))
					return;
				zone = (int32) ((uint32) zone_word ^ 0x80000000);
				if (!access_key_fixed_at(byte, 0, 8, &fixed, error))
					return;
				time = (int64) ((uint64) (int64) (fixed ^ ((uint64) 1 << 63)) -
								(uint64) STD_MUL(i64, (int64) zone, 1000000, 1486, 70));
				for (int i = 0; i < 8; i++)
					timetz[i] = (uint8) ((uint64) time >> (8 * i));
				for (int i = 0; i < 4; i++)
					timetz[8 + i] = (uint8) ((uint32) zone >> (8 * i));
				std_slice_to_vec_Checked(&frame->held.vec[0], timetz, sizeof(timetz), ACCESS_KEY_BYTES);
				frame->held.size[0] = frame->held.align[0] = 1;
				frame->held.live[0] = true;
				access_key_decode_bytes(frame);
				frame->used = 12;
			}
			break;
		case INETOID:
		case CIDROID:
			{
				uint8		family;
				AccessKeyReader shifted = *byte;
				StdVec		network_bytes;
				size_t		network;
				uint64		mask_word;
				uint8		mask;
				size_t		len;
				StdVec	   *value;

				if (!access_key_byte_or_corrupt(byte, 0, &family, error))
					return;
				shifted.from = 1;
				shifted.line = 1491;
				shifted.col = 50;
				if (!access_key_unescape(&shifted, &network_bytes, &network, error))
					return;
				std_vec_free_Checked(&network_bytes, ACCESS_KEY_BYTES);
				if (!access_key_fixed_at(byte, STD_ADD(usize, 1, network, 1492, 33), 4, &mask_word,
										 error))
					return;
				mask = (uint8) mask_word;
				if (!access_key_address_len(family, mask, &len))
				{
					*error = access_key_corrupt();
					return;
				}
				value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
				std_vec_push_Checked(value, &family, ACCESS_KEY_BYTES);
				std_vec_push_Checked(value, &mask, ACCESS_KEY_BYTES);
				for (size_t i = 0; i < len; i++)
				{
					size_t		at = STD_ADD(usize, STD_ADD(usize, 5, network, 1496, 33), i,
											 1496, 33);

					if (!access_key_byte_or_corrupt(byte, at, &b, error))
						return;
					std_vec_push_Checked(value, &b, ACCESS_KEY_BYTES);
				}
				frame->used = STD_ADD(usize, STD_ADD(usize, 5, network, 1498, 37), len,
									  1498, 37);
				access_key_decode_bytes(frame);
			}
			break;
		case BITOID:
		case VARBITOID:
			{
				StdVec		bits;
				size_t		used;
				uint64		len;
				StdVec	   *value;

				if (!access_key_unescape(byte, &bits, &used, error))
					return;
				frame->held.vec[1] = bits;
				frame->held.size[1] = frame->held.align[1] = 1;
				frame->held.live[1] = true;
				if (!access_key_fixed_at(byte, used, 4, &len, error))
					return;
				value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
				std_vec_reserve_Checked(value, 4, ACCESS_KEY_BYTES);
				access_key_le32(value, (uint32) len);
				std_vec_extend_from_slice_Checked(value, bits.ptr, bits.len, ACCESS_KEY_BYTES);
				std_vec_free_Checked(&frame->held.vec[1], ACCESS_KEY_BYTES);
				frame->held.live[1] = false;
				frame->used = STD_ADD(usize, used, 4, 1503, 37);
				access_key_decode_bytes(frame);
			}
			break;
		case OIDVECTOROID:
			{
				uint32		count;
				StdVec	   *value;
				uint32		fields[5] = {1, 0, OIDOID, 0, 0};

				if (!access_key_fixed_at(byte, 0, 4, &fixed, error))
					return;
				count = (uint32) fixed;
				fields[3] = count;
				value = access_key_held_new(&frame->held, 0, ACCESS_KEY_BYTES);
				for (int i = 0; i < 5; i++)
					access_key_le32(value, fields[i]);
				for (size_t i = 1; i <= (size_t) count; i++)
				{
					if (!access_key_fixed_at(byte, STD_MUL(usize, 4, i, 1512, 52), 4, &fixed, error))
						return;
					access_key_le32(value, (uint32) fixed);
				}
				frame->used = STD_ADD(usize, 4, STD_MUL(usize, 4, (size_t) count, 1514, 41),
									  1514, 37);
				access_key_decode_bytes(frame);
			}
			break;
		case BYTEAOID:
			if (!access_key_unescape(byte, &frame->held.vec[0], &frame->used, error))
				return;
			frame->held.size[0] = frame->held.align[0] = 1;
			frame->held.live[0] = true;
			access_key_decode_bytes(frame);
			break;
		case TSVECTOROID:
			if (!access_key_tsvector_decoded(byte, &frame->held.vec[0], &frame->used, error))
				return;
			frame->held.size[0] = frame->held.align[0] = 1;
			frame->held.live[0] = true;
			access_key_decode_bytes(frame);
			break;
		default:
			*error = access_key_unsupported(frame->key_type->typid);
			return;
	}
	frame->ok = true;
}

/*
 * access_key_decode reads a key column of an exact key type in the order,
 * answering the value, or none for NULL, and the bytes after the column.
 */
bool
access_key_decode(const uint8 *key, size_t key_len, const AccessKeyKeyType *key_type, EncodingKeyOrder order,
				  bool *has_result_0, AccessKeyDecoded *result_0, StdBytes *result_1, PgPgError *error)
{
	access_key_decode_t frame = {.key_type = key_type, .result_0 = result_0, .error = error};

	if (key_len == 0)
		return access_key_fail(access_key_corrupt(), error);
	if (key[0] == ENCODING_KEY_NULL_FIRST || key[0] == ENCODING_KEY_NULL_LAST)
	{
		*has_result_0 = false;
		result_1->ptr = key + 1;
		result_1->len = key_len - 1;
		return true;
	}
	if (key[0] != ENCODING_KEY_VALUE)
		return access_key_fail(access_key_corrupt(), error);
	frame.byte.bytes = key + 1;
	frame.byte.len = key_len - 1;
	frame.byte.flip = access_key_flip(order);
	std_landing(&frame, access_key_decode_body, access_key_held_pad);
	access_key_held_pad(&frame);
	if (!frame.ok)
		return false;
	if (key_type->inner.tag == ACCESS_KEY_INNER_COMPARED)
		std_range_from_check(frame.used, frame.byte.len, RUST_FILE, 1438, 34);
	else
		std_range_from_check(frame.used, frame.byte.len, RUST_FILE, 1526, 23);
	*has_result_0 = true;
	result_1->ptr = frame.byte.bytes + frame.used;
	result_1->len = frame.byte.len - frame.used;
	return true;
}

/*
 * access_key_fixed_len answers how many bytes follow the marker of a key
 * column's value for a type whose values all take the same length.
 */
static bool
access_key_fixed_len(const AccessKeyKeyType *key_type, size_t *result)
{
	switch (key_type->typid)
	{
		case BOOLOID:
		case CHAROID:
			*result = 1;
			return true;
		case INT2OID:
			*result = 2;
			return true;
		case INT4OID:
		case DATEOID:
		case OIDOID:
			*result = 4;
			return true;
		case TIDOID:
		case MACADDROID:
			*result = 6;
			return true;
		case INT8OID:
		case MONEYOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case PG_LSNOID:
		case XID8OID:
		case MACADDR8OID:
			*result = 8;
			return true;
		case TIMETZOID:
			*result = 12;
			return true;
		case UUIDOID:
			*result = 16;
			return true;
	}
	return false;
}

/*
 * access_key_escaped_end answers the length of bytes escaped as key::bytes
 * escapes them, their end included, or none when the bytes stop before
 * their end.
 */
static bool
access_key_escaped_end(const AccessKeyReader *byte, bool *has_result, size_t *result, PgPgError *error)
{
	size_t		i = 0;

	for (;;)
	{
		uint8		b;
		uint8		next;
		bool		has_b = access_key_byte(byte, i, &b);
		bool		has_next = access_key_byte(byte, STD_ADD(usize, i, 1, 1549, 30), &next);

		if (!has_b || (b == 0 && !has_next))
		{
			*has_result = false;
			return true;
		}
		if (b == 0 && next == 1)
		{
			*has_result = true;
			*result = STD_ADD(usize, i, 2, 1551, 50);
			return true;
		}
		if (b == 0 && next == 0xff)
			i = STD_ADD(usize, i, 2, 1552, 38);
		else if (b == 0)
			return access_key_fail(access_key_corrupt(), error);
		else
			i = STD_ADD(usize, i, 1, 1554, 29);
	}
}

/* access_key_find_zero answers the first place from a start before an end whose byte is 0, plus one, or none. */
static bool
access_key_find_zero(const AccessKeyReader *byte, size_t start, size_t end, uint32 line, uint32 col,
					 size_t *result)
{
	for (size_t i = start; i < end; i++)
	{
		uint8		b;

		if (access_key_byte(byte, i, &b) && b == 0)
		{
			*result = std_add_usize(i, 1, RUST_FILE, line, col);
			return true;
		}
	}
	return false;
}

/*
 * access_key_end answers the length of the key column of a key type at the
 * start of some bytes, or none when the bytes stop before the column's end,
 * which every type's key shows by itself.
 */
static bool
access_key_end(const uint8 *key, size_t key_len, const AccessKeyKeyType *key_type, EncodingKeyOrder order,
			   bool *has_result, size_t *result, PgPgError *error)
{
	AccessKeyReader byte;
	bool		has = false;
	size_t		used = 0;
	size_t		len;

	*has_result = false;
	if (key_len == 0)
		return true;
	if (key[0] == ENCODING_KEY_NULL_FIRST || key[0] == ENCODING_KEY_NULL_LAST)
	{
		*has_result = true;
		*result = 1;
		return true;
	}
	if (key[0] != ENCODING_KEY_VALUE)
		return access_key_fail(access_key_corrupt(), error);
	byte = (AccessKeyReader)
	{
		key + 1, key_len - 1, access_key_flip(order)
	};
	if (key_type->inner.tag == ACCESS_KEY_INNER_ENUM)
	{
		*has_result = 1 + 4 <= key_len;
		if (*has_result)
			*result = 1 + 4;
		return true;
	}
	if (key_type->inner.tag != ACCESS_KEY_INNER_NONE)
	{
		if (!access_key_escaped_end(&byte, &has, &used, error))
			return false;
		*has_result = has && used <= byte.len;
		if (*has_result)
			*result = STD_ADD(usize, 1, used, 1576, 84);
		return true;
	}
	if (access_key_fixed_len(key_type, &len))
	{
		has = true;
		used = len;
	}
	else
	{
		switch (key_type->typid)
		{
			case FLOAT4OID:
				has = true;
				used = 4;
				break;
			case FLOAT8OID:
				has = true;
				used = 8;
				break;
			case INTERVALOID:
				has = true;
				used = 16;
				break;
			case TEXTOID:
			case VARCHAROID:
			case NAMEOID:
			case BPCHAROID:
				if (key_type->collation.tag == ACCESS_KEY_COLLATION_BYTES)
					has = access_key_find_zero(&byte, 0, byte.len, 1584, 87, &used);
				else if (!key_type->collation.deterministic)
				{
					if (!access_key_escaped_end(&byte, &has, &used, error))
						return false;
				}
				else
				{
					size_t		sorted;

					if (!access_key_escaped_end(&byte, &has, &sorted, error))
						return false;
					if (has)
						has = access_key_find_zero(&byte, sorted, byte.len, 1587, 113, &used);
				}
				break;
			case BYTEAOID:
			case JSONBOID:
				if (!access_key_escaped_end(&byte, &has, &used, error))
					return false;
				break;
			case TSVECTOROID:
				{
					size_t		size;

					has = access_key_tsvector_layout(&byte, &size, &used);
				}
				break;
			case TSQUERYOID:
				if (!access_key_tsquery_len(&byte, &has, &used, error))
					return false;
				break;
			case BITOID:
			case VARBITOID:
				if (!access_key_escaped_end(&byte, &has, &used, error))
					return false;
				if (has)
					used = STD_ADD(usize, used, 4, 1593, 69);
				break;
			case INETOID:
			case CIDROID:
				{
					uint8		family;
					bool		has_family = access_key_byte(&byte, 0, &family);
					AccessKeyReader shifted = byte;
					size_t		network;
					bool		has_network;
					uint8		mask;
					size_t		address;

					shifted.from = 1;
					shifted.line = 1594;
					shifted.col = 75;
					if (!access_key_escaped_end(&shifted, &has_network, &network, error))
						return false;
					if (!has_family || !has_network)
						break;
					if (!access_key_byte(&byte, STD_ADD(usize, STD_ADD(usize, 1, network, 1595, 57), 3,
														1595, 57),
										 &mask))
						break;
					used = STD_ADD(usize, STD_ADD(usize, 1, network, 1596, 36), 4,
								   1596, 36);
					if (!access_key_address_len(family, mask, &address))
						return access_key_fail(access_key_corrupt(), error);
					used = STD_ADD(usize, used, address, 1596, 36);
					has = true;
				}
				break;
			case OIDVECTOROID:
				{
					uint8		count[4];

					if (!access_key_byte(&byte, 0, &count[0]) || !access_key_byte(&byte, 1, &count[1]) ||
						!access_key_byte(&byte, 2, &count[2]) || !access_key_byte(&byte, 3, &count[3]))
						break;
					used = STD_ADD(usize, 4,
								   STD_MUL(usize, 4,
										   (size_t) ((uint32) count[0] << 24 | (uint32) count[1] << 16 |
													 (uint32) count[2] << 8 | count[3]),
										   1602, 62),
								   1602, 58);
					has = true;
				}
				break;
			case NUMERICOID:
				{
					uint8		class;

					if (!access_key_byte(&byte, 0, &class))
						break;
					if (class == 0x02 || class == 0x04)
					{
						AccessKeyReader digit = byte;
						uint8		first;
						size_t		exponent;

						digit.flip ^= class == 0x02 ? 0xff : 0;
						if (!access_key_byte(&digit, 1, &first))
							break;
						if (first >= 0xc0)
							exponent = STD_ADD(usize, 1, (size_t) STD_SUB(u8, first, 0xc0, 1611, 51),
											   1611, 35);
						else if (first < 0x40)
							exponent = STD_ADD(usize, 1, (size_t) STD_SUB(u8, 0x40, first, 1612, 51),
											   1612, 35);
						else
							exponent = 1;
						has = access_key_find_zero(&digit, STD_ADD(usize, 1, exponent, 1615, 47),
												   byte.len, 1615, 112, &used);
					}
					else
					{
						has = true;
						used = 1;
					}
				}
				break;
			default:
				return access_key_fail(access_key_unsupported(key_type->typid), error);
		}
	}
	*has_result = has && used <= byte.len;
	if (*has_result)
		*result = STD_ADD(usize, 1, used, 1621, 59);
	return true;
}

/*
 * access_key_skip_within answers the bytes after a key column of a key type
 * that encode_within wrote at a bound, and whether the column is cut.
 */
bool
access_key_skip_within(const uint8 *key, size_t key_len, const AccessKeyKeyType *key_type, EncodingKeyOrder order,
					   size_t bound, StdBytes *result_0, bool *result_1, PgPgError *error)
{
	bool		has;
	size_t		len;
	size_t		after;

	if (key_len > 0 && key[0] == ENCODING_KEY_OUT_OF_LINE)
	{
		if (1 + ACCESS_KEY_ADDRESS_LEN > key_len)
			return access_key_fail(access_key_corrupt(), error);
		result_0->ptr = key + 1 + ACCESS_KEY_ADDRESS_LEN;
		result_0->len = key_len - (1 + ACCESS_KEY_ADDRESS_LEN);
		*result_1 = true;
		return true;
	}
	std_range_to_check(Min(key_len, bound), key_len, RUST_FILE, 1637, 16);
	if (!access_key_end(key, Min(key_len, bound), key_type, order, &has, &len, error))
		return false;
	if (has)
	{
		std_range_from_check(len, key_len, RUST_FILE, 1638, 27);
		result_0->ptr = key + len;
		result_0->len = key_len - len;
		*result_1 = false;
		return true;
	}
	if (!std_checked_add_usize(bound, ACCESS_KEY_ADDRESS_LEN, &after) || after > key_len)
		return access_key_fail(access_key_corrupt(), error);
	result_0->ptr = key + after;
	result_0->len = key_len - after;
	*result_1 = true;
	return true;
}

#ifdef DOLTGRES_TEST_ACCESS

/* access_key_Reversed_SortKeys_sort_key is Reversed's SortKeys: a text's bytes reversed. */
static bool
access_key_Reversed_SortKeys_sort_key(const void *self, uint32 collation, const uint8 *str, size_t str_len,
									  StdVec *result, PgPgError *error)
{
	std_vec_with_capacity_Checked(result, str_len, ACCESS_KEY_BYTES);
	for (size_t i = str_len; i-- > 0;)
		std_vec_push_Checked(result, &str[i], ACCESS_KEY_BYTES);
	return true;
}

/* access_key_Reversed_Names_name is Reversed's Names: the address of the stored bytes themselves. */
static void
access_key_Reversed_Names_name(const void *self, const uint8 *stored, size_t stored_len, uint8 result[32])
{
	ChunksAddressAddress address = chunks_address_Address_of(stored, stored_len);

	memcpy(result, address.f0, sizeof(address.f0));
}

/* ACCESS_KEY_REVERSED_SORT_KEYS_OPS and ACCESS_KEY_REVERSED_NAMES_OPS are Reversed's SortKeys and Names. */
const AccessKeySortKeysOps ACCESS_KEY_REVERSED_SORT_KEYS_OPS = {access_key_Reversed_SortKeys_sort_key};
const AccessKeyNamesOps ACCESS_KEY_REVERSED_NAMES_OPS = {access_key_Reversed_Names_name};

/* ACCESS_KEY_REVERSED is the tests' sorter, Reversed, as a SortKeys. */
#define ACCESS_KEY_REVERSED ((AccessKeySortKeys) {&ACCESS_KEY_REVERSED_SORT_KEYS_OPS, NULL})

/* access_key_tests_slice returns a slice of some bytes. */
static StdBytes
access_key_tests_slice(const void *ptr, size_t len)
{
	StdBytes	slice = {ptr, len};

	return slice;
}

/* access_key_tests_message_Debug_fmt is an error's message's Debug, an Option<&str>. */
static bool
access_key_tests_message_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdStr *message = value;
	StdOptionRef option = {message->ptr != NULL, {message, std_str_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* access_key_tests_ok is tests' ok: the call's result when it succeeded, or a panic with the error's message. */
static void
access_key_tests_ok(bool ok, PgPgError *error)
{
	if (!ok)
	{
		StdStr		message = pg_PgError_message(error);
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&message, access_key_tests_message_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 1, args};

		std_panic_fmt(RUST_FILE, 1672, 39, &arguments);
	}
}

/* access_key_tests_refused is tests' refused: the error of a call that must fail. */
static PgPgError
access_key_tests_refused(bool ok, PgPgError *error)
{
	if (ok)
		std_panic(RUST_FILE, 1678, 22, "the call succeeded", sizeof("the call succeeded") - 1);
	return *error;
}

/* access_key_tests_skip is tests' skip: the bytes after a key column of a key type that no bound cut. */
static bool
access_key_tests_skip(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order, StdBytes *result,
					  PgPgError *error)
{
	bool		cut;

	return access_key_skip_within(key.ptr, key.len, key_type, order, SIZE_MAX, result, &cut, error);
}

/* access_key_tests_skipped is skip's result where the call must succeed. */
static StdBytes
access_key_tests_skipped(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order)
{
	StdBytes	rest;
	PgPgError	error;

	access_key_tests_ok(access_key_tests_skip(key, key_type, order, &rest, &error), &error);
	return rest;
}

/* access_key_tests_skip_is_err reports whether skip fails, dropping its error. */
static bool
access_key_tests_skip_is_err(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order)
{
	StdBytes	rest;
	PgPgError	error;

	if (access_key_tests_skip(key, key_type, order, &rest, &error))
		return false;
	pg_PgError_drop(&error);
	return true;
}

/* access_key_tests_key is tests' key: a value's key column of a type in an order. */
static StdVec
access_key_tests_key(uint32 ty, const AccessKeyValue *value, EncodingKeyOrder order)
{
	StdVec		out;
	AccessKeyKeyType key_type = access_key_KeyType_of(ty);
	PgPgError	error;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_tests_ok(access_key_encode_option(&out, ACCESS_KEY_REVERSED, &key_type, value, order, &error), &error);
	return out;
}

/* access_key_tests_word returns a Value::Word. */
static AccessKeyValue
access_key_tests_word(uint64 word)
{
	AccessKeyValue value = {.tag = ACCESS_KEY_VALUE_WORD, .u.word = word};

	return value;
}

/* access_key_tests_bytes returns a Value::Bytes. */
static AccessKeyValue
access_key_tests_bytes(const void *bytes, size_t len)
{
	AccessKeyValue value = {.tag = ACCESS_KEY_VALUE_BYTES, .u.bytes = {bytes, len}};

	return value;
}

/* access_key_tests_int4 is tests' int4: an int4's word. */
static AccessKeyValue
access_key_tests_int4(int32 value)
{
	return access_key_tests_word(pg_word_from_int32(value));
}

/* access_key_tests_int4_key is tests' key(INT4OID, int4(value), order). */
static StdVec
access_key_tests_int4_key(int32 value, EncodingKeyOrder order)
{
	AccessKeyValue word = access_key_tests_int4(value);

	return access_key_tests_key(INT4OID, &word, order);
}

/* access_key_tests_vec_lt reports whether a Vec<u8> orders before another, as Vec's Ord does. */
static bool
access_key_tests_vec_lt(const StdVec *a, const StdVec *b)
{
	int			c = memcmp(a->ptr, b->ptr, Min(a->len, b->len));

	return c < 0 || (c == 0 && a->len < b->len);
}

/* access_key_tests_vec_eq reports whether two byte vectors or slices hold the same bytes. */
static bool
access_key_tests_bytes_eq(const void *a, size_t a_len, const void *b, size_t b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/* access_key_tests_vec_Debug_fmt is a Vec<u8>'s Debug. */
static bool
access_key_tests_vec_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *vec = value;
	StdBytes	bytes = {vec->ptr, vec->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* access_key_tests_free frees a byte vector. */
static void
access_key_tests_free(StdVec *vec)
{
	std_vec_free_Checked(vec, ACCESS_KEY_BYTES);
}

/* access_key_Decoded_PartialEq_eq reports whether two decoded values are the same variant with equal contents. */
bool
access_key_Decoded_PartialEq_eq(const AccessKeyDecoded *self, const AccessKeyDecoded *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag == ACCESS_KEY_DECODED_WORD)
		return self->u.word == other->u.word;
	return access_key_tests_bytes_eq(self->u.bytes.ptr, self->u.bytes.len, other->u.bytes.ptr, other->u.bytes.len);
}

/* access_key_Decoded_Debug_fmt is Decoded's derived Debug. */
bool
access_key_Decoded_Debug_fmt(const void *value, StdFormatter *f)
{
	const AccessKeyDecoded *self = value;
	StdDebugTuple t;

	if (self->tag == ACCESS_KEY_DECODED_WORD)
	{
		t = std_fmt_debug_tuple(f, "Word", 4);
		std_DebugTuple_field(&t, &self->u.word, std_u64_Debug_fmt);
	}
	else
	{
		t = std_fmt_debug_tuple(f, "Bytes", 5);
		std_DebugTuple_field(&t, &self->u.bytes, access_key_tests_vec_Debug_fmt);
	}
	return std_DebugTuple_finish(&t);
}

/* access_key_tests_decoded_t is a decode's answer in a test: the value, none for NULL, and the bytes after it. */
typedef struct access_key_tests_decoded_t
{
	bool		has;
	AccessKeyDecoded value;
	StdBytes	rest;
} access_key_tests_decoded_t;

/* access_key_tests_decode is decode's answer where the call must succeed. */
static access_key_tests_decoded_t
access_key_tests_decode(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order)
{
	access_key_tests_decoded_t decoded;
	PgPgError	error;

	access_key_tests_ok(access_key_decode(key.ptr, key.len, key_type, order, &decoded.has, &decoded.value,
										  &decoded.rest, &error), &error);
	return decoded;
}

/* access_key_tests_decode_is_err reports whether decode fails, dropping what it gave. */
static bool
access_key_tests_decode_is_err(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order)
{
	access_key_tests_decoded_t decoded;
	PgPgError	error;

	if (!access_key_decode(key.ptr, key.len, key_type, order, &decoded.has, &decoded.value, &decoded.rest, &error))
	{
		pg_PgError_drop(&error);
		return true;
	}
	if (decoded.has && decoded.value.tag == ACCESS_KEY_DECODED_BYTES)
		access_key_tests_free(&decoded.value.u.bytes);
	return false;
}

/* access_key_tests_decoded_drop frees a decoded value's bytes. */
static void
access_key_tests_decoded_drop(access_key_tests_decoded_t *decoded)
{
	if (decoded->has && decoded->value.tag == ACCESS_KEY_DECODED_BYTES)
		access_key_tests_free(&decoded->value.u.bytes);
}

/* access_key_tests_option_decoded_t is an Option<Decoded> to compare and format. */
typedef struct access_key_tests_option_decoded_t
{
	bool		has;
	const AccessKeyDecoded *value;
} access_key_tests_option_decoded_t;

/* access_key_tests_option_decoded_Debug_fmt is an Option<Decoded>'s Debug. */
static bool
access_key_tests_option_decoded_Debug_fmt(const void *value, StdFormatter *f)
{
	const access_key_tests_option_decoded_t *self = value;
	StdOptionRef option = {self->has, {self->value, access_key_Decoded_Debug_fmt}};

	return std_option_Debug_fmt(&option, f);
}

/* access_key_tests_option_decoded_eq reports whether two Option<Decoded>s are equal. */
static bool
access_key_tests_option_decoded_eq(const access_key_tests_option_decoded_t *a,
								   const access_key_tests_option_decoded_t *b)
{
	return a->has == b->has && (!a->has || access_key_Decoded_PartialEq_eq(a->value, b->value));
}

/* access_key_tests_bytes_Debug_fmt is a &[u8]'s Debug, of a StdBytes. */
#define access_key_tests_bytes_Debug_fmt std_bytes_Debug_fmt

/* ACCESS_KEY_TESTS_TYPE_MESSAGE builds assert_eq!'s "type {ty}" message of a type. */
#define ACCESS_KEY_TESTS_TYPE_MESSAGE(name, ty) \
	StdFmtPiece name##_pieces[] = {STD_FMT_LIT("type "), STD_FMT_ARG(0)}; \
	StdFmtArg	name##_args[] = {{&(ty), std_u32_Display_fmt}}; \
	StdFmtArguments name = {name##_pieces, 2, name##_args}

/* access_key_tests_value_t is a test's (u32, Option<Value>): a type and its value, none for NULL. */
typedef struct access_key_tests_value_t
{
	uint32		ty;
	bool		has;
	AccessKeyValue value;
} access_key_tests_value_t;

/*
 * Exact keys of each type read back to their values and are skipped whole,
 * in either order, and keys order as Postgres orders the values.
 */
void
test_access_key_tests_exact_keys_round_trip_and_keys_order_as_postgres_does(void)
{
	static const uint8 uuid[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
	access_key_tests_value_t values[10] = {
		{INT4OID, true, access_key_tests_word(pg_word_from_int32(-5))},
		{TEXTOID, true, access_key_tests_bytes("h\xc3\xa9llo", 6)},
		{BYTEAOID, true, access_key_tests_bytes("a\0b", 3)},
		{INT8OID, false},
		{BOOLOID, true, access_key_tests_word(pg_word_from_bool(true))},
		{UUIDOID, true, access_key_tests_bytes(uuid, 16)},
		{OIDOID, true, access_key_tests_word(pg_word_from_oid(UINT32_MAX))},
		{CHAROID, true, access_key_tests_word(pg_word_from_char(0xE9))},
		{INT2OID, true, access_key_tests_word(pg_word_from_int16(-2))},
		{PG_LSNOID, true, access_key_tests_word(UINT64_MAX)},
	};
	AccessKeyDecoded expected[10] = {
		{ACCESS_KEY_DECODED_WORD, .u.word = pg_word_from_int32(-5)},
		{ACCESS_KEY_DECODED_BYTES},
		{ACCESS_KEY_DECODED_BYTES},
		{ACCESS_KEY_DECODED_WORD},
		{ACCESS_KEY_DECODED_WORD, .u.word = 1},
		{ACCESS_KEY_DECODED_BYTES},
		{ACCESS_KEY_DECODED_WORD, .u.word = UINT32_MAX},
		{ACCESS_KEY_DECODED_WORD, .u.word = pg_word_from_char(0xE9)},
		{ACCESS_KEY_DECODED_WORD, .u.word = pg_word_from_int16(-2)},
		{ACCESS_KEY_DECODED_WORD, .u.word = UINT64_MAX},
	};
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, ENCODING_KEY_ORDER_DESC};
	PgPgError	error;
	StdVec		a;
	StdVec		b;
	StdVec		c;
	StdVec		d;

	std_slice_to_vec_Checked(&expected[1].u.bytes, "h\xc3\xa9llo", 6, ACCESS_KEY_BYTES);
	std_slice_to_vec_Checked(&expected[2].u.bytes, "a\0b", 3, ACCESS_KEY_BYTES);
	std_slice_to_vec_Checked(&expected[5].u.bytes, uuid, 16, ACCESS_KEY_BYTES);
	for (int o = 0; o < 2; o++)
	{
		EncodingKeyOrder order = orders[o];
		StdVec		out;
		StdBytes	rest;
		StdBytes	skipped;

		std_vec_new(&out, ACCESS_KEY_BYTES);
		for (int v = 0; v < 10; v++)
		{
			AccessKeyKeyType key_type = access_key_KeyType_of(values[v].ty);

			access_key_tests_ok(access_key_encode(&out, ACCESS_KEY_REVERSED, &key_type, values[v].has, values[v].value,
												  order, &error), &error);
		}
		rest.ptr = skipped.ptr = out.ptr;
		rest.len = skipped.len = out.len;
		for (int v = 0; v < 10; v++)
		{
			AccessKeyKeyType key_type = access_key_KeyType_of(values[v].ty);
			access_key_tests_decoded_t decoded = access_key_tests_decode(rest, &key_type, order);
			access_key_tests_option_decoded_t left = {decoded.has, &decoded.value};
			access_key_tests_option_decoded_t right = {v != 3, &expected[v]};

			{
				ACCESS_KEY_TESTS_TYPE_MESSAGE(message, values[v].ty);

				if (!access_key_tests_option_decoded_eq(&left, &right))
					std_assert_failed(RUST_FILE, 1737, 17, "==", &left,
									  access_key_tests_option_decoded_Debug_fmt, &right,
									  access_key_tests_option_decoded_Debug_fmt, &message);
			}
			access_key_tests_decoded_drop(&decoded);
			rest = decoded.rest;
			skipped = access_key_tests_skipped(skipped, &key_type, order);
			{
				ACCESS_KEY_TESTS_TYPE_MESSAGE(message, values[v].ty);

				if (!access_key_tests_bytes_eq(skipped.ptr, skipped.len, rest.ptr, rest.len))
					std_assert_failed(RUST_FILE, 1740, 17, "==", &skipped, std_bytes_Debug_fmt, &rest,
									  std_bytes_Debug_fmt, &message);
			}
		}
		STD_ASSERT(1742, 13, rest.len == 0, "rest.is_empty()");
		access_key_tests_free(&out);
	}
	for (int v = 0; v < 10; v++)
		if (expected[v].tag == ACCESS_KEY_DECODED_BYTES)
			access_key_tests_free(&expected[v].u.bytes);
#define ACCESS_KEY_TESTS_INT4_LT(line, x, ox, y, oy, source) \
	do { \
		a = access_key_tests_int4_key((x), (ox)); \
		b = access_key_tests_int4_key((y), (oy)); \
		STD_ASSERT(line, 9, access_key_tests_vec_lt(&a, &b), source); \
		access_key_tests_free(&a); \
		access_key_tests_free(&b); \
	} while (0)
	ACCESS_KEY_TESTS_INT4_LT(1744, -5, ENCODING_KEY_ORDER_ASC, 3, ENCODING_KEY_ORDER_ASC,
							 "key(INT4OID, int4(-5), Order::ASC) < key(INT4OID, int4(3), Order::ASC)");
	a = access_key_tests_int4_key(-5, ENCODING_KEY_ORDER_DESC);
	b = access_key_tests_int4_key(3, ENCODING_KEY_ORDER_DESC);
	STD_ASSERT(1745, 9, access_key_tests_vec_lt(&b, &a),
			   "key(INT4OID, int4(-5), Order::DESC) > key(INT4OID, int4(3), Order::DESC)");
	access_key_tests_free(&a);
	access_key_tests_free(&b);
	ACCESS_KEY_TESTS_INT4_LT(1746, PG_INT32_MIN, ENCODING_KEY_ORDER_ASC, PG_INT32_MAX, ENCODING_KEY_ORDER_ASC,
							 "key(INT4OID, int4(i32::MIN), Order::ASC) < key(INT4OID, int4(i32::MAX), Order::ASC)");
#undef ACCESS_KEY_TESTS_INT4_LT
	a = access_key_tests_int4_key(3, ENCODING_KEY_ORDER_ASC);
	b = access_key_tests_key(INT4OID, NULL, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(1747, 9, access_key_tests_vec_lt(&a, &b),
			   "key(INT4OID, int4(3), Order::ASC) < key(INT4OID, None, Order::ASC)");
	access_key_tests_free(&a);
	access_key_tests_free(&b);
	a = access_key_tests_int4_key(3, ENCODING_KEY_ORDER_DESC);
	b = access_key_tests_key(INT4OID, NULL, ENCODING_KEY_ORDER_DESC);
	STD_ASSERT(1748, 9, access_key_tests_vec_lt(&b, &a),
			   "key(INT4OID, int4(3), Order::DESC) > key(INT4OID, None, Order::DESC)");
	access_key_tests_free(&a);
	access_key_tests_free(&b);
	{
		AccessKeyValue ab = access_key_tests_bytes("ab", 2);
		AccessKeyValue abc = access_key_tests_bytes("abc", 3);
		AccessKeyValue bv = access_key_tests_bytes("b", 1);
		AccessKeyValue empty = access_key_tests_bytes("", 0);
		AccessKeyValue av = access_key_tests_bytes("a", 1);
		bool		holds;

		a = access_key_tests_key(TEXTOID, &ab, ENCODING_KEY_ORDER_ASC);
		b = access_key_tests_key(TEXTOID, &abc, ENCODING_KEY_ORDER_ASC);
		holds = access_key_tests_vec_lt(&a, &b);
		access_key_tests_free(&a);
		if (holds)
		{
			c = access_key_tests_key(TEXTOID, &bv, ENCODING_KEY_ORDER_ASC);
			holds = access_key_tests_vec_lt(&b, &c);
			access_key_tests_free(&c);
		}
		access_key_tests_free(&b);
		if (holds)
		{
			c = access_key_tests_key(TEXTOID, &empty, ENCODING_KEY_ORDER_ASC);
			d = access_key_tests_key(TEXTOID, &av, ENCODING_KEY_ORDER_ASC);
			holds = access_key_tests_vec_lt(&c, &d);
			access_key_tests_free(&c);
			access_key_tests_free(&d);
		}
		STD_ASSERT(1750, 9, holds, "text(\"ab\") < text(\"abc\") && text(\"abc\") < text(\"b\") && text(\"\") < text(\"a\")");
	}
	{
		AccessKeyValue a1 = access_key_tests_bytes("a", 1);
		AccessKeyValue a2 = access_key_tests_bytes("a\0", 2);
		AccessKeyValue a3 = access_key_tests_bytes("a\0\0", 3);
		AccessKeyValue a4 = access_key_tests_bytes("a\0\xff", 3);
		AccessKeyValue a5 = access_key_tests_bytes("a\x01", 2);
		bool		holds;

		a = access_key_tests_key(BYTEAOID, &a1, ENCODING_KEY_ORDER_ASC);
		b = access_key_tests_key(BYTEAOID, &a2, ENCODING_KEY_ORDER_ASC);
		holds = access_key_tests_vec_lt(&a, &b);
		access_key_tests_free(&a);
		if (holds)
		{
			c = access_key_tests_key(BYTEAOID, &a3, ENCODING_KEY_ORDER_ASC);
			holds = access_key_tests_vec_lt(&b, &c);
			access_key_tests_free(&c);
		}
		access_key_tests_free(&b);
		if (holds)
		{
			c = access_key_tests_key(BYTEAOID, &a4, ENCODING_KEY_ORDER_ASC);
			d = access_key_tests_key(BYTEAOID, &a5, ENCODING_KEY_ORDER_ASC);
			holds = access_key_tests_vec_lt(&c, &d);
			access_key_tests_free(&c);
			access_key_tests_free(&d);
		}
		STD_ASSERT(1752, 9, holds,
				   "bytea(b\"a\") < bytea(b\"a\\0\") && bytea(b\"a\\0\") < bytea(b\"a\\0\\0\") && "
				   "bytea(b\"a\\0\\xff\") < bytea(b\"a\\x01\")");
	}
}

/* access_key_tests_float8 is the float test's float8: a float8's key by its bits. */
static StdVec
access_key_tests_float8(uint64 bits)
{
	AccessKeyValue word = access_key_tests_word(bits);

	return access_key_tests_key(FLOAT8OID, &word, ENCODING_KEY_ORDER_ASC);
}

/* access_key_tests_float4 is the float test's float4: a float4's key by its bits. */
static StdVec
access_key_tests_float4(uint32 bits)
{
	AccessKeyValue word = access_key_tests_word(bits);

	return access_key_tests_key(FLOAT4OID, &word, ENCODING_KEY_ORDER_ASC);
}

/* access_key_tests_f64_bits returns a double's bits, as f64::to_bits does. */
static uint64
access_key_tests_f64_bits(double value)
{
	uint64		bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

/* access_key_tests_f32_bits returns a float's bits, as f32::to_bits does. */
static uint32
access_key_tests_f32_bits(float value)
{
	uint32		bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

/* access_key_tests_chain_lt reports whether each key of a chain orders before the next, freeing them. */
static bool
access_key_tests_chain_lt(StdVec *keys, size_t len)
{
	bool		holds = true;

	for (size_t i = 0; i + 1 < len && holds; i++)
		holds = access_key_tests_vec_lt(&keys[i], &keys[i + 1]);
	for (size_t i = 0; i < len; i++)
		access_key_tests_free(&keys[i]);
	return holds;
}

/* access_key_tests_assert_vec_eq is assert_eq! of two byte vectors at a place, freeing them. */
static void
access_key_tests_assert_vec_eq(uint32 line, uint32 col, StdVec a, StdVec b)
{
	if (!access_key_tests_bytes_eq(a.ptr, a.len, b.ptr, b.len))
		std_assert_failed(RUST_FILE, line, col, "==", &a, access_key_tests_vec_Debug_fmt, &b,
						  access_key_tests_vec_Debug_fmt, NULL);
	access_key_tests_free(&a);
	access_key_tests_free(&b);
}

/*
 * ACCESS_KEY_F64_NEG_INFINITY, ACCESS_KEY_F64_INFINITY, and ACCESS_KEY_F64_NAN
 * are f64's constants' bits, and f32's after them.
 */
#define ACCESS_KEY_F64_NEG_INFINITY UINT64_C(0xFFF0000000000000)
#define ACCESS_KEY_F64_INFINITY UINT64_C(0x7FF0000000000000)
#define ACCESS_KEY_F64_NAN UINT64_C(0x7FF8000000000000)
#define ACCESS_KEY_F32_INFINITY UINT32_C(0x7F800000)
#define ACCESS_KEY_F32_NAN UINT32_C(0x7FC00000)

/*
 * The fork's own: a float's key orders as Postgres' btree orders floats,
 * with a negative zero as zero and every NaN above infinity, and skip passes
 * it by its width.
 */
void
test_access_key_tests_float_keys_order_as_postgres_does_and_are_skipped_by_their_width(void)
{
	StdVec		chain[4];
	StdVec		both;
	StdVec		f8;
	StdBytes	rest;
	AccessKeyKeyType float4 = access_key_KeyType_of(FLOAT4OID);
	AccessKeyKeyType float8 = access_key_KeyType_of(FLOAT8OID);
	static const uint8 nan_marker[5] = {0x7f, 0, 0, 0, 0};
	uint8		null_last[2] = {ENCODING_KEY_NULL_LAST, 9};
	StdBytes	nine;

	chain[0] = access_key_tests_float8(ACCESS_KEY_F64_NEG_INFINITY);
	chain[1] = access_key_tests_float8(access_key_tests_f64_bits(-1.5));
	chain[2] = access_key_tests_float8(access_key_tests_f64_bits(0.0));
	chain[3] = access_key_tests_float8(access_key_tests_f64_bits(2.0));
	STD_ASSERT(1761, 9, access_key_tests_chain_lt(chain, 4),
			   "float8(f64::NEG_INFINITY) < float8(-1.5) && float8(-1.5) < float8(0.0) && float8(0.0) < float8(2.0)");
	chain[0] = access_key_tests_float8(access_key_tests_f64_bits(2.0));
	chain[1] = access_key_tests_float8(ACCESS_KEY_F64_INFINITY);
	chain[2] = access_key_tests_float8(ACCESS_KEY_F64_NAN);
	STD_ASSERT(1762, 9, access_key_tests_chain_lt(chain, 3),
			   "float8(2.0) < float8(f64::INFINITY) && float8(f64::INFINITY) < float8(f64::NAN)");
	access_key_tests_assert_vec_eq(1763, 9, access_key_tests_float8(UINT64_C(0x8000000000000000)),
								   access_key_tests_float8(0));
	access_key_tests_assert_vec_eq(1764, 9, access_key_tests_float8(ACCESS_KEY_F64_NAN),
								   access_key_tests_float8(ACCESS_KEY_F64_NAN | UINT64_C(0x8000000000000000)));
	{
		bool		holds;

		chain[0] = access_key_tests_float4(access_key_tests_f32_bits(-1.5f));
		chain[1] = access_key_tests_float4(access_key_tests_f32_bits(0.25f));
		holds = access_key_tests_chain_lt(chain, 2);
		if (holds)
		{
			chain[0] = access_key_tests_float4(ACCESS_KEY_F32_INFINITY);
			chain[1] = access_key_tests_float4(ACCESS_KEY_F32_NAN);
			holds = access_key_tests_chain_lt(chain, 2);
		}
		STD_ASSERT(1765, 9, holds, "float4(-1.5) < float4(0.25) && float4(f32::INFINITY) < float4(f32::NAN)");
	}
	both = access_key_tests_float4(access_key_tests_f32_bits(1.0f));
	f8 = access_key_tests_float8(access_key_tests_f64_bits(1.0));
	std_vec_extend_from_slice_Checked(&both, f8.ptr, f8.len, ACCESS_KEY_BYTES);
	access_key_tests_free(&f8);
	rest = access_key_tests_skipped(access_key_tests_slice(both.ptr, both.len), &float4, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(1769, 9, access_key_tests_skipped(rest, &float8, ENCODING_KEY_ORDER_ASC).len == 0,
			   "ok(skip(rest, &KeyType::of(FLOAT8OID), Order::ASC)).is_empty()");
	std_range_to_check(3, both.len, RUST_FILE, 1770, 23);
	STD_ASSERT(1770, 9,
			   access_key_tests_skip_is_err(access_key_tests_slice(both.ptr, 3), &float4, ENCODING_KEY_ORDER_ASC),
			   "skip(&both[..3], &KeyType::of(FLOAT4OID), Order::ASC).is_err()");
	STD_ASSERT(1771, 9,
			   access_key_tests_skip_is_err(access_key_tests_slice(nan_marker, sizeof(nan_marker)), &float4,
											ENCODING_KEY_ORDER_ASC),
			   "skip(&[0x7f, 0, 0, 0, 0], &KeyType::of(FLOAT4OID), Order::ASC).is_err()");
	nine = access_key_tests_skipped(access_key_tests_slice(null_last, sizeof(null_last)), &float8, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(1772, 9, nine.len == 1 && nine.ptr[0] == 9,
			   "ok(skip(&[NULL_LAST, 9], &KeyType::of(FLOAT8OID), Order::ASC)) == [9]");
	access_key_tests_free(&both);
}

/* access_key_tests_bpchar is the bpchar test's bpchar: a bpchar's key in an order. */
static StdVec
access_key_tests_bpchar(const char *str, EncodingKeyOrder order)
{
	AccessKeyValue bytes = access_key_tests_bytes(str, strlen(str));

	return access_key_tests_key(BPCHAROID, &bytes, order);
}

/* access_key_tests_option_decoded_Debug_eq is assert_eq! of an Option<Decoded> against a word at a place. */
static void
access_key_tests_assert_word(uint32 line, uint32 col, const access_key_tests_decoded_t *decoded, uint64 word)
{
	AccessKeyDecoded expected = {ACCESS_KEY_DECODED_WORD, .u.word = word};
	access_key_tests_option_decoded_t left = {decoded->has, &decoded->value};
	access_key_tests_option_decoded_t right = {true, &expected};

	if (!access_key_tests_option_decoded_eq(&left, &right))
		std_assert_failed(RUST_FILE, line, col, "==", &left, access_key_tests_option_decoded_Debug_fmt, &right,
						  access_key_tests_option_decoded_Debug_fmt, NULL);
}

/*
 * The fork's own: a bpchar's key leaves out its trailing spaces, as Postgres
 * ignores them in comparisons, and skip finds its end in either order.
 */
void
test_access_key_tests_a_bpchar_key_ignores_trailing_spaces_and_is_skipped_to_its_end(void)
{
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, ENCODING_KEY_ORDER_DESC};
	AccessKeyKeyType bpchar = access_key_KeyType_of(BPCHAROID);
	AccessKeyKeyType int4 = access_key_KeyType_of(INT4OID);
	StdVec		chain[2];

	access_key_tests_assert_vec_eq(1780, 9, access_key_tests_bpchar("ab   ", ENCODING_KEY_ORDER_ASC),
								   access_key_tests_bpchar("ab", ENCODING_KEY_ORDER_ASC));
	access_key_tests_assert_vec_eq(1781, 9, access_key_tests_bpchar("   ", ENCODING_KEY_ORDER_ASC),
								   access_key_tests_bpchar("", ENCODING_KEY_ORDER_ASC));
	chain[0] = access_key_tests_bpchar("ab", ENCODING_KEY_ORDER_ASC);
	chain[1] = access_key_tests_bpchar("ab c", ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(1782, 9, access_key_tests_chain_lt(chain, 2),
			   "bpchar(\"ab\", Order::ASC) < bpchar(\"ab c\", Order::ASC)");
	for (int o = 0; o < 2; o++)
	{
		StdVec		two = access_key_tests_bpchar("ab ", orders[o]);
		StdVec		nine = access_key_tests_int4_key(9, orders[o]);
		StdBytes	rest;
		access_key_tests_decoded_t decoded;

		std_vec_extend_from_slice_Checked(&two, nine.ptr, nine.len, ACCESS_KEY_BYTES);
		access_key_tests_free(&nine);
		rest = access_key_tests_skipped(access_key_tests_slice(two.ptr, two.len), &bpchar, orders[o]);
		decoded = access_key_tests_decode(rest, &int4, orders[o]);
		access_key_tests_assert_word(1787, 13, &decoded, 9);
		access_key_tests_free(&two);
	}
	STD_ASSERT(1789, 9, !access_key_is_exact(&bpchar), "!is_exact(&KeyType::of(BPCHAROID))");
}

/*
 * access_key_tests_unencoded is the refusal test's unencoded: the error of
 * encoding a value of a type in a form it isn't keyed from.
 */
static PgPgError
access_key_tests_unencoded(uint32 ty, AccessKeyValue value)
{
	StdVec		out;
	AccessKeyKeyType key_type = access_key_KeyType_of(ty);
	PgPgError	error;
	bool		ok;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	ok = access_key_encode(&out, ACCESS_KEY_REVERSED, &key_type, true, value, ENCODING_KEY_ORDER_ASC, &error);
	access_key_tests_free(&out);
	return access_key_tests_refused(ok, &error);
}

/* access_key_tests_assert_message is assert_eq!(error.message(), Some(text)) at a place. */
static void
access_key_tests_assert_message(uint32 line, uint32 col, const PgPgError *error, const char *str)
{
	StdStr		message = pg_PgError_message(error);
	StdStr		expected = {str, strlen(str)};

	if (message.ptr == NULL || !access_key_tests_bytes_eq(message.ptr, message.len, expected.ptr, expected.len))
		std_assert_failed(RUST_FILE, line, col, "==", &message, access_key_tests_message_Debug_fmt, &expected,
						  access_key_tests_message_Debug_fmt, NULL);
}

/* access_key_tests_fork_code returns PgError::fork(code, String::new()).sqlerrcode(). */
static int32
access_key_tests_fork_code(const char *code)
{
	StdString	empty;
	PgPgError	fork;
	int32		sqlerrcode;

	std_string_from_Checked(&empty, "", 0);
	fork = pg_PgError_fork((const uint8 *) code, &empty);
	sqlerrcode = pg_PgError_sqlerrcode(&fork);
	pg_PgError_drop(&fork);
	return sqlerrcode;
}

/*
 * access_key_tests_assert_code is assert_eq!(error.sqlerrcode(),
 * PgError::fork(code, String::new()).sqlerrcode()) at a place.
 */
static void
access_key_tests_assert_code(uint32 line, uint32 col, const PgPgError *error, const char *code,
							 const StdFmtArguments *message)
{
	int32		got = pg_PgError_sqlerrcode(error);
	int32		expected = access_key_tests_fork_code(code);

	if (got != expected)
		std_assert_failed(RUST_FILE, line, col, "==", &got, std_i32_Debug_fmt, &expected, std_i32_Debug_fmt, message);
}

/*
 * The fork's own: a key type no key is made of, a value passed in a form its
 * type's key isn't made from, and a key cut short or with an unknown marker
 * are each refused with their error.
 */
void
test_access_key_tests_a_type_without_an_encoding_and_a_key_that_does_not_decode_are_refused(void)
{
	PgPgError	unknown = access_key_tests_unencoded(1, access_key_tests_word(0));
	PgPgError	error;
	AccessKeyKeyType int4 = access_key_KeyType_of(INT4OID);
	StdVec		whole;
	static const uint8 marker[5] = {0x7f, 0, 0, 0, 1};
	StdBytes	cuts[3];
	uint8		str[2] = {ENCODING_KEY_VALUE, 'a'};
	uint8 bytea[2] = {ENCODING_KEY_VALUE, 0};
	AccessKeyKeyType text_type = access_key_KeyType_of(TEXTOID);
	AccessKeyKeyType bytea_type = access_key_KeyType_of(BYTEAOID);
	AccessKeyKeyType float8 = access_key_KeyType_of(FLOAT8OID);
	bool		is_some;

	access_key_tests_assert_message(1800, 9, &unknown, "type 1 has no key encoding");
	access_key_tests_assert_code(1801, 9, &unknown, "XX000", NULL);
	pg_PgError_drop(&unknown);
	error = access_key_tests_unencoded(INT4OID, access_key_tests_bytes("x", 1));
	is_some = pg_PgError_message(&error).ptr != NULL;
	pg_PgError_drop(&error);
	STD_ASSERT(1802, 9, is_some, "unencoded(INT4OID, Value::Bytes(b\"x\")).message().is_some()");
	error = access_key_tests_unencoded(TEXTOID, access_key_tests_word(0));
	is_some = pg_PgError_message(&error).ptr != NULL;
	pg_PgError_drop(&error);
	STD_ASSERT(1803, 9, is_some, "unencoded(TEXTOID, Value::Word(0)).message().is_some()");
	whole = access_key_tests_int4_key(1, ENCODING_KEY_ORDER_ASC);
	std_range_to_check(0, whole.len, RUST_FILE, 1806, 22);
	cuts[0] = access_key_tests_slice(whole.ptr, 0);
	std_range_to_check(3, whole.len, RUST_FILE, 1806, 35);
	cuts[1] = access_key_tests_slice(whole.ptr, 3);
	cuts[2] = access_key_tests_slice(marker, sizeof(marker));
	for (int i = 0; i < 3; i++)
	{
		access_key_tests_decoded_t decoded;
		bool		ok = access_key_decode(cuts[i].ptr, cuts[i].len, &int4, ENCODING_KEY_ORDER_ASC, &decoded.has,
										   &decoded.value, &decoded.rest, &error);

		error = access_key_tests_refused(ok, &error);
		access_key_tests_assert_message(1808, 13, &error, "an index key is corrupt");
		access_key_tests_assert_code(1809, 13, &error, "XX000", NULL);
		pg_PgError_drop(&error);
	}
	std_range_to_check(3, whole.len, RUST_FILE, 1811, 23);
	STD_ASSERT(1811, 9, access_key_tests_skip_is_err(access_key_tests_slice(whole.ptr, 3), &int4, ENCODING_KEY_ORDER_ASC),
			   "skip(&whole[..3], &int, Order::ASC).is_err()");
	STD_ASSERT(1812, 9,
			   access_key_tests_decode_is_err(access_key_tests_slice(str, sizeof(str)), &text_type, ENCODING_KEY_ORDER_ASC),
			   "decode(&[VALUE, b'a'], &KeyType::of(TEXTOID), Order::ASC).is_err()");
	STD_ASSERT(1813, 9,
			   access_key_tests_decode_is_err(access_key_tests_slice(bytea, sizeof(bytea)), &bytea_type, ENCODING_KEY_ORDER_ASC),
			   "decode(&[VALUE, 0], &KeyType::of(BYTEAOID), Order::ASC).is_err()");
	STD_ASSERT(1814, 9,
			   access_key_tests_decode_is_err(access_key_tests_slice(whole.ptr, whole.len), &float8, ENCODING_KEY_ORDER_ASC),
			   "decode(&whole, &KeyType::of(FLOAT8OID), Order::ASC).is_err()");
	access_key_tests_free(&whole);
}

/* access_key_tests_stored is tests' stored: the key column of a value of a type keyed from its stored bytes. */
static StdVec
access_key_tests_stored(uint32 ty, const StdVec *bytes, EncodingKeyOrder order)
{
	AccessKeyValue value = access_key_tests_bytes(bytes->ptr, bytes->len);

	return access_key_tests_key(ty, &value, order);
}

/* access_key_tests_of returns a vector of some bytes. */
static StdVec
access_key_tests_of(const void *bytes, size_t len)
{
	StdVec		vec;

	std_slice_to_vec_Checked(&vec, bytes, len, ACCESS_KEY_BYTES);
	return vec;
}

/* access_key_tests_name is tests' name: a stored name, its text padded with NULs to a name's length. */
static StdVec
access_key_tests_name(const char *str)
{
	StdVec		out = access_key_tests_of(str, strlen(str));
	uint8		zero = 0;

	std_vec_resize(&out, ACCESS_KEY_NAME_LEN, &zero, ACCESS_KEY_BYTES);
	return out;
}

/* access_key_tests_timetz is tests' timetz: microseconds from midnight, then seconds west of UTC. */
static StdVec
access_key_tests_timetz(int64 time, int32 zone)
{
	uint8		bytes[12];

	for (int i = 0; i < 8; i++)
		bytes[i] = (uint8) ((uint64) time >> (8 * i));
	for (int i = 0; i < 4; i++)
		bytes[8 + i] = (uint8) ((uint32) zone >> (8 * i));
	return access_key_tests_of(bytes, sizeof(bytes));
}

/* access_key_tests_interval is tests' interval: its microseconds, then its days, then its months. */
static StdVec
access_key_tests_interval(int32 months, int32 days, int64 time)
{
	uint8		bytes[16];

	for (int i = 0; i < 8; i++)
		bytes[i] = (uint8) ((uint64) time >> (8 * i));
	for (int i = 0; i < 4; i++)
	{
		bytes[8 + i] = (uint8) ((uint32) days >> (8 * i));
		bytes[12 + i] = (uint8) ((uint32) months >> (8 * i));
	}
	return access_key_tests_of(bytes, sizeof(bytes));
}

/* access_key_tests_oidvector is tests' oidvector: a stored oidvector of OIDs. */
static StdVec
access_key_tests_oidvector(const uint32 *oids, size_t len)
{
	StdVec		out;
	uint32		fields[5] = {1, 0, OIDOID, (uint32) len, 0};

	std_vec_new(&out, ACCESS_KEY_BYTES);
	for (int i = 0; i < 5; i++)
		access_key_le32(&out, fields[i]);
	for (size_t i = 0; i < len; i++)
		access_key_le32(&out, oids[i]);
	return out;
}

/*
 * access_key_tests_numeric is tests' numeric: a stored numeric of a sign, a
 * weight, and base-10000 digits, under a short or a long header.
 */
static StdVec
access_key_tests_numeric(bool negative, int16 weight, const uint16 *digits, size_t len, bool is_short)
{
	StdVec		out;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	if (is_short)
		access_key_le16(&out, (uint16) (0x8000 | (uint16) negative << 13 | ((uint16) weight & 0x007f)));
	else
	{
		access_key_le16(&out, (uint16) ((uint16) negative << 14));
		access_key_le16(&out, (uint16) weight);
	}
	for (size_t i = 0; i < len; i++)
		access_key_le16(&out, digits[i]);
	return out;
}

/* access_key_tests_special_numeric is tests' special_numeric: a stored numeric whose header names no special value. */
static StdVec
access_key_tests_special_numeric(void)
{
	StdVec		out;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_le16(&out, 0xe000);
	return out;
}

/* access_key_tests_typed_t is a test's (u32, Vec<u8>): a type and a stored value of it. */
typedef struct access_key_tests_typed_t
{
	uint32		ty;
	StdVec		bytes;
} access_key_tests_typed_t;

/* access_key_tests_pair_t is a (Option<Decoded>, &[u8]) to compare and format, decode's answer. */
typedef struct access_key_tests_pair_t
{
	access_key_tests_option_decoded_t value;
	StdBytes	rest;
} access_key_tests_pair_t;

/* access_key_tests_pair_Debug_fmt is a (Option<Decoded>, &[u8])'s Debug. */
static bool
access_key_tests_pair_Debug_fmt(const void *value, StdFormatter *f)
{
	const access_key_tests_pair_t *pair = value;
	StdTupleRef tuple = STD_TUPLE(2, STD_ITEM(&pair->value, access_key_tests_option_decoded_Debug_fmt),
								  STD_ITEM(&pair->rest, std_bytes_Debug_fmt));

	return std_tuple_Debug_fmt(&tuple, f);
}

/*
 * access_key_tests_assert_pair is assert_eq!((value, rest),
 * (Some(Decoded::Bytes(bytes)), &[after][..])) at a place, with a message.
 */
static void
access_key_tests_assert_pair(uint32 line, uint32 col, const access_key_tests_decoded_t *decoded, const StdVec *bytes,
							 uint8 after, const StdFmtArguments *message)
{
	AccessKeyDecoded expected = {ACCESS_KEY_DECODED_BYTES, .u.bytes = *bytes};
	access_key_tests_pair_t left = {{decoded->has, &decoded->value}, decoded->rest};
	access_key_tests_pair_t right = {{true, &expected}, {&after, 1}};

	if (!access_key_tests_option_decoded_eq(&left.value, &right.value) ||
		!access_key_tests_bytes_eq(left.rest.ptr, left.rest.len, right.rest.ptr, right.rest.len))
		std_assert_failed(RUST_FILE, line, col, "==", &left, access_key_tests_pair_Debug_fmt, &right,
						  access_key_tests_pair_Debug_fmt, message);
}

/* access_key_tests_assert_nine is assert_eq!(rest, [9]) at a place, with a message or none. */
static void
access_key_tests_assert_nine(uint32 line, uint32 col, StdBytes rest, const StdFmtArguments *message)
{
	uint8		nine = 9;
	StdBytes	expected = {&nine, 1};

	if (!access_key_tests_bytes_eq(rest.ptr, rest.len, &nine, 1))
		std_assert_failed(RUST_FILE, line, col, "==", &rest, std_bytes_Debug_fmt, &expected, std_bytes_Debug_fmt,
						  message);
}

/* access_key_tests_decoded_max is decoded_max's answer where the call must succeed. */
static size_t
access_key_tests_decoded_max(const AccessKeyKeyType *key_type, EncodingKeyOrder order, const uint8 *key,
							 size_t key_len)
{
	size_t		max;
	PgPgError	error;

	access_key_tests_ok(access_key_decoded_max(key_type, order, key, key_len, &max, &error), &error);
	return max;
}

/* access_key_tests_typed_drop frees a test's typed values. */
static void
access_key_tests_typed_drop(access_key_tests_typed_t *values, size_t len)
{
	for (size_t i = 0; i < len; i++)
		access_key_tests_free(&values[i].bytes);
}

/*
 * The fork's own: a name, a tid, a time with time zone, network addresses,
 * bit strings, and oidvectors come back from their keys as the bytes
 * Postgres stores, in either order, and are skipped whole.
 */
void
test_access_key_tests_the_stored_forms_of_exact_types_round_trip(void)
{
	static const uint8 tid[6] = {1, 0, 2, 0, 7, 0};
	static const uint8 inet1[6] = {ACCESS_KEY_IPV4, 24, 192, 168, 1, 5};
	static const uint8 inet2[6] = {ACCESS_KEY_IPV4, 0, 0, 0, 0, 0};
	static const uint8 cidr[18] = {ACCESS_KEY_IPV4 + 1, 64, 0x20, 1, 0xd, 0xb8};
	static const uint8 bit[6] = {9, 0, 0, 0, 0xff, 0x80};
	static const uint8 varbit[4] = {0};
	static const uint32 oids[2] = {5, 4000000000u};
	access_key_tests_typed_t values[12] = {
		{NAMEOID, access_key_tests_name("pg_class")},
		{NAMEOID, access_key_tests_name("")},
		{TIDOID, access_key_tests_of(tid, sizeof(tid))},
		{TIMETZOID, access_key_tests_timetz(INT64CONST(3600000000), -3600)},
		{TIMETZOID, access_key_tests_timetz(0, 57599)},
		{INETOID, access_key_tests_of(inet1, sizeof(inet1))},
		{INETOID, access_key_tests_of(inet2, sizeof(inet2))},
		{CIDROID, access_key_tests_of(cidr, sizeof(cidr))},
		{BITOID, access_key_tests_of(bit, sizeof(bit))},
		{VARBITOID, access_key_tests_of(varbit, sizeof(varbit))},
		{OIDVECTOROID, access_key_tests_oidvector(oids, 2)},
		{OIDVECTOROID, access_key_tests_oidvector(NULL, 0)},
	};
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, {true, true}};

	for (int o = 0; o < 2; o++)
	{
		for (int v = 0; v < 12; v++)
		{
			uint32		ty = values[v].ty;
			AccessKeyKeyType key_type = access_key_KeyType_of(ty);
			StdVec		key = access_key_tests_stored(ty, &values[v].bytes, orders[o]);
			uint8		nine = 9;
			access_key_tests_decoded_t decoded;
			bool		sized;
			size_t		cut;

			std_vec_push_Checked(&key, &nine, ACCESS_KEY_BYTES);
			STD_ASSERT(1884, 17, access_key_is_exact(&key_type), "is_exact(&key_type)");
			decoded = access_key_tests_decode(access_key_tests_slice(key.ptr, key.len), &key_type, orders[o]);
			{
				ACCESS_KEY_TESTS_TYPE_MESSAGE(message, ty);

				access_key_tests_assert_pair(1886, 17, &decoded, &values[v].bytes, 9, &message);
			}
			access_key_tests_decoded_drop(&decoded);
			sized = ty == NAMEOID || ty == TIDOID || ty == TIMETZOID;
			cut = STD_SUB(usize, key.len, 1, 1888, 68);
			std_range_to_check(cut, key.len, RUST_FILE, 1888, 62);
			{
				bool		left = access_key_tests_decoded_max(&key_type, orders[o], key.ptr, cut) == 0;

				ACCESS_KEY_TESTS_TYPE_MESSAGE(message, ty);

				if (left != sized)
					std_assert_failed(RUST_FILE, 1888, 17, "==", &left, std_bool_Debug_fmt, &sized,
									  std_bool_Debug_fmt, &message);
			}
			if (!sized)
			{
				cut = STD_SUB(usize, key.len, 1, 1889, 89);
				std_range_to_check(cut, key.len, RUST_FILE, 1889, 83);
				if (!(values[v].bytes.len <= access_key_tests_decoded_max(&key_type, orders[o], key.ptr, cut)))
				{
					ACCESS_KEY_TESTS_TYPE_MESSAGE(message, ty);

					std_panic_fmt(RUST_FILE, 1889, 17, &message);
				}
			}
			{
				ACCESS_KEY_TESTS_TYPE_MESSAGE(message, ty);

				access_key_tests_assert_nine(1890, 17,
											 access_key_tests_skipped(access_key_tests_slice(key.ptr, key.len), &key_type, orders[o]),
											 &message);
			}
			access_key_tests_free(&key);
		}
	}
	access_key_tests_typed_drop(values, 12);
}

/* access_key_tests_option_decoded_bytes_eq is assert_eq!(decoded, Some(Decoded::Bytes(bytes))) at a place. */
static void
access_key_tests_assert_decoded_bytes(uint32 line, uint32 col, const access_key_tests_decoded_t *decoded,
									  const StdVec *bytes)
{
	AccessKeyDecoded expected = {ACCESS_KEY_DECODED_BYTES, .u.bytes = *bytes};
	access_key_tests_option_decoded_t left = {decoded->has, &decoded->value};
	access_key_tests_option_decoded_t right = {true, &expected};

	if (!access_key_tests_option_decoded_eq(&left, &right))
		std_assert_failed(RUST_FILE, line, col, "==", &left, access_key_tests_option_decoded_Debug_fmt, &right,
						  access_key_tests_option_decoded_Debug_fmt, NULL);
}

/* access_key_tests_assert_usize is assert_eq! of two usizes at a place. */
static void
access_key_tests_assert_usize(uint32 line, uint32 col, size_t left, size_t right)
{
	if (left != right)
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt, NULL);
}

/*
 * The fork's own: a varlena value read back from a key takes no more bytes
 * than decoded_max says of the key's length, and a type of a fixed length
 * or without an exact key has no such bound.
 */
void
test_access_key_tests_a_value_read_from_a_key_is_within_its_bound(void)
{
	static const uint8 nul[9] = {0};
	static const uint32 three[3] = {1, 2, 3};
	static const uint8 zeros[100] = {0};
	static const uint32 types[5] = {NAMEOID, TIDOID, INT4OID, NUMERICOID, BPCHAROID};
	access_key_tests_typed_t values[5] = {
		{TEXTOID, access_key_tests_of("abc", 3)},
		{TEXTOID, access_key_tests_of("", 0)},
		{BYTEAOID, access_key_tests_of(nul, sizeof(nul))},
		{OIDVECTOROID, access_key_tests_oidvector(NULL, 0)},
		{OIDVECTOROID, access_key_tests_oidvector(three, 3)},
	};
	AccessKeyKeyType oidvector_type = access_key_KeyType_of(OIDVECTOROID);
	StdVec		empty;

	for (int v = 0; v < 5; v++)
	{
		AccessKeyKeyType key_type = access_key_KeyType_of(values[v].ty);
		StdVec		key = access_key_tests_stored(values[v].ty, &values[v].bytes, ENCODING_KEY_ORDER_ASC);
		access_key_tests_decoded_t decoded = access_key_tests_decode(access_key_tests_slice(key.ptr, key.len), &key_type,
																	 ENCODING_KEY_ORDER_ASC);

		access_key_tests_assert_decoded_bytes(1910, 13, &decoded, &values[v].bytes);
		access_key_tests_decoded_drop(&decoded);
		if (!(values[v].bytes.len <= access_key_tests_decoded_max(&key_type, ENCODING_KEY_ORDER_ASC, key.ptr, key.len)))
		{
			ACCESS_KEY_TESTS_TYPE_MESSAGE(message, values[v].ty);

			std_panic_fmt(RUST_FILE, 1911, 13, &message);
		}
		access_key_tests_free(&key);
	}
	empty = access_key_tests_oidvector(NULL, 0);
	access_key_tests_assert_usize(1913, 9,
								  access_key_tests_decoded_max(&oidvector_type, ENCODING_KEY_ORDER_ASC, zeros, 5),
								  STD_ADD(usize, empty.len, 1, 1913, 86));
	access_key_tests_free(&empty);
	for (int t = 0; t < 5; t++)
	{
		AccessKeyKeyType key_type = access_key_KeyType_of(types[t]);

		access_key_tests_assert_usize(1915, 13,
									  access_key_tests_decoded_max(&key_type, ENCODING_KEY_ORDER_ASC, zeros, 100), 0);
	}
	access_key_tests_typed_drop(values, 5);
}

/*
 * access_key_tests_sorted_keys reports whether stored values' keys of a type
 * in an order are strictly sorted, freeing the values.
 */
static bool
access_key_tests_sorted_keys(uint32 ty, StdVec *values, size_t len, EncodingKeyOrder order)
{
	bool		sorted = true;
	StdVec		previous = {0};

	for (size_t i = 0; i < len; i++)
	{
		StdVec		key = access_key_tests_stored(ty, &values[i], order);

		if (i > 0)
		{
			if (sorted)
				sorted = order.descending ? access_key_tests_vec_lt(&key, &previous) :
					access_key_tests_vec_lt(&previous, &key);
			access_key_tests_free(&previous);
		}
		previous = key;
	}
	if (len > 0)
		access_key_tests_free(&previous);
	for (size_t i = 0; i < len; i++)
		access_key_tests_free(&values[i]);
	return sorted;
}

/* access_key_tests_ascending is the order test's ascending: its keys of stored values of a type are in order. */
static void
access_key_tests_ascending(uint32 ty, StdVec *values, size_t len)
{
	if (!access_key_tests_sorted_keys(ty, values, len, ENCODING_KEY_ORDER_ASC))
	{
		ACCESS_KEY_TESTS_TYPE_MESSAGE(message, ty);

		std_panic_fmt(RUST_FILE, 1925, 13, &message);
	}
}

/*
 * The fork's own: the keys of tids, times with time zones, names, network
 * addresses, bit strings, and oidvectors order as Postgres' comparisons of
 * them do.
 */
void
test_access_key_tests_the_keys_of_stored_forms_order_as_postgres_does(void)
{
	static const uint8 t1[6] = {0, 0, 0, 0, 1, 0};
	static const uint8 t2[6] = {0, 0, 0, 0, 0, 1};
	static const uint8 t3[6] = {0, 0, 1, 0, 0, 0};
	static const uint8 t4[6] = {1, 0, 0, 0, 0, 0};
	static const uint8 i1[6] = {ACCESS_KEY_IPV4, 8, 10, 0, 0, 0};
	static const uint8 i2[6] = {ACCESS_KEY_IPV4, 24, 192, 168, 1, 0};
	static const uint8 i3[6] = {ACCESS_KEY_IPV4, 24, 192, 168, 1, 5};
	static const uint8 i4[6] = {ACCESS_KEY_IPV4, 32, 192, 168, 1, 5};
	static const uint8 i5[18] = {ACCESS_KEY_IPV4 + 1, 0};
	static const uint8 b1[4] = {0, 0, 0, 0};
	static const uint8 b2[5] = {1, 0, 0, 0, 0};
	static const uint8 b3[5] = {2, 0, 0, 0, 0};
	static const uint8 b4[5] = {1, 0, 0, 0, 0x80};
	static const uint32 one[1] = {1};
	static const uint32 two[1] = {2};
	static const uint32 ones[2] = {1, 1};
	StdVec		tids[4] = {access_key_tests_of(t1, 6), access_key_tests_of(t2, 6), access_key_tests_of(t3, 6),
	access_key_tests_of(t4, 6)};
	StdVec		times[5];
	StdVec		names[4];
	StdVec		inets[5];
	StdVec		bits[4];
	StdVec		vectors[4];

	access_key_tests_ascending(TIDOID, tids, 4);
	times[0] = access_key_tests_timetz(0, -7200);
	times[1] = access_key_tests_timetz(1, -7200);
	times[2] = access_key_tests_timetz(INT64CONST(3600000000), -3600);
	times[3] = access_key_tests_timetz(0, 0);
	times[4] = access_key_tests_timetz(INT64CONST(3600000000), 3600);
	access_key_tests_ascending(TIMETZOID, times, 5);
	names[0] = access_key_tests_name("");
	names[1] = access_key_tests_name("a");
	names[2] = access_key_tests_name("ab");
	names[3] = access_key_tests_name("b");
	access_key_tests_ascending(NAMEOID, names, 4);
	inets[0] = access_key_tests_of(i1, 6);
	inets[1] = access_key_tests_of(i2, 6);
	inets[2] = access_key_tests_of(i3, 6);
	inets[3] = access_key_tests_of(i4, 6);
	inets[4] = access_key_tests_of(i5, 18);
	access_key_tests_ascending(INETOID, inets, 5);
	bits[0] = access_key_tests_of(b1, 4);
	bits[1] = access_key_tests_of(b2, 5);
	bits[2] = access_key_tests_of(b3, 5);
	bits[3] = access_key_tests_of(b4, 5);
	access_key_tests_ascending(VARBITOID, bits, 4);
	vectors[0] = access_key_tests_oidvector(NULL, 0);
	vectors[1] = access_key_tests_oidvector(one, 1);
	vectors[2] = access_key_tests_oidvector(two, 1);
	vectors[3] = access_key_tests_oidvector(ones, 2);
	access_key_tests_ascending(OIDVECTOROID, vectors, 4);
}

/* access_key_tests_special is the numeric test's special: a stored numeric of a header alone. */
static StdVec
access_key_tests_special(uint16 header)
{
	StdVec		out;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_le16(&out, header);
	return out;
}

/* access_key_tests_numerics fills the numeric test's numerics, in order. */
static void
access_key_tests_numerics(StdVec numerics[12])
{
	static const uint16 d1[1] = {1};
	static const uint16 d15[2] = {1, 5000};
	static const uint16 d101[3] = {1, 0, 1};

	numerics[0] = access_key_tests_special(0xf000);
	numerics[1] = access_key_tests_numeric(true, 1, d1, 1, false);
	numerics[2] = access_key_tests_numeric(true, 0, d15, 2, true);
	numerics[3] = access_key_tests_numeric(true, -1, d1, 1, true);
	numerics[4] = access_key_tests_numeric(false, 0, NULL, 0, true);
	numerics[5] = access_key_tests_numeric(false, -1, d1, 1, false);
	numerics[6] = access_key_tests_numeric(false, 0, d1, 1, true);
	numerics[7] = access_key_tests_numeric(false, 0, d15, 2, true);
	numerics[8] = access_key_tests_numeric(false, 1, d1, 1, true);
	numerics[9] = access_key_tests_numeric(false, 1, d101, 3, false);
	numerics[10] = access_key_tests_special(0xd000);
	numerics[11] = access_key_tests_special(0xc000);
}

/*
 * The fork's own: numerics order by their values whatever their header and
 * trailing zeros, intervals by their spans, a month as thirty days, and
 * both are skipped whole.
 */
void
test_access_key_tests_numeric_and_interval_keys_order_as_postgres_does_and_are_skipped(void)
{
	static const uint16 d15[2] = {1, 5000};
	static const uint16 d150[3] = {1, 5000, 0};
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, {true, false}};
	AccessKeyKeyType numeric = access_key_KeyType_of(NUMERICOID);
	AccessKeyKeyType interval = access_key_KeyType_of(INTERVALOID);
	StdVec		numerics[12];
	StdVec		same[2];
	StdVec		intervals[4];
	StdVec		keys[4];
	StdVec		key;
	StdVec		thirty;
	uint8		nine = 9;
	bool		sorted = true;

	for (int o = 0; o < 2; o++)
	{
		access_key_tests_numerics(numerics);
		STD_ASSERT(1977, 13, access_key_tests_sorted_keys(NUMERICOID, numerics, 12, orders[o]),
				   "keys.is_sorted_by(|a, b| if order.descending { a > b } else { a < b })");
		access_key_tests_numerics(numerics);
		for (int i = 0; i < 12; i++)
		{
			StdVec		with = access_key_tests_stored(NUMERICOID, &numerics[i], orders[o]);

			std_vec_push_Checked(&with, &nine, ACCESS_KEY_BYTES);
			access_key_tests_assert_nine(1980, 17,
										 access_key_tests_skipped(access_key_tests_slice(with.ptr, with.len), &numeric, orders[o]),
										 NULL);
			access_key_tests_free(&with);
			access_key_tests_free(&numerics[i]);
		}
	}
	same[0] = access_key_tests_numeric(false, 0, d15, 2, true);
	same[1] = access_key_tests_numeric(false, 0, d150, 3, false);
	access_key_tests_assert_vec_eq(1984, 9, access_key_tests_stored(NUMERICOID, &same[0], ENCODING_KEY_ORDER_ASC),
								   access_key_tests_stored(NUMERICOID, &same[1], ENCODING_KEY_ORDER_ASC));
	access_key_tests_free(&same[0]);
	access_key_tests_free(&same[1]);
	STD_ASSERT(1985, 9, !access_key_is_exact(&numeric) && !access_key_is_exact(&interval),
			   "!is_exact(&KeyType::of(NUMERICOID)) && !is_exact(&KeyType::of(INTERVALOID))");
	intervals[0] = access_key_tests_interval(-1, 0, 0);
	intervals[1] = access_key_tests_interval(0, 29, INT64CONST(86399999999));
	intervals[2] = access_key_tests_interval(1, 0, 0);
	intervals[3] = access_key_tests_interval(1, 0, 1);
	for (int i = 0; i < 4; i++)
		keys[i] = access_key_tests_stored(INTERVALOID, &intervals[i], ENCODING_KEY_ORDER_ASC);
	for (int i = 0; i + 1 < 4 && sorted; i++)
		sorted = access_key_tests_vec_lt(&keys[i], &keys[i + 1]);
	STD_ASSERT(1988, 9, sorted, "keys.is_sorted_by(|a, b| a < b)");
	thirty = access_key_tests_interval(0, 30, 0);
	std_index_check(2, 4, RUST_FILE, 1989, 20);
	access_key_tests_assert_vec_eq(1989, 9, access_key_tests_of(keys[2].ptr, keys[2].len),
								   access_key_tests_stored(INTERVALOID, &thirty, ENCODING_KEY_ORDER_ASC));
	access_key_tests_free(&thirty);
	std_index_check(3, 4, RUST_FILE, 1990, 21);
	key = access_key_tests_of(keys[3].ptr, keys[3].len);
	std_vec_push_Checked(&key, &nine, ACCESS_KEY_BYTES);
	access_key_tests_assert_nine(1991, 9,
								 access_key_tests_skipped(access_key_tests_slice(key.ptr, key.len), &interval, ENCODING_KEY_ORDER_ASC),
								 NULL);
	access_key_tests_free(&key);
	for (int i = 0; i < 4; i++)
	{
		access_key_tests_free(&keys[i]);
		access_key_tests_free(&intervals[i]);
	}
}

/*
 * The fork's own: stored bytes that are not their type's form are refused
 * as corrupt data, and a key naming no address's family, or a name longer
 * than a name, doesn't decode.
 */
void
test_access_key_tests_a_stored_value_that_is_not_its_types_form_is_refused(void)
{
	static const uint8 zeros[19] = {0};
	static const uint8 inet1[6] = {9, 0, 1, 2, 3, 4};
	static const uint8 inet2[6] = {ACCESS_KEY_IPV4, 33, 1, 2, 3, 4};
	static const uint8 cidr[6] = {ACCESS_KEY_IPV4 + 1, 0, 1, 2, 3, 4};
	static const uint8 bit[3] = {1, 0, 0};
	static const uint8 two[20] = {2, 0, 0, 0};
	static const uint32 one[1] = {1};
	static const uint16 big[1] = {10000};
	static const uint16 d1[1] = {1};
	uint8		zero = 0;
	access_key_tests_typed_t cases[15] = {
		{TIDOID, access_key_tests_of(zeros, 5)},
		{INTERVALOID, access_key_tests_of(zeros, 15)},
		{TIMETZOID, access_key_tests_timetz(0, 57600)},
		{TIMETZOID, access_key_tests_of(zeros, 11)},
		{INETOID, access_key_tests_of(inet1, 6)},
		{INETOID, access_key_tests_of(inet2, 6)},
		{CIDROID, access_key_tests_of(cidr, 6)},
		{BITOID, access_key_tests_of(bit, 3)},
		{OIDVECTOROID, access_key_tests_of(zeros, 19)},
		{OIDVECTOROID, access_key_tests_of(two, 20)},
		{OIDVECTOROID, access_key_tests_oidvector(one, 1)},
		{NUMERICOID, access_key_tests_of(zeros, 1)},
		{NUMERICOID, access_key_tests_special_numeric()},
		{NUMERICOID, access_key_tests_numeric(false, 0, big, 1, true)},
		{NUMERICOID, access_key_tests_numeric(false, 0, d1, 1, true)},
	};
	StdVec		long_name;
	static const uint8 inet[8] = {ENCODING_KEY_VALUE, 9, 0, 1, 0, 0, 0, 0};
	static const uint8 vector[9] = {ENCODING_KEY_VALUE, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 1};
	static const uint8 marker = ENCODING_KEY_VALUE;
	AccessKeyKeyType name = access_key_KeyType_of(NAMEOID);
	AccessKeyKeyType inet_type = access_key_KeyType_of(INETOID);
	AccessKeyKeyType vector_type = access_key_KeyType_of(OIDVECTOROID);

	std_vec_push_Checked(&cases[10].bytes, &zero, ACCESS_KEY_BYTES);
	std_vec_push_Checked(&cases[14].bytes, &zero, ACCESS_KEY_BYTES);
	for (int i = 0; i < 15; i++)
	{
		PgPgError	error = access_key_tests_unencoded(cases[i].ty, access_key_tests_bytes(cases[i].bytes.ptr,
																						   cases[i].bytes.len));

		ACCESS_KEY_TESTS_TYPE_MESSAGE(message, cases[i].ty);

		access_key_tests_assert_code(2019, 13, &error, "XX001", &message);
		pg_PgError_drop(&error);
	}
	long_name = access_key_tests_of(&marker, 1);
	for (int i = 0; i < ACCESS_KEY_NAME_LEN; i++)
		access_key_push(&long_name, 'a');
	access_key_push(&long_name, 0);
	STD_ASSERT(2022, 9,
			   access_key_tests_decode_is_err(access_key_tests_slice(long_name.ptr, long_name.len), &name,
											  ENCODING_KEY_ORDER_ASC),
			   "decode(&long, &KeyType::of(NAMEOID), Order::ASC).is_err()");
	STD_ASSERT(2023, 9, access_key_tests_decode_is_err(access_key_tests_slice(inet, sizeof(inet)), &inet_type,
													   ENCODING_KEY_ORDER_ASC),
			   "decode(&[VALUE, 9, 0, 1, 0, 0, 0, 0], &KeyType::of(INETOID), Order::ASC).is_err()");
	STD_ASSERT(2024, 9, access_key_tests_decode_is_err(access_key_tests_slice(vector, sizeof(vector)), &vector_type,
													   ENCODING_KEY_ORDER_ASC),
			   "decode(&[VALUE, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 1], &KeyType::of(OIDVECTOROID), Order::ASC).is_err()");
	access_key_tests_free(&long_name);
	access_key_tests_typed_drop(cases, 15);
}

/*
 * access_key_tests_within is tests' within: a value's key column of a type in
 * an order cut at a bound, and whether it was cut.
 */
static StdVec
access_key_tests_within(uint32 ty, StdBytes value, EncodingKeyOrder order, size_t bound, bool *cut)
{
	StdVec		out;
	AccessKeyKeyType key_type = access_key_KeyType_of(ty);
	AccessKeyNames names = {&ACCESS_KEY_REVERSED_NAMES_OPS, NULL};
	PgPgError	error;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_tests_ok(access_key_encode_within(&out, ACCESS_KEY_REVERSED, names, &key_type, true,
												 access_key_tests_bytes(value.ptr, value.len), order, bound, false,
												 cut, &error), &error);
	return out;
}

/* access_key_tests_skip_within is skip_within's answer where the call must succeed. */
static StdBytes
access_key_tests_skip_within(StdBytes key, const AccessKeyKeyType *key_type, EncodingKeyOrder order, size_t bound,
							 bool *cut)
{
	StdBytes	rest;
	PgPgError	error;

	access_key_tests_ok(access_key_skip_within(key.ptr, key.len, key_type, order, bound, &rest, cut, &error), &error);
	return rest;
}

/* access_key_tests_rest_is_nine reports whether bytes are an int4 9's key in an order. */
static bool
access_key_tests_rest_is_nine(StdBytes rest, EncodingKeyOrder order)
{
	StdVec		nine = access_key_tests_int4_key(9, order);
	bool		same = access_key_tests_bytes_eq(rest.ptr, rest.len, nine.ptr, nine.len);

	access_key_tests_free(&nine);
	return same;
}

/*
 * The fork's own (design item 4's form D): a key column longer than its
 * bound is its first bytes up to the bound and the address of the whole
 * encoding, and a reader that knows the bound finds the next column after
 * either form.
 */
void
test_access_key_tests_a_key_column_past_its_bound_is_cut_and_skipped_by_the_bound(void)
{
	size_t		bound = 40;
	uint16		ones[30];
	StdVec		long_numeric;
	StdVec		long_bits;
	StdVec		vector;
	uint32		sevens[12];
	uint8		a[100];
	uint8		b[39];
	uint8		c[64];
	uint8		zeros[30] = {0};
	access_key_tests_typed_t values[7];

	for (int i = 0; i < 30; i++)
		ones[i] = 1;
	for (int i = 0; i < 12; i++)
		sevens[i] = 7;
	memset(a, 'a', sizeof(a));
	memset(b, 'b', sizeof(b));
	memset(c, 'c', sizeof(c));
	long_numeric = access_key_tests_numeric(false, 0, ones, 30, false);
	std_vec_new(&long_bits, ACCESS_KEY_BYTES);
	access_key_le32(&long_bits, 400);
	for (int i = 0; i < 50; i++)
		access_key_push(&long_bits, 0xa5);
	vector = access_key_tests_oidvector(sevens, 12);
	values[0] = (access_key_tests_typed_t)
	{
		TEXTOID, access_key_tests_of(a, sizeof(a))
	};
	values[1] = (access_key_tests_typed_t)
	{
		VARCHAROID, access_key_tests_of(b, sizeof(b))
	};
	values[2] = (access_key_tests_typed_t)
	{
		BPCHAROID, access_key_tests_of(c, sizeof(c))
	};
	values[3] = (access_key_tests_typed_t)
	{
		BYTEAOID, access_key_tests_of(zeros, sizeof(zeros))
	};
	values[4] = (access_key_tests_typed_t)
	{
		NUMERICOID, long_numeric
	};
	values[5] = (access_key_tests_typed_t)
	{
		VARBITOID, long_bits
	};
	values[6] = (access_key_tests_typed_t)
	{
		OIDVECTOROID, vector
	};
	for (int d = 0; d < 2; d++)
	{
		EncodingKeyOrder order = {d == 1, d == 1};

		for (int v = 0; v < 7; v++)
		{
			uint32		ty = values[v].ty;
			AccessKeyKeyType key_type = access_key_KeyType_of(ty);
			StdBytes	value = {values[v].bytes.ptr, values[v].bytes.len};
			StdVec		whole = access_key_tests_stored(ty, &values[v].bytes, order);
			bool		was_cut;
			StdVec		cut;
			StdVec		kept;
			StdVec		nine;
			StdBytes	rest;
			bool		skipped_cut;
			bool		holds;

			STD_ASSERT(2068, 17, whole.len > bound, "whole.len() > bound");
			cut = access_key_tests_within(ty, value, order, bound, &was_cut);
			STD_ASSERT(2070, 17,
					   was_cut && cut.len == STD_ADD(usize, bound, ACCESS_KEY_ADDRESS_LEN, 2070, 49),
					   "was_cut && cut.len() == bound + ADDRESS_LEN");
			std_range_to_check(bound, cut.len, RUST_FILE, 2071, 25);
			std_range_to_check(bound, whole.len, RUST_FILE, 2071, 41);
			holds = memcmp(cut.ptr, whole.ptr, bound) == 0;
			if (holds)
			{
				ChunksAddressAddress address = chunks_address_Address_of(whole.ptr, whole.len);

				std_range_from_check(bound, cut.len, RUST_FILE, 2071, 59);
				holds = access_key_tests_bytes_eq((const uint8 *) cut.ptr + bound, cut.len - bound, address.f0,
												  sizeof(address.f0));
			}
			STD_ASSERT(2071, 17, holds,
					   "cut[..bound] == whole[..bound] && cut[bound..] == chunks::Address::of(&whole).0");
			nine = access_key_tests_int4_key(9, order);
			std_vec_extend_from_slice_Checked(&cut, nine.ptr, nine.len, ACCESS_KEY_BYTES);
			access_key_tests_free(&nine);
			rest = access_key_tests_skip_within(access_key_tests_slice(cut.ptr, cut.len), &key_type, order, bound, &skipped_cut);
			STD_ASSERT(2074, 17, skipped_cut && access_key_tests_rest_is_nine(rest, order),
					   "skipped_cut && rest == key(INT4OID, int4(9), order)");
			kept = access_key_tests_within(ty, value, order, whole.len, &was_cut);
			STD_ASSERT(2076, 17, !was_cut && access_key_tests_bytes_eq(kept.ptr, kept.len, whole.ptr, whole.len),
					   "!was_cut && kept == whole");
			nine = access_key_tests_int4_key(9, order);
			std_vec_extend_from_slice_Checked(&kept, nine.ptr, nine.len, ACCESS_KEY_BYTES);
			access_key_tests_free(&nine);
			rest = access_key_tests_skip_within(access_key_tests_slice(kept.ptr, kept.len), &key_type, order, whole.len,
												&skipped_cut);
			STD_ASSERT(2079, 17, !skipped_cut && access_key_tests_rest_is_nine(rest, order),
					   "!skipped_cut && rest == key(INT4OID, int4(9), order)");
			access_key_tests_free(&kept);
			access_key_tests_free(&cut);
			access_key_tests_free(&whole);
		}
	}
	access_key_tests_typed_drop(values, 7);
}

/*
 * The fork's own: a column is cut exactly when its encoding is longer than
 * the bound, and a key that stops inside a cut column is refused.
 */
void
test_access_key_tests_a_key_column_is_cut_one_byte_past_its_bound(void)
{
	static const struct
	{
		size_t		len;
		bool		cut;
	}			cases[3] = {{37, false}, {38, false}, {39, true}};
	AccessKeyKeyType str = access_key_KeyType_of(TEXTOID);
	uint8		xs[100];
	StdVec		column;
	bool		was_cut;

	memset(xs, 'x', sizeof(xs));
	for (int i = 0; i < 3; i++)
	{
		StdBytes	rest;
		bool		skipped_cut;
		size_t		expected;

		column = access_key_tests_within(TEXTOID, access_key_tests_slice(xs, cases[i].len), ENCODING_KEY_ORDER_ASC, 40,
										 &was_cut);
		expected = cases[i].cut ? STD_ADD(usize, 40, ACCESS_KEY_ADDRESS_LEN, 2091, 64) :
			STD_ADD(usize, cases[i].len, 2, 2091, 90);
		STD_ASSERT(2091, 13, was_cut == cases[i].cut && column.len == expected,
				   "was_cut == cut && column.len() == if cut { 40 + ADDRESS_LEN } else { len + 2 }");
		rest = access_key_tests_skip_within(access_key_tests_slice(column.ptr, column.len), &str, ENCODING_KEY_ORDER_ASC, 40,
											&skipped_cut);
		STD_ASSERT(2093, 13, skipped_cut == cases[i].cut && rest.len == 0,
				   "skipped_cut == cut && rest.is_empty()");
		access_key_tests_free(&column);
	}
	column = access_key_tests_within(TEXTOID, access_key_tests_slice(xs, sizeof(xs)), ENCODING_KEY_ORDER_ASC, 40,
									 &was_cut);
	std_range_to_check(60, column.len, RUST_FILE, 2096, 30);
	{
		StdBytes	rest;
		bool		cut;
		PgPgError	error;
		bool		ok = access_key_skip_within(column.ptr, 60, &str, ENCODING_KEY_ORDER_ASC, 40, &rest, &cut, &error);

		if (!ok)
			pg_PgError_drop(&error);
		STD_ASSERT(2096, 9, !ok, "skip_within(&column[..60], &KeyType::of(TEXTOID), order, 40).is_err()");
	}
	access_key_tests_free(&column);
}

/* access_key_tests_types is the bound tests' types: a run of key types of some types. */
static AccessKeyKeyTypes
access_key_tests_types(AccessKeyKeyType *made, const uint32 *ids, size_t len)
{
	AccessKeyKeyTypes key = {made, len, sizeof(AccessKeyKeyType), 0};

	for (size_t i = 0; i < len; i++)
		made[i] = access_key_KeyType_of(ids[i]);
	return key;
}

/*
 * The fork's own: a table's bound is a function of its key's types, and
 * only a type that can pass the bound can be cut.
 */
void
test_access_key_tests_a_tables_bound_follows_its_keys_types(void)
{
	AccessKeyKeyType made[32];
	uint32		texts[32];
	static const uint32 int_text[2] = {INT4OID, TEXTOID};
	static const uint32 four[4] = {TEXTOID, UUIDOID, BYTEAOID, INT8OID};
	static const uint32 name_inet[2] = {NAMEOID, INETOID};
	static const uint32 cut[8] = {TEXTOID, VARCHAROID, BPCHAROID, BYTEAOID, NUMERICOID, BITOID, VARBITOID, OIDVECTOROID};
	static const uint32 uncut[8] = {INT4OID, UUIDOID, TIMETZOID, FLOAT8OID, INTERVALOID, NAMEOID, INETOID, CIDROID};
	size_t		bound;
	char		ns[64];
	StdVec		stored_name;
	StdVec		key;
	uint8		widest[18] = {3, 128};
	StdVec inet;

	for (int i = 0; i < 32; i++)
		texts[i] = TEXTOID;
	STD_ASSERT(2105, 9, access_key_table_bound(access_key_tests_types(made, texts, 1)) == CHUNKS_MAX_ENTRY / 2 - 33,
			   "table_bound(types(&[TEXTOID]).iter()) == MAX_ENTRY / 2 - 33");
	STD_ASSERT(2106, 9, access_key_table_bound(access_key_tests_types(made, int_text, 2)) == CHUNKS_MAX_ENTRY / 2 - 5 - 33,
			   "table_bound(types(&[INT4OID, TEXTOID]).iter()) == MAX_ENTRY / 2 - 5 - 33");
	STD_ASSERT(2107, 9,
			   access_key_table_bound(access_key_tests_types(made, four, 4)) == (CHUNKS_MAX_ENTRY / 2 - 17 - 9) / 2 - 33,
			   "table_bound(types(&[TEXTOID, UUIDOID, BYTEAOID, INT8OID]).iter()) == (MAX_ENTRY / 2 - 17 - 9) / 2 - 33");
	STD_ASSERT(2108, 9, access_key_table_bound(access_key_tests_types(made, texts, 32)) == 223,
			   "table_bound(types(&[TEXTOID; 32]).iter()) == 223");
	STD_ASSERT(2109, 9, access_key_table_bound(access_key_tests_types(made, name_inet, 2)) == CHUNKS_MAX_ENTRY / 4 - 33,
			   "table_bound(types(&[NAMEOID, INETOID]).iter()) == MAX_ENTRY / 4 - 33");
	bound = access_key_table_bound(access_key_tests_types(made, texts, 32));
	for (int i = 0; i < 8; i++)
	{
		AccessKeyKeyType key_type = access_key_KeyType_of(cut[i]);

		STD_ASSERT(2112, 13, access_key_can_be_cut(&key_type, bound), "can_be_cut(&KeyType::of(ty), bound)");
	}
	for (int i = 0; i < 8; i++)
	{
		AccessKeyKeyType key_type = access_key_KeyType_of(uncut[i]);

		STD_ASSERT(2115, 13, !access_key_can_be_cut(&key_type, bound), "!can_be_cut(&KeyType::of(ty), bound)");
	}
	memset(ns, 'n', 63);
	ns[63] = '\0';
	stored_name = access_key_tests_name(ns);
	key = access_key_tests_stored(NAMEOID, &stored_name, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(2117, 9, key.len == 1 + ACCESS_KEY_NAME_LEN,
			   "stored(NAMEOID, &name(&\"n\".repeat(63)), Order::ASC).len() == 1 + NAME_LEN");
	access_key_tests_free(&key);
	access_key_tests_free(&stored_name);
	inet = access_key_tests_of(widest, sizeof(widest));

	key = access_key_tests_stored(INETOID, &inet, ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(2119, 9, key.len == 56, "stored(INETOID, &widest, Order::ASC).len() == 56");
	access_key_tests_free(&key);
	access_key_tests_free(&inet);
}

/*
 * The fork's own (design item 4's form D): an index's bound leaves room for
 * its value's tuple, so its largest entry fits an entry's limit.
 */
void
test_access_key_tests_an_indexs_bound_fits_its_largest_entry(void)
{
	AccessKeyKeyType made[32];
	uint32		texts[32];
	static const uint32 int4[1] = {INT4OID};
	static const uint32 int_uuid[2] = {INT4OID, UUIDOID};
	static const size_t parts[2] = {ACCESS_KEY_REFERENCE_LEN, 4};
	StdUsizeOption references[32];
	size_t		widest;
	size_t		bound;
	size_t		value;
	size_t		int_key;
	bool		holds;

	for (int i = 0; i < 32; i++)
	{
		texts[i] = TEXTOID;
		references[i] = (StdUsizeOption)
		{
			true, ACCESS_KEY_REFERENCE_LEN
		};
	}
	widest = access_key_table_key_most(access_key_tests_types(made, texts, 32));
	STD_ASSERT(2130, 9, widest == CHUNKS_MAX_ENTRY / 2, "widest == MAX_ENTRY / 2");
	bound = access_key_index_bound(access_key_tests_types(made, texts, 32), widest, 0, NULL, 0);
	STD_ASSERT(2132, 9, bound == 183, "bound == 183");
	value = encoding_tuple_written_len(references, 32, true, false, 0);
	holds = value == 1258;
	if (holds)
	{
		size_t		sum = STD_MUL(usize, 32, STD_ADD(usize, bound, ACCESS_KEY_CUT_MORE, 2134, 40),
								  2134, 34);

		sum = STD_ADD(usize, sum, widest, 2134, 34);
		sum = STD_ADD(usize, sum, value, 2134, 34);
		sum = STD_ADD(usize, sum, ACCESS_HEAPTUPLE_ENTRY_OVERHEAD, 2134, 34);
		holds = sum <= CHUNKS_MAX_ENTRY;
	}
	STD_ASSERT(2134, 9, holds,
			   "value == 1258 && 32 * (bound + CUT_MORE) + widest + value + ENTRY_OVERHEAD <= MAX_ENTRY");
	int_key = access_key_table_key_most(access_key_tests_types(made, int4, 1));
	STD_ASSERT(2136, 9,
			   int_key == 5 && access_key_index_bound(access_key_tests_types(made, texts, 1), int_key, 0, NULL, 0) == 16300,
			   "int_key == 5 && index_bound(types(&[TEXTOID]).iter(), int_key, 0, &[]) == 16_300");
	STD_ASSERT(2137, 9,
			   access_key_index_bound(access_key_tests_types(made, texts, 1), int_key, 10, NULL, 0) == 16290,
			   "index_bound(types(&[TEXTOID]).iter(), int_key, 10, &[]) == 16_290");
	STD_ASSERT(2138, 9,
			   access_key_index_bound(access_key_tests_types(made, int_uuid, 2), int_key, 0, parts, 2) == CHUNKS_MAX_ENTRY,
			   "index_bound(types(&[INT4OID, UUIDOID]).iter(), int_key, 0, &[REFERENCE_LEN, 4]) == MAX_ENTRY");
}

/* access_key_tests_element_t is a test array's element, an Option<Vec<u8>>. */
typedef struct access_key_tests_element_t
{
	bool		has;
	StdVec		bytes;
} access_key_tests_element_t;

/* access_key_tests_dim_t is a test array's dimension, an (i32, i32): its length and lower bound. */
typedef struct access_key_tests_dim_t
{
	int32		dim;
	int32		lb;
} access_key_tests_dim_t;

/*
 * access_key_tests_stored_array is tests' stored_array: an array's stored
 * form without its varlena header, as Postgres' construct_md_array writes
 * it. It takes the elements.
 */
static StdVec
access_key_tests_stored_array(const access_key_tests_dim_t *dims, size_t ndim, access_key_tests_element_t *elements,
							  size_t len, size_t align)
{
	bool		nulls = false;
	size_t		bitmap;
	size_t		start;
	StdVec		out;

	for (size_t i = 0; i < len && !nulls; i++)
		nulls = !elements[i].has;
	bitmap = nulls ? (len + 7) / 8 : 0;
	start = std_next_multiple_of_usize(STD_ADD(usize, STD_ADD(usize, ACCESS_KEY_ARRAY_HEADER,
															  STD_MUL(usize, 8, ndim, 2148, 37),
															  2148, 22),
											   bitmap, 2148, 22),
									   ACCESS_KEY_MAXIMUM_ALIGNOF);
	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_le32(&out, (uint32) ndim);
	access_key_le32(&out, nulls ? (uint32) start : 0);
	access_key_le32(&out, 0);
	for (size_t d = 0; d < ndim; d++)
		access_key_le32(&out, (uint32) dims[d].dim);
	for (size_t d = 0; d < ndim; d++)
		access_key_le32(&out, (uint32) dims[d].lb);
	if (nulls)
	{
		StdVec		bits;
		uint8		zero = 0;

		std_vec_from_elem_zeroed(&bits, bitmap, ACCESS_KEY_BYTES);
		for (size_t i = 0; i < len; i++)
		{
			if (!elements[i].has)
				continue;
			std_index_check(i / 8, bits.len, RUST_FILE, 2159, 21);
			((uint8 *) bits.ptr)[i / 8] |= STD_SHL(u8, 1, i % 8, 2159, 36);
		}
		(void) zero;
		std_vec_extend_from_slice_Checked(&out, bits.ptr, bits.len, ACCESS_KEY_BYTES);
		access_key_tests_free(&bits);
	}
	for (size_t i = 0; i < len; i++)
	{
		if (!elements[i].has)
			continue;
		while (!std_is_multiple_of_usize(STD_ADD(usize, out.len, ACCESS_KEY_VARLENA_HEADER, 2165, 21),
										 align) ||
			   STD_ADD(usize, out.len, ACCESS_KEY_VARLENA_HEADER, 2165, 74) < start)
			access_key_push(&out, 0);
		std_vec_extend_from_slice_Checked(&out, elements[i].bytes.ptr, elements[i].bytes.len, ACCESS_KEY_BYTES);
	}
	for (size_t i = 0; i < len; i++)
		if (elements[i].has)
			access_key_tests_free(&elements[i].bytes);
	return out;
}

/* access_key_tests_int4s is tests' int4s: an int4 array's elements' stored bytes, has false for NULL. */
static void
access_key_tests_int4s(const int32 *values, const bool *has, size_t len, access_key_tests_element_t *result)
{
	for (size_t i = 0; i < len; i++)
	{
		uint8		le[4] = {(uint8) values[i], (uint8) ((uint32) values[i] >> 8), (uint8) ((uint32) values[i] >> 16),
		(uint8) ((uint32) values[i] >> 24)};

		result[i].has = has[i];
		if (has[i])
			result[i].bytes = access_key_tests_of(le, sizeof(le));
	}
}

/* access_key_tests_texts is tests' texts: a text array's elements, each a varlena with its four-byte header. */
static void
access_key_tests_texts(const char *const *values, size_t len, access_key_tests_element_t *result)
{
	for (size_t i = 0; i < len; i++)
	{
		size_t		str_len = strlen(values[i]);

		result[i].has = true;
		std_vec_new(&result[i].bytes, ACCESS_KEY_BYTES);
		access_key_le32(&result[i].bytes, (uint32) STD_ADD(usize, ACCESS_KEY_VARLENA_HEADER, str_len,
														   2180, 38) << 2);
		std_vec_extend_from_slice_Checked(&result[i].bytes, values[i], str_len, ACCESS_KEY_BYTES);
	}
}

/* access_key_tests_array_type returns the key type of an array of elements of a type, which the caller drops. */
static AccessKeyKeyType
access_key_tests_array_type(uint32 element, int16 len, bool by_value, uint8 align)
{
	AccessKeyKeyType key_type = {.typid = 0, .inner.tag = ACCESS_KEY_INNER_ARRAY};

	key_type.inner.u.array.key_type = std_alloc(ACCESS_KEY_KEY_TYPE);
	*key_type.inner.u.array.key_type = access_key_KeyType_of(element);
	key_type.inner.u.array.len = len;
	key_type.inner.u.array.by_value = by_value;
	key_type.inner.u.array.align = align;
	return key_type;
}

/*
 * access_key_tests_array_column is tests' array_column: an array's key column,
 * its elements of a key type. It takes the stored form.
 */
static StdVec
access_key_tests_array_column(uint32 element, int16 len, bool by_value, uint8 align, StdVec stored,
							  EncodingKeyOrder order)
{
	AccessKeyKeyType key_type = access_key_tests_array_type(element, len, by_value, align);
	StdVec		out;
	PgPgError	error;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_tests_ok(access_key_encode(&out, ACCESS_KEY_REVERSED, &key_type, true,
										  access_key_tests_bytes(stored.ptr, stored.len), order, &error), &error);
	access_key_KeyType_drop(&key_type);
	access_key_tests_free(&stored);
	return out;
}

/* access_key_tests_one is the array test's one: a one-dimensional int4 array from 1. */
static StdVec
access_key_tests_one(const int32 *values, const bool *has, size_t len)
{
	access_key_tests_dim_t dim = {(int32) len, 1};
	access_key_tests_element_t elements[3];

	access_key_tests_int4s(values, has, len, elements);
	return access_key_tests_stored_array(&dim, 1, elements, len, 4);
}

/* access_key_tests_ordered fills the array test's ordered arrays. */
static void
access_key_tests_ordered(StdVec ordered[9])
{
	static const bool yes[3] = {true, true, true};
	static const bool some_none[2] = {true, false};
	static const int32 m5[1] = {-5};
	static const int32 v12[2] = {1, 2};
	static const int32 v123[3] = {1, 2, 3};
	static const int32 v13[2] = {1, 3};
	static const int32 v1x[2] = {1, 0};
	static const int32 v2[1] = {2};
	static const access_key_tests_dim_t none_dims[1] = {{0, 1}};
	static const access_key_tests_dim_t from0[1] = {{2, 0}};
	static const access_key_tests_dim_t two_dims[2] = {{1, 1}, {2, 1}};
	access_key_tests_element_t elements[2];

	std_range_to_check(0, 1, RUST_FILE, 2206, 27);
	ordered[0] = access_key_tests_stored_array(none_dims, 0, NULL, 0, 4);
	ordered[1] = access_key_tests_one(m5, yes, 1);
	ordered[2] = access_key_tests_one(v12, yes, 2);
	ordered[3] = access_key_tests_one(v123, yes, 3);
	access_key_tests_int4s(v13, yes, 2, elements);
	ordered[4] = access_key_tests_stored_array(from0, 1, elements, 2, 4);
	ordered[5] = access_key_tests_one(v13, yes, 2);
	access_key_tests_int4s(v13, yes, 2, elements);
	ordered[6] = access_key_tests_stored_array(two_dims, 2, elements, 2, 4);
	ordered[7] = access_key_tests_one(v1x, some_none, 2);
	ordered[8] = access_key_tests_one(v2, yes, 1);
}

/* access_key_tests_pair_message_Debug_fmt is the array test's message: its pair of keys' Debug. */
static void
access_key_tests_assert_pair_lt(uint32 line, uint32 col, const StdVec *a, const StdVec *b, bool message)
{
	if (access_key_tests_vec_lt(a, b))
		return;
	if (message)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(" "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{a, access_key_tests_vec_Debug_fmt}, {b, access_key_tests_vec_Debug_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		std_panic_fmt(RUST_FILE, line, col, &arguments);
	}
	STD_ASSERT(line, col, false, "pair[0] > pair[1]");
}

/* access_key_tests_text_array is the array test's text: a text array's key column. */
static StdVec
access_key_tests_text_array(const char *const *values, size_t len)
{
	access_key_tests_dim_t dim = {(int32) len, 1};
	access_key_tests_element_t elements[2];

	access_key_tests_texts(values, len, elements);
	return access_key_tests_array_column(TEXTOID, -1, false, 'i', access_key_tests_stored_array(&dim, 1, elements, len, 4),
										 ENCODING_KEY_ORDER_ASC);
}

/*
 * The fork's own (Postgres' array_cmp): arrays' keys order them element by
 * element, then by length, dimensions, and lower bounds, each key one
 * column a reader finds the end of, and a descending column orders them the
 * other way.
 */
void
test_access_key_tests_arrays_order_as_postgres_compares_them(void)
{
	StdVec		ordered[9];
	StdVec		keys[9];
	StdVec		descending[9];
	AccessKeyKeyType int_array = access_key_tests_array_type(INT4OID, 4, true, 'i');
	static const char *const ab[1] = {"ab"};
	static const char *const ab_a[2] = {"ab", "a"};
	static const char *const b[1] = {"b"};
	StdVec		chain[3];
	bool		holds;

	access_key_tests_ordered(ordered);
	for (int i = 0; i < 9; i++)
		keys[i] = access_key_tests_array_column(INT4OID, 4, true, 'i', ordered[i], ENCODING_KEY_ORDER_ASC);
	for (int i = 0; i + 1 < 9; i++)
	{
		std_index_check(0, 2, RUST_FILE, 2219, 21);
		std_index_check(1, 2, RUST_FILE, 2219, 31);
		access_key_tests_assert_pair_lt(2219, 13, &keys[i], &keys[i + 1], true);
	}
	for (int i = 0; i < 9; i++)
	{
		bool		has;
		size_t		end;
		PgPgError	error;

		access_key_tests_ok(access_key_end(keys[i].ptr, keys[i].len, &int_array, ENCODING_KEY_ORDER_ASC, &has, &end,
										   &error), &error);
		STD_ASSERT(2227, 13, has && end == keys[i].len, "ok(end(key, &int_array, Order::ASC)) == Some(key.len())");
	}
	access_key_tests_ordered(ordered);
	for (int i = 0; i < 9; i++)
		descending[i] = access_key_tests_array_column(INT4OID, 4, true, 'i', ordered[i], ENCODING_KEY_ORDER_DESC);
	for (int i = 0; i + 1 < 9; i++)
	{
		std_index_check(0, 2, RUST_FILE, 2232, 21);
		std_index_check(1, 2, RUST_FILE, 2232, 31);
		access_key_tests_assert_pair_lt(2232, 13, &descending[i + 1], &descending[i], false);
	}
	chain[0] = access_key_tests_text_array(ab, 1);
	chain[1] = access_key_tests_text_array(ab_a, 2);
	holds = access_key_tests_vec_lt(&chain[0], &chain[1]);
	if (holds)
	{
		chain[2] = access_key_tests_text_array(b, 1);
		holds = access_key_tests_vec_lt(&chain[1], &chain[2]);
		access_key_tests_free(&chain[2]);
	}
	access_key_tests_free(&chain[0]);
	access_key_tests_free(&chain[1]);
	STD_ASSERT(2236, 9, holds, "text(&[\"ab\"]) < text(&[\"ab\", \"a\"]) && text(&[\"ab\", \"a\"]) < text(&[\"b\"])");
	for (int i = 0; i < 9; i++)
	{
		access_key_tests_free(&keys[i]);
		access_key_tests_free(&descending[i]);
	}
	access_key_KeyType_drop(&int_array);
}

/* access_key_tests_collated is the collation test's collated: text under collation 1, deterministic or not. */
static AccessKeyKeyType
access_key_tests_collated(bool deterministic)
{
	AccessKeyKeyType key_type = {.typid = TEXTOID, .collation = {ACCESS_KEY_COLLATION_SORTED, 1, deterministic}};

	return key_type;
}

/* access_key_tests_encoded encodes a value of a key type where the call must succeed. */
static StdVec
access_key_tests_encoded(const AccessKeyKeyType *key_type, AccessKeyValue value, EncodingKeyOrder order)
{
	StdVec		out;
	PgPgError	error;

	std_vec_new(&out, ACCESS_KEY_BYTES);
	access_key_tests_ok(access_key_encode(&out, ACCESS_KEY_REVERSED, key_type, true, value, order, &error), &error);
	return out;
}

/*
 * test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes_closure_1
 * is the test's write: the sort key "ba", then the text "ab".
 */
static void
test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes_closure_1(void *captures, StdVec *out)
{
	encoding_key_bytes(out, (const uint8 *) "ba", 2);
	encoding_key_text(out, (const uint8 *) "ab", 2);
}

/* access_key_tests_assert_seven is assert_eq!(rest, &[7]) at a place. */
static void
access_key_tests_assert_seven(uint32 line, uint32 col, StdBytes rest)
{
	uint8		seven = 7;
	StdBytes	expected = {&seven, 1};

	if (!access_key_tests_bytes_eq(rest.ptr, rest.len, &seven, 1))
		std_assert_failed(RUST_FILE, line, col, "==", &rest, std_bytes_Debug_fmt, &expected, std_bytes_Debug_fmt, NULL);
}

/*
 * The fork's own: a text under a collation keys as its sort key and then,
 * under a deterministic one, its bytes, which decode gives back; under a
 * nondeterministic one the key is the sort key alone and not exact.
 */
void
test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes(void)
{
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, ENCODING_KEY_ORDER_DESC};
	AccessKeyKeyType strict = access_key_tests_collated(true);
	AccessKeyKeyType loose_type = access_key_tests_collated(false);
	StdVec		chain[2];
	bool		holds;
	uint8		seven = 7;

	for (int o = 0; o < 2; o++)
	{
		StdVec		out = access_key_tests_encoded(&strict, access_key_tests_bytes("ab", 2), orders[o]);
		StdVec		plain;
		StdVec		ab = access_key_tests_of("ab", 2);
		access_key_tests_decoded_t decoded;
		StdVec		loose;

		std_vec_new(&plain, ACCESS_KEY_BYTES);
		encoding_key_column(&plain, orders[o],
							test_access_key_tests_collated_text_keys_are_their_sort_keys_then_their_bytes_closure_1, NULL,
							NULL);
		access_key_tests_assert_vec_eq(2257, 13, access_key_tests_of(out.ptr, out.len), plain);
		std_vec_push_Checked(&out, &seven, ACCESS_KEY_BYTES);
		decoded = access_key_tests_decode(access_key_tests_slice(out.ptr, out.len), &strict, orders[o]);
		access_key_tests_assert_pair(2260, 13, &decoded, &ab, 7, NULL);
		access_key_tests_decoded_drop(&decoded);
		access_key_tests_assert_seven(2261, 13,
									  access_key_tests_skipped(access_key_tests_slice(out.ptr, out.len), &strict, orders[o]));
		loose = access_key_tests_encoded(&loose_type, access_key_tests_bytes("ab", 2), orders[o]);
		std_vec_push_Checked(&loose, &seven, ACCESS_KEY_BYTES);
		access_key_tests_assert_seven(2265, 13,
									  access_key_tests_skipped(access_key_tests_slice(loose.ptr, loose.len), &loose_type, orders[o]));
		access_key_tests_free(&loose);
		access_key_tests_free(&ab);
		access_key_tests_free(&out);
	}
	STD_ASSERT(2267, 9, access_key_is_exact(&strict) && !access_key_is_exact(&loose_type),
			   "is_exact(&collated(true)) && !is_exact(&collated(false))");
	{
		AccessKeyValue ab = access_key_tests_bytes("ab", 2);
		AccessKeyValue ba = access_key_tests_bytes("ba", 2);

		chain[0] = access_key_tests_key(TEXTOID, &ab, ENCODING_KEY_ORDER_ASC);
		chain[1] = access_key_tests_key(TEXTOID, &ba, ENCODING_KEY_ORDER_ASC);
		holds = access_key_tests_chain_lt(chain, 2);
		if (holds)
		{
			chain[0] = access_key_tests_encoded(&strict, ab, ENCODING_KEY_ORDER_ASC);
			chain[1] = access_key_tests_encoded(&strict, ba, ENCODING_KEY_ORDER_ASC);
			holds = access_key_tests_vec_lt(&chain[1], &chain[0]);
			access_key_tests_free(&chain[0]);
			access_key_tests_free(&chain[1]);
		}
		STD_ASSERT(2274, 9, holds, "order(b\"ab\") < order(b\"ba\") && sorted(b\"ab\") > sorted(b\"ba\")");
	}
}

/* access_key_tests_compared is the compared test's compared: a key type compared through its class. */
static AccessKeyKeyType
access_key_tests_compared(bool by_value)
{
	AccessKeyKeyType key_type = {.typid = 0, .inner = {.tag = ACCESS_KEY_INNER_COMPARED, .u.by_value = by_value}};

	return key_type;
}

/*
 * The fork's own: a compared value's key is its stored bytes, which decode
 * gives back; an enum's is its label's sort order, refusing a label the
 * type doesn't have; and both key types survive the bytes a slot keeps them
 * as.
 */
void
test_access_key_tests_compared_and_enum_keys_are_their_bytes_and_sort_orders(void)
{
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, ENCODING_KEY_ORDER_DESC};
	AccessKeyKeyType labels = {.typid = 0, .inner.tag = ACCESS_KEY_INNER_ENUM};
	access_TupleU32F32_t five_label = {5, 2.0f};
	access_TupleU32F32_t nine_label = {9, 1.5f};
	StdVec		five;
	StdVec		nine;
	PgPgError	error;
	StdVec		out;
	bool		ok;
	AccessKeyKeyType key_types[2];

	for (int o = 0; o < 2; o++)
	{
		for (int c = 0; c < 2; c++)
		{
			bool		by_value = c == 0;
			AccessKeyKeyType compared = access_key_tests_compared(by_value);
			StdVec		bytes = access_key_tests_of("a\0b", 3);
			AccessKeyValue value = by_value ? access_key_tests_word(0x0102) : access_key_tests_bytes(bytes.ptr, bytes.len);
			uint8		seven = 7;
			access_key_tests_decoded_t decoded;

			out = access_key_tests_encoded(&compared, value, orders[o]);
			std_vec_push_Checked(&out, &seven, ACCESS_KEY_BYTES);
			decoded = access_key_tests_decode(access_key_tests_slice(out.ptr, out.len), &compared, orders[o]);
			if (by_value)
			{
				AccessKeyDecoded expected = {ACCESS_KEY_DECODED_WORD, .u.word = 0x0102};
				access_key_tests_pair_t left = {{decoded.has, &decoded.value}, decoded.rest};
				access_key_tests_pair_t right = {{true, &expected}, {&seven, 1}};

				if (!access_key_tests_option_decoded_eq(&left.value, &right.value) ||
					!access_key_tests_bytes_eq(left.rest.ptr, left.rest.len, &seven, 1))
					std_assert_failed(RUST_FILE, 2292, 17, "==", &left, access_key_tests_pair_Debug_fmt,
									  &right, access_key_tests_pair_Debug_fmt, NULL);
			}
			else
				access_key_tests_assert_pair(2292, 17, &decoded, &bytes, 7, NULL);
			access_key_tests_decoded_drop(&decoded);
			access_key_tests_assert_seven(2293, 17,
										  access_key_tests_skipped(access_key_tests_slice(out.ptr, out.len), &compared, orders[o]));
			access_key_tests_free(&out);
			access_key_tests_free(&bytes);
		}
	}
	std_vec_new(&labels.inner.u.labels, ACCESS_KEY_LABEL);
	std_vec_push_Checked(&labels.inner.u.labels, &five_label, ACCESS_KEY_LABEL);
	std_vec_push_Checked(&labels.inner.u.labels, &nine_label, ACCESS_KEY_LABEL);
	five = access_key_tests_encoded(&labels, access_key_tests_word(pg_word_from_oid(5)), ENCODING_KEY_ORDER_ASC);
	nine = access_key_tests_encoded(&labels, access_key_tests_word(pg_word_from_oid(9)), ENCODING_KEY_ORDER_ASC);
	STD_ASSERT(2302, 9,
			   access_key_tests_vec_lt(&nine, &five) &&
			   access_key_tests_skipped(access_key_tests_slice(five.ptr, five.len), &labels, ENCODING_KEY_ORDER_ASC).len == 0,
			   "nine < five && ok(skip(&five, &labels, Order::ASC)).is_empty()");
	access_key_tests_free(&five);
	access_key_tests_free(&nine);
	std_vec_new(&out, ACCESS_KEY_BYTES);
	ok = access_key_encode(&out, ACCESS_KEY_REVERSED, &labels, true, access_key_tests_word(pg_word_from_oid(6)),
						   ENCODING_KEY_ORDER_ASC, &error);
	access_key_tests_free(&out);
	error = access_key_tests_refused(ok, &error);
	access_key_tests_assert_message(2303, 9, &error, "invalid internal value for enum: 6");
	pg_PgError_drop(&error);
	key_types[0] = access_key_tests_compared(true);
	key_types[1] = labels;
	for (int k = 0; k < 2; k++)
	{
		StdVec		written;
		StdVec		again;
		AccessKeyKeyType read;
		StdBytes	rest;

		std_vec_new(&written, ACCESS_KEY_BYTES);
		access_key_KeyType_write(&key_types[k], &written);
		if (!access_key_KeyType_read(written.ptr, written.len, &read, &rest))
		{
			StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&written, access_key_tests_vec_Debug_fmt}};
			StdFmtArguments arguments = {pieces, 1, args};

			std_panic_fmt(RUST_FILE, 2307, 69, &arguments);
		}
		std_vec_new(&again, ACCESS_KEY_BYTES);
		access_key_KeyType_write(&read, &again);
		STD_ASSERT(2310, 13,
				   access_key_tests_bytes_eq(again.ptr, again.len, written.ptr, written.len) && rest.len == 0,
				   "again == written && rest.is_empty()");
		access_key_KeyType_drop(&read);
		access_key_tests_free(&again);
		access_key_tests_free(&written);
		access_key_KeyType_drop(&key_types[k]);
	}
}

/* access_key_tests_lexeme_t is a tsvector's lexeme as the test gives it: its word and its positions. */
typedef struct access_key_tests_lexeme_t
{
	const uint8 *word;
	size_t		word_len;
	const uint16 *positions;
	size_t		positions_len;
} access_key_tests_lexeme_t;

/* access_key_tests_tsvector is tests' tsvector: a tsvector's stored form from its lexemes in order. */
static StdVec
access_key_tests_tsvector(const access_key_tests_lexeme_t *lexemes, size_t len)
{
	StdVec		entries;
	StdVec		strings;
	StdVec		stored;
	uint8		zero = 0;

	std_vec_new(&entries, ACCESS_KEY_BYTES);
	std_vec_new(&strings, ACCESS_KEY_BYTES);
	for (size_t i = 0; i < len; i++)
	{
		uint32		entry = (uint32) (lexemes[i].positions_len != 0) | (uint32) lexemes[i].word_len << 1 |
			(uint32) strings.len << 12;

		access_key_le32(&entries, entry);
		std_vec_extend_from_slice_Checked(&strings, lexemes[i].word, lexemes[i].word_len, ACCESS_KEY_BYTES);
		if (lexemes[i].positions_len != 0)
		{
			std_vec_resize(&strings, std_next_multiple_of_usize(strings.len, 2), &zero, ACCESS_KEY_BYTES);
			access_key_le16(&strings, (uint16) lexemes[i].positions_len);
			for (size_t p = 0; p < lexemes[i].positions_len; p++)
				access_key_le16(&strings, lexemes[i].positions[p]);
		}
	}
	std_vec_new(&stored, ACCESS_KEY_BYTES);
	access_key_le32(&stored, (uint32) len);
	std_vec_extend_from_slice_Checked(&stored, entries.ptr, entries.len, ACCESS_KEY_BYTES);
	std_vec_extend_from_slice_Checked(&stored, strings.ptr, strings.len, ACCESS_KEY_BYTES);
	access_key_tests_free(&entries);
	access_key_tests_free(&strings);
	return stored;
}

/*
 * Bugs row 70: a tsvector's key is exact, and a tsvector of many short
 * lexemes reads back from its key whole in either order, its bound the
 * stored form's size.
 */
void
test_access_key_tests_a_tsvector_of_many_short_lexemes_reads_back_from_its_key(void)
{
	static const uint16 positions[2] = {1, 2 | 0x4000};
	uint8		words[26];
	access_key_tests_lexeme_t lexemes[26];
	StdVec		bytes;
	AccessKeyKeyType key_type = access_key_KeyType_of(TSVECTOROID);
	EncodingKeyOrder orders[2] = {ENCODING_KEY_ORDER_ASC, {true, true}};

	for (int i = 0; i < 26; i++)
	{
		words[i] = (uint8) ('a' + i);
		lexemes[i] = (access_key_tests_lexeme_t)
		{
			&words[i], 1, i % 3 == 0 ? positions : NULL, i % 3 == 0 ? 2 : 0
		};
	}
	bytes = access_key_tests_tsvector(lexemes, 26);
	STD_ASSERT(2347, 9, access_key_is_exact(&key_type), "is_exact(&key_type)");
	for (int o = 0; o < 2; o++)
	{
		StdVec		key = access_key_tests_stored(TSVECTOROID, &bytes, orders[o]);
		uint8		nine = 9;
		access_key_tests_decoded_t decoded;

		STD_ASSERT(2350, 13, bytes.len > key.len, "bytes.len() > key.len()");
		access_key_tests_assert_usize(2351, 13,
									  access_key_tests_decoded_max(&key_type, orders[o], key.ptr, key.len), bytes.len);
		std_vec_push_Checked(&key, &nine, ACCESS_KEY_BYTES);
		decoded = access_key_tests_decode(access_key_tests_slice(key.ptr, key.len), &key_type, orders[o]);
		access_key_tests_assert_pair(2354, 13, &decoded, &bytes, 9, NULL);
		access_key_tests_decoded_drop(&decoded);
		access_key_tests_assert_nine(2355, 13,
									 access_key_tests_skipped(access_key_tests_slice(key.ptr, key.len), &key_type, orders[o]), NULL);
		access_key_tests_free(&key);
	}
	access_key_tests_free(&bytes);
}

/*
 * test_access_key_tests_an_answer_of_none_leaves_its_length_as_it_was: an
 * enum key too short for its value answers no end, and an address family
 * whose mask passes its address answers no length, each leaving its
 * caller's length as the caller had it (bugs row 288).
 */
void
test_access_key_tests_an_answer_of_none_leaves_its_length_as_it_was(void)
{
	static const uint8 key[] = {ENCODING_KEY_VALUE, 0, 0};
	AccessKeyKeyType key_type = {.inner.tag = ACCESS_KEY_INNER_ENUM};
	PgPgError	error;
	size_t		end = 0x5EED;
	size_t		len = 0x5EED;
	bool		has = true;

	access_key_tests_ok(access_key_end(key, sizeof(key), &key_type, ENCODING_KEY_ORDER_ASC, &has, &end, &error),
						&error);
	STD_ASSERT(__LINE__, 5, !has && end == 0x5EED, "a short enum key answers no end and leaves its end as it was");
	STD_ASSERT(__LINE__, 5, !access_key_address_len(ACCESS_KEY_IPV4, 33, &len) && len == 0x5EED,
			   "a mask past its address answers no length and leaves its length as it was");
}
#endif
#endif
